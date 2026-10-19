package com.cyd.nowplaying

import java.io.ByteArrayOutputStream
import java.util.UUID

// The CYD's now-playing service; must match firmware/src/remote.cpp. Every write is one
// packet whose first byte is its type; numbers are little-endian.
object Protocol {
    val SERVICE: UUID = UUID.fromString("8d5f0001-3c4b-4f1e-9a57-6b1c2d0e7a10")
    val CHARACTERISTIC: UUID = UUID.fromString("8d5f0002-3c4b-4f1e-9a57-6b1c2d0e7a10")

    const val STATE_NONE = 0
    const val STATE_PAUSED = 1
    const val STATE_PLAYING = 2

    private const val SONG: Byte = 0x01
    private const val COVER: Byte = 0x02
    private const val PIECE: Byte = 0x03
    const val PIECE_HEADER = 9

    data class Song(
        val state: Int,
        val volumePercent: Int?,  // null: unknown
        val positionMs: Long,
        val durationMs: Long,     // 0: unknown
        val title: String,
        val artist: String,
        val album: String,
        val source: String,       // the player's name
    )

    // state, volume, position, duration, then four 0-terminated UTF-8 strings, shortened so
    // the whole packet fits in `maxLen` bytes (one write).
    fun song(s: Song, maxLen: Int): ByteArray {
        var cap = 120
        while (true) {
            val out = ByteArrayOutputStream()
            out.write(SONG.toInt())
            out.write(s.state)
            out.write(s.volumePercent?.coerceIn(0, 100) ?: 255)
            writeU32(out, s.positionMs.coerceIn(0, 0xFFFFFFFFL))
            writeU32(out, s.durationMs.coerceIn(0, 0xFFFFFFFFL))
            for (text in listOf(s.title, s.artist, s.album, s.source)) {
                out.write(utf8Prefix(text, cap))
                out.write(0)
            }
            if (out.size() <= maxLen || cap == 0) return out.toByteArray()
            cap = if (cap > 8) cap * 2 / 3 else 0
        }
    }

    fun cover(id: Int, length: Int): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(COVER.toInt())
        writeU32(out, id.toLong() and 0xFFFFFFFFL)
        writeU32(out, length.toLong())
        return out.toByteArray()
    }

    // The cover's JPEG in pieces of at most `maxLen` bytes each, header included.
    fun pieces(id: Int, jpeg: ByteArray, maxLen: Int): List<ByteArray> {
        val step = maxLen - PIECE_HEADER
        return (jpeg.indices step step).map { offset ->
            val out = ByteArrayOutputStream()
            out.write(PIECE.toInt())
            writeU32(out, id.toLong() and 0xFFFFFFFFL)
            writeU32(out, offset.toLong())
            out.write(jpeg, offset, minOf(step, jpeg.size - offset))
            out.toByteArray()
        }
    }

    private fun writeU32(out: ByteArrayOutputStream, v: Long) {
        for (i in 0 until 4) out.write(((v shr (8 * i)) and 0xFF).toInt())
    }

    // At most `maxBytes` of UTF-8, cut between characters.
    private fun utf8Prefix(text: String, maxBytes: Int): ByteArray {
        val bytes = text.replace('\u0000', ' ').toByteArray(Charsets.UTF_8)
        if (bytes.size <= maxBytes) return bytes
        var end = maxBytes
        while (end > 0 && (bytes[end].toInt() and 0xC0) == 0x80) end--
        return bytes.copyOf(end)
    }
}
