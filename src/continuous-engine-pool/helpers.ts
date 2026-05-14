import type { ContinuousEnginePoolAssignment } from "./types";

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
