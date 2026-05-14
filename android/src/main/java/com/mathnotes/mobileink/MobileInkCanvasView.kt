package com.mathnotes.mobileink

import android.content.Context
import android.graphics.Bitmap
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReactContext
import com.facebook.react.uimanager.events.RCTEventEmitter

class MobileInkCanvasView(context: Context) : TextureView(context), TextureView.SurfaceTextureListener {

    private val perpendicularAltitude: Float = Math.PI.toFloat() / 2f
    private val minimumStylusPressure: Float = 0.015f
    private val stylusPressureExponent: Float = 0.88f

    @Volatile private var drawingEngine: Long = 0
    @Volatile private var viewWidth: Int = 0
    @Volatile private var viewHeight: Int = 0
    private var engineWidth: Int = 0
    private var engineHeight: Int = 0
    private var nativeLibraryAvailable: Boolean = false
    @Volatile private var surfaceReady: Boolean = false
    private val renderThread = HandlerThread("MobileInkCanvasViewRender").also { it.start() }
    private val renderHandler = Handler(renderThread.looper)
    private var renderSurface: Surface? = null
    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE

    // Tool state tracking
    private var currentToolType: String = "pen"
    private var currentEraserMode: String = "pixel"
    private var currentStrokeWidth: Float = 3f
    private var currentStrokeColor: Int = android.graphics.Color.BLACK
    private val holdToShapeDelayMs: Long = 300
    private val holdToShapeHandler = Handler(Looper.getMainLooper())
    private val holdToShapeRunnable = Runnable { showHoldToShapePreview() }

