export {
  NativeInkCanvas,
  batchExportPages,
  composeContinuousWindow,
  decomposeContinuousWindow,
  readBodyFileParsed,
} from "./NativeInkCanvas";
export type {
  NativeInkCanvasProps,
  NativeInkCanvasRef,
} from "./NativeInkCanvas";
export {
  aggregateNotebookBenchmarkResults,
  DEFAULT_NATIVE_INK_RENDER_BACKEND,
} from "./benchmark";
export type {
  NativeInkBenchmarkDistribution,
  NativeInkBenchmarkOptions,
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
  NativeInkBenchmarkViewportMetrics,
  NativeInkBenchmarkWorkload,
  NativeInkRenderBackend,
} from "./benchmark";

export {
  ContinuousEnginePool,
} from "./ContinuousEnginePool";
export type {
  ContinuousEnginePoolAssignment,
  ContinuousEnginePoolProps,
  ContinuousEnginePoolRef,
  ContinuousEnginePoolSlotRef,
  ContinuousEnginePoolToolState,
} from "./ContinuousEnginePool";

export { InfiniteInkCanvas } from "./InfiniteInkCanvas";
export type {
  InfiniteInkCanvasProps,
  InfiniteInkCanvasRef,
  InfiniteInkViewportTransform,
} from "./InfiniteInkCanvas";

export { default as ZoomableInkViewport } from "./ZoomableInkViewport";
export type {
  TouchExclusionRect,
  ZoomableInkViewportProps,
  ZoomableInkViewportRef,
} from "./ZoomableInkViewport";

export { NativeInkPageBackground } from "./NativeInkPageBackground";
export type { NativeInkPageBackgroundProps } from "./NativeInkPageBackground";

export {
  CONTINUOUS_ENGINE_POOL_SIZE,
  getContinuousEnginePoolRange,
} from "./utils/continuousEnginePool";
export {
  resolveContinuousTapCoordinates,
} from "./utils/continuousCanvasCoordinates";
export type {
  ContinuousTapCoordinates,
  ContinuousTapInput,
} from "./utils/continuousCanvasCoordinates";
export {
  buildNotebookFromParsed,
  compositeTextBoxesOnImage,
  deserializeNotebookData,
  reconcilePdfNotebookPageCount,
  serializeNotebookData,
} from "./utils/serialization";
export type {
  DeserializedNotebookData,
  PdfNotebookReconciliationResult,
} from "./utils/serialization";
export type {
  InsertedElement,
  InkTextBox,
  InkToolType,
  NativeInkDrawingBeginEvent,
  NativeInkPencilDoubleTapEvent,
  NativeInkSelectionChangeEvent,
  NativeSelectionBounds,
  NotebookPage,
  SerializedNotebookData,
  TextBox,
  ToolType,
} from "./types";
