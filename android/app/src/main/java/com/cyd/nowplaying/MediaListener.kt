package com.cyd.nowplaying

import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import android.util.Log
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors
import java.util.zip.CRC32

// Runs while the app has notification access, which is what lets it see every app's media
// session (Spotify, YouTube Music, podcasts...). It follows the session that's playing and
// sends its details to the CYD. Notifications themselves are ignored.
class MediaListener : NotificationListenerService() {
    private val handler = Handler(Looper.getMainLooper())
    private val artWorker = Executors.newSingleThreadExecutor()
    private lateinit var sessions: MediaSessionManager
    private lateinit var audio: AudioManager
    private lateinit var link: CydLink
    private var controllers: List<MediaController> = emptyList()
    private var current: MediaController? = null
    private var coverKey: Any? = null  // what the last cover was made from
    private var coverJob = 0
    private var running = false

    override fun onListenerConnected() {
        if (running) return
        running = true
        instance = this
        sessions = getSystemService(MediaSessionManager::class.java)
        audio = getSystemService(AudioManager::class.java)
        link = CydLink(this, handler) { AppState.setStatus(it) }
        sessions.addOnActiveSessionsChangedListener(sessionsChanged, me(), handler)
        @Suppress("UnspecifiedRegisterReceiverFlag")
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            registerReceiver(volumeChanged, IntentFilter(VOLUME_CHANGED), RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(volumeChanged, IntentFilter(VOLUME_CHANGED))
        }
        follow(sessions.getActiveSessions(me()))
        reconnect()
        handler.post(resync)
        Log.i(TAG, "listening")
    }

    override fun onListenerDisconnected() = stop()

    private fun me() = ComponentName(this, MediaListener::class.java)

    override fun onDestroy() {
        stop()
        artWorker.shutdownNow()
        super.onDestroy()
    }

    private fun stop() {
        if (!running) return
        running = false
        if (instance === this) instance = null
        sessions.removeOnActiveSessionsChangedListener(sessionsChanged)
        unregisterReceiver(volumeChanged)
        controllers.forEach { it.unregisterCallback(sessionCallback) }
        controllers = emptyList()
        current = null
        handler.removeCallbacks(resync)
        link.close()
        AppState.setStatus("Stopped")
    }

    // Connects to the CYD chosen in the app, or the first paired device whose name has "CYD".
    fun reconnect() {
        link.connect(AppState.deviceAddress(this))
        AppState.setNowPlaying(describe())
    }

    // The CYD may be paired after the listener started, when there was none to pick yet.
    fun reconnectIfNoDevice() {
        if (running && !link.hasDevice) reconnect()
    }

    private val sessionsChanged = MediaSessionManager.OnActiveSessionsChangedListener { list ->
        follow(list ?: emptyList())
    }

    // Watches every active session, so that whichever starts playing takes over the screen.
    private fun follow(list: List<MediaController>) {
        controllers.forEach { it.unregisterCallback(sessionCallback) }
        controllers = list
        controllers.forEach { it.registerCallback(sessionCallback, handler) }
        update()
    }

    private val sessionCallback = object : MediaController.Callback() {
        override fun onPlaybackStateChanged(state: PlaybackState?) = update()
        override fun onMetadataChanged(metadata: MediaMetadata?) = update()
        override fun onAudioInfoChanged(info: MediaController.PlaybackInfo) = update()
        override fun onSessionDestroyed() = follow(sessions.getActiveSessions(me()))
    }

