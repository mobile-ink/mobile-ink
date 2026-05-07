import React from "react";
import { act, render } from "@testing-library/react-native";
import {
  ContinuousEnginePool,
  type ContinuousEnginePoolAssignment,
  type ContinuousEnginePoolRef,
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
});
