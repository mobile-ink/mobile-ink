import { Platform } from "react-native";
import { normalizePagePayloadForNativeLoad } from "../payload";
import {
  MobileInkBridge,
  MobileInkModule,
} from "./nativeModules";

/**
 * Batch export multiple pages to PNG images natively.
 * This is much faster than exporting pages one by one because it:
 * 1. Creates a single Skia engine and surface (reused for all pages)
 * 2. Doesn't switch visible pages (no UI updates)
 * 3. Processes all pages in a single native call
 *
 * @param pagesData Array of page data objects (JSON format with base64 drawing data)
 * @param backgroundTypes Array of background type strings per page
 * @param width Canvas width in pixels
 * @param height Canvas height in pixels
 * @param scale Export scale factor (e.g., 2.0 for retina)
 * @param pdfBackgroundUri Optional PDF file URI for PDF backgrounds
 * @returns Array of base64 PNG data URIs
 */
export async function batchExportPages(
  pagesData: string[],
  backgroundTypes: string[],
  width: number,
  height: number,
  scale: number = 2.0,
  pdfBackgroundUri?: string,
  pageIndices?: number[],
): Promise<string[]> {
  if (pagesData.length === 0) {
    return [];
  }

  __DEV__ && console.log(`[BatchExport] Starting native batch export of ${pagesData.length} pages at ${width}x${height} scale=${scale}`);
  const startTime = Date.now();

  try {
    const sanitizedPagesData = pagesData.map((pageData, index) => {
      const normalized = normalizePagePayloadForNativeLoad(pageData);
      if (!normalized.isValid) {
        console.warn(`[BatchExport] Replacing invalid page payload at index ${index} with a blank page (${normalized.reasonCode})`);
        return '{"pages":{}}';
      }
      return normalized.normalizedPayload || '{"pages":{}}';
    });
    let results: string[];

    if (Platform.OS === "ios") {
      if (!MobileInkBridge) {
        throw new Error("MobileInkBridge not found. Please rebuild the app.");
      }
      // iOS: batchExportPages(pagesDataArray, backgroundTypes, width, height, scale, pdfUri)
      results = await MobileInkBridge.batchExportPages(
        sanitizedPagesData,
        backgroundTypes,
        width,
        height,
        scale,
        pdfBackgroundUri || "",
        pageIndices || []
      );
    } else {
      if (!MobileInkModule) {
        throw new Error("MobileInkModule not found. Please rebuild the app.");
      }
      // Android: batchExportPages(pagesDataArray, backgroundTypes, width, height, scale, pdfUri)
      results = await MobileInkModule.batchExportPages(
        sanitizedPagesData,
        backgroundTypes,
        width,
        height,
        scale,
        pdfBackgroundUri || ""
      );
    }

    const elapsed = Date.now() - startTime;
    const successCount = results.filter(r => r && r.length > 0).length;
    __DEV__ && console.log(`[BatchExport] Completed ${successCount}/${pagesData.length} pages in ${elapsed}ms`);

    return results;
  } catch (error) {
    console.error("[BatchExport] Native batch export failed:", error);
    throw error;
  }
}

/**
 * Native body-file read + parse.
 *
 * Reads the notebook body file in C++ via NSJSONSerialization and returns
 * the parsed structure to JS. Skips Hermes JSON.parse on a multi-MB string,
 * which is the dominant cost of opening a heavy notebook.
 *
 * Resolves with `null` if the file doesn't exist (caller treats as new file)
 * or if the native fast path isn't available (older build). Rejects on real
 * read/parse errors so the caller can fall back to the slow path.
 *
 * iOS-only: MobileInkBridge ships the parser. Android falls through to
 * the existing JS-side read+parse path.
 */
export async function readBodyFileParsed(
  bodyPath: string,
): Promise<Record<string, unknown> | null> {
  if (Platform.OS !== "ios" || !MobileInkBridge?.readBodyFileParsed) {
    return null;
  }
  try {
    const result = await MobileInkBridge.readBodyFileParsed(bodyPath);
    if (result === null || result === undefined) return null;
    if (typeof result !== "object") return null;
    return result as Record<string, unknown>;
  } catch (error) {
    // Native parse failed -- fall through to JS-side read.
    if (__DEV__) {
      console.warn("[NativeInkCanvas] readBodyFileParsed failed:", error);
    }
    return null;
  }
}

export async function composeContinuousWindow(
  pagePayloads: string[],
  pageHeight: number
): Promise<string> {
  if (Platform.OS !== "ios") {
    throw new Error("Continuous window composition is only available on iOS.");
  }

  if (!MobileInkBridge?.composeContinuousWindow) {
    throw new Error("MobileInkBridge.composeContinuousWindow not found. Please rebuild the app.");
  }

  return MobileInkBridge.composeContinuousWindow(pagePayloads, pageHeight);
}

export async function decomposeContinuousWindow(
  windowPayload: string,
  pageCount: number,
  pageHeight: number
): Promise<string[]> {
  if (Platform.OS !== "ios") {
    throw new Error("Continuous window decomposition is only available on iOS.");
  }

  if (!MobileInkBridge?.decomposeContinuousWindow) {
    throw new Error("MobileInkBridge.decomposeContinuousWindow not found. Please rebuild the app.");
  }

  return MobileInkBridge.decomposeContinuousWindow(windowPayload, pageCount, pageHeight);
}
