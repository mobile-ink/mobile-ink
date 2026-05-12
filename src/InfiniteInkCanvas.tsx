import React, {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useMemo,
  useRef,
  useState,
} from "react";
import {
  StyleProp,
  StyleSheet,
  Text,
  View,
  ViewStyle,
} from "react-native";
import { GestureHandlerRootView } from "react-native-gesture-handler";
import { ContinuousEnginePool } from "./ContinuousEnginePool";
import type {
  ContinuousEnginePoolAssignment,
  ContinuousEnginePoolRef,
  ContinuousEnginePoolSlotRef,
  ContinuousEnginePoolToolState,
} from "./ContinuousEnginePool";
import NativeInkPageBackground from "./NativeInkPageBackground";
import type {
  NativeInkBenchmarkOptions,
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
  NativeInkRenderBackend,
} from "./benchmark";
import {
  aggregateNotebookBenchmarkResults,
  DEFAULT_NATIVE_INK_RENDER_BACKEND,
} from "./benchmark";
import ZoomableInkViewport from "./ZoomableInkViewport";
import type { ZoomableInkViewportRef } from "./ZoomableInkViewport";
import type {
  NativeInkPencilDoubleTapEvent,
  NativeSelectionBounds,
  NotebookPage,
  SerializedNotebookData,
} from "./types";
import { getContinuousEnginePoolRange } from "./utils/continuousEnginePool";
import {
  BLANK_PAGE_PAYLOAD,
  createBlankPage,
  withSingleTrailingBlankPage,
} from "./utils/pageGrowth";
const DEFAULT_PAGE_WIDTH = 820;
const DEFAULT_PAGE_HEIGHT = 1061;
const DEFAULT_INITIAL_PAGE_COUNT = 1;
const DEFAULT_CONTENT_PADDING = 16;

export type InfiniteInkViewportTransform = {
  scale: number;
  translateX: number;
  translateY: number;
  containerWidth: number;
  containerHeight: number;
};

export type InfiniteInkCanvasRef = {
  getNotebookData: () => Promise<SerializedNotebookData>;
  loadNotebookData: (data: SerializedNotebookData | string) => Promise<void>;
  addPage: () => Promise<void>;
  undo: () => void;
  redo: () => void;
  clearCurrentPage: () => void;
  setTool: (toolState: ContinuousEnginePoolToolState) => void;
  resetViewport: (animated?: boolean) => void;
  getCurrentPageIndex: () => number;
  scrollToPage: (pageIndex: number, animated?: boolean) => void;
  runBenchmark?: (options?: NativeInkBenchmarkOptions) => Promise<NativeInkBenchmarkResult>;
  startBenchmarkRecording?: (options?: NativeInkBenchmarkRecordingOptions) => Promise<boolean>;
  stopBenchmarkRecording?: () => Promise<NativeInkBenchmarkResult>;
};

export type InfiniteInkCanvasProps = {
  style?: StyleProp<ViewStyle>;
  initialData?: SerializedNotebookData | string;
  initialPageCount?: number;
  pageWidth?: number;
  pageHeight?: number;
  backgroundType?: string;
  renderBackend?: NativeInkRenderBackend;
  pdfBackgroundBaseUri?: string;
  fingerDrawingEnabled?: boolean;
  toolState: ContinuousEnginePoolToolState;
  minScale?: number;
  maxScale?: number;
  contentPadding?: number;
  showPageLabels?: boolean;
  onReady?: () => void;
  onDrawingChange?: (pageId: string) => void;
  onSelectionChange?: (
    pageId: string,
    count: number,
    bounds: NativeSelectionBounds | null,
  ) => void;
  onCurrentPageChange?: (pageIndex: number) => void;
  onPagesChange?: (pages: NotebookPage[]) => void;
  onMotionStateChange?: (isMoving: boolean) => void;
  onPencilDoubleTap?: (event: NativeInkPencilDoubleTapEvent) => void;
};

const clonePage = (page: NotebookPage, pageIndex: number): NotebookPage => ({
  ...page,
  id: page.id || `page-${pageIndex + 1}`,
  title: page.title || `Page ${pageIndex + 1}`,
  data: page.data || BLANK_PAGE_PAYLOAD,
  rotation: page.rotation ?? 0,
});

