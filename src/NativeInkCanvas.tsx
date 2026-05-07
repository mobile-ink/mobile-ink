import React, {
  useRef,
  useImperativeHandle,
  forwardRef,
  useEffect,
  useCallback,
  useState,
} from 'react';
import {
  requireNativeComponent,
  UIManager,
  findNodeHandle,
  Platform,
  ViewStyle,
  NativeModules,
  NativeSyntheticEvent,
} from 'react-native';
import type { NativeSelectionBounds } from './types';
import { normalizePagePayloadForNativeLoad } from "./payload";

// NativeModules for callback-based methods
// iOS uses MobileInkCanvasViewManager (ViewManager methods)
// Android uses MobileInkModule (separate module with promise-based API)
const MobileInkCanvasViewManager = Platform.OS === 'ios'
  ? NativeModules.MobileInkCanvasViewManager
  : null;
const MobileInkModule = Platform.OS === 'android'
  ? NativeModules.MobileInkModule
  : null;
// MobileInkBridge for iOS batch export (static methods not tied to view)
const MobileInkBridge = Platform.OS === 'ios'
  ? NativeModules.MobileInkBridge
  : null;

if (__DEV__) {
  if (Platform.OS === 'ios' && !MobileInkCanvasViewManager) {
    console.warn('[NativeInkCanvas] MobileInkCanvasViewManager not found in NativeModules. Drawing serialization may not work.');
  }
  if (Platform.OS === 'android' && !MobileInkModule) {
    console.warn('[NativeInkCanvas] MobileInkModule not found in NativeModules. Drawing serialization may not work.');
  }
}

const LINKING_ERROR =
  `The package 'MobileInkCanvasView' doesn't seem to be linked. Make sure: \n\n` +
  Platform.select({ ios: "- You have run 'pod install'\n", default: '' }) +
  '- You rebuilt the app after installing the package\n' +
  '- You are not using Expo Go\n';

const ComponentName = 'MobileInkCanvasView';

const MobileInkCanvasViewNative =
  UIManager.getViewManagerConfig(ComponentName) != null
    ? requireNativeComponent<any>(ComponentName)
    : () => {
        throw new Error(LINKING_ERROR);
      };

export interface NativeInkCanvasProps {
  style?: ViewStyle;
  onDrawingChange?: () => void;
  onDrawingBegin?: (event: NativeSyntheticEvent<{ x: number; y: number }>) => void;
  onSelectionChange?: (event: { nativeEvent: { count: number; bounds?: NativeSelectionBounds | null } }) => void;
  onCanvasReady?: () => void;
  backgroundType?: string;
  pdfBackgroundUri?: string;
  renderSuspended?: boolean;
  /** iOS only: Controls whether fingers or only Apple Pencil can draw */
  drawingPolicy?: 'default' | 'anyinput' | 'pencilonly';
  /** iOS only: Fired when Apple Pencil barrel is double-tapped (2nd gen+) */
  onPencilDoubleTap?: (event: NativeSyntheticEvent<{ sequence: number; timestamp: number }>) => void;
}

