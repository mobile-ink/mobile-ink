import React, { useEffect, useMemo, useRef, useState } from "react";
import {
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from "react-native";
import type {
  InfiniteInkCanvasRef,
  NativeInkRenderBackend,
} from "@mathnotes/mobile-ink";
import {
  benchmarkScenarios,
  benchmarkSuitePageCount,
  benchmarkSuiteScenarios,
  formatBytes,
  formatNumber,
  isSuiteResult,
  measureScrollSweep,
  startViewportFrameSampler,
  tools,
  wait,
  type BenchmarkDisplayResult,
  type BenchmarkSuiteStep,
  type ToolConfig,
  type ViewportFrameSampler,
} from "./benchmark";

export function BenchmarkScreen({
  backend,
  canvasRef,
  activeTool,
  applyTool,
  setBackend,
}: {
  backend: NativeInkRenderBackend;
  canvasRef: React.RefObject<InfiniteInkCanvasRef | null>;
  activeTool: ToolConfig;
  applyTool: (tool: ToolConfig) => void;
  setBackend: (backend: NativeInkRenderBackend) => void;
}) {
  const [scenarioId, setScenarioId] = useState(benchmarkScenarios[1].id);
  const [activeRun, setActiveRun] = useState<"replay" | "manual" | "suite" | null>(null);
  const [startedAt, setStartedAt] = useState<number | null>(null);
  const [elapsedMs, setElapsedMs] = useState(0);
  const [result, setResult] = useState<BenchmarkDisplayResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [suiteProgress, setSuiteProgress] = useState<string | null>(null);
  const manualViewportSamplerRef = useRef<ViewportFrameSampler | null>(null);

  const isRunning = activeRun !== null;
  const scenario = benchmarkScenarios.find((item) => item.id === scenarioId) ?? benchmarkScenarios[0];

  useEffect(() => {
    if (!isRunning || startedAt === null) {
      return undefined;
    }

    const timer = setInterval(() => {
      setElapsedMs(Date.now() - startedAt);
    }, 250);

    return () => clearInterval(timer);
  }, [isRunning, startedAt]);

  useEffect(() => () => {
    manualViewportSamplerRef.current?.stop();
    manualViewportSamplerRef.current = null;
  }, []);

  const metrics = useMemo(() => {
    if (!result) {
      return [];
    }

    if (isSuiteResult(result)) {
      return [
        ["Suite steps", result.steps.length.toLocaleString()],
        ["Pages", result.pageCount.toLocaleString()],
        ["Duration", `${formatNumber(result.durationMs / 1000, 1)} s`],
        ["Native steps", result.summary.nativeStepCount.toLocaleString()],
        ["Render p95 avg", `${formatNumber(result.summary.renderP95Average, 2)} ms`],
        ["Latency p95 avg", `${formatNumber(result.summary.latencyP95Average, 2)} ms`],
        ["Dropped frames", result.summary.droppedFrameCount.toLocaleString()],
        ["Scroll FPS", formatNumber(result.summary.scrollFpsAverage, 1)],
        ["Scroll dropped", result.summary.scrollDroppedFrameCount.toLocaleString()],
      ];
    }

    const rows = [
      [result.scope === "notebook" ? "Native FPS avg" : "FPS avg", formatNumber(result.fpsAverage, 1)],
      ["Frame p95", `${formatNumber(result.frameIntervalMs.p95, 2)} ms`],
      ["Frame p99", `${formatNumber(result.frameIntervalMs.p99, 2)} ms`],
      ["Render p95", `${formatNumber(result.renderMs.p95, 2)} ms`],
      ["Latency p95", `${formatNumber(result.inputToPresentLatencyMs.p95, 2)} ms`],
      ["Latency p99", `${formatNumber(result.inputToPresentLatencyMs.p99, 2)} ms`],
      ["Dropped frames", result.droppedFrameCount.toLocaleString()],
      ["Frames", result.renderFrameCount.toLocaleString()],
      ["Ganesh frames", result.ganeshFrameCount.toLocaleString()],
      ["CPU frames", result.cpuFrameCount.toLocaleString()],
      ["Fallback frames", result.ganeshFallbackFrameCount.toLocaleString()],
      ["Memory delta", formatBytes(result.memory.deltaBytes)],
      ["Peak memory delta", formatBytes(result.memory.peakDeltaBytes)],
      ["Peak memory", formatBytes(result.memory.peakBytes)],
    ];

    if (typeof result.frameThroughputFps === "number") {
      rows.splice(1, 0, ["Frame throughput", formatNumber(result.frameThroughputFps, 1)]);
    }

    if (result.viewport) {
      rows.splice(
        1,
        0,
        ["Viewport FPS", formatNumber(result.viewport.fpsAverage, 1)],
        ["Viewport p95", `${formatNumber(result.viewport.frameIntervalMs.p95, 2)} ms`],
        ["Viewport dropped", result.viewport.droppedFrameCount.toLocaleString()],
      );
    }

    if (result.presentationPauseMs && result.presentationPauseMs.count > 0) {
      rows.push(
        ["Presentation pauses", (result.presentationPauseCount ?? result.presentationPauseMs.count).toLocaleString()],
        ["Pause p95", `${formatNumber(result.presentationPauseMs.p95, 2)} ms`],
        ["Pause max", `${formatNumber(result.presentationPauseMs.max, 2)} ms`],
        ["Pause total", `${formatNumber((result.presentationPauseTotalMs ?? 0) / 1000, 1)} s`],
      );
    }

    return rows;
  }, [result]);

  const runBenchmark = async () => {
    if (isRunning) {
      return;
    }

    setError(null);
    setResult(null);
    setElapsedMs(0);
    setStartedAt(Date.now());
    setActiveRun("replay");
    setSuiteProgress(null);

    try {
      applyTool(scenario.tool);
      const nextResult = await canvasRef.current?.runBenchmark?.({
        ...scenario.options,
        scenario: scenario.id,
        backend,
        color: scenario.options.color ?? scenario.tool.color,
        eraserMode: scenario.options.eraserMode ?? scenario.tool.eraserMode,
        toolType: scenario.options.toolType ?? scenario.tool.toolType,
      });

      if (!nextResult) {
        throw new Error("Benchmark runner is unavailable in this native build.");
      }

      setResult(nextResult);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "Benchmark failed.");
    } finally {
      setActiveRun(null);
      setStartedAt(null);
    }
  };

  const runBenchmarkSuite = async () => {
    if (isRunning) {
      return;
    }

    const canvas = canvasRef.current;
    if (!canvas?.runBenchmark) {
      setError("Benchmark runner is unavailable in this native build.");
      return;
    }

    setError(null);
    setResult(null);
    setElapsedMs(0);
    setStartedAt(Date.now());
    setActiveRun("suite");
    setSuiteProgress("page 1");

    const suiteStartedAt = Date.now();
    const steps: BenchmarkSuiteStep[] = [];

    try {
      canvas.resetViewport(false);

      for (let pageIndex = 0; pageIndex < benchmarkSuitePageCount; pageIndex += 1) {
        const suiteScenario =
          benchmarkSuiteScenarios[pageIndex % benchmarkSuiteScenarios.length];

        if (pageIndex > 0) {
          await canvas.addPage();
          await wait(450);
        }

        canvas.scrollToPage(pageIndex, false);
        await wait(350);
        applyTool(suiteScenario.tool);
        setSuiteProgress(`page ${pageIndex + 1}/${benchmarkSuitePageCount} ${suiteScenario.label}`);

        const nextResult = await canvas.runBenchmark({
          ...suiteScenario.options,
          backend,
          scenario: `${suiteScenario.id}-page-${pageIndex + 1}`,
          color: suiteScenario.options.color ?? suiteScenario.tool.color,
          eraserMode: suiteScenario.options.eraserMode ?? suiteScenario.tool.eraserMode,
          toolType: suiteScenario.options.toolType ?? suiteScenario.tool.toolType,
        });

        steps.push({
          type: "native",
          id: `${suiteScenario.id}-page-${pageIndex + 1}`,
          label: `${suiteScenario.label} p${pageIndex + 1}`,
          pageIndex,
          tool: suiteScenario.tool.label,
          result: nextResult,
        });
      }

      setSuiteProgress("scroll sweep");
      const scrollResult = await measureScrollSweep(canvas, benchmarkSuitePageCount);
      steps.push({
        type: "scroll",
        id: "scroll-sweep",
        label: "Scroll sweep",
        result: scrollResult,
      });

      const nativeResults = steps.flatMap((step) => (
        step.type === "native" ? [step.result] : []
      ));
      const renderP95Average = nativeResults.reduce(
        (sum, nextResult) => sum + nextResult.renderMs.p95,
        0,
      ) / Math.max(1, nativeResults.length);
      const latencyP95Average = nativeResults.reduce(
        (sum, nextResult) => sum + nextResult.inputToPresentLatencyMs.p95,
        0,
      ) / Math.max(1, nativeResults.length);
      const droppedFrameCount = nativeResults.reduce(
        (sum, nextResult) => sum + nextResult.droppedFrameCount,
        0,
      );

      setResult({
        sessionId: `suite-${Date.now().toString(36)}`,
        scenario: "suite",
        requestedBackend: backend,
        durationMs: Date.now() - suiteStartedAt,
        pageCount: benchmarkSuitePageCount,
        steps,
        summary: {
          nativeStepCount: nativeResults.length,
          renderP95Average: Number(renderP95Average.toFixed(2)),
          latencyP95Average: Number(latencyP95Average.toFixed(2)),
          droppedFrameCount,
          scrollFpsAverage: scrollResult.fpsAverage,
          scrollDroppedFrameCount: scrollResult.droppedFrameCount,
        },
      });
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "Benchmark suite failed.");
    } finally {
      setActiveRun(null);
      setStartedAt(null);
      setSuiteProgress(null);
    }
  };

  const toggleManualRecording = async () => {
    if (activeRun && activeRun !== "manual") {
      return;
    }

    if (activeRun === "manual") {
      const viewport = manualViewportSamplerRef.current?.stop();
      manualViewportSamplerRef.current = null;
      try {
        const nextResult = await canvasRef.current?.stopBenchmarkRecording?.();
        if (!nextResult) {
          throw new Error("Benchmark recorder is unavailable in this native build.");
        }
        setResult(viewport ? { ...nextResult, viewport } : nextResult);
      } catch (caught) {
        setError(caught instanceof Error ? caught.message : "Benchmark recording failed.");
      } finally {
        setActiveRun(null);
        setStartedAt(null);
      }
      return;
    }

    setError(null);
    setResult(null);
    setElapsedMs(0);
    setSuiteProgress(null);

    try {
      const didStart = await canvasRef.current?.startBenchmarkRecording?.({
        scenario: "manual",
        backend,
      });
      if (!didStart) {
        throw new Error("Benchmark recorder is unavailable in this native build.");
      }
      manualViewportSamplerRef.current = startViewportFrameSampler();
      setStartedAt(Date.now());
      setActiveRun("manual");
    } catch (caught) {
      manualViewportSamplerRef.current?.stop();
      manualViewportSamplerRef.current = null;
      setError(caught instanceof Error ? caught.message : "Benchmark recording failed.");
      setStartedAt(null);
      setActiveRun(null);
    }
  };

  const logResult = () => {
    if (result) {
      console.log("[MobileInkBenchmark]", JSON.stringify(result, null, 2));
    }
  };

  return (
    <View style={styles.benchmarkRoot} pointerEvents="box-none">
      <View style={styles.benchmarkToolbar}>
        <View style={styles.segmentGroup}>
          {(["ganesh", "cpu"] as NativeInkRenderBackend[]).map((item) => (
            <TouchableOpacity
              key={item}
              style={[styles.button, backend === item && styles.activeButton]}
              onPress={() => setBackend(item)}
              disabled={isRunning}
            >
              <Text style={styles.buttonText}>{item === "ganesh" ? "Ganesh" : "CPU"}</Text>
            </TouchableOpacity>
          ))}
        </View>

        <View style={styles.segmentGroup}>
          {benchmarkScenarios.map((item) => (
            <TouchableOpacity
              key={item.id}
              style={[styles.button, scenarioId === item.id && styles.activeButton]}
              onPress={() => setScenarioId(item.id)}
              disabled={isRunning}
            >
              <Text style={styles.buttonText}>{item.label}</Text>
            </TouchableOpacity>
          ))}
        </View>

        <View style={styles.segmentGroup}>
          {tools.map((tool) => (
            <TouchableOpacity
              key={tool.label}
              style={[styles.button, activeTool.label === tool.label && styles.activeButton]}
              onPress={() => applyTool(tool)}
              disabled={activeRun === "replay" || activeRun === "suite"}
            >
              <Text style={styles.buttonText}>{tool.label}</Text>
            </TouchableOpacity>
          ))}
        </View>

        <TouchableOpacity
          style={[styles.button, styles.primaryButton, isRunning && styles.disabledButton]}
          onPress={runBenchmark}
          disabled={isRunning}
        >
          <Text style={styles.primaryButtonText}>{activeRun === "replay" ? "Running" : "Run"}</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[styles.button, activeRun === "suite" && styles.disabledButton]}
          onPress={runBenchmarkSuite}
          disabled={isRunning}
        >
          <Text style={styles.buttonText}>{activeRun === "suite" ? "Suite" : "Run Suite"}</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[
            styles.button,
            activeRun === "manual" && styles.stopButton,
            (activeRun === "replay" || activeRun === "suite") && styles.disabledButton,
          ]}
          onPress={toggleManualRecording}
          disabled={activeRun === "replay" || activeRun === "suite"}
        >
          <Text style={[styles.buttonText, activeRun === "manual" && styles.stopButtonText]}>
            {activeRun === "manual" ? "Stop" : "Record Manual"}
          </Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={styles.button}
          onPress={() => canvasRef.current?.clearCurrentPage()}
          disabled={isRunning}
        >
          <Text style={styles.buttonText}>Clear</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={styles.button}
          onPress={logResult}
          disabled={!result || isRunning}
        >
          <Text style={styles.buttonText}>Log JSON</Text>
        </TouchableOpacity>
      </View>

      <View style={styles.benchmarkStatus}>
        <Text style={styles.statusText}>Backend {backend}</Text>
        <Text style={styles.statusText}>Scenario {scenario.label}</Text>
        <Text style={styles.statusText}>Tool {activeTool.label}</Text>
        <Text style={styles.statusText}>
          {activeRun ? `${activeRun} ${formatNumber(elapsedMs / 1000, 1)}s` : "idle"}
        </Text>
        {suiteProgress ? <Text style={styles.statusText}>{suiteProgress}</Text> : null}
        {error ? <Text style={styles.errorText}>{error}</Text> : null}
      </View>

      <View style={styles.benchmarkSpacer} pointerEvents="none" />

      <View style={styles.metricsPanel}>
        <ScrollView contentContainerStyle={styles.metricsContent}>
          {metrics.length > 0 ? (
            <>
              <View style={styles.metricsGrid}>
                {metrics.map(([label, value]) => (
                  <View key={label} style={styles.metricCell}>
                    <Text style={styles.metricLabel}>{label}</Text>
                    <Text style={styles.metricValue}>{value}</Text>
                  </View>
                ))}
              </View>
              <Text selectable style={styles.resultJson}>
                {JSON.stringify(result, null, 2)}
              </Text>
            </>
          ) : (
            <Text style={styles.emptyMetrics}>Run a scenario to collect on-device metrics.</Text>
          )}
        </ScrollView>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  benchmarkRoot: {
    ...StyleSheet.absoluteFillObject,
    justifyContent: "space-between",
  },
  benchmarkToolbar: {
    alignItems: "center",
    flexDirection: "row",
    flexWrap: "wrap",
    gap: 8,
    minHeight: 56,
    paddingHorizontal: 12,
    paddingVertical: 8,
    backgroundColor: "#FFFFFF",
    borderBottomColor: "#D7DEE8",
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  benchmarkStatus: {
    alignItems: "center",
    flexDirection: "row",
    flexWrap: "wrap",
    gap: 14,
    minHeight: 34,
    paddingHorizontal: 14,
    backgroundColor: "#F9FAFC",
    borderBottomColor: "#D7DEE8",
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  benchmarkSpacer: {
    flex: 1,
  },
  segmentGroup: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: 6,
  },
  button: {
    borderRadius: 8,
    paddingHorizontal: 12,
    paddingVertical: 8,
    backgroundColor: "#EDF1F5",
  },
  activeButton: {
    backgroundColor: "#CFE1FF",
  },
  primaryButton: {
    backgroundColor: "#17202A",
  },
  stopButton: {
    backgroundColor: "#B42318",
  },
  disabledButton: {
    opacity: 0.55,
  },
  buttonText: {
    color: "#17202A",
    fontWeight: "600",
  },
  primaryButtonText: {
    color: "#FFFFFF",
    fontWeight: "700",
  },
  stopButtonText: {
    color: "#FFFFFF",
  },
  statusText: {
    color: "#4B5563",
    fontSize: 12,
    fontWeight: "700",
  },
  errorText: {
    color: "#B42318",
    fontSize: 12,
    fontWeight: "700",
  },
  metricsPanel: {
    maxHeight: 330,
    minHeight: 190,
    backgroundColor: "#FFFFFF",
    borderTopColor: "#D7DEE8",
    borderTopWidth: StyleSheet.hairlineWidth,
  },
  metricsContent: {
    gap: 12,
    padding: 12,
  },
  metricsGrid: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: 8,
  },
  metricCell: {
    minWidth: 132,
    borderRadius: 8,
    paddingHorizontal: 10,
    paddingVertical: 8,
    backgroundColor: "#F4F6F8",
  },
  metricLabel: {
    color: "#667085",
    fontSize: 11,
    fontWeight: "700",
  },
  metricValue: {
    color: "#17202A",
    fontSize: 17,
    fontWeight: "800",
    marginTop: 2,
  },
  resultJson: {
    color: "#334155",
    fontFamily: "Menlo",
    fontSize: 11,
    lineHeight: 16,
  },
  emptyMetrics: {
    color: "#667085",
    fontSize: 13,
    fontWeight: "600",
  },
});
