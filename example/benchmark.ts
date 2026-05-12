import type {
  ContinuousEnginePoolToolState,
  InfiniteInkCanvasRef,
  NativeInkBenchmarkOptions,
  NativeInkBenchmarkResult,
  NativeInkBenchmarkViewportMetrics,
  NativeInkRenderBackend,
} from "@mathnotes/mobile-ink";

export type ToolConfig = ContinuousEnginePoolToolState & {
  label: string;
};

export type BenchmarkScenario = {
  id: string;
  label: string;
  tool: ToolConfig;
  options: NativeInkBenchmarkOptions & Required<Pick<
    NativeInkBenchmarkOptions,
    "strokeCount" | "pointsPerStroke" | "pointIntervalMs" | "strokeGapMs" | "settleMs" | "strokeWidth"
  >>;
};

export type FrameBenchmarkDistribution = {
  count: number;
  average: number;
  p50: number;
  p95: number;
  p99: number;
  max: number;
};

export type ScrollBenchmarkResult = {
  scenario: "scroll";
  durationMs: number;
  pageCount: number;
  fpsAverage: number;
  droppedFrameCount: number;
  frameIntervalMs: FrameBenchmarkDistribution;
};

export type BenchmarkSuiteStep =
  | {
      type: "native";
      id: string;
      label: string;
      pageIndex: number;
      tool: string;
      result: NativeInkBenchmarkResult;
    }
  | {
      type: "scroll";
      id: string;
      label: string;
      result: ScrollBenchmarkResult;
    };

export type BenchmarkSuiteResult = {
  sessionId: string;
  scenario: "suite";
  requestedBackend: NativeInkRenderBackend;
  durationMs: number;
  pageCount: number;
  steps: BenchmarkSuiteStep[];
  summary: {
    nativeStepCount: number;
    renderP95Average: number;
    latencyP95Average: number;
    droppedFrameCount: number;
    scrollFpsAverage: number;
    scrollDroppedFrameCount: number;
  };
};

export type BenchmarkDisplayResult = NativeInkBenchmarkResult | BenchmarkSuiteResult;

export const tools: ToolConfig[] = [
  { label: "Pen", toolType: "pen", width: 3, color: "#111111", eraserMode: "pixel" },
  { label: "Highlighter", toolType: "highlighter", width: 18, color: "#FFE066", eraserMode: "pixel" },
  { label: "Crayon", toolType: "crayon", width: 9, color: "#1E6BFF", eraserMode: "pixel" },
  { label: "Calligraphy", toolType: "calligraphy", width: 7, color: "#111111", eraserMode: "pixel" },
  { label: "Eraser", toolType: "eraser", width: 64, color: "#FFFFFF", eraserMode: "pixel" },
  { label: "Select", toolType: "select", width: 3, color: "#111111", eraserMode: "pixel" },
];

export const benchmarkScenarios: BenchmarkScenario[] = [
  {
    id: "smoke",
    label: "Smoke",
    tool: tools[0],
    options: {
      workload: "draw",
      toolType: "pen",
      strokeCount: 40,
      pointsPerStroke: 24,
      pointIntervalMs: 8,
      strokeGapMs: 12,
      settleMs: 500,
      strokeWidth: 3,
    },
  },
  {
    id: "writing",
    label: "Writing",
    tool: tools[0],
    options: {
      workload: "draw",
      toolType: "pen",
      strokeCount: 140,
      pointsPerStroke: 28,
      pointIntervalMs: 8,
      strokeGapMs: 18,
      settleMs: 700,
      strokeWidth: 3,
    },
  },
  {
    id: "dense",
    label: "Dense",
    tool: tools[0],
    options: {
      workload: "draw",
      toolType: "pen",
      strokeCount: 320,
      pointsPerStroke: 20,
      pointIntervalMs: 4,
      strokeGapMs: 6,
      settleMs: 900,
      strokeWidth: 3,
    },
  },
  {
    id: "endurance",
    label: "Endurance",
    tool: tools[0],
    options: {
      workload: "draw",
      toolType: "pen",
      strokeCount: 600,
      pointsPerStroke: 24,
      pointIntervalMs: 6,
      strokeGapMs: 12,
      settleMs: 1_000,
      strokeWidth: 3,
    },
  },
  {
    id: "eraser",
    label: "Eraser",
    tool: tools[4],
    options: {
      workload: "erase",
      toolType: "eraser",
      color: "#FFFFFF",
      eraserMode: "pixel",
      strokeCount: 140,
      pointsPerStroke: 24,
      pointIntervalMs: 6,
      strokeGapMs: 10,
      settleMs: 700,
      strokeWidth: 54,
      seedStrokeCount: 80,
    },
  },
  {
    id: "selection-move",
    label: "Move",
    tool: tools[5],
    options: {
      workload: "selectionMove",
      toolType: "select",
      strokeCount: 80,
      pointsPerStroke: 24,
      pointIntervalMs: 6,
      strokeGapMs: 0,
      settleMs: 700,
      strokeWidth: 3,
      seedStrokeCount: 70,
      moveStepCount: 90,
      moveDeltaX: 1.5,
      moveDeltaY: 0.75,
    },
  },
];

