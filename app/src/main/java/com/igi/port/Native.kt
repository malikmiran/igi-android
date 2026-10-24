package com.igi.port

object Native {
    init { System.loadLibrary("igi_native") }

    external fun scan(dir: String): String
    external fun onSurfaceCreated()
    external fun onSurfaceChanged(w: Int, h: Int)
    external fun onDraw()
    external fun onDrag(dx: Float, dy: Float)
    external fun onZoom(scale: Float)
}
