package com.cyd.nowplaying

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
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
// Everything runs on `handler`'s thread; Bluetooth callbacks are passed over to it.
@SuppressLint("MissingPermission")  // checked by hasPermission() before anything starts
class CydLink(
    private val context: Context,
    private val handler: Handler,
    private val onStatus: (String) -> Unit,
) {
    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var address: String? = null
    private var deviceName = "CYD"
    private var maxWrite = 20  // bytes per write: the MTU less 3, at most MAX_VALUE
    private var busy = false
    private var refreshed = false  // asked Android to forget its cached service list
    private var retryDelay = FIRST_RETRY  // ms; grows while reconnecting keeps failing

    private var song: Protocol.Song? = null
    private var songDirty = false
    private var coverId = 0
    private var cover: ByteArray? = null  // null: no cover
    private val coverQueue = ArrayDeque<ByteArray>()

    val ready get() = characteristic != null
    val hasDevice get() = address != null

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
        handler.removeCallbacks(reconnect)
        gatt?.close()
        gatt = null
        characteristic = null
        busy = false
        coverQueue.clear()
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

    private fun queueCover() {
        coverQueue.clear()
        val jpeg = cover
        coverQueue.add(Protocol.cover(coverId, jpeg?.size ?: 0))
        if (jpeg != null) coverQueue.addAll(Protocol.pieces(coverId, jpeg, maxWrite))
    }

    private fun pump() {
        val g = gatt ?: return
        val c = characteristic ?: return
        if (busy) return
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

    @Suppress("DEPRECATION")
    private fun write(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray): Boolean {
        val type = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT  // acknowledged, so in order
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, value, type) == BluetoothStatusCodes.SUCCESS
        } else {
            c.writeType = type
            c.value = value
            g.writeCharacteristic(c)
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

    private fun onReady(c: BluetoothGattCharacteristic) {
        characteristic = c
        busy = false
        retryDelay = FIRST_RETRY
        onStatus("Connected to $deviceName")
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
                val c = g.getService(Protocol.SERVICE)?.getCharacteristic(Protocol.CHARACTERISTIC)
                when {
                    c != null -> onReady(c)
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
                if (status != BluetoothGatt.GATT_SUCCESS) Log.w(TAG, "write failed, status $status")
                busy = false
                pump()
            }
        }
    }

    companion object {
        private const val TAG = "CydLink"
        // Android refuses longer writes even when the MTU would allow more (517 - 3 = 514).
        private const val MAX_VALUE = 512
        private const val FIRST_RETRY = 500L
        private const val LAST_RETRY = 5000L
    }
}
