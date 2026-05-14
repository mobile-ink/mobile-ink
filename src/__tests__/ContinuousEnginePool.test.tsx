import React from "react";
import { act, render } from "@testing-library/react-native";
import {
  ContinuousEnginePool,
  type ContinuousEnginePoolAssignment,
  type ContinuousEnginePoolRef,
  type ContinuousEnginePoolSlotRef,
  type ContinuousEnginePoolToolState,
} from "../ContinuousEnginePool";
import type { NotebookPage } from "../types";

const mockLoadBase64Data = jest.fn(async () => true);
const mockGetBase64Data = jest.fn(async () => '{"pages":{"0":"persisted"}}');
const mockReleaseEngine = jest.fn(async () => undefined);
const mockSetTool = jest.fn();
const mockSetNativeProps = jest.fn();
const mockUndo = jest.fn();
const mockRedo = jest.fn();
const mockClear = jest.fn();
const mockRunBenchmark = jest.fn(async (options?: Record<string, unknown>) => ({
  sessionId: "benchmark-session",
  scenario: options?.scenario ?? "custom",
  requestedBackend: options?.backend ?? "ganesh",
  durationMs: 1,
  fpsAverage: 60,
  renderFrameCount: 1,
  cpuFrameCount: 0,
  ganeshFrameCount: 1,
  ganeshFallbackFrameCount: 0,
  droppedFrameCount: 0,
  inputEventCount: 1,
  syntheticStrokeCount: 1,
  syntheticPointCount: 1,
  renderMs: { count: 1, average: 1, p50: 1, p95: 1, p99: 1, max: 1 },
  frameIntervalMs: { count: 1, average: 16, p50: 16, p95: 16, p99: 16, max: 16 },
  inputToPresentLatencyMs: { count: 1, average: 4, p50: 4, p95: 4, p99: 4, max: 4 },
  memory: {
    startBytes: 1,
    endBytes: 1,
    peakBytes: 1,
    lowBytes: 1,
    deltaBytes: 0,
    peakDeltaBytes: 0,
  },
}));
const mockStartBenchmarkRecording = jest.fn(async () => true);
const mockStopBenchmarkRecording = jest.fn(async () => mockRunBenchmark());
const mockNativeCanvasProps: any[] = [];

jest.mock("../NativeInkCanvas", () => {
  const React = require("react");
  const { View } = require("react-native");

  return {
    NativeInkCanvas: React.forwardRef((props: any, ref: any) => {
      mockNativeCanvasProps.push(props);

      React.useImperativeHandle(ref, () => ({
        clear: mockClear,
        undo: mockUndo,
        redo: mockRedo,
        setTool: mockSetTool,
        setNativeProps: mockSetNativeProps,
        loadBase64Data: mockLoadBase64Data,
        getBase64Data: mockGetBase64Data,
        releaseEngine: mockReleaseEngine,
        runBenchmark: mockRunBenchmark,
        startBenchmarkRecording: mockStartBenchmarkRecording,
        stopBenchmarkRecording: mockStopBenchmarkRecording,
      }));

      React.useEffect(() => {
        props.onCanvasReady?.();
      }, [props.onCanvasReady]);

      return <View testID="mock-native-canvas" />;
    }),
  };
});

const page = (index: number): NotebookPage => ({
  id: `page-${index}`,
  title: `Page ${index}`,
  rotation: 0,
  data: `data-${index}`,
  dataSignature: `signature-${index}`,
});

const defaultToolState: ContinuousEnginePoolToolState = {
  toolType: "pen",
  width: 3,
  color: "#000000",
  eraserMode: "pixel",
};

const defaultProps = {
  canvasHeight: 800,
  backgroundType: "regular",
  pdfBackgroundBaseUri: undefined,
  fingerDrawingEnabled: true,
  getToolState: () => defaultToolState,
  onCanvasReady: jest.fn(),
  onPerPageDrawingChange: jest.fn(),
  registerPerPageSlot: jest.fn(),
  shouldCaptureBeforeReassign: jest.fn(() => false),
  onSlotCaptureBeforeUnmount: jest.fn(),
};

const buildAssignments = (
  sourcePages: NotebookPage[],
  startIndex: number,
): ContinuousEnginePoolAssignment[] =>
  sourcePages.map((nextPage, offset) => ({
    page: nextPage,
    pageIndex: startIndex + offset,
  }));

