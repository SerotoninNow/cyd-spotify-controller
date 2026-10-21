package com.cyd.nowplaying

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.os.Handler
import android.os.Looper

// What the screen in MainActivity shows, and which CYD to talk to. Main thread only.
object AppState {
    var status = "Not running: turn on notification access"
        private set
    var nowPlaying = ""
        private set
    private val listeners = mutableSetOf<() -> Unit>()
    private val main = Handler(Looper.getMainLooper())

    fun listen(l: () -> Unit) = listeners.add(l)
    fun unlisten(l: () -> Unit) = listeners.remove(l)

    fun setStatus(s: String) {
        status = s
        main.post { listeners.toList().forEach { it() } }
    }

    fun setNowPlaying(s: String) {
        if (s == nowPlaying) return
        nowPlaying = s
        main.post { listeners.toList().forEach { it() } }
    }

    // What the connected CYD says it runs (see Firmware.check); null while not connected.
    var cydFirmware: Map<String, String>? = null
        private set

    fun setCydFirmware(info: Map<String, String>?) {
        if (info == cydFirmware) return
        cydFirmware = info
        main.post { listeners.toList().forEach { it() } }
    }

    // A firmware update's progress, or how the last one ended; "" if there's been none.
    var update = ""
        private set
    var updating = false
        private set

    fun setUpdate(text: String, running: Boolean) {
        update = text
        updating = running
        main.post { listeners.toList().forEach { it() } }
    }

    private fun prefs(c: Context) = c.getSharedPreferences("cyd", Context.MODE_PRIVATE)

    @SuppressLint("MissingPermission")  // callers check for BLUETOOTH_CONNECT
    fun pairedDevices(c: Context): List<BluetoothDevice> = try {
        c.getSystemService(BluetoothManager::class.java)?.adapter?.bondedDevices?.toList()
            ?.sortedBy { if (it.name?.contains("CYD", true) == true) 0 else 1 } ?: emptyList()
    } catch (e: SecurityException) {
        emptyList()
    }

    // The chosen CYD, else the first paired device with "CYD" in its name.
    @SuppressLint("MissingPermission")
    fun deviceAddress(c: Context): String? =
        prefs(c).getString("device", null)
            ?: pairedDevices(c).firstOrNull { it.name?.contains("CYD", true) == true }?.address

    fun chooseDevice(c: Context, address: String) {
        prefs(c).edit().putString("device", address).apply()
        MediaListener.instance?.reconnect()
    }
}
