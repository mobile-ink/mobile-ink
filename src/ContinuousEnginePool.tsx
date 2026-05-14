import React, {
  forwardRef,
  memo,
  useCallback,
  useImperativeHandle,
  useRef,
} from "react";
import { DEFAULT_NATIVE_INK_RENDER_BACKEND } from "./benchmark";
import type {
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
} from "./benchmark";
import {
  getAssignmentKey,
} from "./continuous-engine-pool/helpers";
import { PooledCanvasSlot } from "./continuous-engine-pool/PooledCanvasSlot";
import type {
  ContinuousEnginePoolAssignment,
  ContinuousEnginePoolProps,
  ContinuousEnginePoolRef,
  ContinuousEnginePoolToolState,
  PooledCanvasSlotHandle,
} from "./continuous-engine-pool/types";
import { CONTINUOUS_ENGINE_POOL_SIZE } from "./utils/continuousEnginePool";

export type {
  ContinuousEnginePoolAssignment,
  ContinuousEnginePoolProps,
  ContinuousEnginePoolRef,
  ContinuousEnginePoolSlotRef,
  ContinuousEnginePoolToolState,
} from "./continuous-engine-pool/types";

export const ContinuousEnginePool = memo(forwardRef<
  ContinuousEnginePoolRef,
  ContinuousEnginePoolProps
>(function ContinuousEnginePool({
  canvasHeight,
  backgroundType,
  renderBackend = DEFAULT_NATIVE_INK_RENDER_BACKEND,
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
  const benchmarkRecordingOptionsRef = useRef<NativeInkBenchmarkRecordingOptions | null>(null);
  const benchmarkRecordingSlotIndexesRef = useRef(new Set<number>());

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

  const startSlotBenchmarkRecording = useCallback(async (
    poolIndex: number,
    slotRef: PooledCanvasSlotHandle | null | undefined,
    options: NativeInkBenchmarkRecordingOptions,
  ) => {
    if (!slotRef || benchmarkRecordingSlotIndexesRef.current.has(poolIndex)) {
      return false;
    }

    const didStart = await slotRef.startBenchmarkRecording(options);
    if (didStart) {
      benchmarkRecordingSlotIndexesRef.current.add(poolIndex);
    }
    return didStart;
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
    const benchmarkRecordingOptions = benchmarkRecordingOptionsRef.current;
    if (benchmarkRecordingOptions) {
      await Promise.all(
        nextSlotAssignments.map((assignment, poolIndex) => (
          assignment
            ? startSlotBenchmarkRecording(
                poolIndex,
                slotRefs.current[poolIndex],
                benchmarkRecordingOptions,
              )
            : Promise.resolve(false)
        )),
      );
    }
    if (assignmentKeyRef.current === nextAssignmentKey) {
      onAssignmentReadyRef.current?.(nextAssignmentKey);
    }
  }, [
    backgroundType,
    canvasHeight,
    pdfBackgroundBaseUri,
    startSlotBenchmarkRecording,
  ]);

  const applyToolState = useCallback((toolState: ContinuousEnginePoolToolState) => {
    for (const slotRef of slotRefs.current) {
      slotRef?.applyToolState(toolState);
    }
  }, []);

  const startBenchmarkRecording = useCallback(async (
    options: NativeInkBenchmarkRecordingOptions = {},
  ) => {
    if (benchmarkRecordingOptionsRef.current) {
      return true;
    }

    benchmarkRecordingOptionsRef.current = options;
    benchmarkRecordingSlotIndexesRef.current.clear();
    const starts = await Promise.all(
      slotRefs.current.map((slotRef, poolIndex) => (
        startSlotBenchmarkRecording(poolIndex, slotRef, options)
      )),
    );

    const didStartAny = starts.some(Boolean);
    if (!didStartAny) {
      benchmarkRecordingOptionsRef.current = null;
      benchmarkRecordingSlotIndexesRef.current.clear();
    }
    return didStartAny;
  }, [startSlotBenchmarkRecording]);

  const stopBenchmarkRecording = useCallback(async () => {
    const recordingSlotIndexes = Array.from(
      benchmarkRecordingSlotIndexesRef.current,
    );
    benchmarkRecordingOptionsRef.current = null;
    benchmarkRecordingSlotIndexesRef.current.clear();

    const results = await Promise.all(
      recordingSlotIndexes.map((poolIndex) => (
        slotRefs.current[poolIndex]?.stopBenchmarkRecording() ?? Promise.resolve(null)
      )),
    );

    return results.filter(
      (result): result is NativeInkBenchmarkResult => result !== null,
    );
  }, []);

  const release = useCallback(async () => {
    benchmarkRecordingOptionsRef.current = null;
    benchmarkRecordingSlotIndexesRef.current.clear();
    await Promise.all(
      slotRefs.current.map((slotRef) => slotRef?.release() ?? Promise.resolve()),
    );
  }, []);

  useImperativeHandle(ref, () => ({
    assignPages,
    applyToolState,
    startBenchmarkRecording,
    stopBenchmarkRecording,
    release,
  }), [
    applyToolState,
    assignPages,
    release,
    startBenchmarkRecording,
    stopBenchmarkRecording,
  ]);

  return (
    <>
      {Array.from({ length: CONTINUOUS_ENGINE_POOL_SIZE }, (_, poolIndex) => (
        <PooledCanvasSlot
          key={`continuous-engine-pool-${poolIndex}`}
          ref={(slotRef) => handleSlotRef(poolIndex, slotRef)}
          poolIndex={poolIndex}
          canvasHeight={canvasHeight}
          backgroundType={backgroundType}
          renderBackend={renderBackend}
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
  prev.renderBackend === next.renderBackend &&
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
