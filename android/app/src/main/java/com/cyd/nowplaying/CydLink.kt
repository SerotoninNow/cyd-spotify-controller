package com.cyd.nowplaying

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.util.Log

// The Bluetooth LE link to the CYD. The CYD is already paired with the phone as a media
// remote, so this opens a GATT connection on the same link and writes to its now-playing
// service. Writes go one at a time (Android allows one in flight); the newest song details
// go before any cover pieces, and a new cover replaces one still on its way.
//
// On connecting it also reads which firmware the CYD runs, and it can send a firmware update;
// while one runs, nothing else is sent.
//
// Everything runs on `handler`'s thread; Bluetooth callbacks are passed over to it.
@SuppressLint("MissingPermission")  // checked by hasPermission() before anything starts
class CydLink(
    private val context: Context,
    private val handler: Handler,
    private val onStatus: (String) -> Unit,
) {
    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var infoCharacteristic: BluetoothGattCharacteristic? = null
    private var updateCharacteristic: BluetoothGattCharacteristic? = null
    private var address: String? = null
    private var deviceName = "CYD"
    private var maxWrite = 20  // bytes per write: the MTU less 3, at most MAX_VALUE
    private var busy = false
    private var refreshed = false  // asked Android to forget its cached service list
    private var retryDelay = FIRST_RETRY  // ms; grows while reconnecting keeps failing
    // Reads and settings that go before any writes once connected; each returns whether it
    // started (and so has a callback coming).
    private val setup = ArrayDeque<(BluetoothGatt) -> Boolean>()

    private var song: Protocol.Song? = null
    private var songDirty = false
    private var coverId = 0
    private var cover: ByteArray? = null  // null: no cover
    private val coverQueue = ArrayDeque<ByteArray>()

    // Firmware update: begin, wait for "ready", the image in order, end, wait for "done".
    private enum class Phase { IDLE, BEGIN, WAIT_READY, DATA, WAIT_DONE }
    private var phase = Phase.IDLE
    private var image: ByteArray? = null
    private var imageSent = 0
    private var imageAcked = 0  // what the CYD has confirmed writing ("ack:<n>")
    private var shownPercent = -1

    val ready get() = characteristic != null
    val hasDevice get() = address != null
    val canUpdate get() = ready && updateCharacteristic != null && phase == Phase.IDLE

    fun hasPermission() =
        Build.VERSION.SDK_INT < 31 ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    fun connect(address: String?) {
        close()
        this.address = address
        if (address == null) return onStatus("No CYD chosen")
        if (!hasPermission()) return onStatus("Needs the Nearby devices permission")
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null || !adapter.isEnabled) {
            onStatus("Bluetooth is off")
            retryLater()
            return
        }
        val device = adapter.getRemoteDevice(address)
        deviceName = device.name ?: address
        refreshed = false
        onStatus("Waiting for $deviceName")
        // autoConnect: Android connects whenever the CYD is in range, with no time limit.
        gatt = device.connectGatt(context, true, callback, BluetoothDevice.TRANSPORT_LE)
    }

    fun close() {
        if (phase != Phase.IDLE) failUpdate("the connection closed")
        handler.removeCallbacks(reconnect)
        gatt?.close()
        gatt = null
        characteristic = null
        infoCharacteristic = null
        updateCharacteristic = null
        setup.clear()
        busy = false
        coverQueue.clear()
        AppState.setCydFirmware(null)
    }

    fun sendSong(s: Protocol.Song) {
        song = s
        songDirty = true
        pump()
    }

    fun sendCover(id: Int, jpeg: ByteArray?) {
        if (id == coverId && jpeg.contentEquals(cover)) return
        coverId = id
        cover = jpeg
        queueCover()
        pump()
    }

    // Sends `firmware` to the CYD, which installs it and restarts. Progress goes to AppState.
    fun startUpdate(firmware: ByteArray): Boolean {
        if (!canUpdate) return false
        image = firmware
        imageSent = 0
        imageAcked = 0
        shownPercent = -1
        phase = Phase.BEGIN
        gatt?.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
        AppState.setUpdate("Starting the update…", true)
        heardFromCyd()
        pump()
        return true
    }

    // An update gives up if the CYD goes quiet: every step and every 8 KB brings an answer.
    private val stalled = Runnable { failUpdate("the CYD stopped answering") }

    private fun heardFromCyd() {
        handler.removeCallbacks(stalled)
        handler.postDelayed(stalled, UPDATE_TIMEOUT)
    }

    private fun queueCover() {
        coverQueue.clear()
        val jpeg = cover
        coverQueue.add(Protocol.cover(coverId, jpeg?.size ?: 0))
        if (jpeg != null) coverQueue.addAll(Protocol.pieces(coverId, jpeg, maxWrite))
    }

    private fun pump() {
        val g = gatt ?: return
        if (busy) return
        while (setup.isNotEmpty()) {
            if (setup.removeFirst()(g)) {
                busy = true
                return
            }
        }
        if (phase != Phase.IDLE) return pumpUpdate(g)
        val c = characteristic ?: return
        val packet = when {
            songDirty -> song?.let { Protocol.song(it, maxWrite) }.also { songDirty = false }
            coverQueue.isNotEmpty() -> coverQueue.removeFirst()
            else -> null
        } ?: return
        val sent = try {
            write(g, c, packet)
        } catch (e: Exception) {  // a packet Android refuses outright: skip it rather than crash
            Log.w(TAG, "write refused", e)
            handler.post { pump() }
            return
        }
        if (sent) {
            busy = true
        } else {  // the stack is busy with something else: try again shortly
            if (packet[0].toInt() == 1) songDirty = true else coverQueue.addFirst(packet)
            handler.postDelayed({ pump() }, 50)
        }
    }

    private fun pumpUpdate(g: BluetoothGatt) {
        val c = updateCharacteristic ?: return failUpdate("the CYD has no update service")
        val img = image ?: return failUpdate("no firmware")
        val data = phase == Phase.DATA && imageSent < img.size
        // Data goes unacknowledged, many writes to a connection event, but no further ahead of
        // what the CYD has confirmed than UPDATE_WINDOW: it writes to flash as it goes.
        if (data && imageSent - imageAcked >= UPDATE_WINDOW) return
        val packet = when {
            phase == Phase.BEGIN -> Protocol.updateBegin(img.size, Firmware.sha256(img))
            data -> Protocol.updateData(imageSent, img, maxWrite)
            phase == Phase.DATA -> Protocol.updateEnd()
            else -> return  // waiting for the CYD's answer
        }
        val sent = try {
            write(g, c, packet, if (data) BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE else BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        } catch (e: Exception) {
            Log.w(TAG, "update write refused", e)
            return failUpdate("Android refused a write")
        }
        if (!sent) {
            handler.postDelayed({ pump() }, 50)
            return
        }
        busy = true
        when {
            phase == Phase.BEGIN -> phase = Phase.WAIT_READY
            imageSent < img.size -> {
                imageSent += packet.size - Protocol.UPDATE_DATA_HEADER
                val percent = (imageSent.toLong() * 100 / img.size).toInt()
                if (percent != shownPercent) {
                    shownPercent = percent
                    AppState.setUpdate("Sending the firmware: $percent%", true)
                }
            }
            else -> {
                phase = Phase.WAIT_DONE
                AppState.setUpdate("Checking and installing…", true)
            }
        }
    }

    // A notification from the CYD about the update.
    private fun onUpdateMessage(msg: String) {
        if (!msg.startsWith("ack:")) Log.i(TAG, "update: $msg")
        if (phase != Phase.IDLE) heardFromCyd()
        when {
            msg == "ready" && phase == Phase.WAIT_READY -> {
                phase = Phase.DATA
                pump()
            }
            msg.startsWith("ack:") && phase == Phase.DATA -> {
                imageAcked = msg.removePrefix("ack:").toIntOrNull() ?: imageAcked
                pump()
            }
            msg == "done" && phase == Phase.WAIT_DONE -> {
                endUpdate()
                AppState.setUpdate("Installed. The CYD is restarting with the new firmware.", false)
            }
            msg.startsWith("err:") -> failUpdate("the CYD said: ${msg.removePrefix("err:")}")
        }
    }

    private fun failUpdate(why: String) {
        if (phase == Phase.IDLE) return
        val atEnd = phase == Phase.WAIT_DONE
        endUpdate()
        AppState.setUpdate(
            if (atEnd) "The link dropped as the update finished; the firmware line shows whether it was installed."
            else "Update stopped: $why. The CYD keeps the firmware it had.",
            false,
        )
        handler.post { pump() }  // back to song details
    }

    private fun endUpdate() {
        handler.removeCallbacks(stalled)
        phase = Phase.IDLE
        image = null
        gatt?.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_BALANCED)
    }

    @Suppress("DEPRECATION")
    // Acknowledged by default, so everything arrives in order and Android reports each.
    private fun write(
        g: BluetoothGatt,
        c: BluetoothGattCharacteristic,
        value: ByteArray,
        type: Int = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
    ): Boolean {
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, value, type) == BluetoothStatusCodes.SUCCESS
        } else {
            c.writeType = type
            c.value = value
            g.writeCharacteristic(c)
        }
    }

    @Suppress("DEPRECATION")
    private fun enableNotifications(g: BluetoothGatt, c: BluetoothGattCharacteristic): Boolean {
        val d = c.getDescriptor(Protocol.NOTIFY_CONFIG) ?: return false
        if (!g.setCharacteristicNotification(c, true)) return false
        val on = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, on) == BluetoothStatusCodes.SUCCESS
        } else {
            d.value = on
            g.writeDescriptor(d)
        }
    }

    private val reconnect = Runnable { connect(address) }

    // Soon at first: a link that drops while the CYD restarts, or a first write refused before
    // the phone has re-encrypted the link, usually works on the next try. Slower if it keeps failing.
    private fun retryLater() {
        handler.removeCallbacks(reconnect)
        handler.postDelayed(reconnect, retryDelay)
        retryDelay = minOf(retryDelay * 2, LAST_RETRY)
    }

    // Android keeps the service list of a paired device; after reflashing the CYD it can be
    // stale and miss the now-playing service. BluetoothGatt.refresh() is hidden, hence reflection.
    private fun refreshCache(g: BluetoothGatt): Boolean = try {
        g.javaClass.getMethod("refresh").invoke(g) as Boolean
    } catch (e: Exception) {
        Log.w(TAG, "refresh failed", e)
        false
    }

    private fun onReady(c: BluetoothGattCharacteristic, info: BluetoothGattCharacteristic?, update: BluetoothGattCharacteristic?) {
        characteristic = c
        infoCharacteristic = info
        updateCharacteristic = update
        busy = false
        retryDelay = FIRST_RETRY
        onStatus("Connected to $deviceName")
        setup.clear()
        if (info != null) setup.add { g -> g.readCharacteristic(info) } else AppState.setCydFirmware(emptyMap())
        if (update != null) setup.add { g -> enableNotifications(g, update) }
        // The CYD forgets everything when the link drops: send it all again.
        songDirty = song != null
        queueCover()
        pump()
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            handler.post {
                if (g != gatt) return@post
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    onStatus("Connecting to $deviceName")
                    if (!g.requestMtu(517)) g.discoverServices()
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    Log.i(TAG, "disconnected, status $status")
                    failUpdate("the Bluetooth link dropped")
                    // Start over: a fresh autoConnect request waits for the CYD to come back.
                    close()
                    onStatus("Waiting for $deviceName")
                    retryLater()
            }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            handler.post {
                if (g != gatt) return@post
                maxWrite = minOf((if (status == BluetoothGatt.GATT_SUCCESS) mtu else 23) - 3, MAX_VALUE)
                Log.i(TAG, "MTU $mtu, writes of up to $maxWrite bytes")
                g.discoverServices()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            handler.post {
                if (g != gatt) return@post
                val service = g.getService(Protocol.SERVICE)
                val c = service?.getCharacteristic(Protocol.CHARACTERISTIC)
                val info = service?.getCharacteristic(Protocol.FIRMWARE_INFO)
                when {
                    // Without firmware info, either the firmware predates it or Android's list is stale.
                    c != null && (info != null || refreshed) ->
                        onReady(c, info, service.getCharacteristic(Protocol.FIRMWARE_UPDATE))
                    !refreshed -> {
                        refreshed = true
                        refreshCache(g)
                        handler.postDelayed({ if (g == gatt) g.discoverServices() }, 500)
                    }
                    else -> onStatus("$deviceName has no now-playing service: flash the remote firmware from this repository, or forget and pair it again")
            }
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            handler.post {
                if (g != gatt) return@post
                busy = false
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    Log.w(TAG, "write failed, status $status")
                    if (c.uuid == Protocol.FIRMWARE_UPDATE) return@post failUpdate("a write failed (status $status)")
                }
                pump()
            }
        }

        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
            handler.post {
                if (g != gatt) return@post
                if (c.uuid == Protocol.FIRMWARE_INFO) {
                    AppState.setCydFirmware(if (status == BluetoothGatt.GATT_SUCCESS) Protocol.firmwareInfo(value) else emptyMap())
                }
                busy = false
                pump()
            }
        }

        @Deprecated("Android 12 and older")
        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (Build.VERSION.SDK_INT < 33) onCharacteristicRead(g, c, c.value ?: ByteArray(0), status)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            handler.post {
                if (g != gatt) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) Log.w(TAG, "notification setup failed, status $status")
                busy = false
                pump()
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            val msg = String(value, Charsets.UTF_8)
            handler.post {
                if (g == gatt && c.uuid == Protocol.FIRMWARE_UPDATE) onUpdateMessage(msg)
            }
        }

        @Deprecated("Android 12 and older")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) onCharacteristicChanged(g, c, c.value ?: ByteArray(0))
        }
    }

    companion object {
        private const val TAG = "CydLink"
        // Android refuses longer writes even when the MTU would allow more (517 - 3 = 514).
        private const val MAX_VALUE = 512
        private const val FIRST_RETRY = 500L
        private const val LAST_RETRY = 5000L
        // Firmware bytes sent ahead of the CYD's last "ack" (it acks every 8 KB).
        private const val UPDATE_WINDOW = 24 * 1024
        private const val UPDATE_TIMEOUT = 15_000L
    }
}
