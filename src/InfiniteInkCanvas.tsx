import React, {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useRef,
  useState,
} from "react";
import { View } from "react-native";
import { GestureHandlerRootView } from "react-native-gesture-handler";
import { ContinuousEnginePool } from "./ContinuousEnginePool";
import type {
  ContinuousEnginePoolAssignment,
  ContinuousEnginePoolRef,
  ContinuousEnginePoolSlotRef,
  ContinuousEnginePoolToolState,
} from "./ContinuousEnginePool";
import type { NativeInkBenchmarkRecordingOptions } from "./benchmark";
import {
  aggregateNotebookBenchmarkResults,
  DEFAULT_NATIVE_INK_RENDER_BACKEND,
} from "./benchmark";
import ZoomableInkViewport from "./ZoomableInkViewport";
import type { ZoomableInkViewportRef } from "./ZoomableInkViewport";
import type { NativeSelectionBounds, NotebookPage } from "./types";
import {
  DEFAULT_CONTENT_PADDING,
  DEFAULT_INITIAL_PAGE_COUNT,
  DEFAULT_PAGE_HEIGHT,
  DEFAULT_PAGE_WIDTH,
} from "./infinite-ink-canvas/constants";
import {
  clonePage,
  createInitialPages,
  getVisiblePageIndex,
  parseNotebookData,
} from "./infinite-ink-canvas/notebookPages";
import { PageBackgrounds, PageBreaks } from "./infinite-ink-canvas/PageStack";
import { styles } from "./infinite-ink-canvas/styles";
import type {
  InfiniteInkCanvasProps,
  InfiniteInkCanvasRef,
  InfiniteInkViewportTransform,
} from "./infinite-ink-canvas/types";
import { getContinuousEnginePoolRange } from "./utils/continuousEnginePool";
import { computeDataSignature } from "./utils/dataSignature";
import {
  BLANK_PAGE_PAYLOAD,
  withSingleTrailingBlankPage,
} from "./utils/pageGrowth";

export type {
  InfiniteInkCanvasProps,
  InfiniteInkCanvasRef,
  InfiniteInkViewportTransform,
} from "./infinite-ink-canvas/types";

const PAGE_PREVIEW_CAPTURE_SCALE = 0.25;

const withCapturedPageData = (
  page: NotebookPage,
  data: string,
  previewUri?: string,
): NotebookPage => {
  const dataSignature = computeDataSignature(data);
  const hasFreshPreview = Boolean(previewUri);
  const hasMatchingPreview = page.previewDataSignature === dataSignature;

  return {
    ...page,
    data,
    dataSignature,
    previewUri: hasFreshPreview
      ? previewUri
      : hasMatchingPreview
        ? page.previewUri
        : undefined,
    previewDataSignature: hasFreshPreview || hasMatchingPreview
      ? dataSignature
      : undefined,
  };
};

