package io.github.ripdog.th2xr_portable

import org.libsdl.app.SDLActivity
import java.io.File

class GameActivity : SDLActivity() {

    override fun getArguments(): Array<String> {
        val dataDir = File(filesDir, "game-data")
        return arrayOf(dataDir.absolutePath)
    }
}
