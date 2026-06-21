package io.github.ripdog.th2xr_portable

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.os.Bundle
import android.view.WindowManager
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.libsdl.app.SDLActivity
import java.io.File

class GameActivity : SDLActivity() {
    companion object {
        private const val REQUEST_CODE_EXPORT_SAVES = 0x5432

        @JvmStatic
        external fun nativeSaveBundleExportResult(uri: String?, error: String?)

        @JvmStatic
        fun showSaveBundleExportDialog(defaultFilename: String): Boolean {
            val activity = getContext() ?: return false
            val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "application/octet-stream"
                putExtra(Intent.EXTRA_TITLE, defaultFilename)
            }
            return try {
                activity.startActivityForResult(intent, REQUEST_CODE_EXPORT_SAVES)
                true
            } catch (exception: ActivityNotFoundException) {
                nativeSaveBundleExportResult(null, "Unable to open file dialog")
                false
            }
        }
    }

    override fun getArguments(): Array<String> {
        val dataDir = File(filesDir, "game-data")
        return arrayOf(dataDir.absolutePath)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setImmersiveMode()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            setImmersiveMode()
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQUEST_CODE_EXPORT_SAVES) {
            if (resultCode == Activity.RESULT_OK) {
                nativeSaveBundleExportResult(data?.data?.toString(), null)
            } else {
                nativeSaveBundleExportResult(null, null)
            }
            return
        }
        super.onActivityResult(requestCode, resultCode, data)
    }

    private fun setImmersiveMode() {
        // Keep the screen on while the game is running.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Hide the status and navigation bars for a true fullscreen experience.
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }
}
