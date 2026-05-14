import { NotebookPage, SerializedNotebookData, TextBox } from '../types';
import { computeDataSignature } from './dataSignature';

/**
 * Serialization utilities for notebook data persistence.
 * Handles conversion between NotebookPage[] and JSON strings for storage.
 */

/**
 * Serializes notebook pages to JSON string for storage
 * @param pages - Array of notebook pages to serialize
 * @param canvasWidth - Current canvas width (stored for cross-device scaling)
 */
export const serializeNotebookData = (pages: NotebookPage[], canvasWidth?: number): string => {
  const data: SerializedNotebookData = {
    version: "1.0",
    pages: pages.map((p) => ({
      id: p.id,
      title: p.title,
      data: p.data,
      dataSignature: p.dataSignature,
      previewUri: p.previewUri,
      previewDataSignature: p.previewDataSignature,
      rotation: p.rotation,
      textBoxes: p.textBoxes,
      insertedElements: p.insertedElements,
      pdfPageNumber: p.pdfPageNumber,
      pageType: p.pageType,
      graphState: p.graphState,
    })),
    originalCanvasWidth: canvasWidth,
  };
  return JSON.stringify(data);
};

export interface DeserializedNotebookData {
  pages: NotebookPage[];
  originalCanvasWidth?: number;
}

export interface PdfNotebookReconciliationResult extends DeserializedNotebookData {
  notebookData: string;
  changed: boolean;
  reasonCode: string;
}

function createBlankPdfPages(startIndex: number, pageCount: number): NotebookPage[] {
  return Array.from({ length: pageCount }, (_, index) => {
    const pageNumber = startIndex + index + 1;
    return {
      id: `page-${pageNumber}`,
      title: `Page ${pageNumber}`,
      data: '',
      rotation: 0,
      pdfPageNumber: pageNumber,
    };
  });
}

/**
 * Build the in-memory pages structure from an already-parsed notebook
 * object. Pulled out of deserializeNotebookData so the native fast-load
 * path (readBodyFileParsed -> structured object) can share the same
 * migration logic without paying a JSON.parse round-trip on the JS side.
 */
export const buildNotebookFromParsed = (
  data: SerializedNotebookData | null | undefined,
): DeserializedNotebookData => {
  if (data && data.version === "1.0" && Array.isArray(data.pages)) {
    // Migration: If pages don't have pdfPageNumber but there are multiple pages,
    // assume it's a PDF and assign sequential page numbers
    const pages = data.pages.map((page, index) => {
      let next = page;
      if (page.pdfPageNumber === undefined && data.pages.length > 1) {
        next = { ...next, pdfPageNumber: index + 1 };
      }
      // Seed dataSignature so the preview cache can compare across the
      // session even after the page's data is evicted to disk.
      if (!next.dataSignature && next.data) {
        next = { ...next, dataSignature: computeDataSignature(next.data) };
      }
      return next;
    });
    return { pages, originalCanvasWidth: data.originalCanvasWidth };
  }
  return { pages: [] };
};

/**
 * Deserializes JSON string back to notebook pages
 * Includes migration logic for backward compatibility
 * @returns Object with pages array and optional originalCanvasWidth for scaling
 */
export const deserializeNotebookData = (json: string): DeserializedNotebookData => {
  try {
    const data = JSON.parse(json) as SerializedNotebookData;
    return buildNotebookFromParsed(data);
  } catch (err) {
    const errorMsg = err instanceof Error ? err.message : 'Parse error';
    console.error("Failed to parse notebook data:", errorMsg, "Data length:", json?.length || 0);
    return { pages: [] };
  }
};

/**
 * Reconciles detected PDF page count with an existing notebook.
 * Existing pages are never replaced or removed; only missing PDF page shells are appended.
 */
export const reconcilePdfNotebookPageCount = (
  existingNotebookData: string | null | undefined,
  detectedPageCount: number,
): PdfNotebookReconciliationResult => {
  const safePageCount = Math.max(1, Math.floor(detectedPageCount) || 1);
  const trimmedNotebookData = existingNotebookData?.trim() ?? '';

  if (!trimmedNotebookData) {
    const pages = createBlankPdfPages(0, safePageCount);
    return {
      notebookData: serializeNotebookData(pages),
      pages,
      changed: true,
      reasonCode: 'created_pdf_placeholders',
    };
  }

  const { pages, originalCanvasWidth } = deserializeNotebookData(trimmedNotebookData);
  if (pages.length >= safePageCount) {
    return {
      notebookData: trimmedNotebookData,
      pages,
      originalCanvasWidth,
      changed: false,
      reasonCode: 'existing_pages_sufficient',
    };
  }

  const missingPageCount = safePageCount - pages.length;
  const reconciledPages = [
    ...pages,
    ...createBlankPdfPages(pages.length, missingPageCount),
  ];

  return {
    notebookData: serializeNotebookData(reconciledPages, originalCanvasWidth),
    pages: reconciledPages,
    originalCanvasWidth,
    changed: true,
    reasonCode: pages.length > 0 ? 'appended_missing_pdf_pages' : 'created_pdf_placeholders',
  };
};

/**
 * Helper function to composite text boxes onto a canvas base64 image
 * Currently returns the original image as text boxes are rendered in PDF HTML template
 *
 * @param base64Image - Base64 encoded image string
 * @param textBoxes - Array of text boxes to composite (currently unused)
 * @param scale - Scaling factor for rendering (currently unused)
 * @returns Base64 encoded image string
 */
export const compositeTextBoxesOnImage = async (
  base64Image: string,
  textBoxes: TextBox[] | undefined,
  scale: number = 2.0
): Promise<string> => {
  // If no text boxes, return original image
  if (!textBoxes || textBoxes.length === 0) {
    return base64Image;
  }

  // Note: Text boxes will be rendered in the HTML template for PDF export
  // For now, we return the base image and will handle text box rendering
  // in the PDF export HTML template instead of compositing here
  // This is simpler and preserves text quality
  return base64Image;
};
