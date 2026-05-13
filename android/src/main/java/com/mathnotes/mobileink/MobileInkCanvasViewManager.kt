package com.mathnotes.mobileink

import android.graphics.Color
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.uimanager.SimpleViewManager
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.annotations.ReactProp

/**
 * MobileInkCanvasViewManager - manages the MobileInkCanvasView which handles drawing.
 * Background patterns are rendered in the C++ Skia layer, not as a separate Android View.
 * This ensures eraser works correctly and view registration is proper.
 */
class MobileInkCanvasViewManager(private val reactContext: ReactApplicationContext) :
    SimpleViewManager<MobileInkCanvasView>() {

    override fun getName() = "MobileInkCanvasView"

    override fun createViewInstance(reactContext: ThemedReactContext): MobileInkCanvasView {
        return MobileInkCanvasView(reactContext)
    }

    override fun getExportedCustomBubblingEventTypeConstants(): Map<String, Any> {
        return mapOf(
            "onDrawingChange" to mapOf(
                "phasedRegistrationNames" to mapOf("bubbled" to "onDrawingChange")
            ),
            "onDrawingBegin" to mapOf(
                "phasedRegistrationNames" to mapOf("bubbled" to "onDrawingBegin")
            ),
            "onSelectionChange" to mapOf(
                "phasedRegistrationNames" to mapOf("bubbled" to "onSelectionChange")
            )
        )
    }

    @ReactProp(name = "backgroundColor", customType = "Color")
    fun setDrawingBackgroundColor(view: MobileInkCanvasView, color: Int) {
        view.setDrawingBackgroundColor(color)
    }

    @ReactProp(name = "backgroundType")
    fun setBackgroundType(view: MobileInkCanvasView, type: String?) {
        view.setBackgroundType(type ?: "plain")
    }

    @ReactProp(name = "pdfBackgroundUri")
    fun setPdfBackgroundUri(view: MobileInkCanvasView, uri: String?) {
        view.setPdfBackgroundUri(uri)
    }

    @ReactProp(name = "drawingPolicy")
    fun setDrawingPolicy(view: MobileInkCanvasView, policy: String?) {
        view.drawingPolicy = policy ?: "default"
    }

    @ReactProp(name = "renderSuspended")
    fun setRenderSuspended(view: MobileInkCanvasView, suspended: Boolean) {
        view.renderSuspended = suspended
    }

    @ReactProp(name = "renderBackend")
    fun setRenderBackend(view: MobileInkCanvasView, backend: String?) {
        view.renderBackend = backend ?: "ganesh"
    }

    override fun receiveCommand(
        root: MobileInkCanvasView,
        commandId: String,
        args: ReadableArray?
    ) {
        when (commandId) {
            "clear" -> root.clear()
            "undo" -> root.undo()
            "redo" -> root.redo()
            "setTool" -> {
                if (args != null && args.size() >= 3) {
                    val toolType = args.getString(0) ?: "pen"
                    val width = args.getDouble(1).toFloat()
                    val colorHex = args.getString(2) ?: "#000000"
                    val color = Color.parseColor(colorHex)
                    root.setTool(toolType, width, color)
                }
            }
            "setToolWithParams" -> {
                if (args != null && args.size() >= 3) {
                    val toolType = args.getString(0) ?: "pen"
                    val width = args.getDouble(1).toFloat()
                    val colorHex = args.getString(2) ?: "#000000"
                    val color = Color.parseColor(colorHex)
                    val eraserMode = if (args.size() > 3) args.getString(3) else null
                    root.setToolWithParams(toolType, width, color, eraserMode)
                }
            }
            "selectAt" -> {
                if (args != null && args.size() >= 2) {
                    val x = args.getDouble(0).toFloat()
                    val y = args.getDouble(1).toFloat()
                    root.selectAt(x, y)
                }
            }
            "clearSelection" -> root.clearSelection()
            "deleteSelection" -> root.deleteSelection()
            "copySelection" -> root.copySelection()
            // Aliases used by JS side
            "performCopy" -> root.copySelection()
            "performPaste" -> root.pasteSelection(20f, 20f) // Default offset for paste
            "performDelete" -> root.deleteSelection()
            "pasteSelection" -> {
                if (args != null && args.size() >= 2) {
                    val offsetX = args.getDouble(0).toFloat()
                    val offsetY = args.getDouble(1).toFloat()
                    root.pasteSelection(offsetX, offsetY)
                }
            }
            "moveSelection" -> {
                if (args != null && args.size() >= 2) {
                    val dx = args.getDouble(0).toFloat()
                    val dy = args.getDouble(1).toFloat()
                    root.moveSelection(dx, dy)
                }
            }
            "finalizeMove" -> root.finalizeMoveSelection()
            "deserializeDrawing" -> {
                // Deserialize from JSON string (format: {"pages":{"0":"<base64>"}})
                // Match iOS behavior: clear canvas when data is empty/missing
                if (args != null && args.size() >= 1) {
                    val jsonString = args.getString(0)
                    if (jsonString != null) {
                        try {
                            // Parse JSON to extract base64 from pages.0
                            val json = org.json.JSONObject(jsonString)
                            val pages = json.optJSONObject("pages")
                            val base64 = pages?.optString("0")
                            if (!base64.isNullOrEmpty()) {
                                val data = android.util.Base64.decode(base64, android.util.Base64.DEFAULT)
                                root.deserializeDrawing(data)
                            } else {
                                // Match iOS: clear canvas when no data for page 0
                                root.clear()
                            }
                        } catch (e: Exception) {
                            // Clear on parse error to prevent stale data (match iOS behavior)
                            root.clear()
                            android.util.Log.e("MobileInkCanvasViewManager", "Failed to deserialize drawing", e)
                        }
                    } else {
                        // Clear when jsonString is null
                        root.clear()
                    }
                } else {
                    // Clear when no args provided
                    root.clear()
                }
            }
        }
    }
}