export const benchmarkSuitePageCount = 12;

export const benchmarkSuiteScenarios: BenchmarkScenario[] = [
  {
    id: "suite-pen",
    label: "Pen",
    tool: tools[0],
    options: {
      workload: "draw",
      toolType: "pen",
      color: "#111111",
      strokeCount: 44,
      pointsPerStroke: 18,
      pointIntervalMs: 5,
      strokeGapMs: 8,
      settleMs: 250,
      strokeWidth: 3,
    },
  },
  {
    id: "suite-highlighter",
    label: "Highlighter",
    tool: tools[1],
    options: {
      workload: "draw",
      toolType: "highlighter",
      color: "#FFE066",
      strokeCount: 36,
      pointsPerStroke: 18,
      pointIntervalMs: 5,
      strokeGapMs: 8,
      settleMs: 250,
      strokeWidth: 18,
    },
  },
  {
    id: "suite-crayon",
    label: "Crayon",
    tool: tools[2],
    options: {
      workload: "draw",
      toolType: "crayon",
      color: "#1E6BFF",
      strokeCount: 40,
      pointsPerStroke: 18,
      pointIntervalMs: 5,
      strokeGapMs: 8,
      settleMs: 250,
      strokeWidth: 9,
    },
  },
  {
    id: "suite-calligraphy",
    label: "Calligraphy",
    tool: tools[3],
    options: {
      workload: "draw",
      toolType: "calligraphy",
      color: "#111111",
      strokeCount: 40,
      pointsPerStroke: 18,
      pointIntervalMs: 5,
      strokeGapMs: 8,
      settleMs: 250,
      strokeWidth: 7,
    },
  },
  {
    id: "suite-eraser",
    label: "Eraser",
    tool: tools[4],
    options: {
      workload: "erase",
      toolType: "eraser",
      color: "#FFFFFF",
      eraserMode: "pixel",
      strokeCount: 40,
      pointsPerStroke: 18,
      pointIntervalMs: 5,
      strokeGapMs: 8,
      settleMs: 250,
      strokeWidth: 54,
      seedStrokeCount: 44,
    },
  },
  {
    id: "suite-selection",
    label: "Selection",
    tool: tools[5],
    options: {
      workload: "selectionMove",
      toolType: "select",
      strokeCount: 36,
      pointsPerStroke: 18,
      pointIntervalMs: 5,
      strokeGapMs: 0,
      settleMs: 250,
      strokeWidth: 3,
      seedStrokeCount: 42,
      moveStepCount: 56,
      moveDeltaX: 1.5,
      moveDeltaY: 0.75,
    },
  },
];

export const formatNumber = (value: number, fractionDigits = 1) => {
  if (!Number.isFinite(value)) {
    return "0";
  }
  return value.toLocaleString(undefined, {
    maximumFractionDigits: fractionDigits,
    minimumFractionDigits: fractionDigits,
  });
};

export const formatBytes = (bytes: number) => {
  const absBytes = Math.abs(bytes);
  if (absBytes < 1024) {
    return `${bytes} B`;
  }
  if (absBytes < 1024 * 1024) {
    return `${formatNumber(bytes / 1024, 1)} KB`;
  }
  return `${formatNumber(bytes / (1024 * 1024), 1)} MB`;
};

