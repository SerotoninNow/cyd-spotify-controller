package com.cyd.nowplaying

import android.content.Context
import org.json.JSONObject
import java.security.MessageDigest

// The CYD firmware packed into this app by bundle-firmware.sh (assets/firmware), and how it
// compares with what the CYD runs.
object Firmware {
    class Bundled(val id: String, val flavor: String, private val boards: JSONObject) {
        fun hasBoard(board: String) = boards.has(board)

        // The image for `board`, checked against the manifest; null if it isn't there or intact.
        fun image(c: Context, board: String): ByteArray? {
            val entry = boards.optJSONObject(board) ?: return null
            val bytes = try {
                c.assets.open("firmware/" + entry.getString("file")).use { it.readBytes() }
            } catch (e: Exception) {
                return null
            }
            return bytes.takeIf { sha256(it).toHex() == entry.getString("sha256") }
        }
    }

    // null: this build of the app carries no firmware.
    fun bundled(c: Context): Bundled? = try {
        val json = JSONObject(c.assets.open("firmware/manifest.json").use { String(it.readBytes()) })
        Bundled(json.getString("id"), json.getString("flavor"), json.getJSONObject("boards"))
    } catch (e: Exception) {
        null
    }

    enum class Offer { NONE, UPDATE, REPLACE_FLAVOR }

    class Check(val text: String, val offer: Offer)

    // `cyd`: what the CYD reported (id, flavor, board); null while not connected; empty if its
    // firmware predates updates over Bluetooth.
    fun check(bundled: Bundled?, cyd: Map<String, String>?): Check {
        if (bundled == null) return Check("This build of the app carries no CYD firmware.", Offer.NONE)
        if (cyd == null) return Check("Shown once the CYD is connected.", Offer.NONE)
        val id = cyd["id"]
        val flavor = cyd["flavor"] ?: "public"
        val board = cyd["board"]
        return when {
            id == null || board == null -> Check(
                "The CYD's firmware is too old to update over Bluetooth. Flash the remote firmware over USB once (see the README); later updates can come from here.",
                Offer.NONE,
            )
            !bundled.hasBoard(board) -> Check("This app carries no firmware for the CYD's board ($board).", Offer.NONE)
            flavor != bundled.flavor -> Check(
                "The CYD runs the $flavor firmware ($id); this app carries the ${bundled.flavor} firmware (${bundled.id}). Updating replaces one with the other.",
                Offer.REPLACE_FLAVOR,
            )
            id == bundled.id -> Check("Up to date ($id).", Offer.NONE)
            else -> Check("Update available: $id → ${bundled.id}.", Offer.UPDATE)
        }
    }

    fun sha256(b: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(b)

    private fun ByteArray.toHex() = joinToString("") { "%02x".format(it) }
}