    private val volumeChanged = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) = update()
    }

    // While playing, the CYD's clock drifts from the player's: line them up now and then.
    // Also picks up a CYD paired since the last look.
    private val resync = object : Runnable {
        override fun run() {
            reconnectIfNoDevice()
            if (current?.playbackState?.state == PlaybackState.STATE_PLAYING) update()
            handler.postDelayed(this, 15_000)
        }
    }

    // The playing session, else the most recent one (Android lists them newest first).
    private fun pick(): MediaController? =
        controllers.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: controllers.firstOrNull { it.metadata != null }

    private fun update() {
        if (!running) return
        current = pick()
        val c = current
        val song = c?.let { songOf(it) } ?: Protocol.Song(Protocol.STATE_NONE, volume(null), 0, 0, "", "", "", "")
        link.sendSong(song)
        updateCover(c?.metadata)
        AppState.setNowPlaying(describe())
    }

    private fun describe(): String {
        val m = current?.metadata ?: return "Nothing playing"
        val title = m.getString(MediaMetadata.METADATA_KEY_TITLE) ?: "?"
        return "$title  (${playerName(current!!.packageName)})"
    }

    private fun songOf(c: MediaController): Protocol.Song {
        val m = c.metadata
        val ps = c.playbackState
        val playing = ps?.state == PlaybackState.STATE_PLAYING
        var position = 0L
        if (ps != null && ps.position != PlaybackState.PLAYBACK_POSITION_UNKNOWN) {
            position = ps.position
            if (playing && ps.lastPositionUpdateTime > 0) {
                position += ((SystemClock.elapsedRealtime() - ps.lastPositionUpdateTime) * ps.playbackSpeed).toLong()
            }
        }
        val duration = m?.getLong(MediaMetadata.METADATA_KEY_DURATION)?.takeIf { it > 0 } ?: 0L
        if (duration > 0) position = position.coerceAtMost(duration)
        return Protocol.Song(
            state = if (playing) Protocol.STATE_PLAYING else Protocol.STATE_PAUSED,
            volumePercent = volume(c),
            positionMs = position,
            durationMs = duration,
            title = m.text(MediaMetadata.METADATA_KEY_TITLE, MediaMetadata.METADATA_KEY_DISPLAY_TITLE),
            artist = m.text(
                MediaMetadata.METADATA_KEY_ARTIST,
                MediaMetadata.METADATA_KEY_ALBUM_ARTIST,
                MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE,
            ),
            album = m.text(MediaMetadata.METADATA_KEY_ALBUM),
            source = playerName(c.packageName),
        )
    }

    private fun MediaMetadata?.text(vararg keys: String): String {
        if (this == null) return ""
        for (k in keys) getString(k)?.trim()?.takeIf { it.isNotEmpty() }?.let { return it }
        return ""
    }

    // The volume the player plays at: the phone's media volume, or the cast device's.
    private fun volume(c: MediaController?): Int? {
        val info = c?.playbackInfo
        if (info != null && info.playbackType == MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE) {
            return if (info.maxVolume > 0) info.currentVolume * 100 / info.maxVolume else null
        }
        val max = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
        return if (max > 0) audio.getStreamVolume(AudioManager.STREAM_MUSIC) * 100 / max else null
    }

    private val names = HashMap<String, String>()

    private fun playerName(pkg: String): String = names.getOrPut(pkg) {
        try {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(pkg, 0)).toString()
        } catch (e: Exception) {
            pkg.substringAfterLast('.').replaceFirstChar { it.uppercase() }
        }
    }

    // --- cover ---

    private fun updateCover(m: MediaMetadata?) {
        val bitmap = m?.let {
            it.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
                ?: it.getBitmap(MediaMetadata.METADATA_KEY_ART)
                ?: it.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
        }
        val uri = m?.let {
            it.getString(MediaMetadata.METADATA_KEY_ALBUM_ART_URI)
                ?: it.getString(MediaMetadata.METADATA_KEY_ART_URI)
                ?: it.getString(MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI)
        }
        val key: Any? = bitmap?.let { it.generationId to System.identityHashCode(it) } ?: uri
        if (key == coverKey) return
        coverKey = key
        val job = ++coverJob
        if (bitmap == null && uri == null) {
            link.sendCover(0, null)
            return
        }
        // Scaling, compressing and reading a URI can take a moment: off the main thread.
        artWorker.execute {
            val jpeg = try {
                (bitmap ?: loadUri(uri!!))?.let { coverJpeg(it) }
            } catch (e: Exception) {
                Log.w(TAG, "cover failed", e)
                null
            }
            handler.post {
                if (job != coverJob) return@post  // the song has changed since
                val id = jpeg?.let { CRC32().apply { update(it) }.value.toInt() } ?: 0
                link.sendCover(id, jpeg)
            }
        }
    }

    private fun loadUri(uri: String): Bitmap? {
        val u = Uri.parse(uri)
        if (u.scheme != "content" && u.scheme != "android.resource" && u.scheme != "file") return null
        return contentResolver.openInputStream(u)?.use { BitmapFactory.decodeStream(it) }
    }

    // The middle square, at the CYD's cover size, as a baseline JPEG its decoder can read.
    private fun coverJpeg(src: Bitmap): ByteArray {
        val side = minOf(src.width, src.height)
        val from = Rect((src.width - side) / 2, (src.height - side) / 2, (src.width + side) / 2, (src.height + side) / 2)
        val out = Bitmap.createBitmap(COVER_SIZE, COVER_SIZE, Bitmap.Config.ARGB_8888)
        val soft = if (src.config == Bitmap.Config.HARDWARE) src.copy(Bitmap.Config.ARGB_8888, false) else src
        Canvas(out).drawBitmap(soft, from, Rect(0, 0, COVER_SIZE, COVER_SIZE), Paint(Paint.FILTER_BITMAP_FLAG))
        val bytes = ByteArrayOutputStream()
        out.compress(Bitmap.CompressFormat.JPEG, 85, bytes)
        out.recycle()
        return bytes.toByteArray()
    }

    companion object {
        private const val TAG = "MediaListener"
        private const val COVER_SIZE = 128
        private const val VOLUME_CHANGED = "android.media.VOLUME_CHANGED_ACTION"

        var instance: MediaListener? = null
            private set
    }
}
