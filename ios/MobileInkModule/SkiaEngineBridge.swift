import Foundation

// MARK: - C++ Drawing Engine Bridge Declarations
// These are Skia/C++ function declarations used by MobileInkCanvasView

// MARK: - Engine Lifecycle
@_silgen_name("createDrawingEngine")
func createDrawingEngine(_ width: Int32, _ height: Int32) -> OpaquePointer

@_silgen_name("destroyDrawingEngine")
func destroyDrawingEngine(_ engine: OpaquePointer)

// MARK: - Touch Input
@_silgen_name("touchBegan")
func touchBegan(_ engine: OpaquePointer, _ x: Float, _ y: Float, _ pressure: Float, _ azimuth: Float, _ altitude: Float, _ timestamp: Int64, _ isPencilInput: Bool)

@_silgen_name("touchMoved")
func touchMoved(_ engine: OpaquePointer, _ x: Float, _ y: Float, _ pressure: Float, _ azimuth: Float, _ altitude: Float, _ timestamp: Int64, _ isPencilInput: Bool)

@_silgen_name("touchEnded")
func touchEnded(_ engine: OpaquePointer, _ timestamp: Int64)

@_silgen_name("updateHoldShapePreview")
func updateHoldShapePreview(_ engine: OpaquePointer, _ timestamp: Int64) -> Bool

// MARK: - Predictive Touch (Apple Pencil low-latency rendering)
@_silgen_name("clearPredictedPoints")
func clearPredictedPoints(_ engine: OpaquePointer)

@_silgen_name("addPredictedPoint")
func addPredictedPoint(_ engine: OpaquePointer, _ x: Float, _ y: Float, _ pressure: Float, _ azimuth: Float, _ altitude: Float, _ timestamp: Int64, _ isPencilInput: Bool)

// MARK: - Canvas Operations
@_silgen_name("clearCanvas")
func clearCanvas(_ engine: OpaquePointer)

@_silgen_name("undoStroke")
func undoStroke(_ engine: OpaquePointer)

@_silgen_name("redoStroke")
func redoStroke(_ engine: OpaquePointer)

// MARK: - Tool Settings
@_silgen_name("setStrokeColor")
func setStrokeColor(_ engine: OpaquePointer, _ color: UInt32)

@_silgen_name("setStrokeWidth")
func setStrokeWidth(_ engine: OpaquePointer, _ width: Float)

@_silgen_name("setTool")
func nativeSetTool(_ engine: OpaquePointer, _ toolType: UnsafePointer<CChar>)

@_silgen_name("setToolWithParams")
func nativeSetToolWithParams(_ engine: OpaquePointer, _ toolType: UnsafePointer<CChar>, _ width: Float, _ color: UInt32, _ eraserMode: UnsafePointer<CChar>)

@_silgen_name("setBackgroundType")
func nativeSetBackgroundType(_ engine: OpaquePointer, _ backgroundType: UnsafePointer<CChar>)

// MARK: - State Queries
@_silgen_name("canUndo")
func canUndo(_ engine: OpaquePointer) -> Bool

@_silgen_name("canRedo")
func canRedo(_ engine: OpaquePointer) -> Bool

@_silgen_name("isEmpty")
func isEmpty(_ engine: OpaquePointer) -> Bool

// MARK: - Rendering
@_silgen_name("renderToCanvas")
func renderToCanvas(_ engine: OpaquePointer, _ canvas: OpaquePointer)

@_silgen_name("createGaneshMetalContext")
func createGaneshMetalContext(_ device: UnsafeMutableRawPointer, _ commandQueue: UnsafeMutableRawPointer) -> OpaquePointer?

@_silgen_name("destroyGaneshMetalContext")
func destroyGaneshMetalContext(_ context: OpaquePointer)

@_silgen_name("renderToGaneshMetalTexture")
func renderToGaneshMetalTexture(
  _ engine: OpaquePointer,
  _ context: OpaquePointer,
  _ texture: UnsafeMutableRawPointer,
  _ width: Int32,
  _ height: Int32
) -> Bool

// MARK: - Skia Canvas Helpers
@_silgen_name("createSkiaCanvas")
func createSkiaCanvas(_ pixels: UnsafeMutableRawPointer, _ width: Int32, _ height: Int32, _ rowBytes: Int32) -> OpaquePointer?

@_silgen_name("scaleSkiaCanvas")
func scaleSkiaCanvas(_ canvas: OpaquePointer, _ scaleX: Float, _ scaleY: Float)

@_silgen_name("destroySkiaCanvas")
func destroySkiaCanvas(_ canvas: OpaquePointer)

