package com.cyd.nowplaying

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.ClipData
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView

// One screen: the two permissions the app needs, which CYD to send to, and how it's going.
// The work itself happens in MediaListener, which runs whether or not this screen is open.
class MainActivity : Activity() {
    private lateinit var accessText: TextView
    private lateinit var accessButton: Button
    private lateinit var bluetoothText: TextView
    private lateinit var bluetoothButton: Button
    private lateinit var devices: RadioGroup
    private lateinit var statusText: TextView
    private lateinit var playingText: TextView
    private val refresh: () -> Unit = { showStatus() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = dp(20)
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        val scroll = ScrollView(this).apply { addView(column) }
        // Android 15 draws apps under the status and navigation bars: keep clear of them.
        scroll.setOnApplyWindowInsetsListener { v, insets ->
            @Suppress("DEPRECATION")
            v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            insets
        }
        setContentView(scroll)

        column.addView(text("Shows what's playing on this phone on the CYD remote: title, artist, album and cover. Keep the CYD paired as a Bluetooth remote; the music stays on the phone.", 15f))

        column.addView(heading("1. Notification access"))
        accessText = text("", 15f)
        column.addView(accessText)
        accessButton = button("Open notification access") { openNotificationAccess() }
        column.addView(accessButton)

        column.addView(heading("2. Nearby devices"))
        bluetoothText = text("", 15f)
        column.addView(bluetoothText)
        bluetoothButton = button("Allow") {
            if (Build.VERSION.SDK_INT >= 31) requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_CONNECT), 1)
        }
        column.addView(bluetoothButton)

        column.addView(heading("3. Your CYD"))
        column.addView(text("Pair it in Bluetooth settings first (it's called CYD Remote), then pick it here.", 15f))
        devices = RadioGroup(this)
        column.addView(devices)
        column.addView(button("Bluetooth settings") { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) })

        column.addView(heading("Status"))
        statusText = text("", 15f)
        column.addView(statusText)
        playingText = text("", 15f)
        column.addView(playingText)

        column.addView(heading("Share"))
        column.addView(text("Send this app to another phone, by Quick Share or any other way of sending files.", 15f))
        column.addView(button("Share this app") { shareApp() })
    }

    override fun onResume() {
        super.onResume()
        AppState.listen(refresh)
        val access = hasNotificationAccess()
        accessText.text = if (access) "Allowed. The app reads only what's playing, not your notifications."
        else "Needed to see what's playing in other apps. Turn on CYD Now Playing on the next screen. If it's greyed out, see the README (App info > ⋮ > Allow restricted settings)."
        accessButton.visibility = if (access) View.GONE else View.VISIBLE
        // After an update the system may not have restarted the listener yet.
        if (access && MediaListener.instance == null) {
            NotificationListenerService.requestRebind(ComponentName(this, MediaListener::class.java))
        }

        val bt = hasBluetoothPermission()
        bluetoothText.text = if (bt) "Allowed." else "Needed to talk to the CYD."
        bluetoothButton.visibility = if (bt) View.GONE else View.VISIBLE
        showDevices()
        showStatus()
    }

    override fun onPause() {
        AppState.unlisten(refresh)
        super.onPause()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        MediaListener.instance?.reconnect()
        onResume()
    }

    private fun showStatus() {
        statusText.text = when {
            !hasNotificationAccess() -> "Waiting for notification access"
            MediaListener.instance == null -> "Starting…"
            else -> AppState.status
        }
        playingText.text = if (MediaListener.instance != null) "Playing: ${AppState.nowPlaying}" else ""
    }

    @SuppressLint("MissingPermission")  // pairedDevices() is empty without the permission
    private fun showDevices() {
        devices.setOnCheckedChangeListener(null)
        devices.removeAllViews()
        val paired = AppState.pairedDevices(this)
        val chosen = AppState.deviceAddress(this)
        if (paired.isEmpty()) {
            devices.addView(text(if (hasBluetoothPermission()) "No paired devices yet." else "Allow nearby devices to see them.", 15f))
            return
        }
        for ((i, d) in paired.withIndex()) {
            devices.addView(RadioButton(this).apply {
                id = 1000 + i
                text = "${d.name ?: "Unnamed"}  ${d.address}"
                tag = d.address
                isChecked = d.address == chosen
            })
        }
        devices.setOnCheckedChangeListener { group, id ->
            val address = group.findViewById<RadioButton>(id)?.tag as? String ?: return@setOnCheckedChangeListener
            AppState.chooseDevice(this, address)
        }
    }

    private fun hasNotificationAccess(): Boolean {
        val enabled = Settings.Secure.getString(contentResolver, "enabled_notification_listeners") ?: return false
        return enabled.split(':').any { ComponentName.unflattenFromString(it)?.packageName == packageName }
    }

    private fun hasBluetoothPermission() =
        Build.VERSION.SDK_INT < 31 || checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun openNotificationAccess() {
        val me = ComponentName(this, MediaListener::class.java)
        val detail = if (Build.VERSION.SDK_INT >= 30) {
            Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS)
                .putExtra(Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME, me.flattenToString())
        } else null
        try {
            startActivity(detail ?: Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        } catch (e: Exception) {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
    }

    private fun shareApp() {
        val uri = ApkProvider.uri(packageName)
        val send = Intent(Intent.ACTION_SEND)
            .setType(ApkProvider.MIME)
            .putExtra(Intent.EXTRA_STREAM, uri)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        send.clipData = ClipData.newRawUri(ApkProvider.NAME, uri)  // carries the permission through the chooser
        startActivity(Intent.createChooser(send, "Share CYD Now Playing"))
    }

    // --- views ---

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun text(s: String, size: Float) = TextView(this).apply {
        text = s
        textSize = size
        setPadding(0, dp(4), 0, dp(4))
    }

    private fun heading(s: String) = text(s, 18f).apply {
        setTypeface(typeface, Typeface.BOLD)
        setPadding(0, dp(20), 0, dp(4))
    }

    private fun button(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        setOnClickListener { onClick() }
    }
}
