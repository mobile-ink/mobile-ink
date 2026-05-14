import type { NotebookPage, SerializedNotebookData } from "../types";
import {
  BLANK_PAGE_PAYLOAD,
  createBlankPage,
  withSingleTrailingBlankPage,
} from "../utils/pageGrowth";
import type { InfiniteInkViewportTransform } from "./types";

export const clonePage = (page: NotebookPage, pageIndex: number): NotebookPage => ({
  ...page,
  id: page.id || `page-${pageIndex + 1}`,
  title: page.title || `Page ${pageIndex + 1}`,
  data: page.data || BLANK_PAGE_PAYLOAD,
  rotation: page.rotation ?? 0,
});

export const parseNotebookData = (
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

export const createInitialPages = (
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

export const getVisiblePageIndex = (
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
