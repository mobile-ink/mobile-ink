import React, {
  useRef,
  useImperativeHandle,
  forwardRef,
  useEffect,
  useCallback,
  useState,
} from 'react';
import {
  UIManager,
  findNodeHandle,
  Platform,
} from 'react-native';
import {
  DEFAULT_NATIVE_INK_RENDER_BACKEND,
} from './benchmark';
import type {
  NativeInkBenchmarkOptions,
  NativeInkBenchmarkRecordingOptions,
  NativeInkBenchmarkResult,
} from './benchmark';
import { normalizePagePayloadForNativeLoad } from "./payload";
import {
  MobileInkBridge,
  MobileInkCanvasViewManager,
  MobileInkCanvasViewNative,
  MobileInkModule,
  supportsRenderBackendProp,
} from "./native-ink-canvas/nativeModules";
import type {
  NativeInkCanvasProps,
  NativeInkCanvasRef,
} from "./native-ink-canvas/types";

export type {
  NativeInkCanvasProps,
  NativeInkCanvasRef,
} from "./native-ink-canvas/types";
export {
  batchExportPages,
  composeContinuousWindow,
  decomposeContinuousWindow,
  readBodyFileParsed,
} from "./native-ink-canvas/notebookBridge";

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

    runBenchmark: async (
      options: NativeInkBenchmarkOptions = {},
    ): Promise<NativeInkBenchmarkResult> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node || Platform.OS !== 'ios' || !MobileInkCanvasViewManager?.runBenchmark) {
        throw new Error('Native benchmark runner is only available on iOS after rebuilding the app.');
      }

      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.runBenchmark(
          node,
          options,
          (error: string | null, result: NativeInkBenchmarkResult | null) => {
            if (error) {
              reject(new Error(error));
            } else if (!result) {
              reject(new Error('Native benchmark finished without metrics.'));
            } else {
              resolve(result);
            }
          }
        );
      });
    },

    startBenchmarkRecording: async (
      options: NativeInkBenchmarkRecordingOptions = {},
    ): Promise<boolean> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node || Platform.OS !== 'ios' || !MobileInkCanvasViewManager?.startBenchmarkRecording) {
        throw new Error('Native benchmark recorder is only available on iOS after rebuilding the app.');
      }

      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.startBenchmarkRecording(
          node,
          options,
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

    stopBenchmarkRecording: async (): Promise<NativeInkBenchmarkResult> => {
      const node = findNodeHandle(nativeRef.current);
      if (!node || Platform.OS !== 'ios' || !MobileInkCanvasViewManager?.stopBenchmarkRecording) {
        throw new Error('Native benchmark recorder is only available on iOS after rebuilding the app.');
      }

      return new Promise((resolve, reject) => {
        MobileInkCanvasViewManager.stopBenchmarkRecording(
          node,
          (error: string | null, result: NativeInkBenchmarkResult | null) => {
            if (error) {
              reject(new Error(error));
            } else if (!result) {
              reject(new Error('Native benchmark recording stopped without metrics.'));
            } else {
              resolve(result);
            }
          }
        );
      });
    },
  }), []);

  const nativeProps = {
    ref: handleNativeRef,
    style: props.style,
    onDrawingChange: props.onDrawingChange,
    onDrawingBegin: props.onDrawingBegin,
    onSelectionChange: props.onSelectionChange,
    backgroundType: props.backgroundType,
    pdfBackgroundUri: props.pdfBackgroundUri,
    renderSuspended: props.renderSuspended,
    drawingPolicy: props.drawingPolicy,
    onPencilDoubleTap: props.onPencilDoubleTap,
    ...(supportsRenderBackendProp
      ? { renderBackend: props.renderBackend ?? DEFAULT_NATIVE_INK_RENDER_BACKEND }
      : {}),
  };

  return (
    <MobileInkCanvasViewNative {...nativeProps} />
  );
});

NativeInkCanvas.displayName = 'NativeInkCanvas';
