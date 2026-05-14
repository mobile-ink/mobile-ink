import type { StyleProp, ViewStyle } from "react-native";
import type { ContinuousEnginePoolToolState } from "../ContinuousEnginePool";
import type {
  NativeInkBenchmarkOptions,
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
  NativeInkRenderBackend,
} from "../benchmark";
import type {
  NativeInkPencilDoubleTapEvent,
  NativeSelectionBounds,
  NotebookPage,
  SerializedNotebookData,
} from "../types";

export type InfiniteInkViewportTransform = {
  scale: number;
  translateX: number;
  translateY: number;
  containerWidth: number;
  containerHeight: number;
};

export type InfiniteInkCanvasRef = {
  getNotebookData: () => Promise<SerializedNotebookData>;
  loadNotebookData: (data: SerializedNotebookData | string) => Promise<void>;
  addPage: () => Promise<void>;
  undo: () => void;
  redo: () => void;
  clearCurrentPage: () => void;
  setTool: (toolState: ContinuousEnginePoolToolState) => void;
  resetViewport: (animated?: boolean) => void;
  getCurrentPageIndex: () => number;
  scrollToPage: (pageIndex: number, animated?: boolean) => void;
  runBenchmark?: (options?: NativeInkBenchmarkOptions) => Promise<NativeInkBenchmarkResult>;
  startBenchmarkRecording?: (options?: NativeInkBenchmarkRecordingOptions) => Promise<boolean>;
  stopBenchmarkRecording?: () => Promise<NativeInkBenchmarkResult>;
};

export type InfiniteInkCanvasProps = {
  style?: StyleProp<ViewStyle>;
  initialData?: SerializedNotebookData | string;
  initialPageCount?: number;
  pageWidth?: number;
  pageHeight?: number;
  backgroundType?: string;
  renderBackend?: NativeInkRenderBackend;
  pdfBackgroundBaseUri?: string;
  fingerDrawingEnabled?: boolean;
  toolState: ContinuousEnginePoolToolState;
  minScale?: number;
  maxScale?: number;
  contentPadding?: number;
  showPageLabels?: boolean;
  onReady?: () => void;
  onDrawingChange?: (pageId: string) => void;
  onSelectionChange?: (
    pageId: string,
    count: number,
    bounds: NativeSelectionBounds | null,
  ) => void;
  onCurrentPageChange?: (pageIndex: number) => void;
  onPagesChange?: (pages: NotebookPage[]) => void;
  onMotionStateChange?: (isMoving: boolean) => void;
  onPencilDoubleTap?: (event: NativeInkPencilDoubleTapEvent) => void;
};
