import type { ToolType } from "../types";

export type NativeInkRenderBackend = "cpu" | "ganesh";

export const DEFAULT_NATIVE_INK_RENDER_BACKEND: NativeInkRenderBackend = "ganesh";

export type NativeInkBenchmarkWorkload = "draw" | "erase" | "selectionMove";

export type NativeInkBenchmarkOptions = {
  scenario?: string;
  backend?: NativeInkRenderBackend;
  workload?: NativeInkBenchmarkWorkload;
  toolType?: ToolType;
  color?: string;
  eraserMode?: string;
  clearCanvas?: boolean;
  strokeCount?: number;
  pointsPerStroke?: number;
  pointIntervalMs?: number;
  strokeGapMs?: number;
  settleMs?: number;
  strokeWidth?: number;
  seedStrokeCount?: number;
  moveStepCount?: number;
  moveDeltaX?: number;
  moveDeltaY?: number;
};

export type NativeInkBenchmarkRecordingOptions = {
  scenario?: string;
  backend?: NativeInkRenderBackend;
  clearCanvas?: boolean;
};

export type NativeInkBenchmarkDistribution = {
  count: number;
  average: number;
  p50: number;
  p95: number;
  p99: number;
  max: number;
};

export type NativeInkBenchmarkViewportMetrics = {
  durationMs: number;
  fpsAverage: number;
  droppedFrameCount: number;
  frameIntervalMs: NativeInkBenchmarkDistribution;
};

export type NativeInkBenchmarkResult = {
  sessionId: string;
  scenario: string;
  requestedBackend: NativeInkRenderBackend;
  scope?: "page" | "notebook";
  recorderCount?: number;
  recorderResults?: NativeInkBenchmarkResult[];
  frameThroughputFps?: number;
  viewport?: NativeInkBenchmarkViewportMetrics;
  durationMs: number;
  fpsAverage: number;
  renderFrameCount: number;
  cpuFrameCount: number;
  ganeshFrameCount: number;
  ganeshFallbackFrameCount: number;
  droppedFrameCount: number;
  inputEventCount: number;
  syntheticStrokeCount: number;
  syntheticPointCount: number;
  renderMs: NativeInkBenchmarkDistribution;
  frameIntervalMs: NativeInkBenchmarkDistribution;
  presentationPauseMs?: NativeInkBenchmarkDistribution;
  presentationPauseCount?: number;
  presentationPauseTotalMs?: number;
  inputToPresentLatencyMs: NativeInkBenchmarkDistribution;
  memory: {
    startBytes: number;
    endBytes: number;
    peakBytes: number;
    lowBytes: number;
    deltaBytes: number;
    peakDeltaBytes: number;
  };
};