    // Selection move state
    private var isMovingSelection: Boolean = false
    private var isTransformingSelection: Boolean = false
    private var hasSelectionMoveDelta: Boolean = false
    private var selectionTransformHandleIndex: Int = -1
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
        isOpaque = false
        isClickable = true
        isFocusable = true
        surfaceTextureListener = this
    }

    override fun onSurfaceTextureAvailable(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        if (width > 0 && height > 0) {
            surfaceTexture.setDefaultBufferSize(width, height)
        }
        queueEvent {
            createRenderSurface(surfaceTexture, width, height)
        }
    }

    override fun onSurfaceTextureSizeChanged(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        if (width > 0 && height > 0) {
            surfaceTexture.setDefaultBufferSize(width, height)
        }
        queueEvent {
            makeRenderContextCurrent()
            configureSurfaceSize(width, height)
            renderFrame()
        }
    }

    override fun onSurfaceTextureDestroyed(surfaceTexture: SurfaceTexture): Boolean {
        runOnGlThreadSync(2000) {
            destroyRenderSurface()
            true
        }
        return true
    }

    override fun onSurfaceTextureUpdated(surfaceTexture: SurfaceTexture) = Unit

    private fun configureSurfaceSize(width: Int, height: Int) {
        viewWidth = width
        viewHeight = height

        if (width <= 0 || height <= 0) {
            return
        }
        if (!nativeLibraryAvailable) {
            android.util.Log.e("MobileInkCanvasView", "Cannot configure drawing engine because native library is unavailable")
            return
        }

        val shouldPreserveDrawing = drawingEngine != 0L
        if (drawingEngine == 0L || engineWidth != width || engineHeight != height) {
            configureDrawingEngine(width, height, shouldPreserveDrawing)
        }

        // Re-apply PDF background if we have one (needs to be re-rendered at new size)
        if (!currentPdfBackgroundUri.isNullOrEmpty()) {
            setPdfBackgroundUri(currentPdfBackgroundUri)
        }
    }

    private fun createRenderSurface(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        destroyRenderSurface()

        renderSurface = Surface(surfaceTexture)
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            android.util.Log.e("MobileInkCanvasView", "Unable to get EGL display")
            return
        }

        val version = IntArray(2)
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            android.util.Log.e("MobileInkCanvasView", "Unable to initialize EGL")
            destroyRenderSurface()
            return
        }

        val configAttributes = intArrayOf(
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_DEPTH_SIZE, 0,
            EGL14.EGL_STENCIL_SIZE, 0,
            EGL14.EGL_NONE,
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val configCount = IntArray(1)
        if (!EGL14.eglChooseConfig(
                eglDisplay,
                configAttributes,
                0,
                configs,
                0,
                configs.size,
                configCount,
                0,
            ) || configCount[0] == 0 || configs[0] == null
        ) {
            android.util.Log.e("MobileInkCanvasView", "Unable to choose EGL config")
            destroyRenderSurface()
            return
        }

        val resolvedConfig = configs[0]
        if (resolvedConfig == null) {
            android.util.Log.e("MobileInkCanvasView", "EGL config was unexpectedly null")
            destroyRenderSurface()
            return
        }

        eglContext = EGL14.eglCreateContext(
            eglDisplay,
            resolvedConfig,
            EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE),
            0,
        )
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            android.util.Log.e("MobileInkCanvasView", "Unable to create EGL context")
            destroyRenderSurface()
            return
        }

        eglSurface = EGL14.eglCreateWindowSurface(
            eglDisplay,
            resolvedConfig,
            renderSurface,
            intArrayOf(EGL14.EGL_NONE),
            0,
        )
        if (eglSurface == EGL14.EGL_NO_SURFACE) {
            android.util.Log.e("MobileInkCanvasView", "Unable to create EGL window surface")
            destroyRenderSurface()
            return
        }

        if (!makeRenderContextCurrent()) {
            destroyRenderSurface()
            return
        }

        surfaceReady = true
        configureSurfaceSize(width, height)
        renderFrame()
    }

    private fun makeRenderContextCurrent(): Boolean {
        if (
            eglDisplay == EGL14.EGL_NO_DISPLAY ||
            eglContext == EGL14.EGL_NO_CONTEXT ||
            eglSurface == EGL14.EGL_NO_SURFACE
        ) {
            return false
        }

        val success = EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)
        if (!success) {
            android.util.Log.e("MobileInkCanvasView", "Unable to make EGL context current")
        }
        return success
    }

    private fun destroyRenderSurface() {
        surfaceReady = false

        if (drawingEngine != 0L && eglDisplay != EGL14.EGL_NO_DISPLAY) {
            makeRenderContextCurrent()
            releaseGaneshContext(drawingEngine)
        }

        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(
                eglDisplay,
                EGL14.EGL_NO_SURFACE,
                EGL14.EGL_NO_SURFACE,
                EGL14.EGL_NO_CONTEXT,
            )
            if (eglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, eglSurface)
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext)
            }
            EGL14.eglTerminate(eglDisplay)
        }

        eglSurface = EGL14.EGL_NO_SURFACE
        eglContext = EGL14.EGL_NO_CONTEXT
        eglDisplay = EGL14.EGL_NO_DISPLAY
        renderSurface?.release()
        renderSurface = null
    }

    private fun configureDrawingEngine(width: Int, height: Int, preserveExistingDrawing: Boolean) {
        val preservedDrawing = if (preserveExistingDrawing && drawingEngine != 0L) {
            try {
                serializeDrawing(drawingEngine)
            } catch (e: Exception) {
                android.util.Log.e("MobileInkCanvasView", "Failed to preserve drawing while resizing", e)
                null
            }
        } else {
            null
        }

        if (drawingEngine != 0L) {
            releaseGaneshContext(drawingEngine)
            destroyDrawingEngine(drawingEngine)
            drawingEngine = 0
        }

        drawingEngine = createDrawingEngine(width, height)
        engineWidth = width
        engineHeight = height

        if (drawingEngine == 0L) {
            return
        }

        setBackgroundType(drawingEngine, currentBackgroundType)
        applyCurrentToolToEngine(drawingEngine)

        if (preservedDrawing != null && preservedDrawing.isNotEmpty()) {
            deserializeDrawing(drawingEngine, preservedDrawing)
        }

        resetTransientInteractionState()
        post { emitSelectionChange() }
    }

    private fun renderFrame() {
        val engine = drawingEngine
        if (!surfaceReady || engine == 0L || viewWidth <= 0 || viewHeight <= 0) {
            return
        }
        if (!makeRenderContextCurrent()) {
            return
        }
        if (renderGaneshToCurrentSurface(engine, viewWidth, viewHeight)) {
            EGL14.eglSwapBuffers(eglDisplay, eglSurface)
        }
    }

    private fun queueEvent(block: () -> Unit) {
        if (Looper.myLooper() == renderHandler.looper) {
            block()
            return
        }

        renderHandler.post(block)
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
            val pdfBitmap = PdfLoader.loadAndRenderPdf(context, uri, viewWidth, viewHeight)
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

        runOnGlThreadSync(2000) {
            if (drawingEngine != 0L) {
                if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                    makeRenderContextCurrent()
                    releaseGaneshContext(drawingEngine)
                }
                destroyDrawingEngine(drawingEngine)
                drawingEngine = 0
            }
            destroyRenderSurface()
            true
        }
        renderThread.quitSafely()
        super.onDetachedFromWindow()
    }

    private fun requestInkRender(force: Boolean = false) {
        if (!renderSuspended || force) {
            queueEvent {
                renderFrame()
            }
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

    private fun normalizedPressure(rawPressure: Float, isStylusInput: Boolean): Float {
        val clamped = if (rawPressure.isFinite()) rawPressure.coerceIn(0f, 1f) else 1f
        return if (isStylusInput) {
            maxOf(minimumStylusPressure, Math.pow(clamped.toDouble(), stylusPressureExponent.toDouble()).toFloat())
        } else {
            maxOf(0.1f, clamped)
        }
    }

    private fun normalizedAltitudeFromTilt(rawTilt: Float, isStylusInput: Boolean): Float {
        if (!isStylusInput) {
            return perpendicularAltitude
        }
        val altitude = perpendicularAltitude - rawTilt
        return if (altitude.isFinite()) altitude.coerceIn(0f, perpendicularAltitude) else perpendicularAltitude
    }

    private fun selectionBoundsContain(x: Float, y: Float, bounds: FloatArray, padding: Float): Boolean {
        return x >= bounds[0] - padding &&
            x <= bounds[2] + padding &&
            y >= bounds[1] - padding &&
            y <= bounds[3] + padding
    }

    private fun selectionHandleHitTest(x: Float, y: Float, bounds: FloatArray): Int? {
        if (bounds.size != 4 || bounds[2] <= bounds[0] || bounds[3] <= bounds[1]) {
            return null
        }

        val centerX = (bounds[0] + bounds[2]) * 0.5f
        val centerY = (bounds[1] + bounds[3]) * 0.5f
        val handles = arrayOf(
            0 to floatArrayOf(bounds[0], bounds[1]),
            1 to floatArrayOf(centerX, bounds[1]),
            2 to floatArrayOf(bounds[2], bounds[1]),
            3 to floatArrayOf(bounds[0], centerY),
            4 to floatArrayOf(bounds[2], centerY),
            5 to floatArrayOf(bounds[0], bounds[3]),
            6 to floatArrayOf(centerX, bounds[3]),
            7 to floatArrayOf(bounds[2], bounds[3])
        )
        val hitRadius = 28f * resources.displayMetrics.density
        val hitRadiusSquared = hitRadius * hitRadius

        for ((handleIndex, point) in handles) {
            val dx = x - point[0]
            val dy = y - point[1]
            if (dx * dx + dy * dy <= hitRadiusSquared) {
                return handleIndex
            }
        }
        return null
    }

    private fun handleFingerSelectionTouch(x: Float, y: Float): Boolean {
        if (currentToolType == "text" || drawingEngine == 0L) {
            return false
        }

        cancelHoldToShapePreview()
        showEraserCursor = false

        val selectionCount = getSelectionCount()
        val bounds = if (selectionCount > 0) getSelectionBounds() else null
        if (bounds != null) {
            val handleIndex = selectionHandleHitTest(x, y, bounds)
            if (handleIndex != null) {
                queueEvent {
                    if (drawingEngine != 0L) {
                        beginSelectionTransform(drawingEngine, handleIndex)
                    }
                }
                isTransformingSelection = true
                selectionTransformHandleIndex = handleIndex
                hasSelectionMoveDelta = false
                lastDragX = x
                lastDragY = y
                requestInkRender()
                return true
            }

            if (selectionBoundsContain(x, y, bounds, 24f * resources.displayMetrics.density)) {
                isMovingSelection = true
                hasSelectionMoveDelta = false
                lastDragX = x
                lastDragY = y
                requestInkRender()
                return true
            }
        }

        val hadSelection = selectionCount > 0
        val selectedShape = runOnGlThreadSync {
            if (drawingEngine != 0L) {
                if (hadSelection) {
                    clearSelection(drawingEngine)
                }
                selectShapeStrokeAt(drawingEngine, x, y)
            } else {
                false
            }
        } ?: false

        if (selectedShape) {
            isMovingSelection = true
            hasSelectionMoveDelta = false
            lastDragX = x
            lastDragY = y
            requestInkRender()
            emitSelectionChange()
            return true
        }

        if (hadSelection) {
            requestInkRender()
            emitSelectionChange()
            return true
        }

        return false
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

        val x = event.x
        val y = event.y
        val toolType = event.getToolType(0)
        val isStylusInput = toolType == MotionEvent.TOOL_TYPE_STYLUS ||
            toolType == MotionEvent.TOOL_TYPE_ERASER

        if (event.actionMasked == MotionEvent.ACTION_DOWN &&
            !isStylusInput &&
            handleFingerSelectionTouch(x, y)
        ) {
            parent?.requestDisallowInterceptTouchEvent(true)
            return true
        }

        val isSelectionInteraction = currentToolType == "select" || isMovingSelection || isTransformingSelection
        if (drawingPolicy == "pencilonly" && !isStylusInput && !isSelectionInteraction) {
            return false
        }

        // CRITICAL: Request parent to not intercept touch events
        // This is essential for drawing - without it, ScrollView or other parents
        // will intercept ACTION_MOVE events, causing only dots to appear
        parent?.requestDisallowInterceptTouchEvent(true)

        val pressure = normalizedPressure(event.pressure, isStylusInput)

        // Get stylus tilt (altitude) - AXIS_TILT is tilt angle from perpendicular
        // 0 = perpendicular to screen, pi/2 = parallel to screen
        val tilt = event.getAxisValue(MotionEvent.AXIS_TILT)
        val altitude = normalizedAltitudeFromTilt(tilt, isStylusInput)

        // Get stylus orientation (azimuth) - angle around the perpendicular axis
        val azimuth = if (isStylusInput) event.getAxisValue(MotionEvent.AXIS_ORIENTATION) else 0f
        val eventTimestamp = event.eventTime

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // For select tool, check if tapping inside existing selection to move it
                if (currentToolType == "select" && drawingEngine != 0L) {
                    val selectionCount = getSelectionCount()
                    if (selectionCount > 0) {
                        val bounds = getSelectionBounds()
                        if (bounds != null && isPointInBounds(x, y, bounds)) {
                            // Start moving the selection
                            isMovingSelection = true
                            hasSelectionMoveDelta = false
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
                if (isTransformingSelection && drawingEngine != 0L) {
                    cancelHoldToShapePreview()
                    val moved = x != lastDragX || y != lastDragY
                    if (moved) {
                        hasSelectionMoveDelta = true
                    }
                    queueEvent {
                        if (drawingEngine != 0L) {
                            updateSelectionTransform(drawingEngine, x, y)
                            post { emitSelectionChange() }
                        }
                    }
                    lastDragX = x
                    lastDragY = y
                    requestInkRender()
                    return true
                }

                // Handle selection move
                if (isMovingSelection && drawingEngine != 0L) {
                    cancelHoldToShapePreview()
                    val dx = x - lastDragX
                    val dy = y - lastDragY
                    if (dx != 0f || dy != 0f) {
                        hasSelectionMoveDelta = true
                        queueEvent {
                            if (drawingEngine != 0L) {
                                moveSelection(drawingEngine, dx, dy)
                                post { emitSelectionChange() }
                            }
                        }
                    }
                    lastDragX = x
                    lastDragY = y
                    requestInkRender()
                    return true
                }

                // Update eraser cursor position (local state)
                if (currentToolType == "eraser" && currentEraserMode == "pixel") {
                    eraserCursorX = x
                    eraserCursorY = y
                }

                // Pixel eraser interpolates between samples in C++ and scans
                // existing strokes per sample, so replaying Android history here
                // multiplies the expensive work without improving coverage.
                val shouldProcessHistoricalPoints =
                    currentToolType != "eraser" || currentEraserMode != "pixel"

                // Collect historical points for batch processing
                val historySize = event.historySize
                val historicalPoints = mutableListOf<FloatArray>()
                val historicalTimestamps = mutableListOf<Long>()
                if (shouldProcessHistoricalPoints) {
                    for (i in 0 until historySize) {
                        val hx = event.getHistoricalX(i)
                        val hy = event.getHistoricalY(i)
                        val hp = normalizedPressure(event.getHistoricalPressure(i), isStylusInput)
                        val hTilt = event.getHistoricalAxisValue(MotionEvent.AXIS_TILT, i)
                        val hAltitude = normalizedAltitudeFromTilt(hTilt, isStylusInput)
                        val hAzimuth = if (isStylusInput) {
                            event.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, i)
                        } else {
                            0f
                        }
                        historicalPoints.add(floatArrayOf(hx, hy, hp, hAzimuth, hAltitude))
                        historicalTimestamps.add(event.getHistoricalEventTime(i))
                    }
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
                val isCancel = event.actionMasked == MotionEvent.ACTION_CANCEL

                if (isTransformingSelection && drawingEngine != 0L) {
                    val didTransformSelection = hasSelectionMoveDelta && !isCancel
                    queueEvent {
                        if (drawingEngine != 0L) {
                            if (didTransformSelection) {
                                finalizeSelectionTransform(drawingEngine)
                            } else {
                                cancelSelectionTransform(drawingEngine)
                            }
                            post { emitSelectionChange() }
                        }
                    }
                    isTransformingSelection = false
                    selectionTransformHandleIndex = -1
                    hasSelectionMoveDelta = false
                    requestInkRender()
                    if (didTransformSelection) {
                        sendEvent("onDrawingChange", Arguments.createMap())
                    }
                    return true
                }

                // Finalize selection move
                if (isMovingSelection && drawingEngine != 0L) {
                    val didMoveSelection = hasSelectionMoveDelta && !isCancel
                    if (didMoveSelection) {
                        queueEvent {
                            if (drawingEngine != 0L) {
                                finalizeMove(drawingEngine)
                                post { emitSelectionChange() }
                            }
                        }
                    } else {
                        post { emitSelectionChange() }
                    }
                    isMovingSelection = false
                    hasSelectionMoveDelta = false
                    requestInkRender()
                    if (didMoveSelection) {
                        sendEvent("onDrawingChange", Arguments.createMap())
                    }
                    return true
                }

                // Hide eraser cursor (local state)
                showEraserCursor = false

                // Queue touch end to GL thread to avoid race conditions
                queueEvent {
                    if (drawingEngine != 0L) {
                        setEraserCursor(drawingEngine, 0f, 0f, 0f, false)
                        touchEnded(drawingEngine, if (isCancel) 0L else eventTimestamp)
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
        clearCanvasForLoad()
        sendEvent("onDrawingChange", Arguments.createMap())
    }

    fun clearCanvasForLoad(): Boolean {
        val success = runOnGlThreadSync {
            if (drawingEngine != 0L) {
                clearCanvas(drawingEngine)
                true
            } else {
                false
            }
        } ?: false

        if (success) {
            resetTransientInteractionState()
            requestInkRender()
            emitSelectionChange()
        }

        return success
    }

    fun clearCanvasAsyncForLoad() {
        queueEvent {
            if (drawingEngine != 0L) {
                clearCanvas(drawingEngine)
                post { emitSelectionChange() }
            }
        }
        requestInkRender()
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
        cancelHoldToShapePreview()
        currentToolType = toolType
        currentEraserMode = "pixel"
        currentStrokeWidth = width
        currentStrokeColor = color
        resetTransientInteractionState()

        queueEvent {
            if (drawingEngine != 0L) {
                if (toolType != "select") {
                    clearSelection(drawingEngine)
                    post { emitSelectionChange() }
                }
                applyCurrentToolToEngine(drawingEngine)
            }
        }
        requestInkRender()
    }

    fun setToolWithParams(toolType: String, width: Float, color: Int, eraserMode: String?) {
        cancelHoldToShapePreview()

        // Update tool state (local - doesn't need queuing)
        currentToolType = toolType
        currentEraserMode = eraserMode ?: "pixel"
        currentStrokeWidth = width
        currentStrokeColor = color
        resetTransientInteractionState()

        // Hide eraser cursor if switching away from pixel eraser
        if (toolType != "eraser" || eraserMode != "pixel") {
            showEraserCursor = false
        }

        // Queue engine operations to GL thread
        queueEvent {
            if (drawingEngine != 0L) {
                // Clear selection when switching away from select tool
                if (toolType != "select") {
                    clearSelection(drawingEngine)
                    post { emitSelectionChange() }
                }
                applyCurrentToolToEngine(drawingEngine)
            }
        }
        requestInkRender()
    }

    private fun applyCurrentToolToEngine(engine: Long) {
        setToolWithParams(
            engine,
            currentToolType,
            currentStrokeWidth,
            currentStrokeColor,
            currentEraserMode
        )
    }

    private fun resetTransientInteractionState() {
        isMovingSelection = false
        isTransformingSelection = false
        hasSelectionMoveDelta = false
        selectionTransformHandleIndex = -1
        lastDragX = 0f
        lastDragY = 0f
    }

    // Helper to check if a point is inside selection bounds
    private fun isPointInBounds(x: Float, y: Float, bounds: FloatArray): Boolean {
        // bounds is [minX, minY, maxX, maxY]
        return x >= bounds[0] && x <= bounds[2] && y >= bounds[1] && y <= bounds[3]
    }

    fun setDrawingBackgroundColor(color: Int) {
        setBackgroundColor(color)
        requestInkRender()
    }

    // Selection operations
    fun selectAt(x: Float, y: Float): Boolean {
        val result = runOnGlThreadSync {
            if (drawingEngine != 0L) selectStrokeAt(drawingEngine, x, y) else false
        } ?: false
        if (result) {
            requestInkRender()
            emitSelectionChange()
        }
        return result
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
        sendEvent("onDrawingChange", Arguments.createMap())
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
        sendEvent("onDrawingChange", Arguments.createMap())
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
        return runOnGlThreadSync {
            if (drawingEngine != 0L) getSelectionCount(drawingEngine) else 0
        } ?: 0
    }

    fun getSelectionBounds(): FloatArray? {
        return runOnGlThreadSync {
            if (drawingEngine != 0L) getSelectionBounds(drawingEngine) else null
        }
    }

    // State queries
    fun canUndo(): Boolean = runOnGlThreadSync {
        drawingEngine != 0L && canUndo(drawingEngine)
    } ?: false
    fun canRedo(): Boolean = runOnGlThreadSync {
        drawingEngine != 0L && canRedo(drawingEngine)
    } ?: false
    fun isEmpty(): Boolean = runOnGlThreadSync {
        drawingEngine == 0L || isEmpty(drawingEngine)
    } ?: true

    // Helper for synchronous GL thread operations with timeout
    private fun <T> runOnGlThreadSync(timeoutMs: Long = 2000, block: () -> T?): T? {
        if (Looper.myLooper() == renderHandler.looper) {
            return block()
        }

        val latch = java.util.concurrent.CountDownLatch(1)
        var result: T? = null
        var failure: Throwable? = null
        queueEvent {
            try {
                result = block()
            } catch (throwable: Throwable) {
                failure = throwable
            } finally {
                latch.countDown()
            }
        }
        try {
            val completed = latch.await(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)
            if (!completed) {
                android.util.Log.e("MobileInkCanvasView", "GL thread operation timed out after ${timeoutMs}ms")
            }
        } catch (e: InterruptedException) {
            android.util.Log.e("MobileInkCanvasView", "GL thread operation interrupted", e)
            Thread.currentThread().interrupt()
        }
        failure?.let { throw it }
        return result
    }

    fun serializeDrawing(): ByteArray? {
        if (drawingEngine == 0L) return null
        return runOnGlThreadSync(3000) {
            if (drawingEngine != 0L) serializeDrawing(drawingEngine) else null
        }
    }

    fun deserializeDrawing(data: ByteArray): Boolean {
        val success = runOnGlThreadSync {
            if (drawingEngine != 0L) { deserializeDrawing(drawingEngine, data); true } else false
        } ?: false
        if (success) {
            resetTransientInteractionState()
            requestInkRender()
            emitSelectionChange()
        }
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
            val actualScale = if (scale > 0f) scale else resources.displayMetrics.density
            val width = (viewWidth * actualScale).toInt().coerceAtLeast(1)
            val height = (viewHeight * actualScale).toInt().coerceAtLeast(1)
            val finalBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            if (drawingEngine != 0L) renderToPixelsScaled(drawingEngine, finalBitmap, actualScale)

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
        sendEvent("onInkSelectionChange", payload)
    }

    // Native method declarations
    private external fun createDrawingEngine(width: Int, height: Int): Long
    private external fun destroyDrawingEngine(engine: Long)

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
    private external fun beginSelectionTransform(engine: Long, handleIndex: Int)
    private external fun updateSelectionTransform(engine: Long, x: Float, y: Float)
    private external fun finalizeSelectionTransform(engine: Long)
    private external fun cancelSelectionTransform(engine: Long)
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
    private external fun renderGaneshToCurrentSurface(engine: Long, width: Int, height: Int): Boolean
    private external fun releaseGaneshContext(engine: Long)
    private external fun renderToPixelsScaled(engine: Long, bitmap: Bitmap, scale: Float)

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
