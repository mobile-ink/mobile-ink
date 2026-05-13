package com.mathnotes.mobileink

import android.content.Context
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.MotionEvent
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReactContext
import com.facebook.react.uimanager.events.RCTEventEmitter

class MobileInkCanvasView(context: Context) : GLSurfaceView(context), DrawingEngineHost {

    private var drawingEngine: Long = 0
    private val renderer: DrawingRenderer
    private var viewWidth: Int = 0
    private var viewHeight: Int = 0
    private var nativeLibraryAvailable: Boolean = false

    // Tool state tracking
    private var currentToolType: String = "pen"
    private var currentEraserMode: String = "pixel"
    private var currentStrokeWidth: Float = 3f
    private val holdToShapeDelayMs: Long = 300
    private val holdToShapeHandler = Handler(Looper.getMainLooper())
    private val holdToShapeRunnable = Runnable { showHoldToShapePreview() }

    // Selection move state
    private var isMovingSelection: Boolean = false
    private var lastDragX: Float = 0f
    private var lastDragY: Float = 0f

    // Eraser cursor state (for pixel eraser)
    private var eraserCursorX: Float = 0f
    private var eraserCursorY: Float = 0f
    private var showEraserCursor: Boolean = false

    // Background type for pattern rendering (handled in C++ Skia layer)
    private var currentBackgroundType: String = "plain"
    private var currentPdfBackgroundUri: String? = null

    // Drawing policy: "default", "anyinput", or "pencilonly"
    // When "pencilonly", only stylus touches are processed for drawing
    var drawingPolicy: String = "default"
    var renderBackend: String = "ganesh"
    var renderSuspended: Boolean = false
        set(value) {
            if (field == value) return
            field = value
            if (!value) {
                requestInkRender(force = true)
            }
        }

    init {
        // Try to load native library - don't crash if it fails
        nativeLibraryAvailable = ensureLibraryLoaded()

        setEGLContextClientVersion(2)
        setEGLConfigChooser(8, 8, 8, 8, 16, 0)
        holder.setFormat(PixelFormat.RGBA_8888)

        renderer = DrawingRenderer(this)
        setRenderer(renderer)
        renderMode = RENDERMODE_WHEN_DIRTY
    }

    // DrawingEngineHost interface implementation
    override fun getDrawingEngine(): Long = drawingEngine

    override fun onSurfaceSizeChanged(width: Int, height: Int) {
        viewWidth = width
        viewHeight = height

        // Create or recreate drawing engine
        if (drawingEngine != 0L) {
            resizeEngine(drawingEngine, width, height)
        } else {
            drawingEngine = createDrawingEngine(width, height)
        }

        // Re-apply background type to engine (in case it was set before engine was created,
        // or engine was recreated). Always apply to ensure consistency.
        if (drawingEngine != 0L) {
            setBackgroundType(drawingEngine, currentBackgroundType)
        }

        // Re-apply PDF background if we have one (needs to be re-rendered at new size)
        if (!currentPdfBackgroundUri.isNullOrEmpty()) {
            setPdfBackgroundUri(currentPdfBackgroundUri)
        }
    }

    override fun renderEngineToPixels(engine: Long, bitmap: Bitmap) {
        renderToPixels(engine, bitmap)
    }

    /**
     * Set background type for pattern rendering.
     * Patterns are rendered in the C++ Skia layer.
     */
    fun setBackgroundType(type: String) {
        currentBackgroundType = type
        queueEvent {
            if (drawingEngine != 0L) {
                setBackgroundType(drawingEngine, type)
            }
        }
        requestInkRender()
    }

    /**
     * Set PDF background URI for PDF page rendering.
     * Loads the PDF, renders it to a bitmap, and passes to C++ for display.
     */
    fun setPdfBackgroundUri(uri: String?) {
        currentPdfBackgroundUri = uri

        if (uri.isNullOrEmpty()) {
            // Clear PDF background
            queueEvent {
                if (drawingEngine != 0L) {
                    setPdfBackgroundBitmap(drawingEngine, null)
                }
            }
            requestInkRender()
            return
        }

        // Load PDF and render to bitmap on background thread
        Thread {
            val pdfBitmap = PdfLoader.loadAndRenderPdf(context, uri, viewWidth)
            if (pdfBitmap != null) {
                queueEvent {
                    if (drawingEngine != 0L) {
                        setPdfBackgroundBitmap(drawingEngine, pdfBitmap)
                        setBackgroundType(drawingEngine, "pdf")
                    }
                    // Recycle bitmap after passing to C++ (it copies the pixels)
                    pdfBitmap.recycle()
                }
                requestInkRender()
            } else {
                android.util.Log.e("MobileInkCanvasView", "Failed to load PDF background")
            }
        }.start()
    }

