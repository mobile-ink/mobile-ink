import type {
  NativeInkBenchmarkDistribution,
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
} from "./types";

const emptyDistribution = (): NativeInkBenchmarkDistribution => ({
  count: 0,
  average: 0,
  p50: 0,
  p95: 0,
  p99: 0,
  max: 0,
});

const round = (value: number) => Math.round(value * 100) / 100;

const weightedAverage = (
  values: NativeInkBenchmarkDistribution[],
  field: keyof Pick<NativeInkBenchmarkDistribution, "average" | "p50" | "p95" | "p99">,
) => {
  const totalCount = values.reduce((sum, value) => sum + value.count, 0);
  if (totalCount === 0) {
    return 0;
  }
  return values.reduce(
    (sum, value) => sum + value[field] * value.count,
    0,
  ) / totalCount;
};

const aggregateDistribution = (
  values: NativeInkBenchmarkDistribution[],
): NativeInkBenchmarkDistribution => {
  const totalCount = values.reduce((sum, value) => sum + value.count, 0);
  if (totalCount === 0) {
    return emptyDistribution();
  }

  return {
    count: totalCount,
    average: round(weightedAverage(values, "average")),
    p50: round(weightedAverage(values, "p50")),
    p95: round(weightedAverage(values, "p95")),
    p99: round(weightedAverage(values, "p99")),
    max: round(Math.max(...values.map((value) => value.max))),
  };
};

const getDistribution = (
  distribution: NativeInkBenchmarkDistribution | undefined,
) => distribution ?? emptyDistribution();

const aggregateFpsAverage = (results: NativeInkBenchmarkResult[]) => {
  const totalDurationMs = results.reduce(
    (sum, result) => sum + result.durationMs,
    0,
  );
  if (totalDurationMs === 0) {
    return 0;
  }

  return round(
    results.reduce(
      (sum, result) => sum + result.fpsAverage * result.durationMs,
      0,
    ) / totalDurationMs,
  );
};

export const aggregateNotebookBenchmarkResults = (
  results: NativeInkBenchmarkResult[],
  options: NativeInkBenchmarkRecordingOptions = {},
): NativeInkBenchmarkResult => {
  if (results.length === 0) {
    throw new Error("Benchmark recording is not running.");
  }

  if (results.length === 1) {
    return {
      ...results[0],
      scope: "notebook",
      recorderCount: 1,
      recorderResults: results,
      frameThroughputFps: results[0].fpsAverage,
    };
  }

  const durationMs = Math.max(...results.map((result) => result.durationMs));
  const renderFrameCount = results.reduce(
    (sum, result) => sum + result.renderFrameCount,
    0,
  );
  const frameThroughputFps = round(
    renderFrameCount / Math.max(0.001, durationMs / 1000),
  );
  const startBytes = Math.min(...results.map((result) => result.memory.startBytes));
  const endBytes = Math.max(...results.map((result) => result.memory.endBytes));
  const peakBytes = Math.max(...results.map((result) => result.memory.peakBytes));
  const lowBytes = Math.min(...results.map((result) => result.memory.lowBytes));

  return {
    sessionId: `notebook-${Date.now().toString(36)}`,
    scenario: options.scenario ?? results[0].scenario,
    requestedBackend: options.backend ?? results[0].requestedBackend,
    scope: "notebook",
    recorderCount: results.length,
    recorderResults: results,
    durationMs: round(durationMs),
    fpsAverage: aggregateFpsAverage(results),
    frameThroughputFps,
    renderFrameCount,
    cpuFrameCount: results.reduce((sum, result) => sum + result.cpuFrameCount, 0),
    ganeshFrameCount: results.reduce((sum, result) => sum + result.ganeshFrameCount, 0),
    ganeshFallbackFrameCount: results.reduce(
      (sum, result) => sum + result.ganeshFallbackFrameCount,
      0,
    ),
    droppedFrameCount: results.reduce((sum, result) => sum + result.droppedFrameCount, 0),
    inputEventCount: results.reduce((sum, result) => sum + result.inputEventCount, 0),
    syntheticStrokeCount: results.reduce(
      (sum, result) => sum + result.syntheticStrokeCount,
      0,
    ),
    syntheticPointCount: results.reduce(
      (sum, result) => sum + result.syntheticPointCount,
      0,
    ),
    renderMs: aggregateDistribution(results.map((result) => result.renderMs)),
    frameIntervalMs: aggregateDistribution(results.map((result) => result.frameIntervalMs)),
    presentationPauseMs: aggregateDistribution(
      results.map((result) => getDistribution(result.presentationPauseMs)),
    ),
    presentationPauseCount: results.reduce(
      (sum, result) => sum + (result.presentationPauseCount ?? 0),
      0,
    ),
    presentationPauseTotalMs: round(results.reduce(
      (sum, result) => sum + (result.presentationPauseTotalMs ?? 0),
      0,
    )),
    inputToPresentLatencyMs: aggregateDistribution(
      results.map((result) => result.inputToPresentLatencyMs),
    ),
    memory: {
      startBytes,
      endBytes,
      peakBytes,
      lowBytes,
      deltaBytes: endBytes - startBytes,
      peakDeltaBytes: peakBytes - startBytes,
    },
  };
};
