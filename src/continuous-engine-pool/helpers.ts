import type {
  ContinuousEnginePoolAssignment,
  NativeCanvasRef,
} from "./types";

export const BLANK_PAGE_PAYLOAD = '{"pages":{}}';
export const OFFSCREEN_TOP = -100000;

export const getAssignmentKey = (
  assignments: ContinuousEnginePoolAssignment[],
) => {
  return assignments
    .map(({ page, pageIndex }) => [
      pageIndex,
      page.id,
      page.pdfPageNumber ?? "",
      page.rotation ?? 0,
    ].join(":"))
    .join("|");
};

export const getPdfBackgroundUri = (
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

export const waitForNextFrame = () => new Promise<void>((resolve) => {
  requestAnimationFrame(() => resolve());
});

export const loadCanvasDataWithRetry = async (
  canvas: NativeCanvasRef,
  payload: string,
  attempts = 90,
) => {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    if (await canvas.loadBase64Data(payload)) {
      return true;
    }

    await waitForNextFrame();
  }

  return false;
};
