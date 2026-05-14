import type { NativeSyntheticEvent, ViewStyle } from "react-native";
import type {
  NativeInkBenchmarkOptions,
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
  NativeInkRenderBackend,
} from "../benchmark";
import type { NativeSelectionBounds } from "../types";

export interface NativeInkCanvasProps {
  style?: ViewStyle;
  onDrawingChange?: () => void;
  onDrawingBegin?: (event: NativeSyntheticEvent<{ x: number; y: number }>) => void;
  onSelectionChange?: (event: { nativeEvent: { count: number; bounds?: NativeSelectionBounds | null } }) => void;
  onCanvasReady?: () => void;
  backgroundType?: string;
  pdfBackgroundUri?: string;
  renderSuspended?: boolean;
  /** iOS only: Chooses the native render path for A/B performance tests. */
  renderBackend?: NativeInkRenderBackend;
  /** iOS only: Controls whether fingers or only Apple Pencil can draw */
  drawingPolicy?: "default" | "anyinput" | "pencilonly";
  /** iOS only: Fired when Apple Pencil barrel is double-tapped (2nd gen+) */
  onPencilDoubleTap?: (event: NativeSyntheticEvent<{ sequence: number; timestamp: number }>) => void;
}

export interface NativeInkCanvasRef {
  setNativeProps?: (nativeProps: Record<string, unknown>) => void;
  clear: () => void;
  undo: () => void;
  redo: () => void;
  setTool: (toolType: string, width: number, color: string, eraserMode?: string) => void;
  getBase64Data: () => Promise<string>;
  loadBase64Data: (base64String: string) => Promise<boolean>;
  /**
   * Eagerly release the heavy native state (~13 MB pixel buffer + the
   * C++ drawing engine + queued JS callbacks) without waiting for ARC.
   * The continuous engine pool calls this only on final pool unmount,
   * never for normal page switching. Optional so tests don't have to
   * mock it; iOS-only.
   */
  releaseEngine?: () => Promise<void>;
  /**
   * Native-side single-page persistence: tells the engine to serialize its
   * current state (one page payload) and write directly to the file at
   * `path`. Body bytes never cross the JS<->native bridge.
   *
   * Useful for paged-mode (Android primary) where one engine = one page.
   * Continuous mode should use persistFullNotebookToFile instead so non-
   * visible pages are preserved.
   */
  persistEngineToFile: (path: string) => Promise<boolean>;
  loadEngineFromFile: (path: string) => Promise<boolean>;
  /**
   * Native-side full-notebook autosave (iOS continuous mode).
   *
   * Reads the existing body file, replaces ONLY the visible window's
   * per-page data with the engine's fresh state, writes back atomically.
   * Body bytes (which can be many MB) never cross the JS<->native bridge:
   * JS only sends the small visible-page-IDs array + lightweight
   * pagesMetadata (no data fields).
   *
   * Returns true on success. Returns false (without throwing) when the
   * native fast-path isn't available (older build) so callers can fall
   * back to the existing slow path.
   */
  persistFullNotebookToFile: (params: {
    visiblePageIds: string[];
    pagesMetadata: Array<Record<string, unknown>>;
    originalCanvasWidth?: number;
    pageHeight: number;
    bodyPath: string;
  }) => Promise<boolean>;
  /**
   * Inverse of persistFullNotebookToFile. Reads the body file in native,
   * loads visible-window pages into the engine, returns just the slim
   * metadata array (no per-page data) plus the originalCanvasWidth.
   *
   * Returns null (without throwing) when the file is missing, malformed,
   * or the native fast-path isn't available.
   */
  loadNotebookForVisibleWindow: (params: {
    bodyPath: string;
    visiblePageIds: string[];
    pageHeight: number;
  }) => Promise<{
    success: boolean;
    pagesMetadata?: Array<Record<string, unknown>>;
    originalCanvasWidth?: number | null;
    reason?: string;
  } | null>;
  stageBase64Data?: (base64String: string) => Promise<boolean>;
  presentDeferredLoad?: () => Promise<boolean>;
  getBase64PngData: (scale?: number) => Promise<string>;
  getBase64JpegData: (scale?: number, compression?: number) => Promise<string>;
  performCopy: () => void;
  performPaste: () => void;
  performDelete: () => void;
  simulatePencilDoubleTap?: () => Promise<boolean>;
  runBenchmark?: (options?: NativeInkBenchmarkOptions) => Promise<NativeInkBenchmarkResult>;
  startBenchmarkRecording?: (options?: NativeInkBenchmarkRecordingOptions) => Promise<boolean>;
  stopBenchmarkRecording?: () => Promise<NativeInkBenchmarkResult>;
}
