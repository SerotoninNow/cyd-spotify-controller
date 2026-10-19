package com.cyd.nowplaying

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.io.FileNotFoundException

// Hands this app's own APK to whatever the user shares it with (Quick Share, Bluetooth, a
// messaging app), so another phone can install it. Serves that one file and nothing else;
// not exported, so other apps only get in through the read permission a share grants.
class ApkProvider : ContentProvider() {
    override fun onCreate() = true

    private fun apk() = File(context!!.applicationInfo.sourceDir)

    override fun getType(uri: Uri) = MIME

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (uri.lastPathSegment != NAME || mode != "r") throw FileNotFoundException(uri.toString())
        return ParcelFileDescriptor.open(apk(), ParcelFileDescriptor.MODE_READ_ONLY)
    }

    // Receivers ask for the name and size to show before saving.
    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, args: Array<out String>?, sort: String?): Cursor {
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        val row = columns.map {
            when (it) {
                OpenableColumns.DISPLAY_NAME -> NAME
                OpenableColumns.SIZE -> apk().length()
                else -> null
            }
        }
        return MatrixCursor(columns, 1).apply { addRow(row) }
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun update(uri: Uri, values: ContentValues?, selection: String?, args: Array<out String>?) = 0
    override fun delete(uri: Uri, selection: String?, args: Array<out String>?) = 0

    companion object {
        const val MIME = "application/vnd.android.package-archive"
        const val NAME = "CYD-Now-Playing.apk"

        fun uri(packageName: String): Uri = Uri.parse("content://$packageName.apk/$NAME")
    }
}
