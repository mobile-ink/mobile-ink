import React, {
  forwardRef,
  memo,
  useCallback,
  useEffect,
  useImperativeHandle,
  useMemo,
  useRef,
} from "react";
import { StyleSheet, View } from "react-native";
import { NativeInkCanvas } from "./NativeInkCanvas";
import type {
  NativeInkCanvasProps,
  NativeInkCanvasRef,
} from "./NativeInkCanvas";
import type { NativeSelectionBounds } from "./types";
import type { NotebookPage, ToolType } from "./types";
import { CONTINUOUS_ENGINE_POOL_SIZE } from "./utils/continuousEnginePool";

const BLANK_PAGE_PAYLOAD = '{"pages":{}}';
const OFFSCREEN_TOP = -100000;

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
};

export type ContinuousEnginePoolRef = {
  assignPages: (assignments: ContinuousEnginePoolAssignment[]) => Promise<void>;
  applyToolState: (toolState: ContinuousEnginePoolToolState) => void;
  release: () => Promise<void>;
};

export type ContinuousEnginePoolProps = {
  canvasHeight: number;
  backgroundType: string;
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

type SlotAssignOptions = {
  assignment: ContinuousEnginePoolAssignment | null;
  assignmentKey: string;
  canvasHeight: number;
  backgroundType: string;
  pdfBackgroundBaseUri?: string;
};

type PooledCanvasSlotHandle = {
  assign: (options: SlotAssignOptions) => Promise<void>;
  applyToolState: (toolState: ContinuousEnginePoolToolState) => void;
  release: () => Promise<void>;
};

type PooledCanvasSlotProps = {
  poolIndex: number;
  canvasHeight: number;
  backgroundType: string;
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

const getAssignmentKey = (assignments: ContinuousEnginePoolAssignment[]) => {
  return assignments
    .map(({ page, pageIndex }) => [
      pageIndex,
      page.id,
      page.pdfPageNumber ?? "",
      page.rotation ?? 0,
    ].join(":"))
    .join("|");
};

const getPdfBackgroundUri = (
  assignment: ContinuousEnginePoolAssignment,
  backgroundType: string,
  pdfBackgroundBaseUri?: string,
) => {
  if (backgroundType !== "pdf" || !pdfBackgroundBaseUri) {
    return undefined;
  }

  return `${pdfBackgroundBaseUri}#page=${
    assignment.page.pdfPageNumber || assignment.pageIndex + 1
  }`;
};

const waitForNextFrame = () => new Promise<void>((resolve) => {
  requestAnimationFrame(() => resolve());
});

const PooledCanvasSlot = memo(forwardRef<PooledCanvasSlotHandle, PooledCanvasSlotProps>(
  function PooledCanvasSlot({
    poolIndex,
    canvasHeight,
    backgroundType,
    pdfBackgroundBaseUri,
    drawingPolicy,
    getToolState,
    onCanvasReady,
    onSlotLoaded,
    onDrawingChange,
    onSelectionChange,
    onDrawingBegin,
    onPencilDoubleTap,
    registerRef,
    shouldCaptureBeforeReassign,
    onCaptureBeforeReassign,
  }, ref) {
    const slotViewRef = useRef<View | null>(null);
    const canvasRef = useRef<NativeInkCanvasRef | null>(null);
    const lastAttachedCanvasRef = useRef<NativeInkCanvasRef | null>(null);
    const nativeReadyRef = useRef(false);
    const nativeReadyWaitersRef = useRef<Array<() => void>>([]);
    const isLoadedRef = useRef(false);
    const loadedPageIdRef = useRef<string | null>(null);
    const registeredPageIdRef = useRef<string | null>(null);
    const currentAssignmentRef = useRef<ContinuousEnginePoolAssignment | null>(null);
    const assignmentKeyRef = useRef("");
    const loadTokenRef = useRef(0);
    const forwardedReadyRef = useRef(false);
    const shouldCaptureBeforeReassignRef = useRef(shouldCaptureBeforeReassign);
    const captureCallbackRef = useRef(onCaptureBeforeReassign);
    const onCanvasReadyRef = useRef(onCanvasReady);
    const onSlotLoadedRef = useRef(onSlotLoaded);
    const onSelectionChangeRef = useRef(onSelectionChange);
    const getToolStateRef = useRef(getToolState);

    shouldCaptureBeforeReassignRef.current = shouldCaptureBeforeReassign;
    captureCallbackRef.current = onCaptureBeforeReassign;
    onCanvasReadyRef.current = onCanvasReady;
    onSlotLoadedRef.current = onSlotLoaded;
    onSelectionChangeRef.current = onSelectionChange;
    getToolStateRef.current = getToolState;

    const setNativeCanvasRef = useCallback((nextRef: NativeInkCanvasRef | null) => {
      canvasRef.current = nextRef;
      if (nextRef) {
        lastAttachedCanvasRef.current = nextRef;
      }
    }, []);

    const waitForNativeReady = useCallback(() => {
      if (nativeReadyRef.current && canvasRef.current) {
        return Promise.resolve();
      }

      return new Promise<void>((resolve) => {
        nativeReadyWaitersRef.current.push(resolve);
      });
    }, []);

    const setSlotFrame = useCallback((
      pageIndex: number | null,
      height: number,
      isVisible: boolean,
    ) => {
      slotViewRef.current?.setNativeProps({
        style: {
          top: pageIndex === null ? OFFSCREEN_TOP : pageIndex * height,
          height,
          opacity: pageIndex === null || !isVisible ? 0 : 1,
        },
      });
    }, []);

    const setNativeCanvasVisible = useCallback((isVisible: boolean) => {
      canvasRef.current?.setNativeProps?.({
        style: {
          opacity: isVisible ? 1 : 0,
        },
      });
    }, []);

    const applyToolState = useCallback((toolState: ContinuousEnginePoolToolState) => {
      canvasRef.current?.setTool(
        toolState.toolType,
        toolState.width,
        toolState.color,
        toolState.eraserMode,
      );
    }, []);

    const applyCurrentTool = useCallback(() => {
      applyToolState(getToolStateRef.current());
    }, [applyToolState]);

    const slotRef = useMemo<ContinuousEnginePoolSlotRef>(
      () => ({
        getBase64Data: async () => {
          if (!canvasRef.current) return BLANK_PAGE_PAYLOAD;
          try {
            const data = await canvasRef.current.getBase64Data();
            return data || BLANK_PAGE_PAYLOAD;
          } catch {
            return BLANK_PAGE_PAYLOAD;
          }
        },
        isLoaded: () => isLoadedRef.current,
        setTool: (toolType, width, color, eraserMode) => {
          canvasRef.current?.setTool(toolType, width, color, eraserMode);
        },
        clear: () => {
          canvasRef.current?.clear();
        },
        undo: () => {
          canvasRef.current?.undo();
        },
        redo: () => {
          canvasRef.current?.redo();
        },
        performCopy: () => {
          canvasRef.current?.performCopy();
        },
        performPaste: () => {
          canvasRef.current?.performPaste();
        },
        performDelete: () => {
          canvasRef.current?.performDelete();
        },
      }),
      [],
    );

    const captureLoadedPage = useCallback(async (
      pageId: string | null,
      sourceRef: NativeInkCanvasRef | null = canvasRef.current,
    ) => {
      if (
        !pageId ||
        !sourceRef ||
        !isLoadedRef.current ||
        !shouldCaptureBeforeReassignRef.current(pageId)
      ) {
        return;
      }

      try {
        const data = await sourceRef.getBase64Data();
        if (data) {
          captureCallbackRef.current(pageId, data);
        }
      } catch {
        // A native serialize can fail during teardown. The normal save path
        // still has pages[i].data, and the parent has a blank-overwrite guard.
      }
    }, []);

    const unregisterCurrentPage = useCallback(() => {
      const pageId = registeredPageIdRef.current;
      if (!pageId) {
        return;
      }
      registerRef(pageId, null, slotRef);
      registeredPageIdRef.current = null;
    }, [registerRef, slotRef]);

    const registerAssignment = useCallback((pageId: string) => {
      if (registeredPageIdRef.current === pageId) {
        return;
      }

      unregisterCurrentPage();
      registerRef(pageId, slotRef);
      registeredPageIdRef.current = pageId;
    }, [registerRef, slotRef, unregisterCurrentPage]);

    const clearAssignment = useCallback(async () => {
      const token = loadTokenRef.current + 1;
      loadTokenRef.current = token;
      const previousPageId = loadedPageIdRef.current;
      await captureLoadedPage(previousPageId);
      if (loadTokenRef.current !== token) {
        return;
      }

      unregisterCurrentPage();
      currentAssignmentRef.current = null;
      isLoadedRef.current = false;
      loadedPageIdRef.current = null;
      setNativeCanvasVisible(false);
      setSlotFrame(null, canvasHeight, false);
    }, [
      canvasHeight,
      captureLoadedPage,
      setNativeCanvasVisible,
      setSlotFrame,
      unregisterCurrentPage,
    ]);

    const assign = useCallback(async ({
      assignment,
      assignmentKey,
      canvasHeight: nextCanvasHeight,
      backgroundType: nextBackgroundType,
      pdfBackgroundBaseUri: nextPdfBackgroundBaseUri,
    }: SlotAssignOptions) => {
      assignmentKeyRef.current = assignmentKey;
      if (!assignment) {
        await clearAssignment();
        return;
      }

      const nextPageId = assignment.page.id;
      const previousPageId = loadedPageIdRef.current;
      const previousPageIndex = currentAssignmentRef.current?.pageIndex ?? null;
      const isAlreadyLoadedPage =
        previousPageId === nextPageId && isLoadedRef.current;
      const token = loadTokenRef.current + 1;
      loadTokenRef.current = token;
      currentAssignmentRef.current = assignment;

      if (!isAlreadyLoadedPage) {
        setNativeCanvasVisible(false);
        setSlotFrame(previousPageIndex, nextCanvasHeight, false);
        await waitForNextFrame();
        if (
          loadTokenRef.current !== token ||
          currentAssignmentRef.current?.page.id !== nextPageId
        ) {
          return;
        }
      }

      setSlotFrame(assignment.pageIndex, nextCanvasHeight, isAlreadyLoadedPage);

      await waitForNativeReady();
      if (
        loadTokenRef.current !== token ||
        currentAssignmentRef.current?.page.id !== nextPageId
      ) {
        return;
      }

      const canvas = canvasRef.current;
      if (!canvas) {
        return;
      }

      if (isAlreadyLoadedPage) {
        canvas.setNativeProps?.({
          backgroundType: nextBackgroundType,
          pdfBackgroundUri: getPdfBackgroundUri(
            assignment,
            nextBackgroundType,
            nextPdfBackgroundBaseUri,
          ),
          style: {
            opacity: 1,
          },
        });
        registerAssignment(nextPageId);
        applyCurrentTool();
        onSlotLoadedRef.current?.(
          poolIndex,
          assignmentKey,
          nextPageId,
          assignment.pageIndex,
        );
        return;
      }

      canvas.setNativeProps?.({
        backgroundType: nextBackgroundType,
        pdfBackgroundUri: getPdfBackgroundUri(
          assignment,
          nextBackgroundType,
          nextPdfBackgroundBaseUri,
        ),
        style: {
          opacity: 0,
        },
      });

      if (previousPageId && previousPageId !== nextPageId) {
        await captureLoadedPage(previousPageId);
        if (loadTokenRef.current !== token) {
          return;
        }
      }

      registerAssignment(nextPageId);
      isLoadedRef.current = false;
      loadedPageIdRef.current = null;

      try {
        await canvas.loadBase64Data(assignment.page.data || BLANK_PAGE_PAYLOAD);
        if (loadTokenRef.current !== token) {
          return;
        }

        loadedPageIdRef.current = nextPageId;
        isLoadedRef.current = true;
        applyCurrentTool();
        await new Promise<void>((resolve) => {
          requestAnimationFrame(() => {
            requestAnimationFrame(() => resolve());
          });
        });
        if (
          loadTokenRef.current !== token ||
          currentAssignmentRef.current?.page.id !== nextPageId
        ) {
          return;
        }

        setSlotFrame(assignment.pageIndex, nextCanvasHeight, true);
        setNativeCanvasVisible(true);
        onSlotLoadedRef.current?.(
          poolIndex,
          assignmentKey,
          nextPageId,
          assignment.pageIndex,
        );
      } catch {
        if (loadTokenRef.current === token) {
          loadedPageIdRef.current = null;
          isLoadedRef.current = false;
          setNativeCanvasVisible(false);
        }
      }
    }, [
      applyCurrentTool,
      captureLoadedPage,
      clearAssignment,
      poolIndex,
      registerAssignment,
      setNativeCanvasVisible,
      setSlotFrame,
      waitForNativeReady,
    ]);

    const release = useCallback(async () => {
      const nativeCanvas = canvasRef.current ?? lastAttachedCanvasRef.current;
      const pageId = loadedPageIdRef.current;
      loadTokenRef.current += 1;
      unregisterCurrentPage();
      if (nativeCanvas) {
        await captureLoadedPage(pageId, nativeCanvas);
        await nativeCanvas.releaseEngine?.().catch(() => {});
        if (lastAttachedCanvasRef.current === nativeCanvas) {
          lastAttachedCanvasRef.current = null;
        }
      }
    }, [captureLoadedPage, unregisterCurrentPage]);

    useImperativeHandle(ref, () => ({
      assign,
      applyToolState,
      release,
    }), [assign, applyToolState, release]);

    useEffect(() => {
      return () => {
        void release();
      };
    }, [release]);

    const handleNativeCanvasReady = useCallback(() => {
      nativeReadyRef.current = true;
      applyCurrentTool();
      if (!forwardedReadyRef.current) {
        forwardedReadyRef.current = true;
        onCanvasReadyRef.current?.();
      }

      const waiters = nativeReadyWaitersRef.current;
      nativeReadyWaitersRef.current = [];
      for (const resolve of waiters) {
        resolve();
      }
    }, [applyCurrentTool]);

    const handleDrawingChange = useCallback(() => {
      const pageId =
        loadedPageIdRef.current ?? currentAssignmentRef.current?.page.id;
      if (pageId) {
        onDrawingChange(pageId);
      }
    }, [onDrawingChange]);

    const handleSelectionChange = useCallback((event: {
      nativeEvent: { count: number; bounds?: NativeSelectionBounds | null };
    }) => {
      const pageId =
        loadedPageIdRef.current ?? currentAssignmentRef.current?.page.id;
      if (pageId) {
        onSelectionChangeRef.current?.(
          pageId,
          event.nativeEvent.count,
          event.nativeEvent.bounds ?? null,
        );
      }
    }, []);

    return (
      <View
        ref={slotViewRef}
        style={styles.slot}
        pointerEvents="auto"
        testID={`continuous-engine-pool-slot-${poolIndex}`}
      >
        <NativeInkCanvas
          ref={setNativeCanvasRef}
          style={styles.nativeCanvas}
          drawingPolicy={drawingPolicy}
          onCanvasReady={handleNativeCanvasReady}
          onDrawingBegin={onDrawingBegin}
          onDrawingChange={handleDrawingChange}
          onSelectionChange={handleSelectionChange}
          onPencilDoubleTap={onPencilDoubleTap}
        />
      </View>
    );
  },
), (prev, next) => (
  prev.poolIndex === next.poolIndex &&
  prev.canvasHeight === next.canvasHeight &&
  prev.backgroundType === next.backgroundType &&
  prev.pdfBackgroundBaseUri === next.pdfBackgroundBaseUri &&
  prev.drawingPolicy === next.drawingPolicy &&
  prev.getToolState === next.getToolState &&
  prev.onCanvasReady === next.onCanvasReady &&
  prev.onSlotLoaded === next.onSlotLoaded &&
  prev.onDrawingChange === next.onDrawingChange &&
  prev.onSelectionChange === next.onSelectionChange &&
  prev.onDrawingBegin === next.onDrawingBegin &&
  prev.onPencilDoubleTap === next.onPencilDoubleTap &&
  prev.registerRef === next.registerRef &&
  prev.shouldCaptureBeforeReassign === next.shouldCaptureBeforeReassign &&
  prev.onCaptureBeforeReassign === next.onCaptureBeforeReassign
));

export const ContinuousEnginePool = memo(forwardRef<
  ContinuousEnginePoolRef,
  ContinuousEnginePoolProps
>(function ContinuousEnginePool({
  canvasHeight,
  backgroundType,
  pdfBackgroundBaseUri,
  fingerDrawingEnabled,
  getToolState,
  onCanvasReady,
  onAssignmentReady,
  onPageAssignmentReady,
  onPerPageDrawingChange,
  onPerPageSelectionChange,
  onDrawingBegin,
  onPencilDoubleTap,
  registerPerPageSlot,
  shouldCaptureBeforeReassign,
  onSlotCaptureBeforeUnmount,
}, ref) {
  const slotRefs = useRef<Array<PooledCanvasSlotHandle | null>>([]);
  const assignmentKeyRef = useRef("");
  const assignedPageIdsRef = useRef<Array<string | null>>(
    Array.from({ length: CONTINUOUS_ENGINE_POOL_SIZE }, () => null),
  );
  const onAssignmentReadyRef = useRef(onAssignmentReady);
  const onPageAssignmentReadyRef = useRef(onPageAssignmentReady);

  onAssignmentReadyRef.current = onAssignmentReady;
  onPageAssignmentReadyRef.current = onPageAssignmentReady;

  const handleSlotRef = useCallback((
    poolIndex: number,
    slotRef: PooledCanvasSlotHandle | null,
  ) => {
    slotRefs.current[poolIndex] = slotRef;
  }, []);

  const handleSlotLoaded = useCallback((
    _poolIndex: number,
    loadedAssignmentKey: string,
    pageId: string,
    pageIndex: number,
  ) => {
    if (loadedAssignmentKey !== assignmentKeyRef.current) {
      return;
    }

    onPageAssignmentReadyRef.current?.(pageId, pageIndex, loadedAssignmentKey);
  }, []);

  const assignPages = useCallback(async (
    assignments: ContinuousEnginePoolAssignment[],
  ) => {
    const nextAssignmentKey = getAssignmentKey(assignments);
    assignmentKeyRef.current = nextAssignmentKey;
    const previousSlotByPageId = new Map<string, number>();
    assignedPageIdsRef.current.forEach((pageId, poolIndex) => {
      if (pageId !== null) {
        previousSlotByPageId.set(pageId, poolIndex);
      }
    });

    const nextSlotAssignments: Array<ContinuousEnginePoolAssignment | null> =
      Array.from({ length: CONTINUOUS_ENGINE_POOL_SIZE }, () => null);
    const placedPageIds = new Set<string>();

    for (const assignment of assignments) {
      const previousPoolIndex = previousSlotByPageId.get(assignment.page.id);
      if (
        previousPoolIndex === undefined ||
        nextSlotAssignments[previousPoolIndex] !== null
      ) {
        continue;
      }

      nextSlotAssignments[previousPoolIndex] = assignment;
      placedPageIds.add(assignment.page.id);
    }

    for (const assignment of assignments) {
      if (placedPageIds.has(assignment.page.id)) {
        continue;
      }

      const availablePoolIndex = nextSlotAssignments.findIndex(
        (slotAssignment) => slotAssignment === null,
      );
      if (availablePoolIndex === -1) {
        break;
      }

      nextSlotAssignments[availablePoolIndex] = assignment;
      placedPageIds.add(assignment.page.id);
    }

    assignedPageIdsRef.current = nextSlotAssignments.map(
      (assignment) => assignment?.page.id ?? null,
    );
    const slotPromises = Array.from(
      { length: CONTINUOUS_ENGINE_POOL_SIZE },
      (_, poolIndex) => {
        const slotRef = slotRefs.current[poolIndex];
        if (!slotRef) {
          return Promise.resolve();
        }

        return slotRef.assign({
          assignment: nextSlotAssignments[poolIndex],
          assignmentKey: nextAssignmentKey,
          canvasHeight,
          backgroundType,
          pdfBackgroundBaseUri,
        });
      },
    );

    await Promise.all(slotPromises);
    if (assignmentKeyRef.current === nextAssignmentKey) {
      onAssignmentReadyRef.current?.(nextAssignmentKey);
    }
  }, [backgroundType, canvasHeight, pdfBackgroundBaseUri]);

  const applyToolState = useCallback((toolState: ContinuousEnginePoolToolState) => {
    for (const slotRef of slotRefs.current) {
      slotRef?.applyToolState(toolState);
    }
  }, []);

  const release = useCallback(async () => {
    await Promise.all(
      slotRefs.current.map((slotRef) => slotRef?.release() ?? Promise.resolve()),
    );
  }, []);

  useImperativeHandle(ref, () => ({
    assignPages,
    applyToolState,
    release,
  }), [applyToolState, assignPages, release]);

  return (
    <>
      {Array.from({ length: CONTINUOUS_ENGINE_POOL_SIZE }, (_, poolIndex) => (
        <PooledCanvasSlot
          key={`continuous-engine-pool-${poolIndex}`}
          ref={(slotRef) => handleSlotRef(poolIndex, slotRef)}
          poolIndex={poolIndex}
          canvasHeight={canvasHeight}
          backgroundType={backgroundType}
          pdfBackgroundBaseUri={pdfBackgroundBaseUri}
          drawingPolicy={fingerDrawingEnabled ? "anyinput" : "pencilonly"}
          getToolState={getToolState}
          onCanvasReady={poolIndex === 0 ? onCanvasReady : undefined}
          onSlotLoaded={handleSlotLoaded}
          onDrawingChange={onPerPageDrawingChange}
          onSelectionChange={onPerPageSelectionChange}
          onDrawingBegin={onDrawingBegin}
          onPencilDoubleTap={onPencilDoubleTap}
          registerRef={registerPerPageSlot}
          shouldCaptureBeforeReassign={shouldCaptureBeforeReassign}
          onCaptureBeforeReassign={onSlotCaptureBeforeUnmount}
        />
      ))}
    </>
  );
}), (prev, next) => (
  prev.canvasHeight === next.canvasHeight &&
  prev.backgroundType === next.backgroundType &&
  prev.pdfBackgroundBaseUri === next.pdfBackgroundBaseUri &&
  prev.fingerDrawingEnabled === next.fingerDrawingEnabled &&
  prev.getToolState === next.getToolState &&
  prev.onCanvasReady === next.onCanvasReady &&
  prev.onAssignmentReady === next.onAssignmentReady &&
  prev.onPageAssignmentReady === next.onPageAssignmentReady &&
  prev.onPerPageDrawingChange === next.onPerPageDrawingChange &&
  prev.onDrawingBegin === next.onDrawingBegin &&
  prev.onPencilDoubleTap === next.onPencilDoubleTap &&
  prev.registerPerPageSlot === next.registerPerPageSlot &&
  prev.shouldCaptureBeforeReassign === next.shouldCaptureBeforeReassign &&
  prev.onSlotCaptureBeforeUnmount === next.onSlotCaptureBeforeUnmount
));

const styles = StyleSheet.create({
  slot: {
    position: "absolute",
    left: 0,
    right: 0,
    top: OFFSCREEN_TOP,
    height: 0,
    opacity: 0,
  },
  nativeCanvas: {
    ...StyleSheet.absoluteFillObject,
  },
});
