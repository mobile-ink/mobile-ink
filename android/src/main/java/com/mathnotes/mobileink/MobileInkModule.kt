package com.mathnotes.mobileink

import android.net.Uri
import android.graphics.Bitmap
import android.util.Base64
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.bridge.Arguments
import com.facebook.react.module.annotations.ReactModule
import org.json.JSONObject
import java.io.File
import java.lang.ref.WeakReference
import java.util.concurrent.Executors

/**
 * MobileInkModule provides callback-based APIs for the drawing canvas.
 * Used for operations that need to return data (like serialization, state queries).
 */
@ReactModule(name = MobileInkModule.NAME)
class MobileInkModule(private val reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    companion object {
        const val NAME = "MobileInkModule"

        // Thread pool for background processing
        private val executor = Executors.newSingleThreadExecutor()

        // Static registry of MobileInkCanvasView instances by their React tag
        // Uses WeakReference to avoid memory leaks when views are unmounted
        private val viewRegistry = mutableMapOf<Int, WeakReference<MobileInkCanvasView>>()

        /**
         * Register a view when it's created (called from MobileInkCanvasView)
         */
        fun registerView(tag: Int, view: MobileInkCanvasView) {
            viewRegistry[tag] = WeakReference(view)
        }

        /**
         * Unregister a view when it's destroyed (called from MobileInkCanvasView)
         */
        fun unregisterView(tag: Int) {
            viewRegistry.remove(tag)
        }

        /**
         * Get a view by its React tag
         */
        fun getView(tag: Int): MobileInkCanvasView? {
            return viewRegistry[tag]?.get()
        }

        // Native method for batch export - static because it creates its own temp engine
        @JvmStatic
        private external fun nativeBatchExportPages(
            pagesDataArray: Array<ByteArray?>,
            backgroundTypes: Array<String>,
            pdfBackgrounds: Array<Bitmap?>,
            pageIndices: IntArray,
            width: Int,
            height: Int,
            scale: Float
        ): Array<String?>
    }

    override fun getName() = NAME

    private fun findDrawingView(viewTag: Int): MobileInkCanvasView? {
        return getView(viewTag)
    }

    private fun fileFromBridgePath(filePath: String): File {
        if (filePath.startsWith("file://")) {
            val uriPath = Uri.parse(filePath).path
            if (!uriPath.isNullOrEmpty()) {
                return File(uriPath)
            }
            return File(filePath.removePrefix("file://"))
        }
        return File(filePath)
    }

    @ReactMethod
    fun getBase64Data(viewTag: Int, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                if (view != null) {
                    val data = view.serializeDrawing()
                    if (data != null && data.isNotEmpty()) {
                        val base64 = Base64.encodeToString(data, Base64.NO_WRAP)
                        // Wrap in JSON format like iOS: {"pages":{"0":"<base64>"}}
                        val json = """{"pages":{"0":"$base64"}}"""
                        promise.resolve(json)
                    } else {
                        // Empty drawing - return empty JSON like iOS
                        promise.resolve("""{"pages":{}}""")
                    }
                } else {
                    // View not found - likely unmounted during auto-save, return null gracefully
                    android.util.Log.w("MobileInkModule", "getBase64Data: view not found for tag $viewTag")
                    promise.resolve(null)
                }
            } catch (e: Exception) {
                // Don't reject on exceptions either - gracefully return null
                android.util.Log.e("MobileInkModule", "getBase64Data: exception", e)
                promise.resolve(null)
            }
        }
    }

    @ReactMethod
    fun canUndo(viewTag: Int, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                promise.resolve(view?.canUndo() ?: false)
            } catch (e: Exception) {
                promise.reject("ERROR", e.message)
            }
        }
    }

    @ReactMethod
    fun canRedo(viewTag: Int, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                promise.resolve(view?.canRedo() ?: false)
            } catch (e: Exception) {
                promise.reject("ERROR", e.message)
            }
        }
    }

    @ReactMethod
    fun isEmpty(viewTag: Int, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                promise.resolve(view?.isEmpty() ?: true)
            } catch (e: Exception) {
                promise.reject("ERROR", e.message)
            }
        }
    }

    @ReactMethod
    fun getSelectionCount(viewTag: Int, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                promise.resolve(view?.getSelectionCount() ?: 0)
            } catch (e: Exception) {
                promise.reject("ERROR", e.message)
            }
        }
    }

    @ReactMethod
    fun getSelectionBounds(viewTag: Int, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                val bounds = view?.getSelectionBounds()
                if (bounds != null && bounds.size == 4) {
                    val result = Arguments.createMap()
                    result.putDouble("minX", bounds[0].toDouble())
                    result.putDouble("minY", bounds[1].toDouble())
                    result.putDouble("maxX", bounds[2].toDouble())
                    result.putDouble("maxY", bounds[3].toDouble())
                    promise.resolve(result)
                } else {
                    promise.resolve(null)
                }
            } catch (e: Exception) {
                promise.reject("ERROR", e.message)
            }
        }
    }

    @ReactMethod
    fun getBase64PngData(viewTag: Int, scale: Double, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                if (view != null) {
                    val data = view.getBase64PngData(scale.toFloat())
                    if (data != null) {
                        promise.resolve(data)
                    } else {
                        promise.resolve("")
                    }
                } else {
                    android.util.Log.w("MobileInkModule", "getBase64PngData: view not found for tag $viewTag")
                    promise.resolve("")
                }
            } catch (e: Exception) {
                android.util.Log.e("MobileInkModule", "getBase64PngData: exception", e)
                promise.resolve("")
            }
        }
    }

    @ReactMethod
    fun getBase64JpegData(viewTag: Int, scale: Double, compression: Double, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                if (view != null) {
                    val data = view.getBase64JpegData(scale.toFloat(), compression.toFloat())
                    if (data != null) {
                        promise.resolve(data)
                    } else {
                        promise.resolve("")
                    }
                } else {
                    android.util.Log.w("MobileInkModule", "getBase64JpegData: view not found for tag $viewTag")
                    promise.resolve("")
                }
            } catch (e: Exception) {
                android.util.Log.e("MobileInkModule", "getBase64JpegData: exception", e)
                promise.resolve("")
            }
        }
    }

    /**
     * Load base64 drawing data into the canvas.
     * Returns a Promise that resolves to true when loading is complete.
     * This ensures the caller can wait for the load to finish before proceeding.
     */
    @ReactMethod
    fun loadBase64Data(viewTag: Int, jsonString: String, promise: Promise) {
        reactContext.runOnUiQueueThread {
            try {
                val view = findDrawingView(viewTag)
                if (view == null) {
                    android.util.Log.w("MobileInkModule", "loadBase64Data: view not found for tag $viewTag")
                    promise.resolve(false)
                    return@runOnUiQueueThread
                }

                // Parse JSON format: {"pages":{"0":"<base64>"}}
                val json = JSONObject(jsonString)
                val pages = json.optJSONObject("pages")
                if (pages == null) {
                    view.clear()
                    promise.resolve(true)
                    return@runOnUiQueueThread
                }

                // Get page 0 data (current page)
                val base64 = pages.optString("0", "")
                if (base64.isEmpty()) {
                    view.clear()
                    promise.resolve(true)
                    return@runOnUiQueueThread
                }

                // Decode and load
                val data = Base64.decode(base64, Base64.DEFAULT)

                // deserializeDrawing is now synchronous (waits for GL thread)
                val success = view.deserializeDrawing(data)
                promise.resolve(success)
            } catch (e: Exception) {
                android.util.Log.e("MobileInkModule", "loadBase64Data: exception", e)
                promise.resolve(false)
            }
        }
    }

    /**
     * Native-side persistence: serialize the engine's current state to base64 JSON
     * and write directly to disk. The body never crosses the JS<->native bridge --
     * a major drawing-time win on heavy notebooks where the body is multi-MB.
     *
     * Atomicity: write goes to {path}.tmp first, then rename to {path}. POSIX
     * rename is atomic on the same filesystem so a partial write under memory
     * pressure can never poison the canonical body file.
     */
    @ReactMethod
    fun persistEngineToFile(viewTag: Int, filePath: String, promise: Promise) {
        reactContext.runOnUiQueueThread {
            val view = findDrawingView(viewTag)
            if (view == null) {
                android.util.Log.w("MobileInkModule", "persistEngineToFile: view not found for tag $viewTag")
                promise.reject("VIEW_NOT_FOUND", "MobileInkCanvasView not found")
                return@runOnUiQueueThread
            }

            // Serialize on the UI queue (deserialize/serialize already coordinate
            // with the GL thread inside the view), then move the file write to a
            // background executor so it doesn't block the UI thread.
            val data: ByteArray? = try {
                view.serializeDrawing()
            } catch (e: Exception) {
                android.util.Log.e("MobileInkModule", "persistEngineToFile: serialize exception", e)
                promise.reject("GET_DATA_ERROR", e.message ?: "serialize failed", e)
                return@runOnUiQueueThread
            }

            executor.execute {
                try {
                    val body = if (data != null && data.isNotEmpty()) {
                        val base64 = Base64.encodeToString(data, Base64.NO_WRAP)
                        """{"pages":{"0":"$base64"}}"""
                    } else {
                        """{"pages":{}}"""
                    }

                    val target = fileFromBridgePath(filePath)
                    target.parentFile?.let { parent ->
                        if (!parent.exists()) parent.mkdirs()
                    }
                    val tmp = File("${target.absolutePath}.tmp")
                    // Best-effort: drop a leftover .tmp from a previous failed write.
                    if (tmp.exists()) tmp.delete()
                    tmp.writeText(body)
                    val renamed = tmp.renameTo(target)
                    if (!renamed) {
                        // Rename failed -- fall back to a regular write into target,
                        // then drop the .tmp. Loses atomicity but at least the body
                        // is durable on disk.
                        target.writeText(body)
                        if (tmp.exists()) tmp.delete()
                    }
                    promise.resolve(true)
                } catch (e: Exception) {
                    android.util.Log.e("MobileInkModule", "persistEngineToFile: write exception", e)
                    promise.reject("WRITE_FAILED", e.message ?: "write failed", e)
                }
            }
        }
    }

    /**
     * Inverse of persistEngineToFile: read the body file directly and feed it
     * into the engine without the bytes ever crossing the bridge as a JS string.
     *
     * Resolves true on successful load, false if the file is missing/empty.
     * Rejects only on actual deserialization errors.
     */
    @ReactMethod
    fun loadEngineFromFile(viewTag: Int, filePath: String, promise: Promise) {
        executor.execute {
            val target = fileFromBridgePath(filePath)
            if (!target.exists() || target.length() == 0L) {
                promise.resolve(false)
                return@execute
            }

            val body: String = try {
                target.readText()
            } catch (e: Exception) {
                android.util.Log.e("MobileInkModule", "loadEngineFromFile: read exception", e)
                promise.resolve(false)
                return@execute
            }

            reactContext.runOnUiQueueThread {
                try {
                    val view = findDrawingView(viewTag)
                    if (view == null) {
                        promise.reject("VIEW_NOT_FOUND", "MobileInkCanvasView not found")
                        return@runOnUiQueueThread
                    }

                    val json = JSONObject(body)
                    val pages = json.optJSONObject("pages")
                    if (pages == null) {
                        view.clear()
                        promise.resolve(true)
                        return@runOnUiQueueThread
                    }

                    val base64 = pages.optString("0", "")
                    if (base64.isEmpty()) {
                        view.clear()
                        promise.resolve(true)
                        return@runOnUiQueueThread
                    }

                    val data = Base64.decode(base64, Base64.DEFAULT)
                    val success = view.deserializeDrawing(data)
                    promise.resolve(success)
                } catch (e: Exception) {
                    android.util.Log.e("MobileInkModule", "loadEngineFromFile: deserialize exception", e)
                    promise.reject("LOAD_DATA_ERROR", e.message ?: "deserialize failed", e)
                }
            }
        }
    }

    /**
     * Batch export multiple pages to PNG images.
     * This is much faster than exporting pages one by one because it:
     * 1. Creates a single Skia engine and surface (reused for all pages)
     * 2. Doesn't switch visible pages (no UI updates)
     * 3. Processes all pages in a single native call
     *
     * @param pagesDataArray Array of JSON strings with format {"pages":{"0":"<base64>"}}
     * @param backgroundTypes Array of background type strings ("plain", "lined", "grid", "pdf")
     * @param width Canvas width in pixels
     * @param height Canvas height in pixels
     * @param scale Export scale factor (e.g., 2.0 for retina)
     * @param pdfBackgroundUri Optional PDF file URI for PDF backgrounds.
     * @param pageIndices Original notebook page indices for page-aware background export.
     * @param promise Resolves to array of base64 PNG data URIs
     */
    @ReactMethod
    fun batchExportPages(
        pagesDataArray: ReadableArray,
        backgroundTypes: ReadableArray,
        width: Int,
        height: Int,
        scale: Double,
        pdfBackgroundUri: String,
        pageIndices: ReadableArray,
        promise: Promise
    ) {
        // Ensure native library is loaded
        if (!MobileInkCanvasView.ensureLibraryLoaded()) {
            promise.reject("LIBRARY_NOT_LOADED", "Failed to load native drawing library")
            return
        }

        // Process on background thread to avoid blocking UI
        executor.execute {
            try {
                val numPages = pagesDataArray.size()
                if (numPages == 0) {
                    promise.resolve(Arguments.createArray())
                    return@execute
                }

                // Decode page payloads before JNI so valid JSON formatting differences
                // do not leak into the C++ boundary.
                val pagesData = Array<ByteArray?>(numPages) { i ->
                    decodePagePayload(pagesDataArray.getString(i) ?: "")
                }
                val bgTypes = Array(numPages) { i ->
                    if (i < backgroundTypes.size()) backgroundTypes.getString(i) ?: "plain" else "plain"
                }
                val resolvedPageIndices = IntArray(numPages) { i ->
                    if (i < pageIndices.size()) pageIndices.getInt(i) else i
                }
                val pdfBackgrounds = Array<Bitmap?>(numPages) { i ->
                    if (bgTypes[i] != "pdf" || pdfBackgroundUri.isEmpty()) {
                        null
                    } else {
                        val pageAwareUri = pdfUriForPage(pdfBackgroundUri, resolvedPageIndices[i], numPages)
                        PdfLoader.loadAndRenderPdf(reactApplicationContext, pageAwareUri, width)
                    }
                }

                // Call native batch export
                val results = nativeBatchExportPages(
                    pagesData,
                    bgTypes,
                    pdfBackgrounds,
                    resolvedPageIndices,
                    width,
                    height,
                    scale.toFloat()
                )

                pdfBackgrounds.forEach { it?.recycle() }

                // Convert to WritableArray
                val resultArray = Arguments.createArray()
                for (result in results) {
                    resultArray.pushString(result ?: "")
                }

                promise.resolve(resultArray)
            } catch (e: Exception) {
                android.util.Log.e("MobileInkModule", "batchExportPages: exception", e)
                promise.reject("BATCH_EXPORT_ERROR", e.message, e)
            }
        }
    }

    private fun pdfUriForPage(pdfBackgroundUri: String, pageIndex: Int, exportedPageCount: Int): String {
        if (exportedPageCount == 1 && pdfBackgroundUri.contains("#page=")) {
            return pdfBackgroundUri
        }
        val cleanUri = pdfBackgroundUri.substringBefore("#")
        return "$cleanUri#page=${pageIndex + 1}"
    }

    private fun decodePagePayload(pageJson: String): ByteArray? {
        return try {
            val json = JSONObject(pageJson)
            val pages = json.optJSONObject("pages") ?: return null
            val base64 = pages.optString("0", "")
            if (base64.isEmpty()) {
                null
            } else {
                Base64.decode(base64, Base64.DEFAULT).takeIf { it.isNotEmpty() }
            }
        } catch (e: Exception) {
            android.util.Log.w("MobileInkModule", "decodePagePayload: treating invalid page payload as blank")
            null
        }
    }
}