// MARK: - Selection Operations
@_silgen_name("selectStrokeAt")
func selectStrokeAt(_ engine: OpaquePointer, _ x: Float, _ y: Float) -> Bool

@_silgen_name("selectShapeStrokeAt")
func selectShapeStrokeAt(_ engine: OpaquePointer, _ x: Float, _ y: Float) -> Bool

@_silgen_name("clearSelection")
func clearSelection(_ engine: OpaquePointer)

@_silgen_name("deleteSelection")
func deleteSelection(_ engine: OpaquePointer)

@_silgen_name("copySelection")
func copySelection(_ engine: OpaquePointer)

@_silgen_name("pasteSelection")
func pasteSelection(_ engine: OpaquePointer, _ offsetX: Float, _ offsetY: Float)

@_silgen_name("moveSelection")
func moveSelection(_ engine: OpaquePointer, _ dx: Float, _ dy: Float)

@_silgen_name("finalizeMove")
func finalizeMove(_ engine: OpaquePointer)

@_silgen_name("beginSelectionTransform")
func beginSelectionTransform(_ engine: OpaquePointer, _ handleIndex: Int32)

@_silgen_name("updateSelectionTransform")
func updateSelectionTransform(_ engine: OpaquePointer, _ x: Float, _ y: Float)

@_silgen_name("finalizeSelectionTransform")
func finalizeSelectionTransform(_ engine: OpaquePointer)

@_silgen_name("cancelSelectionTransform")
func cancelSelectionTransform(_ engine: OpaquePointer)

@_silgen_name("getSelectionCount")
func getSelectionCount(_ engine: OpaquePointer) -> Int32

@_silgen_name("getSelectionBounds")
func getSelectionBounds(_ engine: OpaquePointer, _ outBounds: UnsafeMutablePointer<Float>)

// MARK: - Serialization
@_silgen_name("serializeDrawing")
func serializeDrawing(_ engine: OpaquePointer, _ outSize: UnsafeMutablePointer<Int32>) -> UnsafeMutablePointer<UInt8>?

@_silgen_name("freeSerializedData")
func freeSerializedData(_ data: UnsafeMutablePointer<UInt8>)

@_silgen_name("deserializeDrawing")
func deserializeDrawing(_ engine: OpaquePointer, _ data: UnsafePointer<UInt8>, _ size: Int32) -> Bool

@_silgen_name("composeContinuousWindow")
func nativeComposeContinuousWindow(
  _ pageDataPtrs: UnsafePointer<UnsafePointer<UInt8>?>,
  _ pageDataSizes: UnsafePointer<Int32>,
  _ pageCount: Int32,
  _ pageHeight: Float,
  _ outSize: UnsafeMutablePointer<Int32>
) -> UnsafeMutablePointer<UInt8>?

@_silgen_name("decomposeContinuousWindow")
func nativeDecomposeContinuousWindow(
  _ windowData: UnsafePointer<UInt8>?,
  _ windowDataSize: Int32,
  _ pageCount: Int32,
  _ pageHeight: Float,
  _ outPageDataPtrs: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>,
  _ outPageDataSizes: UnsafeMutablePointer<Int32>
) -> Int32

// MARK: - Snapshots
@_silgen_name("makeSnapshot")
func makeSnapshot(_ engine: OpaquePointer) -> OpaquePointer?

@_silgen_name("releaseSkImage")
func releaseSkImage(_ image: OpaquePointer)

// MARK: - Batch Export
// Note: Named nativeBatchExportPages to avoid collision with Swift class method
@_silgen_name("batchExportPages")
func nativeBatchExportPages(
    _ pagesDataPtrs: UnsafePointer<UnsafePointer<UInt8>?>,
    _ pagesDataSizes: UnsafePointer<Int32>,
    _ backgroundTypes: UnsafePointer<UnsafePointer<CChar>?>,
    _ pdfPixelDataPtrs: UnsafePointer<UnsafePointer<UInt8>?>?,
    _ pdfPixelDataWidths: UnsafePointer<Int32>?,
    _ pdfPixelDataHeights: UnsafePointer<Int32>?,
    _ pageIndices: UnsafePointer<Int32>?,
    _ numPages: Int32,
    _ width: Int32,
    _ height: Int32,
    _ scale: Float,
    _ outResults: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>,
    _ outResultLengths: UnsafeMutablePointer<Int32>
) -> Int32

@_silgen_name("freeBatchExportResult")
func freeBatchExportResult(_ result: UnsafeMutablePointer<CChar>?)