    // Track the registered React tag to properly unregister on detach
    private var registeredTag: Int = -1

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        // Register this view with MobileInkModule for bridgeless architecture support
        // The 'id' is set by React Native to the view's React tag
        if (id != -1 && id != 0) {
            registeredTag = id
            MobileInkModule.registerView(id, this)
        }
    }

    // Called by React Native when the view's id (React tag) is set
    override fun setId(id: Int) {
        super.setId(id)
        // Register with the new id if we haven't already and we're attached
        if (id != -1 && id != 0 && isAttachedToWindow && registeredTag != id) {
            // Unregister old tag if any
            if (registeredTag != -1) {
                MobileInkModule.unregisterView(registeredTag)
            }
            registeredTag = id
            MobileInkModule.registerView(id, this)
        }
    }

    override fun onDetachedFromWindow() {
        cancelHoldToShapePreview()

        // Unregister this view from MobileInkModule
        if (registeredTag != -1) {
            MobileInkModule.unregisterView(registeredTag)
            registeredTag = -1
        }

        queueEvent {
            if (drawingEngine != 0L) {
                destroyDrawingEngine(drawingEngine)
                drawingEngine = 0
            }
        }
        super.onDetachedFromWindow()
    }

    private fun requestInkRender(force: Boolean = false) {
        if (!renderSuspended || force) {
            super.requestRender()
        }
    }

    private fun canPreviewHoldToShape(): Boolean {
        return currentToolType == "pen" ||
            currentToolType == "pencil" ||
            currentToolType == "marker" ||
            currentToolType == "highlighter" ||
            currentToolType == "crayon" ||
            currentToolType == "calligraphy"
    }

    private fun scheduleHoldToShapePreview() {
        cancelHoldToShapePreview()
        if (!canPreviewHoldToShape()) return
        holdToShapeHandler.postDelayed(holdToShapeRunnable, holdToShapeDelayMs)
    }

    private fun cancelHoldToShapePreview() {
        holdToShapeHandler.removeCallbacks(holdToShapeRunnable)
    }

    private fun showHoldToShapePreview() {
        if (!canPreviewHoldToShape() || drawingEngine == 0L) return

        val timestamp = SystemClock.uptimeMillis()
        queueEvent {
            if (drawingEngine != 0L) {
                updateHoldShapePreview(drawingEngine, timestamp)
            }
        }
        requestInkRender()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!nativeLibraryAvailable) {
            android.util.Log.e("MobileInkCanvasView", "onTouchEvent: native library not available")
            return false
        }
        if (drawingEngine == 0L) {
            android.util.Log.e("MobileInkCanvasView", "onTouchEvent: drawingEngine is 0")
            return false
        }

        val toolType = event.getToolType(0)
        val isFingerInput = toolType == MotionEvent.TOOL_TYPE_FINGER
        val isSelectionInteraction = currentToolType == "select" || isMovingSelection
        if (drawingPolicy == "pencilonly" && isFingerInput && !isSelectionInteraction) {
            return false
        }

        // CRITICAL: Request parent to not intercept touch events
        // This is essential for drawing - without it, ScrollView or other parents
        // will intercept ACTION_MOVE events, causing only dots to appear
        parent?.requestDisallowInterceptTouchEvent(true)

        val x = event.x
        val y = event.y

        // Extract stylus data - pressure, tilt, and orientation
        val pressure = event.pressure.coerceIn(0f, 1f)

        // Get stylus tilt (altitude) - AXIS_TILT is tilt angle from perpendicular
        // 0 = perpendicular to screen, pi/2 = parallel to screen
        val tilt = event.getAxisValue(MotionEvent.AXIS_TILT)
        // Convert to altitude (perpendicular = pi/2, parallel = 0)
        val altitude = (Math.PI.toFloat() / 2f) - tilt

        // Get stylus orientation (azimuth) - angle around the perpendicular axis
        val azimuth = event.getAxisValue(MotionEvent.AXIS_ORIENTATION)
        val isStylusInput = toolType == MotionEvent.TOOL_TYPE_STYLUS ||
            toolType == MotionEvent.TOOL_TYPE_ERASER
        val eventTimestamp = event.eventTime

        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                // For select tool, check if tapping inside existing selection to move it
                if (currentToolType == "select" && drawingEngine != 0L) {
                    val selectionCount = getSelectionCount(drawingEngine)
                    if (selectionCount > 0) {
                        val bounds = getSelectionBounds(drawingEngine)
                        if (bounds != null && isPointInBounds(x, y, bounds)) {
                            // Start moving the selection
                            isMovingSelection = true
                            lastDragX = x
                            lastDragY = y
                            cancelHoldToShapePreview()
                            return true
                        }
                    }
                }

                // Update eraser cursor position for pixel eraser
                if (currentToolType == "eraser" && currentEraserMode == "pixel") {
                    eraserCursorX = x
                    eraserCursorY = y
                    showEraserCursor = true
                }

                // Queue touch event to GL thread to avoid race conditions with render thread
                queueEvent {
                    if (drawingEngine != 0L) {
                        // Set eraser cursor in C++ engine (radius is half the stroke width)
                        if (currentToolType == "eraser" && currentEraserMode == "pixel") {
                            setEraserCursor(drawingEngine, x, y, currentStrokeWidth / 2f, true)
                        }
                        touchBegan(drawingEngine, x, y, pressure, azimuth, altitude, eventTimestamp, isStylusInput)
                    }
                }
                requestInkRender()
                scheduleHoldToShapePreview()
                sendDrawingBeginEvent(x, y)
            }
            MotionEvent.ACTION_MOVE -> {
                // Handle selection move
                if (isMovingSelection && drawingEngine != 0L) {
                    cancelHoldToShapePreview()
                    val dx = x - lastDragX
                    val dy = y - lastDragY
                    queueEvent {
                        if (drawingEngine != 0L) {
                            moveSelection(drawingEngine, dx, dy)
                        }
                    }
                    lastDragX = x
                    lastDragY = y
                    requestInkRender()
                    emitSelectionChange()
                    return true
                }

                // Update eraser cursor position (local state)
                if (currentToolType == "eraser" && currentEraserMode == "pixel") {
                    eraserCursorX = x
                    eraserCursorY = y
                }

                // Collect historical points for batch processing
                val historySize = event.historySize
                val historicalPoints = mutableListOf<FloatArray>()
                val historicalTimestamps = mutableListOf<Long>()
                for (i in 0 until historySize) {
                    val hx = event.getHistoricalX(i)
                    val hy = event.getHistoricalY(i)
                    val hp = event.getHistoricalPressure(i).coerceIn(0f, 1f)
                    val hTilt = event.getHistoricalAxisValue(MotionEvent.AXIS_TILT, i)
                    val hAltitude = (Math.PI.toFloat() / 2f) - hTilt
                    val hAzimuth = event.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, i)
                    historicalPoints.add(floatArrayOf(hx, hy, hp, hAzimuth, hAltitude))
                    historicalTimestamps.add(event.getHistoricalEventTime(i))
                }

                // Queue all touch moves to GL thread
                queueEvent {
                    if (drawingEngine != 0L) {
                        // Update eraser cursor in C++ engine
                        if (currentToolType == "eraser" && currentEraserMode == "pixel") {
                            setEraserCursor(drawingEngine, x, y, currentStrokeWidth / 2f, true)
                        }
                        // Process historical points
                        for ((index, point) in historicalPoints.withIndex()) {
                            touchMoved(
                                drawingEngine,
                                point[0],
                                point[1],
                                point[2],
                                point[3],
                                point[4],
                                historicalTimestamps[index],
                                isStylusInput
                            )
                        }
                        // Process current point
                        touchMoved(drawingEngine, x, y, pressure, azimuth, altitude, eventTimestamp, isStylusInput)
                    }
                }
                requestInkRender()
                scheduleHoldToShapePreview()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                cancelHoldToShapePreview()

                // Finalize selection move
                if (isMovingSelection && drawingEngine != 0L) {
                    queueEvent {
                        if (drawingEngine != 0L) {
                            finalizeMove(drawingEngine)
                        }
                        post { emitSelectionChange() }
                    }
                    isMovingSelection = false
                    requestInkRender()
                    sendEvent("onDrawingChange", Arguments.createMap())
                    return true
                }

                // Hide eraser cursor (local state)
                showEraserCursor = false

                // Queue touch end to GL thread to avoid race conditions
                queueEvent {
                    if (drawingEngine != 0L) {
                        setEraserCursor(drawingEngine, 0f, 0f, 0f, false)
                        touchEnded(drawingEngine, eventTimestamp)
                    }
                    if (currentToolType == "select") {
                        post { emitSelectionChange() }
                    }
                }
                requestInkRender()
                if (currentToolType != "select") {
                    sendEvent("onDrawingChange", Arguments.createMap())
                }
            }
        }

        return true
    }

    fun clear() {
        queueEvent {
            if (drawingEngine != 0L) {
                clearCanvas(drawingEngine)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
        sendEvent("onDrawingChange", Arguments.createMap())
    }

    fun undo() {
        queueEvent {
            if (drawingEngine != 0L) {
                undoStroke(drawingEngine)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
        sendEvent("onDrawingChange", Arguments.createMap())
    }

    fun redo() {
        queueEvent {
            if (drawingEngine != 0L) {
                redoStroke(drawingEngine)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
        sendEvent("onDrawingChange", Arguments.createMap())
    }

    fun setTool(toolType: String, width: Float, color: Int) {
        queueEvent {
            if (drawingEngine != 0L) {
                setStrokeWidth(drawingEngine, width)
                setStrokeColor(drawingEngine, color)
                setTool(drawingEngine, toolType)
            }
        }
    }

    fun setToolWithParams(toolType: String, width: Float, color: Int, eraserMode: String?) {
        cancelHoldToShapePreview()
        val wasSelectionMode = currentToolType == "select"

        // Update tool state (local - doesn't need queuing)
        currentToolType = toolType
        currentEraserMode = eraserMode ?: "pixel"
        currentStrokeWidth = width

        // Hide eraser cursor if switching away from pixel eraser
        if (toolType != "eraser" || eraserMode != "pixel") {
            showEraserCursor = false
        }

        // Queue engine operations to GL thread
        queueEvent {
            if (drawingEngine != 0L) {
                // Clear selection when switching away from select tool
                if (wasSelectionMode && toolType != "select") {
                    clearSelection(drawingEngine)
                    post { emitSelectionChange() }
                }
                setToolWithParams(drawingEngine, toolType, width, color, eraserMode ?: "")
            }
        }
        if (wasSelectionMode && toolType != "select") {
            requestInkRender()
        }
    }

    // Helper to check if a point is inside selection bounds
    private fun isPointInBounds(x: Float, y: Float, bounds: FloatArray): Boolean {
        // bounds is [minX, minY, maxX, maxY]
        return x >= bounds[0] && x <= bounds[2] && y >= bounds[1] && y <= bounds[3]
    }

    fun setDrawingBackgroundColor(color: Int) {
        renderer.backgroundColor = color
        requestInkRender()
    }

    // Selection operations
    // Note: selectAt needs to return synchronously, so we can't easily queue it
    // It only reads data, doesn't modify, so should be safe
    fun selectAt(x: Float, y: Float): Boolean {
        return if (drawingEngine != 0L) {
            val result = selectStrokeAt(drawingEngine, x, y)
            requestInkRender()
            emitSelectionChange()
            result
        } else false
    }

    fun clearSelection() {
        queueEvent {
            if (drawingEngine != 0L) {
                clearSelection(drawingEngine)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
    }

    fun deleteSelection() {
        queueEvent {
            if (drawingEngine != 0L) {
                deleteSelection(drawingEngine)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
    }

    fun copySelection() {
        queueEvent {
            if (drawingEngine != 0L) {
                copySelection(drawingEngine)
            }
        }
    }

    fun pasteSelection(offsetX: Float, offsetY: Float) {
        queueEvent {
            if (drawingEngine != 0L) {
                pasteSelection(drawingEngine, offsetX, offsetY)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
    }

    fun moveSelection(dx: Float, dy: Float) {
        queueEvent {
            if (drawingEngine != 0L) {
                moveSelection(drawingEngine, dx, dy)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
    }

    fun finalizeMoveSelection() {
        queueEvent {
            if (drawingEngine != 0L) {
                finalizeMove(drawingEngine)
                post { emitSelectionChange() }
            }
        }
    }

    fun getSelectionCount(): Int {
        return if (drawingEngine != 0L) getSelectionCount(drawingEngine) else 0
    }

    fun getSelectionBounds(): FloatArray? {
        return if (drawingEngine != 0L) getSelectionBounds(drawingEngine) else null
    }

    // State queries
    fun canUndo(): Boolean = drawingEngine != 0L && canUndo(drawingEngine)
    fun canRedo(): Boolean = drawingEngine != 0L && canRedo(drawingEngine)
    fun isEmpty(): Boolean = drawingEngine == 0L || isEmpty(drawingEngine)

    // Helper for synchronous GL thread operations with timeout
    private fun <T> runOnGlThreadSync(timeoutMs: Long = 2000, block: () -> T?): T? {
        val latch = java.util.concurrent.CountDownLatch(1)
        var result: T? = null
        queueEvent {
            result = block()
            latch.countDown()
        }
        try {
            latch.await(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)
        } catch (e: InterruptedException) {
            android.util.Log.e("MobileInkCanvasView", "GL thread operation interrupted", e)
        }
        return result
    }

    fun serializeDrawing(): ByteArray? {
        if (drawingEngine == 0L) return null
        return runOnGlThreadSync(3000) {
            if (drawingEngine != 0L) serializeDrawing(drawingEngine) else null
        }
    }

    fun deserializeDrawing(data: ByteArray): Boolean {
        if (drawingEngine == 0L) return false
        val success = runOnGlThreadSync {
            if (drawingEngine != 0L) { deserializeDrawing(drawingEngine, data); true } else false
        } ?: false
        requestInkRender()
        return success
    }

    fun getBase64PngData(scale: Float): String? {
        if (drawingEngine == 0L || viewWidth == 0 || viewHeight == 0) return null
        return runOnGlThreadSync { exportToBase64(scale, Bitmap.CompressFormat.PNG, 100) }
    }

    fun getBase64JpegData(scale: Float, compression: Float): String? {
        if (drawingEngine == 0L || viewWidth == 0 || viewHeight == 0) return null
        val quality = (compression * 100).toInt().coerceIn(0, 100)
        return runOnGlThreadSync { exportToBase64(scale, Bitmap.CompressFormat.JPEG, quality) }
    }

    private fun exportToBase64(scale: Float, format: Bitmap.CompressFormat, quality: Int): String? {
        return try {
            val fullBitmap = Bitmap.createBitmap(viewWidth, viewHeight, Bitmap.Config.ARGB_8888)
            if (drawingEngine != 0L) renderToPixels(drawingEngine, fullBitmap)

            val finalBitmap = if (scale != 1f) {
                val w = (viewWidth * scale).toInt().coerceAtLeast(1)
                val h = (viewHeight * scale).toInt().coerceAtLeast(1)
                Bitmap.createScaledBitmap(fullBitmap, w, h, true).also { fullBitmap.recycle() }
            } else fullBitmap

            val stream = java.io.ByteArrayOutputStream()
            finalBitmap.compress(format, quality, stream)
            finalBitmap.recycle()

            val mimeType = if (format == Bitmap.CompressFormat.PNG) "png" else "jpeg"
            "data:image/$mimeType;base64," + android.util.Base64.encodeToString(stream.toByteArray(), android.util.Base64.NO_WRAP)
        } catch (e: Exception) {
            android.util.Log.e("MobileInkCanvasView", "Export error", e)
            null
        }
    }

    private fun sendEvent(eventName: String, params: com.facebook.react.bridge.WritableMap) {
        val reactContext = context as ReactContext
        reactContext
            .getJSModule(RCTEventEmitter::class.java)
            .receiveEvent(id, eventName, params)
    }

    private fun sendDrawingBeginEvent(x: Float, y: Float) {
        val payload = Arguments.createMap()
        payload.putDouble("x", x.toDouble())
        payload.putDouble("y", y.toDouble())
        sendEvent("onDrawingBegin", payload)
    }

    private fun emitSelectionChange() {
        val payload = Arguments.createMap()
        val bounds = getSelectionBounds()
        val count = getSelectionCount()
        payload.putInt("count", count)
        if (count > 0 && bounds != null && bounds.size == 4) {
            val boundsMap = Arguments.createMap()
            boundsMap.putDouble("x", bounds[0].toDouble())
            boundsMap.putDouble("y", bounds[1].toDouble())
            boundsMap.putDouble("width", (bounds[2] - bounds[0]).toDouble())
            boundsMap.putDouble("height", (bounds[3] - bounds[1]).toDouble())
            payload.putMap("bounds", boundsMap)
        } else {
            payload.putNull("bounds")
        }
        sendEvent("onSelectionChange", payload)
    }

    // Native method declarations
    private external fun createDrawingEngine(width: Int, height: Int): Long
    private external fun destroyDrawingEngine(engine: Long)
    private external fun resizeEngine(engine: Long, width: Int, height: Int)

    // Touch handling with full stylus support
    private external fun touchBegan(engine: Long, x: Float, y: Float, pressure: Float, azimuth: Float, altitude: Float, timestamp: Long, isStylusInput: Boolean)
    private external fun touchMoved(engine: Long, x: Float, y: Float, pressure: Float, azimuth: Float, altitude: Float, timestamp: Long, isStylusInput: Boolean)
    private external fun touchEnded(engine: Long, timestamp: Long)
    private external fun updateHoldShapePreview(engine: Long, timestamp: Long): Boolean

    // Canvas operations
    private external fun clearCanvas(engine: Long)
    private external fun undoStroke(engine: Long)
    private external fun redoStroke(engine: Long)

    // Tool settings
    private external fun setStrokeColor(engine: Long, color: Int)
    private external fun setStrokeWidth(engine: Long, width: Float)
    private external fun setTool(engine: Long, toolType: String)
    private external fun setToolWithParams(engine: Long, toolType: String, width: Float, color: Int, eraserMode: String)
    private external fun setEraserCursor(engine: Long, x: Float, y: Float, radius: Float, visible: Boolean)
    private external fun setBackgroundType(engine: Long, backgroundType: String)
    private external fun setPdfBackgroundBitmap(engine: Long, bitmap: Bitmap?)

    // Selection operations
    private external fun selectStrokeAt(engine: Long, x: Float, y: Float): Boolean
    private external fun selectShapeStrokeAt(engine: Long, x: Float, y: Float): Boolean
    private external fun clearSelection(engine: Long)
    private external fun deleteSelection(engine: Long)
    private external fun copySelection(engine: Long)
    private external fun pasteSelection(engine: Long, offsetX: Float, offsetY: Float)
    private external fun moveSelection(engine: Long, dx: Float, dy: Float)
    private external fun finalizeMove(engine: Long)
    private external fun getSelectionCount(engine: Long): Int
    private external fun getSelectionBounds(engine: Long): FloatArray?

    // State queries
    private external fun canUndo(engine: Long): Boolean
    private external fun canRedo(engine: Long): Boolean
    private external fun isEmpty(engine: Long): Boolean

    // Serialization
    private external fun serializeDrawing(engine: Long): ByteArray?
    private external fun deserializeDrawing(engine: Long, data: ByteArray)

    // Rendering
    private external fun renderToPixels(engine: Long, bitmap: Bitmap)
    private external fun renderToByteArray(engine: Long, pixels: ByteArray, width: Int, height: Int)

    companion object {
        private var libraryLoaded = false

        @Synchronized
        fun ensureLibraryLoaded(): Boolean {
            if (!libraryLoaded) {
                try {
                    System.loadLibrary("mobileink")
                    libraryLoaded = true
                } catch (e: UnsatisfiedLinkError) {
                    android.util.Log.e("MobileInkCanvasView", "Failed to load mobileink library: ${e.message}")
                    return false
                }
            }
            return libraryLoaded
        }
    }
}