export const wait = (durationMs: number) =>
  new Promise<void>((resolve) => {
    setTimeout(resolve, durationMs);
  });

export const summarizeDistribution = (values: number[]): FrameBenchmarkDistribution => {
  if (values.length === 0) {
    return {
      count: 0,
      average: 0,
      p50: 0,
      p95: 0,
      p99: 0,
      max: 0,
    };
  }

  const sorted = [...values].sort((left, right) => left - right);
  const average = values.reduce((sum, value) => sum + value, 0) / values.length;
  const percentile = (target: number) => {
    const boundedTarget = Math.max(0, Math.min(1, target));
    const rawIndex = boundedTarget * (sorted.length - 1);
    const lowerIndex = Math.floor(rawIndex);
    const upperIndex = Math.ceil(rawIndex);
    if (lowerIndex === upperIndex) {
      return sorted[lowerIndex];
    }
    const fraction = rawIndex - lowerIndex;
    return sorted[lowerIndex] * (1 - fraction) + sorted[upperIndex] * fraction;
  };

  return {
    count: values.length,
    average: Number(average.toFixed(2)),
    p50: Number(percentile(0.5).toFixed(2)),
    p95: Number(percentile(0.95).toFixed(2)),
    p99: Number(percentile(0.99).toFixed(2)),
    max: Number((sorted[sorted.length - 1] ?? 0).toFixed(2)),
  };
};

export type ViewportFrameSampler = {
  stop: () => NativeInkBenchmarkViewportMetrics;
};

export const summarizeViewportFrames = (
  frameTimes: number[],
  durationMs: number,
): NativeInkBenchmarkViewportMetrics => {
  const intervals = frameTimes.slice(1).map((timestamp, index) => (
    timestamp - frameTimes[index]
  ));
  const frameIntervalMs = summarizeDistribution(intervals);
  const frameBudgetMs = 1000 / 60;
  const droppedFrameCount = intervals.reduce((sum, intervalMs) => {
    if (intervalMs <= frameBudgetMs * 1.5) {
      return sum;
    }
    return sum + Math.max(1, Math.floor(intervalMs / frameBudgetMs) - 1);
  }, 0);

  return {
    durationMs,
    fpsAverage: Number(((frameTimes.length / Math.max(1, durationMs)) * 1000).toFixed(2)),
    droppedFrameCount,
    frameIntervalMs,
  };
};

export const startViewportFrameSampler = (): ViewportFrameSampler => {
  const frameTimes: number[] = [];
  const startedAt = Date.now();
  let isSampling = true;
  let animationFrameId: number | null = null;

  const sample = (timestamp: number) => {
    frameTimes.push(timestamp);
    if (isSampling) {
      animationFrameId = requestAnimationFrame(sample);
    }
  };

  animationFrameId = requestAnimationFrame(sample);

  return {
    stop: () => {
      isSampling = false;
      if (animationFrameId !== null) {
        cancelAnimationFrame(animationFrameId);
      }
      return summarizeViewportFrames(frameTimes, Date.now() - startedAt);
    },
  };
};

export const measureScrollSweep = async (
  canvas: InfiniteInkCanvasRef,
  pageCount: number,
): Promise<ScrollBenchmarkResult> => {
  const sampler = startViewportFrameSampler();
  const startedAt = Date.now();

  for (let pageIndex = 0; pageIndex < pageCount; pageIndex += 1) {
    canvas.scrollToPage(pageIndex, true);
    await wait(220);
  }
  for (let pageIndex = pageCount - 2; pageIndex >= 0; pageIndex -= 1) {
    canvas.scrollToPage(pageIndex, true);
    await wait(220);
  }

  await wait(350);

  const durationMs = Date.now() - startedAt;
  const viewport = sampler.stop();

  return {
    scenario: "scroll",
    durationMs,
    pageCount,
    fpsAverage: viewport.fpsAverage,
    droppedFrameCount: viewport.droppedFrameCount,
    frameIntervalMs: viewport.frameIntervalMs,
  };
};

export const isSuiteResult = (
  result: BenchmarkDisplayResult | null,
): result is BenchmarkSuiteResult => (
  !!result && result.scenario === "suite"
);