export interface NativeInkCanvasRef {
  setNativeProps?: (nativeProps: Record<string, unknown>) => void;
  clear: () => void;
  undo: () => void;
  redo: () => void;
  setTool: (toolType: string, width: number, color: string, eraserMode?: string) => void;
  getBase64Data: () => Promise<string>;
  loadBase64Data: (base64String: string) => Promise<boolean>;
  /**
   * Eagerly release the heavy native state (~13 MB pixel buffer + the
   * C++ drawing engine + queued JS callbacks) without waiting for ARC.
   * The continuous engine pool calls this only on final pool unmount,
   * never for normal page switching. Optional so tests don't have to
   * mock it; iOS-only.
   */
  releaseEngine?: () => Promise<void>;
  /**
   * Native-side single-page persistence: tells the engine to serialize its
   * current state (one page payload) and write directly to the file at
   * `path`. Body bytes never cross the JS<->native bridge.
   *
   * Useful for paged-mode (Android primary) where one engine = one page.
   * Continuous mode should use persistFullNotebookToFile instead so non-
   * visible pages are preserved.
   */
  persistEngineToFile: (path: string) => Promise<boolean>;
  loadEngineFromFile: (path: string) => Promise<boolean>;
  /**
   * Native-side full-notebook autosave (iOS continuous mode).
   *
   * Reads the existing body file, replaces ONLY the visible window's
   * per-page data with the engine's fresh state, writes back atomically.
   * Body bytes (which can be many MB) never cross the JS<->native bridge:
   * JS only sends the small visible-page-IDs array + lightweight
   * pagesMetadata (no data fields).
   *
   * Returns true on success. Returns false (without throwing) when the
   * native fast-path isn't available (older build) so callers can fall
   * back to the existing slow path.
   */
  persistFullNotebookToFile: (params: {
    visiblePageIds: string[];
    pagesMetadata: Array<Record<string, unknown>>;
    originalCanvasWidth?: number;
    pageHeight: number;
    bodyPath: string;
  }) => Promise<boolean>;
  /**
   * Inverse of persistFullNotebookToFile. Reads the body file in native,
   * loads visible-window pages into the engine, returns just the slim
   * metadata array (no per-page data) plus the originalCanvasWidth.
   *
   * Returns null (without throwing) when the file is missing, malformed,
   * or the native fast-path isn't available.
   */
  loadNotebookForVisibleWindow: (params: {
    bodyPath: string;
    visiblePageIds: string[];
    pageHeight: number;
  }) => Promise<{
    success: boolean;
    pagesMetadata?: Array<Record<string, unknown>>;
    originalCanvasWidth?: number | null;
    reason?: string;
  } | null>;
  stageBase64Data?: (base64String: string) => Promise<boolean>;
  presentDeferredLoad?: () => Promise<boolean>;
  getBase64PngData: (scale?: number) => Promise<string>;
  getBase64JpegData: (scale?: number, compression?: number) => Promise<string>;
  performCopy: () => void;
  performPaste: () => void;
  performDelete: () => void;
  simulatePencilDoubleTap?: () => Promise<boolean>;
}

export const NativeInkCanvas = forwardRef<
  NativeInkCanvasRef,
  NativeInkCanvasProps
