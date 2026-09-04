// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.utils

import android.net.Uri
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.NativeLibrary
import java.io.File

/**
 * LSFG extracts its shaders from Lossless.dll, so frame generation cannot start
 * without it. Canonical home is the Azahar user directory, which keeps it alive
 * independently of the user's Roms folder; the old Roms path stays supported so
 * existing setups keep working.
 */
object LosslessDll {
    private const val LEGACY_PATH = "/sdcard/Roms/Lossless.dll"

    val targetFile: File
        get() = File(File(NativeLibrary.getUserDirectory(), "lsfg"), "Lossless.dll")

    fun path(): String? {
        val inUserDir = targetFile
        if (inUserDir.exists()) {
            return inUserDir.absolutePath
        }
        val legacy = File(LEGACY_PATH)
        return if (legacy.exists()) legacy.absolutePath else null
    }

    fun exists(): Boolean = path() != null

    /** Copies the picked document into the user directory. Returns true on success. */
    fun install(uri: Uri): Boolean = try {
        val dest = targetFile
        dest.parentFile?.mkdirs()
        CitraApplication.appContext.contentResolver.openInputStream(uri).use { input ->
            if (input == null) {
                false
            } else {
                dest.outputStream().use { output -> input.copyTo(output) }
                dest.length() > 0
            }
        }
    } catch (_: Exception) {
        false
    }
}
