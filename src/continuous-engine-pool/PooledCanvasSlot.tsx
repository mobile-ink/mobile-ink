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
import { NativeInkCanvas } from "../NativeInkCanvas";
import type { NativeInkBenchmarkRecordingOptions } from "../benchmark";
import type { NativeSelectionBounds } from "../types";
import {
  BLANK_PAGE_PAYLOAD,
  getPdfBackgroundUri,
  loadCanvasDataWithRetry,
  OFFSCREEN_TOP,
  waitForNextFrame,
} from "./helpers";
import type {
  ContinuousEnginePoolAssignment,
  ContinuousEnginePoolSlotRef,
  ContinuousEnginePoolToolState,
  NativeCanvasRef,
  PooledCanvasSlotAssignOptions,
  PooledCanvasSlotHandle,
  PooledCanvasSlotProps,
} from "./types";

export const PooledCanvasSlot = memo(forwardRef<PooledCanvasSlotHandle, PooledCanvasSlotProps>(
  function PooledCanvasSlot({
    poolIndex,
    canvasHeight,
    backgroundType,
    renderBackend,
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
    const canvasRef = useRef<NativeCanvasRef | null>(null);
    const lastAttachedCanvasRef = useRef<NativeCanvasRef | null>(null);
    const nativeReadyRef = useRef(false);
    const nativeReadyWaitersRef = useRef<Array<() => void>>([]);
    const isLoadedRef = useRef(false);
    const loadedPageIdRef = useRef<string | null>(null);
    const registeredPageIdRef = useRef<string | null>(null);
    const currentAssignmentRef = useRef<ContinuousEnginePoolAssignment | null>(null);
    const loadTokenRef = useRef(0);
    const forwardedReadyRef = useRef(false);
    const benchmarkRecordingActiveRef = useRef(false);
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

    const setNativeCanvasRef = useCallback((nextRef: NativeCanvasRef | null) => {
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
        runBenchmark: (options) => {
          if (!canvasRef.current?.runBenchmark) {
            return Promise.reject(new Error("Native benchmark runner is unavailable."));
          }
          return canvasRef.current.runBenchmark(options);
        },
        startBenchmarkRecording: (options) => {
          if (!canvasRef.current?.startBenchmarkRecording) {
            return Promise.reject(new Error("Native benchmark recorder is unavailable."));
          }
          return canvasRef.current.startBenchmarkRecording(options);
        },
        stopBenchmarkRecording: () => {
          if (!canvasRef.current?.stopBenchmarkRecording) {
            return Promise.reject(new Error("Native benchmark recorder is unavailable."));
          }
          return canvasRef.current.stopBenchmarkRecording();
        },
      }),
      [],
    );

    const captureLoadedPage = useCallback(async (
      pageId: string | null,
      sourceRef: NativeCanvasRef | null = canvasRef.current,
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
    }: PooledCanvasSlotAssignOptions) => {
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
        const didLoad = await loadCanvasDataWithRetry(
          canvas,
          assignment.page.data || BLANK_PAGE_PAYLOAD,
        );
        if (!didLoad) {
          throw new Error(`Native canvas engine was not ready for page ${nextPageId}.`);
        }
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
        if (benchmarkRecordingActiveRef.current) {
          await nativeCanvas.stopBenchmarkRecording?.().catch(() => null);
          benchmarkRecordingActiveRef.current = false;
        }
        await captureLoadedPage(pageId, nativeCanvas);
        await nativeCanvas.releaseEngine?.().catch(() => {});
        if (lastAttachedCanvasRef.current === nativeCanvas) {
          lastAttachedCanvasRef.current = null;
        }
      }
    }, [captureLoadedPage, unregisterCurrentPage]);

    const startBenchmarkRecording = useCallback(async (
      options?: NativeInkBenchmarkRecordingOptions,
    ) => {
      if (benchmarkRecordingActiveRef.current) {
        return true;
      }
      if (!isLoadedRef.current || !canvasRef.current?.startBenchmarkRecording) {
        return false;
      }

      const didStart = await canvasRef.current.startBenchmarkRecording(options);
      benchmarkRecordingActiveRef.current = didStart;
      return didStart;
    }, []);

    const stopBenchmarkRecording = useCallback(async () => {
      if (!benchmarkRecordingActiveRef.current) {
        return null;
      }
      if (!canvasRef.current?.stopBenchmarkRecording) {
        benchmarkRecordingActiveRef.current = false;
        return null;
      }

      try {
        const result = await canvasRef.current.stopBenchmarkRecording();
        benchmarkRecordingActiveRef.current = false;
        return result;
      } catch (error) {
        benchmarkRecordingActiveRef.current = false;
        const message = error instanceof Error ? error.message : String(error);
        if (message.includes("not running")) {
          return null;
        }
        throw error;
      }
    }, []);

    useImperativeHandle(ref, () => ({
      assign,
      applyToolState,
      startBenchmarkRecording,
      stopBenchmarkRecording,
      release,
    }), [
      assign,
      applyToolState,
      release,
      startBenchmarkRecording,
      stopBenchmarkRecording,
    ]);

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
          renderBackend={renderBackend}
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
  prev.renderBackend === next.renderBackend &&
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