>((props, ref) => {
  const nativeRef = useRef<any>(null);
  const lastToolSigRef = useRef<string | null>(null);
  const [nativeReadyVersion, setNativeReadyVersion] = useState(0);

  const handleNativeRef = useCallback((node: any) => {
    if (nativeRef.current !== node) {
      lastToolSigRef.current = null;
    }
    nativeRef.current = node;
    if (node) {
      setNativeReadyVersion((previousVersion) => previousVersion + 1);
    }
  }, []);

  useEffect(() => {
    if (nativeReadyVersion <= 0) {
      return;
    }

    props.onCanvasReady?.();
  }, [nativeReadyVersion, props.onCanvasReady]);

  useImperativeHandle(ref, () => ({
    setNativeProps: (nativeProps: Record<string, unknown>) => {
      nativeRef.current?.setNativeProps?.(nativeProps);
    },

    clear: () => {
      const node = findNodeHandle(nativeRef.current);
      if (node) {
        lastToolSigRef.current = null;
        if (Platform.OS === 'ios' && MobileInkCanvasViewManager) {
          MobileInkCanvasViewManager.clear(node);
        } else {
          UIManager.dispatchViewManagerCommand(node, 'clear', []);
        }
      }
    },

    undo: () => {
      const node = findNodeHandle(nativeRef.current);
      if (node) {
        UIManager.dispatchViewManagerCommand(node, 'undo', []);
      }
    },

    redo: () => {
      const node = findNodeHandle(nativeRef.current);
      if (node) {
        UIManager.dispatchViewManagerCommand(node, 'redo', []);
      }
    },

    setTool: (toolType: string, width: number, color: string, eraserMode?: string) => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) return;
      const mode = eraserMode || 'pixel';
      // Dedupe: parent's broadcast loop in ContinuousSkiaNotebook fires
      // setTool on every slot whenever activeCanvasReadyVersion bumps
      // (pool assignment/native-ready events can trigger it). Without this guard
      // the JS-thread cost was 100+ identical bridge calls per scroll
      // gesture -- pure overhead since the native side already has the
      // exact same tool. The signature comparison is cheap (string
      // concat + ===) and short-circuits the JSI hop entirely.
      const sig = `${toolType}|${width}|${color}|${mode}`;
      if (lastToolSigRef.current === sig) return;
      lastToolSigRef.current = sig;
      __DEV__ && console.log(`[Bridge] setTool: ${toolType}, ${width}, ${color}, ${mode}`);
      if (Platform.OS === 'ios' && MobileInkCanvasViewManager) {
        MobileInkCanvasViewManager.setTool(node, toolType, width, color, mode);
      } else {
        UIManager.dispatchViewManagerCommand(
          node,
          'setToolWithParams',
          [toolType, width, color, mode]
        );
      }
    },

    releaseEngine: async (): Promise<void> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) return;
      // iOS only -- explicit release. Android's bridge releases on view
      // teardown reliably (no comparable retain issue observed there).
      if (Platform.OS !== 'ios' || !MobileInkCanvasViewManager?.releaseEngine) return;
      try {
        lastToolSigRef.current = null;
        await new Promise<void>((resolve) => {
          MobileInkCanvasViewManager.releaseEngine(node, () => resolve());
        });
      } catch {
        // Best-effort cleanup; swallow errors so unmount isn't blocked.
      }
    },

    getBase64Data: async (): Promise<string> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      if (Platform.OS === 'android') {
        // Android uses MobileInkModule with promise-based API
        if (!MobileInkModule) {
          throw new Error('MobileInkModule not found. Please rebuild the app.');
        }
        return MobileInkModule.getBase64Data(node);
      } else {
        // iOS uses MobileInkCanvasViewManager with callback-based API
        if (!MobileInkCanvasViewManager) {
          throw new Error('MobileInkCanvasViewManager not found. Please rebuild the app.');
        }
        return new Promise((resolve, reject) => {
          MobileInkCanvasViewManager.getBase64Data(node, (error: string | null, data: string | null) => {
            if (error) {
              reject(new Error(error));
            } else {
              resolve(data || '');
            }
          });
        });
      }
    },

    loadBase64Data: async (base64String: string): Promise<boolean> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      lastToolSigRef.current = null;
      const trimmedPayload = base64String?.trim() ?? '';
      if (!trimmedPayload) {
        if (Platform.OS === 'ios' && MobileInkCanvasViewManager) {
          MobileInkCanvasViewManager.clear(node);
          return true;
        }
        UIManager.dispatchViewManagerCommand(node, 'clear', []);
        return true;
      }

      const normalizedPayload = normalizePagePayloadForNativeLoad(trimmedPayload);
      if (!normalizedPayload.isValid) {
        throw new Error(`Unsafe drawing payload blocked: ${normalizedPayload.reasonCode}`);
      }

      if (Platform.OS === 'android') {
        // Android: use MobileInkModule with promise-based API
        // This waits for the GL thread to finish loading before resolving
        if (!MobileInkModule) {
          throw new Error('MobileInkModule not found. Please rebuild the app.');
        }
        return MobileInkModule.loadBase64Data(node, normalizedPayload.normalizedPayload);
      } else {
        // iOS uses MobileInkCanvasViewManager with callback-based API
        if (!MobileInkCanvasViewManager) {
          throw new Error('MobileInkCanvasViewManager not found. Please rebuild the app.');
        }
        return new Promise((resolve, reject) => {
          MobileInkCanvasViewManager.loadBase64Data(node, normalizedPayload.normalizedPayload, (error: string | null, success: boolean | null) => {
            if (error) {
              reject(new Error(error));
            } else {
              resolve(success !== null ? success : false);
            }
          });
        });
      }
    },

    /**
     * Native-side persist. The body never crosses the bridge -- the engine
     * serializes and writes the file in Swift/Kotlin directly. Falls back to
     * the slow path (getBase64Data + JS-side FS write) if the native method
     * isn't present, so JS code can adopt this without a hard rebuild
     * dependency.
     */
    persistEngineToFile: async (path: string): Promise<boolean> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      if (Platform.OS === 'android') {
        if (MobileInkModule?.persistEngineToFile) {
          return MobileInkModule.persistEngineToFile(node, path);
        }
        return false;
      }

      // iOS: MobileInkBridge has the promise-based persist method
      if (MobileInkBridge?.persistEngineToFile) {
        return MobileInkBridge.persistEngineToFile(node, path);
      }
      return false;
    },

    loadEngineFromFile: async (path: string): Promise<boolean> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      if (Platform.OS === 'android') {
        if (MobileInkModule?.loadEngineFromFile) {
          return MobileInkModule.loadEngineFromFile(node, path);
        }
        return false;
      }

      if (MobileInkBridge?.loadEngineFromFile) {
        return MobileInkBridge.loadEngineFromFile(node, path);
      }
      return false;
    },

    persistFullNotebookToFile: async ({
      visiblePageIds,
      pagesMetadata,
      originalCanvasWidth,
      pageHeight,
      bodyPath,
    }) => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }
      // iOS-only: continuous-mode compose/decompose lives on MobileInkBridge
      // (Android doesn't ship it). Returns false on Android so callers fall back
      // to the existing slow path -- the bridge-bypass win is iOS-only here.
      if (Platform.OS !== 'ios' || !MobileInkBridge?.persistFullNotebookToFile) {
        return false;
      }
      return MobileInkBridge.persistFullNotebookToFile(
        node,
        visiblePageIds,
        pagesMetadata,
        originalCanvasWidth ?? null,
        pageHeight,
        bodyPath,
      );
    },

    loadNotebookForVisibleWindow: async ({ bodyPath, visiblePageIds, pageHeight }) => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }
      if (Platform.OS !== 'ios' || !MobileInkBridge?.loadNotebookForVisibleWindow) {
        return null;
      }
      const result = await MobileInkBridge.loadNotebookForVisibleWindow(
        node,
        bodyPath,
        visiblePageIds,
        pageHeight,
      );
      if (!result || typeof result !== 'object') return null;
      return result as {
        success: boolean;
        pagesMetadata?: Array<Record<string, unknown>>;
        originalCanvasWidth?: number | null;
        reason?: string;
      };
    },

    stageBase64Data: async (base64String: string): Promise<boolean> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      const trimmedPayload = base64String?.trim() ?? '';
      const normalizedPayload = trimmedPayload
        ? normalizePagePayloadForNativeLoad(trimmedPayload)
        : { isValid: true, normalizedPayload: '', reasonCode: null };

      if (!normalizedPayload.isValid) {
        throw new Error(`Unsafe drawing payload blocked: ${normalizedPayload.reasonCode}`);
      }

      if (Platform.OS !== 'ios' || !MobileInkCanvasViewManager?.stageBase64Data) {
        if (Platform.OS === 'ios' && MobileInkCanvasViewManager) {
          return new Promise((resolve, reject) => {
            MobileInkCanvasViewManager.loadBase64Data(
              node,
              normalizedPayload.normalizedPayload || '',
              (error: string | null, success: boolean | null) => {
                if (error) {
                  reject(new Error(error));
                } else {
                  resolve(success !== null ? success : false);
                }
              }
            );
          });
        }

        if (!MobileInkModule) {
          throw new Error('MobileInkModule not found. Please rebuild the app.');
        }

        return MobileInkModule.loadBase64Data(
          node,
          normalizedPayload.normalizedPayload || ''
        );
      }

      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.stageBase64Data(
          node,
          normalizedPayload.normalizedPayload || '',
          (error: string | null, success: boolean | null) => {
            if (error) {
              reject(new Error(error));
            } else {
              resolve(success !== null ? success : false);
            }
          }
        );
      });
    },

    presentDeferredLoad: async (): Promise<boolean> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      if (Platform.OS !== 'ios' || !MobileInkCanvasViewManager?.presentDeferredLoad) {
        return true;
      }

      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.presentDeferredLoad(
          node,
          (error: string | null, success: boolean | null) => {
            if (error) {
              reject(new Error(error));
            } else {
              resolve(success !== null ? success : false);
            }
          }
        );
      });
    },

    getBase64PngData: async (scale: number = 1): Promise<string> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      if (Platform.OS === 'android') {
        // Android: use MobileInkModule with promise-based API
        if (!MobileInkModule) {
          throw new Error('MobileInkModule not found. Please rebuild the app.');
        }
        return MobileInkModule.getBase64PngData(node, scale);
      }

      if (!MobileInkCanvasViewManager) {
        throw new Error('MobileInkCanvasViewManager not found. Please rebuild the app.');
      }
      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.getBase64PngData(node, scale, (error: string | null, data: string | null) => {
          if (error) {
            reject(new Error(error));
          } else {
            resolve(data || '');
          }
        });
      });
    },

    getBase64JpegData: async (
      scale: number = 1,
      compression: number = 0.9
    ): Promise<string> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node) {
        throw new Error('View not found');
      }

      if (Platform.OS === 'android') {
        // Android: use MobileInkModule with promise-based API
        if (!MobileInkModule) {
          throw new Error('MobileInkModule not found. Please rebuild the app.');
        }
        return MobileInkModule.getBase64JpegData(node, scale, compression);
      }

      if (!MobileInkCanvasViewManager) {
        throw new Error('MobileInkCanvasViewManager not found. Please rebuild the app.');
      }
      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.getBase64JpegData(node, scale, compression, (error: string | null, data: string | null) => {
          if (error) {
            reject(new Error(error));
          } else {
            resolve(data || '');
          }
        });
      });
    },

    performCopy: () => {
      const node = findNodeHandle(nativeRef.current);
      if (node) {
        UIManager.dispatchViewManagerCommand(node, 'performCopy', []);
      }
    },

    performPaste: () => {
      const node = findNodeHandle(nativeRef.current);
      if (node) {
        UIManager.dispatchViewManagerCommand(node, 'performPaste', []);
      }
    },

    performDelete: () => {
      const node = findNodeHandle(nativeRef.current);
      if (node) {
        UIManager.dispatchViewManagerCommand(node, 'performDelete', []);
      }
    },

    simulatePencilDoubleTap: async (): Promise<boolean> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node || Platform.OS !== 'ios' || !MobileInkCanvasViewManager?.simulatePencilDoubleTap) {
        return false;
      }

      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.simulatePencilDoubleTap(
          node,
          (error: string | null, success: boolean | null) => {
            if (error) {
              reject(new Error(error));
            } else {
              resolve(success === true);
            }
          }
        );
      });
    },
  }), []);

  return (
    <MobileInkCanvasViewNative
      ref={handleNativeRef}
      style={props.style}
      onDrawingChange={props.onDrawingChange}
      onDrawingBegin={props.onDrawingBegin}
      onSelectionChange={props.onSelectionChange}
      backgroundType={props.backgroundType}
      pdfBackgroundUri={props.pdfBackgroundUri}
      renderSuspended={props.renderSuspended}
      drawingPolicy={props.drawingPolicy}
      onPencilDoubleTap={props.onPencilDoubleTap}
    />
  );
});

