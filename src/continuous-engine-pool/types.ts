import type {
  NativeInkBenchmarkOptions,
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
  NativeInkRenderBackend,
} from "../benchmark";
import type {
  NativeInkCanvasProps,
  NativeInkCanvasRef,
} from "../NativeInkCanvas";
import type { NativeSelectionBounds, NotebookPage, ToolType } from "../types";

export type ContinuousEnginePoolAssignment = {
  page: NotebookPage;
  pageIndex: number;
};

export type ContinuousEnginePoolToolState = {
  toolType: ToolType;
  width: number;
  color: string;
  eraserMode: string;
};

export type ContinuousEnginePoolSlotRef = {
  getBase64Data: () => Promise<string>;
  isLoaded: () => boolean;
  setTool: (
    toolType: string,
    width: number,
    color: string,
    eraserMode: string,
  ) => void;
  clear: () => void;
  undo: () => void;
  redo: () => void;
  performCopy: () => void;
  performPaste: () => void;
  performDelete: () => void;
  runBenchmark?: (options?: NativeInkBenchmarkOptions) => Promise<NativeInkBenchmarkResult>;
  startBenchmarkRecording?: (options?: NativeInkBenchmarkRecordingOptions) => Promise<boolean>;
  stopBenchmarkRecording?: () => Promise<NativeInkBenchmarkResult>;
};

export type ContinuousEnginePoolRef = {
  assignPages: (assignments: ContinuousEnginePoolAssignment[]) => Promise<void>;
  applyToolState: (toolState: ContinuousEnginePoolToolState) => void;
  startBenchmarkRecording: (options?: NativeInkBenchmarkRecordingOptions) => Promise<boolean>;
  stopBenchmarkRecording: () => Promise<NativeInkBenchmarkResult[]>;
  release: () => Promise<void>;
};

export type ContinuousEnginePoolProps = {
  canvasHeight: number;
  backgroundType: string;
  renderBackend?: NativeInkRenderBackend;
  pdfBackgroundBaseUri: string | undefined;
  fingerDrawingEnabled: boolean;
  getToolState: () => ContinuousEnginePoolToolState;
  onCanvasReady: () => void;
  onAssignmentReady?: (assignmentKey: string) => void;
  onPageAssignmentReady?: (
    pageId: string,
    pageIndex: number,
    assignmentKey: string,
  ) => void;
  onPerPageDrawingChange: (pageId: string) => void;
  onPerPageSelectionChange?: (
    pageId: string,
    count: number,
    bounds: NativeSelectionBounds | null,
  ) => void;
  onDrawingBegin?: () => void;
  onPencilDoubleTap?: NativeInkCanvasProps["onPencilDoubleTap"];
  registerPerPageSlot: (
    pageId: string,
    ref: ContinuousEnginePoolSlotRef | null,
    sourceRef?: ContinuousEnginePoolSlotRef,
  ) => void;
  shouldCaptureBeforeReassign: (pageId: string) => boolean;
  onSlotCaptureBeforeUnmount: (pageId: string, data: string) => void;
};

export type PooledCanvasSlotAssignOptions = {
  assignment: ContinuousEnginePoolAssignment | null;
  assignmentKey: string;
  canvasHeight: number;
  backgroundType: string;
  pdfBackgroundBaseUri?: string;
};

export type PooledCanvasSlotHandle = {
  assign: (options: PooledCanvasSlotAssignOptions) => Promise<void>;
  applyToolState: (toolState: ContinuousEnginePoolToolState) => void;
  startBenchmarkRecording: (options?: NativeInkBenchmarkRecordingOptions) => Promise<boolean>;
  stopBenchmarkRecording: () => Promise<NativeInkBenchmarkResult | null>;
  release: () => Promise<void>;
};

export type PooledCanvasSlotProps = {
  poolIndex: number;
  canvasHeight: number;
  backgroundType: string;
  renderBackend?: NativeInkRenderBackend;
  pdfBackgroundBaseUri?: string;
  drawingPolicy: "anyinput" | "pencilonly";
  getToolState: () => ContinuousEnginePoolToolState;
  onCanvasReady?: () => void;
  onSlotLoaded: (
    poolIndex: number,
    assignmentKey: string,
    pageId: string,
    pageIndex: number,
  ) => void;
  onDrawingChange: (pageId: string) => void;
  onSelectionChange?: (
    pageId: string,
    count: number,
    bounds: NativeSelectionBounds | null,
  ) => void;
  onDrawingBegin?: () => void;
  onPencilDoubleTap?: NativeInkCanvasProps["onPencilDoubleTap"];
  registerRef: (
    pageId: string,
    ref: ContinuousEnginePoolSlotRef | null,
    sourceRef?: ContinuousEnginePoolSlotRef,
  ) => void;
  shouldCaptureBeforeReassign: (pageId: string) => boolean;
  onCaptureBeforeReassign: (pageId: string, data: string) => void;
};

export type NativeCanvasRef = NativeInkCanvasRef;