const parseNotebookData = (
  data: SerializedNotebookData | string | undefined,
): SerializedNotebookData | null => {
  if (!data) {
    return null;
  }

  if (typeof data !== "string") {
    return data;
  }

  const parsed = JSON.parse(data) as SerializedNotebookData;
  if (!Array.isArray(parsed.pages)) {
    throw new Error("Invalid mobile-ink notebook data: pages must be an array.");
  }
  return parsed;
};

const createInitialPages = (
  initialData: SerializedNotebookData | string | undefined,
  initialPageCount: number,
) => {
  const parsed = parseNotebookData(initialData);
  if (parsed?.pages.length) {
    return withSingleTrailingBlankPage(parsed.pages.map(clonePage));
  }

  return withSingleTrailingBlankPage(Array.from(
    { length: Math.max(1, initialPageCount) },
    (_, pageIndex) => createBlankPage(pageIndex),
  ));
};

const getVisiblePageIndex = (
  transform: InfiniteInkViewportTransform,
  pageHeight: number,
  pageCount: number,
  contentPadding: number,
) => {
  const scale = Math.max(transform.scale, 0.0001);
  const visibleTopY = Math.max(0, (-transform.translateY) / scale - contentPadding);
  const visibleCenterY = visibleTopY + transform.containerHeight / (2 * scale);
  return Math.max(
    0,
    Math.min(pageCount - 1, Math.floor(visibleCenterY / pageHeight)),
  );
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

  const updatePageData = useCallback((pageId: string, data: string) => {
    if (!data) {
      return;
    }

    const nextPages = pagesRef.current.map((page) => (
      page.id === pageId ? { ...page, data } : page
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
      const pageIndex = nextPages.findIndex((page) => page.id === pageId);
      if (pageIndex === -1 || !data) {
        continue;
      }

      nextPages[pageIndex] = { ...nextPages[pageIndex], data };
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
            ? { ...candidatePage, data: BLANK_PAGE_PAYLOAD }
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

  const pageBackgrounds = useMemo(() => pages.map((page, pageIndex) => {
    const pdfBackgroundUri = backgroundType === "pdf" && pdfBackgroundBaseUri
      ? `${pdfBackgroundBaseUri}#page=${page.pdfPageNumber || pageIndex + 1}`
      : undefined;

    return (
      <View
        key={`mobile-ink-page-background-${page.id}`}
        pointerEvents="none"
        style={[
          styles.page,
          {
            top: pageIndex * pageHeight,
            width: pageWidth,
            height: pageHeight,
          },
        ]}
      >
        <NativeInkPageBackground
          style={StyleSheet.absoluteFillObject}
          backgroundType={backgroundType}
          pdfBackgroundUri={pdfBackgroundUri}
        />
        {showPageLabels ? (
          <Text style={styles.pageLabel}>{pageIndex + 1}</Text>
        ) : null}
      </View>
    );
  }), [
    backgroundType,
    pageHeight,
    pageWidth,
    pages,
    pdfBackgroundBaseUri,
    showPageLabels,
  ]);

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
            {pageBackgrounds}
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
            {pages.slice(1).map((page, pageIndex) => (
              <View
                key={`mobile-ink-page-break-${page.id}`}
                pointerEvents="none"
                style={[
                  styles.pageBreak,
                  { top: (pageIndex + 1) * pageHeight - StyleSheet.hairlineWidth / 2 },
                ]}
              />
            ))}
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

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: "#F4F6F8",
  },
  viewport: {
    flex: 1,
    overflow: "hidden",
  },
  canvasShell: {
    flex: 1,
    alignItems: "center",
    justifyContent: "flex-start",
    zIndex: 1,
  },
  page: {
    position: "absolute",
    left: 0,
    overflow: "hidden",
    backgroundColor: "#FFFFFF",
  },
  pageLabel: {
    position: "absolute",
    top: 12,
    right: 14,
    color: "rgba(71, 85, 105, 0.32)",
    fontSize: 13,
    fontWeight: "700",
  },
  pageBreak: {
    position: "absolute",
    left: 0,
    right: 0,
    height: StyleSheet.hairlineWidth,
    backgroundColor: "rgba(100, 116, 139, 0.18)",
    zIndex: 3,
  },
});

export default InfiniteInkCanvas;