NativeInkCanvas.displayName = 'NativeInkCanvas';

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

    if (Platform.OS === 'ios') {
      if (!MobileInkBridge) {
        throw new Error('MobileInkBridge not found. Please rebuild the app.');
      }
      // iOS: batchExportPages(pagesDataArray, backgroundTypes, width, height, scale, pdfUri)
      results = await MobileInkBridge.batchExportPages(
        sanitizedPagesData,
        backgroundTypes,
        width,
        height,
        scale,
        pdfBackgroundUri || '',
        pageIndices || []
      );
    } else {
      if (!MobileInkModule) {
        throw new Error('MobileInkModule not found. Please rebuild the app.');
      }
      // Android: batchExportPages(pagesDataArray, backgroundTypes, width, height, scale, pdfUri)
      results = await MobileInkModule.batchExportPages(
        sanitizedPagesData,
        backgroundTypes,
        width,
        height,
        scale,
        pdfBackgroundUri || ''
      );
    }

    const elapsed = Date.now() - startTime;
    const successCount = results.filter(r => r && r.length > 0).length;
    __DEV__ && console.log(`[BatchExport] Completed ${successCount}/${pagesData.length} pages in ${elapsed}ms`);

    return results;
  } catch (error) {
    console.error('[BatchExport] Native batch export failed:', error);
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
  if (Platform.OS !== 'ios' || !MobileInkBridge?.readBodyFileParsed) {
    return null;
  }
  try {
    const result = await MobileInkBridge.readBodyFileParsed(bodyPath);
    if (result === null || result === undefined) return null;
    if (typeof result !== 'object') return null;
    return result as Record<string, unknown>;
  } catch (error) {
    // Native parse failed -- fall through to JS-side read.
    if (__DEV__) {
      console.warn('[NativeInkCanvas] readBodyFileParsed failed:', error);
    }
    return null;
  }
}

export async function composeContinuousWindow(
  pagePayloads: string[],
  pageHeight: number
): Promise<string> {
  if (Platform.OS !== 'ios') {
    throw new Error('Continuous window composition is only available on iOS.');
  }

  if (!MobileInkBridge?.composeContinuousWindow) {
    throw new Error('MobileInkBridge.composeContinuousWindow not found. Please rebuild the app.');
  }

  return MobileInkBridge.composeContinuousWindow(pagePayloads, pageHeight);
}

export async function decomposeContinuousWindow(
  windowPayload: string,
  pageCount: number,
  pageHeight: number
): Promise<string[]> {
  if (Platform.OS !== 'ios') {
    throw new Error('Continuous window decomposition is only available on iOS.');
  }

  if (!MobileInkBridge?.decomposeContinuousWindow) {
    throw new Error('MobileInkBridge.decomposeContinuousWindow not found. Please rebuild the app.');
  }

  return MobileInkBridge.decomposeContinuousWindow(windowPayload, pageCount, pageHeight);
}
