package com.igi.port

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.documentfile.provider.DocumentFile
import java.io.File
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Stage 0 shell: user picks their own IGI install folder, the .res archives are copied to
 * app-private storage (native code cannot read SAF URIs), then the native core scans them.
 * No game data is bundled with this app.
 */
class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var glView: GLSurfaceView
    private lateinit var scale: ScaleGestureDetector
    private var lastX = 0f
    private var lastY = 0f

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            setEGLConfigChooser(8, 8, 8, 8, 24, 0)
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, c: EGLConfig?) = Native.onSurfaceCreated()
                override fun onSurfaceChanged(gl: GL10?, w: Int, h: Int) = Native.onSurfaceChanged(w, h)
                override fun onDrawFrame(gl: GL10?) = Native.onDraw()
            })
        }
        scale = ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                glView.queueEvent { Native.onZoom(d.scaleFactor) }
                return true
            }
        })
        glView.setOnTouchListener { _, e ->
            scale.onTouchEvent(e)
            if (!scale.isInProgress && e.pointerCount == 1) {
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> { lastX = e.x; lastY = e.y }
                    MotionEvent.ACTION_MOVE -> {
                        val dx = e.x - lastX; val dy = e.y - lastY
                        lastX = e.x; lastY = e.y
                        glView.queueEvent { Native.onDrag(dx, dy) }
                    }
                }
            }
            true
        }

        status = TextView(this).apply {
            setTextColor(Color.WHITE); textSize = 11f; typeface = android.graphics.Typeface.MONOSPACE
            setPadding(16, 16, 16, 16)
            text = "Pick the folder where your own copy of Project I.G.I. is installed.\n" +
                    "The .res archives will be copied into this app's private storage."
        }
        val pick = Button(this).apply {
            text = "Select IGI folder"
            setOnClickListener { startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_TREE) }
        }
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(200, 0, 0, 0))
            addView(pick)
            addView(ScrollView(this@MainActivity).apply { addView(status) },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 360))
        }
        setContentView(FrameLayout(this).apply {
            addView(glView)
            addView(panel, FrameLayout.LayoutParams(900, ViewGroup.LayoutParams.WRAP_CONTENT))
        })
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_TREE || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        status.text = "Copying .res files..."
        Thread {
            val result = try { importAndScan(uri) } catch (t: Throwable) { "Import failed: ${t.message}" }
            runOnUiThread { status.text = result }
        }.start()
    }

    private fun importAndScan(tree: Uri): String {
        val root = DocumentFile.fromTreeUri(this, tree) ?: return "Cannot open that folder."
        val dest = File(filesDir, "igi").apply { mkdirs() }
        var copied = 0
        fun walk(dir: DocumentFile, depth: Int) {
            for (f in dir.listFiles()) {
                if (f.isDirectory && depth < 3) walk(f, depth + 1)
                else if (f.isFile && f.name?.lowercase()?.endsWith(".res") == true) {
                    val out = File(dest, f.name!!)
                    if (!out.exists() || out.length() != f.length()) {
                        contentResolver.openInputStream(f.uri)?.use { i -> out.outputStream().use { o -> i.copyTo(o) } }
                    }
                    copied++
                    runOnUiThread { status.text = "Copied $copied: ${f.name}" }
                }
            }
        }
        walk(root, 0)
        if (copied == 0) return "No .res files found. Choose the folder that contains your IGI install."
        return "Copied $copied archive(s).\n\n" + Native.scan(dest.absolutePath)
    }

    override fun onPause() { super.onPause(); glView.onPause() }
    override fun onResume() { super.onResume(); glView.onResume() }

    companion object { private const val REQ_TREE = 1001 }
}