function InfiniteInkCanvasImpl(
  {
    style,
    initialData,
    initialPageCount = DEFAULT_INITIAL_PAGE_COUNT,
    pageWidth = DEFAULT_PAGE_WIDTH,
    pageHeight = DEFAULT_PAGE_HEIGHT,
    backgroundType = "plain",
    renderBackend = DEFAULT_NATIVE_INK_RENDER_BACKEND,
    pdfBackgroundBaseUri,
    fingerDrawingEnabled = false,
    toolState,
    minScale = 1,
    maxScale = 5,
    contentPadding = DEFAULT_CONTENT_PADDING,
    showPageLabels = true,
    onReady,
    onDrawingChange,
    onSelectionChange,
    onCurrentPageChange,
    onPagesChange,
    onMotionStateChange,
    onPencilDoubleTap,
  }: InfiniteInkCanvasProps,
  ref: React.Ref<InfiniteInkCanvasRef>,
) {
  const [pages, setPages] = useState<NotebookPage[]>(() =>
    createInitialPages(initialData, initialPageCount),
  );
  const pagesRef = useRef(pages);
  const currentPageIndexRef = useRef(0);
  const toolStateRef = useRef(toolState);
  const isMovingRef = useRef(false);
  const latestTransformRef = useRef<InfiniteInkViewportTransform | null>(null);
  const enginePoolRef = useRef<ContinuousEnginePoolRef | null>(null);
  const viewportRef = useRef<ZoomableInkViewportRef | null>(null);
  const perPageSlotRefs = useRef(new Map<string, ContinuousEnginePoolSlotRef>());
  const dirtyPageIdsRef = useRef(new Set<string>());
  const lastEditedPageIdRef = useRef<string | null>(null);
  const nativeReadyRef = useRef(false);
  const benchmarkRecordingOptionsRef = useRef<NativeInkBenchmarkRecordingOptions | null>(null);

  const contentHeight = pages.length * pageHeight;

  useEffect(() => {
    toolStateRef.current = toolState;
    enginePoolRef.current?.applyToolState(toolState);
  }, [toolState]);

  const replacePages = useCallback((nextPages: NotebookPage[]) => {
    pagesRef.current = nextPages;
    if (currentPageIndexRef.current >= nextPages.length) {
      currentPageIndexRef.current = Math.max(0, nextPages.length - 1);
      onCurrentPageChange?.(currentPageIndexRef.current);
    }
    setPages(nextPages);
    onPagesChange?.(nextPages);
  }, [onCurrentPageChange, onPagesChange]);

  const getToolState = useCallback(() => toolStateRef.current, []);

  const buildAssignmentsForIndex = useCallback((pageIndex: number) => {
    const sourcePages = pagesRef.current;
    const range = getContinuousEnginePoolRange(pageIndex, sourcePages.length);
    const assignments: ContinuousEnginePoolAssignment[] = [];
    for (let index = range.startIndex; index <= range.endIndex; index += 1) {
      const page = sourcePages[index];
      if (page) {
        assignments.push({ page, pageIndex: index });
      }
    }
    return assignments;
  }, []);

  const assignEnginesToPage = useCallback(async (pageIndex: number) => {
    await enginePoolRef.current?.assignPages(buildAssignmentsForIndex(pageIndex));
  }, [buildAssignmentsForIndex]);

  const setCurrentPage = useCallback((pageIndex: number) => {
    if (pageIndex === currentPageIndexRef.current) {
      return;
    }

    currentPageIndexRef.current = pageIndex;
    onCurrentPageChange?.(pageIndex);
  }, [onCurrentPageChange]);

  const ensureSingleTrailingBlankPage = useCallback(() => {
    const nextPages = withSingleTrailingBlankPage(
      pagesRef.current,
      dirtyPageIdsRef.current,
    );
    if (nextPages !== pagesRef.current) {
      replacePages(nextPages);
    }
    return nextPages;
  }, [replacePages]);

  const commitViewportPage = useCallback((pageIndex: number) => {
    const pageCount = pagesRef.current.length;
    const boundedPageIndex = Math.max(0, Math.min(pageCount - 1, pageIndex));
    setCurrentPage(boundedPageIndex);
    void assignEnginesToPage(boundedPageIndex);
  }, [assignEnginesToPage, setCurrentPage]);

  const commitLatestViewportPage = useCallback(() => {
    const transform = latestTransformRef.current;
    if (!transform) {
      commitViewportPage(currentPageIndexRef.current);
      return;
    }

    commitViewportPage(
      getVisiblePageIndex(
        transform,
        pageHeight,
        pagesRef.current.length,
        contentPadding,
      ),
    );
  }, [commitViewportPage, contentPadding, pageHeight]);

  const handleTransformChange = useCallback((transform: InfiniteInkViewportTransform) => {
    latestTransformRef.current = transform;
    if (isMovingRef.current) {
      return;
    }

    commitViewportPage(
      getVisiblePageIndex(
        transform,
        pageHeight,
        pagesRef.current.length,
        contentPadding,
      ),
    );
  }, [commitViewportPage, contentPadding, pageHeight]);

  const handleMotionStateChange = useCallback((isMoving: boolean) => {
    isMovingRef.current = isMoving;
    onMotionStateChange?.(isMoving);
    if (!isMoving) {
      commitLatestViewportPage();
    }
  }, [commitLatestViewportPage, onMotionStateChange]);

  const registerPerPageSlot = useCallback((
    pageId: string,
    slotRef: ContinuousEnginePoolSlotRef | null,
    sourceRef?: ContinuousEnginePoolSlotRef,
  ) => {
    if (!slotRef) {
      const currentRef = perPageSlotRefs.current.get(pageId);
      if (!sourceRef || currentRef === sourceRef) {
        perPageSlotRefs.current.delete(pageId);
      }
      return;
    }

    perPageSlotRefs.current.set(pageId, slotRef);
  }, []);

  const shouldCaptureBeforeReassign = useCallback((pageId: string) => {
    return dirtyPageIdsRef.current.has(pageId);
  }, []);

  const updatePageData = useCallback((
    pageId: string,
    data: string,
    previewUri?: string,
  ) => {
    if (!data) {
      return;
    }

    const nextPages = pagesRef.current.map((page) => (
      page.id === pageId ? withCapturedPageData(page, data, previewUri) : page
    ));
    dirtyPageIdsRef.current.delete(pageId);
    replacePages(withSingleTrailingBlankPage(nextPages, dirtyPageIdsRef.current));
  }, [replacePages]);

  const handleDrawingChange = useCallback((pageId: string) => {
    dirtyPageIdsRef.current.add(pageId);
    lastEditedPageIdRef.current = pageId;
    ensureSingleTrailingBlankPage();
    onDrawingChange?.(pageId);
  }, [ensureSingleTrailingBlankPage, onDrawingChange]);

  const handleSelectionChange = useCallback((
    pageId: string,
    count: number,
    bounds: NativeSelectionBounds | null,
  ) => {
    onSelectionChange?.(pageId, count, bounds);
  }, [onSelectionChange]);

  const getActiveSlot = useCallback(() => {
    const lastEditedPageId = lastEditedPageIdRef.current;
    if (lastEditedPageId) {
      const lastEditedSlot = perPageSlotRefs.current.get(lastEditedPageId);
      if (lastEditedSlot) {
        return lastEditedSlot;
      }
    }

    const activePage = pagesRef.current[currentPageIndexRef.current];
    return activePage ? perPageSlotRefs.current.get(activePage.id) : undefined;
  }, []);

  const getCurrentPageSlot = useCallback(() => {
    const activePage = pagesRef.current[currentPageIndexRef.current];
    return activePage ? perPageSlotRefs.current.get(activePage.id) : undefined;
  }, []);

  const captureDirtyPages = useCallback(async () => {
    const nextPages = [...pagesRef.current];
    let didChange = false;

    for (const pageId of Array.from(dirtyPageIdsRef.current)) {
      const slotRef = perPageSlotRefs.current.get(pageId);
      if (!slotRef) {
        continue;
      }

      const data = await slotRef.getBase64Data();
      const previewUri = await slotRef.getPreviewData(PAGE_PREVIEW_CAPTURE_SCALE);
      const pageIndex = nextPages.findIndex((page) => page.id === pageId);
      if (pageIndex === -1 || !data) {
        continue;
      }

      nextPages[pageIndex] = withCapturedPageData(
        nextPages[pageIndex],
        data,
        previewUri ?? undefined,
      );
      dirtyPageIdsRef.current.delete(pageId);
      didChange = true;
    }

    const normalizedPages = withSingleTrailingBlankPage(
      nextPages,
      dirtyPageIdsRef.current,
    );

    if (didChange || normalizedPages !== pagesRef.current) {
      replacePages(normalizedPages);
    }

    return normalizedPages;
  }, [replacePages]);

  const scrollToPage = useCallback((pageIndex: number, animated = true) => {
    const boundedPageIndex = Math.max(
      0,
      Math.min(pagesRef.current.length - 1, pageIndex),
    );
    setCurrentPage(boundedPageIndex);
    void assignEnginesToPage(boundedPageIndex);
    viewportRef.current?.setTransform({
      scale: 1,
      translateX: 0,
      translateY: -(boundedPageIndex * pageHeight + contentPadding),
      animated,
    });
  }, [assignEnginesToPage, contentPadding, pageHeight, setCurrentPage]);

  const addPage = useCallback(async () => {
    const capturedPages = await captureDirtyPages();
    const trailingBlankPageIndex = Math.max(0, capturedPages.length - 1);
    setCurrentPage(trailingBlankPageIndex);
    scrollToPage(trailingBlankPageIndex, true);
    void assignEnginesToPage(trailingBlankPageIndex);
  }, [
    assignEnginesToPage,
    captureDirtyPages,
    scrollToPage,
    setCurrentPage,
  ]);

  useImperativeHandle(ref, () => ({
    getNotebookData: async () => ({
      version: "1.0",
      pages: await captureDirtyPages(),
      originalCanvasWidth: pageWidth,
    }),
    loadNotebookData: async (data) => {
      const parsed = parseNotebookData(data);
      if (!parsed?.pages.length) {
        throw new Error("Cannot load empty mobile-ink notebook data.");
      }

      const nextPages = withSingleTrailingBlankPage(parsed.pages.map(clonePage));
      dirtyPageIdsRef.current.clear();
      perPageSlotRefs.current.clear();
      replacePages(nextPages);
      setCurrentPage(0);
      viewportRef.current?.setTransform({
        scale: 1,
        translateX: 0,
        translateY: 0,
        animated: false,
      });
      await assignEnginesToPage(0);
    },
    addPage,
    undo: () => getActiveSlot()?.undo(),
    redo: () => getActiveSlot()?.redo(),
    clearCurrentPage: () => {
      const page = pagesRef.current[currentPageIndexRef.current];
      if (page) {
        dirtyPageIdsRef.current.delete(page.id);
        getActiveSlot()?.clear();
        const nextPages = pagesRef.current.map((candidatePage) => (
          candidatePage.id === page.id
            ? {
                ...candidatePage,
                data: BLANK_PAGE_PAYLOAD,
                dataSignature: computeDataSignature(BLANK_PAGE_PAYLOAD),
                previewUri: undefined,
                previewDataSignature: undefined,
              }
            : candidatePage
        ));
        replacePages(withSingleTrailingBlankPage(nextPages, dirtyPageIdsRef.current));
      }
    },
    setTool: (nextToolState) => {
      toolStateRef.current = nextToolState;
      enginePoolRef.current?.applyToolState(nextToolState);
    },
    resetViewport: (animated = true) => {
      if (animated) {
        viewportRef.current?.resetZoomAnimated();
      } else {
        viewportRef.current?.resetZoom();
      }
    },
    getCurrentPageIndex: () => currentPageIndexRef.current,
    scrollToPage,
    runBenchmark: async (options) => {
      const activeSlot = getCurrentPageSlot();
      if (!activeSlot?.runBenchmark) {
        throw new Error("Native benchmark runner is unavailable for the active page.");
      }
      return activeSlot.runBenchmark(options);
    },
    startBenchmarkRecording: async (options) => {
      if (!enginePoolRef.current?.startBenchmarkRecording) {
        throw new Error("Native benchmark recorder is unavailable for this notebook.");
      }
      benchmarkRecordingOptionsRef.current = options ?? {};
      const didStart = await enginePoolRef.current.startBenchmarkRecording(options);
      if (!didStart) {
        benchmarkRecordingOptionsRef.current = null;
      }
      return didStart;
    },
    stopBenchmarkRecording: async () => {
      if (!enginePoolRef.current?.stopBenchmarkRecording) {
        throw new Error("Native benchmark recorder is unavailable for this notebook.");
      }
      const recordingOptions = benchmarkRecordingOptionsRef.current ?? {
        scenario: "manual-notebook",
        backend: renderBackend,
      };
      benchmarkRecordingOptionsRef.current = null;
      const results = await enginePoolRef.current.stopBenchmarkRecording();
      return aggregateNotebookBenchmarkResults(results, recordingOptions);
    },
  }), [
    addPage,
    assignEnginesToPage,
    captureDirtyPages,
    getActiveSlot,
    getCurrentPageSlot,
    pageWidth,
    replacePages,
    renderBackend,
    scrollToPage,
    setCurrentPage,
  ]);

  useEffect(() => {
    if (nativeReadyRef.current) {
      void assignEnginesToPage(currentPageIndexRef.current);
    }
  }, [assignEnginesToPage, pages.length]);

  const handleCanvasReady = useCallback(() => {
    nativeReadyRef.current = true;
    onReady?.();
    void assignEnginesToPage(currentPageIndexRef.current);
  }, [assignEnginesToPage, onReady]);

  return (
    <GestureHandlerRootView style={[styles.root, style]}>
      <ZoomableInkViewport
        ref={viewportRef}
        style={styles.viewport}
        minScale={minScale}
        maxScale={maxScale}
        contentWidth={pageWidth}
        contentHeight={contentHeight}
        contentPadding={contentPadding}
        isLandscape={true}
        fingerDrawingEnabled={fingerDrawingEnabled}
        enableMomentumScroll={true}
        lockHorizontalPanNearFit={true}
        transformNotificationMinIntervalMs={80}
        onTransformChange={handleTransformChange}
        onMotionStateChange={handleMotionStateChange}
      >
        <View style={[styles.canvasShell, { paddingTop: contentPadding, paddingBottom: contentPadding }]}>
          <View
            style={{
              width: pageWidth,
              height: contentHeight,
            }}
          >
            <PageBackgrounds
              pages={pages}
              pageWidth={pageWidth}
              pageHeight={pageHeight}
              backgroundType={backgroundType}
              pdfBackgroundBaseUri={pdfBackgroundBaseUri}
              showPageLabels={showPageLabels}
            />
            <ContinuousEnginePool
              ref={enginePoolRef}
              canvasHeight={pageHeight}
              backgroundType={backgroundType}
              renderBackend={renderBackend}
              pdfBackgroundBaseUri={pdfBackgroundBaseUri}
              fingerDrawingEnabled={fingerDrawingEnabled}
              getToolState={getToolState}
              onCanvasReady={handleCanvasReady}
              onPerPageDrawingChange={handleDrawingChange}
              onPerPageSelectionChange={handleSelectionChange}
              onPencilDoubleTap={onPencilDoubleTap}
              registerPerPageSlot={registerPerPageSlot}
              shouldCaptureBeforeReassign={shouldCaptureBeforeReassign}
              onSlotCaptureBeforeUnmount={updatePageData}
            />
            <PageBreaks pages={pages} pageHeight={pageHeight} />
          </View>
        </View>
      </ZoomableInkViewport>
    </GestureHandlerRootView>
  );
}

export const InfiniteInkCanvas = forwardRef<InfiniteInkCanvasRef, InfiniteInkCanvasProps>(
  InfiniteInkCanvasImpl,
);

InfiniteInkCanvas.displayName = "InfiniteInkCanvas";

export default InfiniteInkCanvas;
