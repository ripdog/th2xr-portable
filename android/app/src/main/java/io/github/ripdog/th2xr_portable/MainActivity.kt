package io.github.ripdog.th2xr_portable

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.FileOutputStream

class MainActivity : Activity() {

    companion object {
        private const val REQUEST_CODE_PICK_GAME_DATA = 1
        private const val PREFS_NAME = "th2xr_portable_prefs"
        private const val KEY_GAME_DATA_IMPORTED = "game_data_imported"
        private const val GAME_DATA_DIR = "game-data"
        private const val EXE_NAME = "TOHEART2.EXE"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        if (prefs.getBoolean(KEY_GAME_DATA_IMPORTED, false)) {
            launchGame()
            return
        }

        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            putExtra(Intent.EXTRA_TITLE, "Select the ToHeart2 game data folder")
        }
        startActivityForResult(intent, REQUEST_CODE_PICK_GAME_DATA)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)

        if (requestCode != REQUEST_CODE_PICK_GAME_DATA) {
            return
        }

        if (resultCode != RESULT_OK || data == null) {
            Toast.makeText(this, "Game data selection is required", Toast.LENGTH_LONG).show()
            finish()
            return
        }

        val treeUri = data.data
        if (treeUri == null) {
            Toast.makeText(this, "No folder selected", Toast.LENGTH_LONG).show()
            finish()
            return
        }

        importGameData(treeUri)
    }

    private fun importGameData(treeUri: Uri) {
        val root = DocumentFile.fromTreeUri(this, treeUri)
        if (root == null || !root.isDirectory) {
            Toast.makeText(this, "Invalid folder selected", Toast.LENGTH_LONG).show()
            finish()
            return
        }

        val exe = root.findFile(EXE_NAME)
        if (exe == null || !exe.isFile) {
            Toast.makeText(
                this,
                "Selected folder does not contain $EXE_NAME",
                Toast.LENGTH_LONG
            ).show()
            finish()
            return
        }

        val destDir = File(filesDir, GAME_DATA_DIR)
        destDir.deleteRecursively()
        destDir.mkdirs()

        try {
            copyDocumentTree(root, destDir)
        } catch (e: Exception) {
            Toast.makeText(this, "Failed to copy game data: ${e.message}", Toast.LENGTH_LONG).show()
            destDir.deleteRecursively()
            finish()
            return
        }

        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_GAME_DATA_IMPORTED, true)
            .apply()

        Toast.makeText(this, "Game data imported", Toast.LENGTH_SHORT).show()
        launchGame()
    }

    private fun copyDocumentTree(source: DocumentFile, destDir: File) {
        for (child in source.listFiles()) {
            val destFile = File(destDir, child.name ?: continue)
            when {
                child.isDirectory -> {
                    destFile.mkdirs()
                    copyDocumentTree(child, destFile)
                }
                child.isFile -> {
                    contentResolver.openInputStream(child.uri)?.use { input ->
                        FileOutputStream(destFile).use { output ->
                            input.copyTo(output)
                        }
                    }
                }
            }
        }
    }

    private fun launchGame() {
        val intent = Intent(this, GameActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_NEW_TASK
        }
        startActivity(intent)
        finish()
    }
}