const renderPool = (props?: Partial<React.ComponentProps<typeof ContinuousEnginePool>>) => {
  const poolRef = React.createRef<ContinuousEnginePoolRef>();
  const view = render(
    <ContinuousEnginePool
      {...defaultProps}
      {...props}
      ref={poolRef}
    />,
  );
  return { poolRef, view };
};

const assignPages = async (
  poolRef: React.RefObject<ContinuousEnginePoolRef | null>,
  assignments: ContinuousEnginePoolAssignment[],
) => {
  await act(async () => {
    await poolRef.current?.assignPages(assignments);
  });
};

describe("ContinuousEnginePool", () => {
  let originalRequestAnimationFrame: typeof global.requestAnimationFrame;

  beforeEach(() => {
    jest.clearAllMocks();
    mockNativeCanvasProps.length = 0;
    originalRequestAnimationFrame = global.requestAnimationFrame;
    global.requestAnimationFrame = ((callback: FrameRequestCallback) => {
      callback(Date.now());
      return 0;
    }) as typeof global.requestAnimationFrame;
  });

  afterEach(() => {
    global.requestAnimationFrame = originalRequestAnimationFrame;
  });

  it("keeps three native canvases mounted and reassigns without release", async () => {
    const pages = [page(0), page(1), page(2), page(3)];
    const onSlotCaptureBeforeUnmount = jest.fn();
    const shouldCaptureBeforeReassign = jest.fn((pageId: string) => pageId === "page-0");
    const { poolRef, view } = renderPool({
      shouldCaptureBeforeReassign,
      onSlotCaptureBeforeUnmount,
    });

    await act(async () => {});
    expect(view.getAllByTestId("mock-native-canvas")).toHaveLength(3);

    await assignPages(poolRef, buildAssignments(pages.slice(0, 3), 0));
    await assignPages(poolRef, buildAssignments(pages.slice(1, 4), 1));

    expect(view.getAllByTestId("mock-native-canvas")).toHaveLength(3);
    expect(mockReleaseEngine).not.toHaveBeenCalled();
    expect(onSlotCaptureBeforeUnmount).toHaveBeenCalledWith(
      "page-0",
      '{"pages":{"0":"persisted"}}',
    );

    view.unmount();
    await act(async () => {});
    expect(mockReleaseEngine).toHaveBeenCalledTimes(3);
  });

  it("defaults pooled native canvases to the Ganesh backend", async () => {
    renderPool();

    await act(async () => {});

    expect(mockNativeCanvasProps).toHaveLength(3);
    for (const props of mockNativeCanvasProps) {
      expect(props.renderBackend).toBe("ganesh");
    }
  });

  it("applies the latest current tool when a slot is assigned", async () => {
    const pages = [page(0), page(1), page(2), page(3)];
    let currentToolState: ContinuousEnginePoolToolState = {
      toolType: "pen",
      width: 3,
      color: "#000000",
      eraserMode: "pixel",
    };
    const getToolState = jest.fn(() => currentToolState);
    const { poolRef } = renderPool({ getToolState });

    await act(async () => {});
    await assignPages(poolRef, buildAssignments(pages.slice(0, 3), 0));
    expect(mockSetTool).toHaveBeenCalledWith("pen", 3, "#000000", "pixel");

    mockSetTool.mockClear();
    currentToolState = {
      toolType: "highlighter",
      width: 8,
      color: "#ffcc00",
      eraserMode: "pixel",
    };
    await assignPages(poolRef, buildAssignments(pages.slice(1, 4), 1));
    expect(mockSetTool).toHaveBeenCalledWith("highlighter", 8, "#ffcc00", "pixel");
  });

  it("does not reload pooled engines when only same-page drawing signatures change", async () => {
    const pages = [page(0), page(1), page(2)];
    const { poolRef } = renderPool();

    await act(async () => {});
    await assignPages(poolRef, buildAssignments(pages, 0));
    mockLoadBase64Data.mockClear();

    await assignPages(
      poolRef,
      buildAssignments(
        pages.map((nextPage) => ({
          ...nextPage,
          data: `${nextPage.data}-saved`,
          dataSignature: `${nextPage.dataSignature}-saved`,
        })),
        0,
      ),
    );
    expect(mockLoadBase64Data).not.toHaveBeenCalled();
  });

  it("retries page loads until the native engine accepts the payload", async () => {
    const pages = [page(0)];
    mockLoadBase64Data
      .mockResolvedValueOnce(false)
      .mockResolvedValueOnce(false)
      .mockResolvedValueOnce(true);
    const { poolRef } = renderPool();

    await act(async () => {});
    await assignPages(poolRef, buildAssignments(pages, 0));

    expect(mockLoadBase64Data).toHaveBeenCalledTimes(3);
    expect(mockLoadBase64Data).toHaveBeenNthCalledWith(1, "data-0");
    expect(mockLoadBase64Data).toHaveBeenNthCalledWith(2, "data-0");
    expect(mockLoadBase64Data).toHaveBeenNthCalledWith(3, "data-0");
  });

  it("preserves already loaded page slots across adjacent page changes", async () => {
    const pages = [page(0), page(1), page(2), page(3)];
    const { poolRef } = renderPool();

    await act(async () => {});
    await assignPages(poolRef, buildAssignments(pages.slice(0, 3), 0));
    mockLoadBase64Data.mockClear();

    await assignPages(poolRef, buildAssignments(pages.slice(1, 4), 1));

    expect(mockLoadBase64Data).toHaveBeenCalledTimes(1);
    expect(mockLoadBase64Data).toHaveBeenCalledWith("data-3");
  });

  it("forwards pencil double-tap events to every pooled native canvas", async () => {
    const onPencilDoubleTap = jest.fn();

    renderPool({ onPencilDoubleTap });

    await act(async () => {});
    expect(mockNativeCanvasProps).toHaveLength(3);
    for (const props of mockNativeCanvasProps) {
      expect(props.onPencilDoubleTap).toBe(onPencilDoubleTap);
    }
  });

  it("forwards benchmark methods through the registered page slot", async () => {
    const pages = [page(0), page(1), page(2)];
    const registerPerPageSlot = jest.fn();
    const { poolRef } = renderPool({ registerPerPageSlot });

    await act(async () => {});
    await assignPages(poolRef, buildAssignments(pages, 0));

    const slotRef = registerPerPageSlot.mock.calls.find(
      ([pageId, registeredRef]) => pageId === "page-0" && registeredRef,
    )?.[1] as ContinuousEnginePoolSlotRef | undefined;

    expect(slotRef).toBeDefined();
    await expect(slotRef?.runBenchmark?.({
      scenario: "eraser",
      backend: "ganesh",
      workload: "erase",
      toolType: "eraser",
    })).resolves.toMatchObject({
      scenario: "eraser",
      requestedBackend: "ganesh",
    });
    await expect(slotRef?.startBenchmarkRecording?.({
      scenario: "manual",
      backend: "cpu",
    })).resolves.toBe(true);
    await expect(slotRef?.stopBenchmarkRecording?.()).resolves.toMatchObject({
      sessionId: "benchmark-session",
    });

    expect(mockRunBenchmark).toHaveBeenCalledWith({
      scenario: "eraser",
      backend: "ganesh",
      workload: "erase",
      toolType: "eraser",
    });
    expect(mockStartBenchmarkRecording).toHaveBeenCalledWith({
      scenario: "manual",
      backend: "cpu",
    });
    expect(mockStopBenchmarkRecording).toHaveBeenCalledTimes(1);
  });

  it("starts and stops notebook benchmark recording across the mounted pool", async () => {
    const pages = [page(0), page(1), page(2)];
    const { poolRef } = renderPool();

    await act(async () => {});
    await assignPages(poolRef, buildAssignments(pages, 0));

    await expect(poolRef.current?.startBenchmarkRecording({
      scenario: "manual",
      backend: "ganesh",
    })).resolves.toBe(true);
    await expect(poolRef.current?.stopBenchmarkRecording()).resolves.toHaveLength(3);

    expect(mockStartBenchmarkRecording).toHaveBeenCalledTimes(3);
    expect(mockStartBenchmarkRecording).toHaveBeenCalledWith({
      scenario: "manual",
      backend: "ganesh",
    });
    expect(mockStopBenchmarkRecording).toHaveBeenCalledTimes(3);
  });
});
