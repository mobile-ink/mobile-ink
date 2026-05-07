import React, { forwardRef, useImperativeHandle, useCallback, useMemo, RefObject } from 'react';
import { StyleSheet, View, ViewStyle, LayoutChangeEvent } from 'react-native';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import Animated, {
  useAnimatedStyle,
  useAnimatedReaction,
  useSharedValue,
  withDecay,
  withTiming,
  cancelAnimation,
  runOnJS,
} from 'react-native-reanimated';

export interface ZoomableInkViewportRef {
  resetZoom: () => void;
  resetZoomAnimated: () => void;
  isZoomed: () => boolean;
  getScale: () => number;
  getTransform: () => {
    scale: number;
    translateX: number;
    translateY: number;
    containerWidth: number;
    containerHeight: number;
  };
  setTransform: (nextTransform: {
    scale?: number;
    translateX?: number;
    translateY?: number;
    animated?: boolean;
  }) => void;
}

export interface TouchExclusionRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

export interface ZoomableInkViewportProps {
  children: React.ReactNode;
  style?: ViewStyle;
  minScale?: number;
  maxScale?: number;
  enabled?: boolean;
  onZoomChange?: (scale: number) => void;
  /** Called when gesture state changes - use to block canvas touches */
  onGestureStateChange?: (isGesturing: boolean) => void;
  /** Called when viewport movement starts or stops, including decay/clamp animation. */
  onMotionStateChange?: (isMoving: boolean) => void;
  /** Content width - used to calculate pan bounds in landscape */
  contentWidth?: number;
  /** Content height - used to calculate pan bounds in landscape */
  contentHeight?: number;
  /** Padding around content (e.g., from canvasShell) - affects pan bounds */
  contentPadding?: number;
  /** Whether device is in landscape orientation - affects pan bounds alignment */
  isLandscape?: boolean;
  /** When true (finger mode), pan requires 2 fingers. When false (pencil mode), 1-finger pan is enabled. */
  fingerDrawingEnabled?: boolean;
  /** Width of edge zones (px) where 1-finger touches are rejected to allow page swipe */
  edgeExclusionWidth?: number;
  /** Ref attached to the clip container for viewport capture (snipping) */
  viewportRef?: RefObject<View | null>;
  /** Finger-only interactive regions that should block navigation gestures. */
  blockedTouchRects?: TouchExclusionRect[];
  /** When true, pan gestures continue with inertial decay after release. */
  enableMomentumScroll?: boolean;
  /** When false, pinch stays available but one/two-finger content pan is disabled. */
  panEnabled?: boolean;
  /** Locks small horizontal drift while continuous content is near fit scale. */
  lockHorizontalPanNearFit?: boolean;
  /** Reports viewport transform changes for virtualized layouts. */
  onTransformChange?: (transform: {
    scale: number;
    translateX: number;
    translateY: number;
    containerWidth: number;
    containerHeight: number;
  }) => void;
  /** Minimum time between JS transform notifications; UI-thread scrolling remains unthrottled. */
  transformNotificationMinIntervalMs?: number;
  /** Short finger taps reported in untransformed content coordinates. */
  onContentTap?: (event: {
    nativeEvent: {
      locationX: number;
      locationY: number;
      isZoomableContentTap: true;
    };
  }) => void;
}

const HORIZONTAL_LOCK_MAX_SCALE = 1.12;

/**
 * ZoomableInkViewport - Wraps content with pinch-to-zoom and pan gestures
 *
 * Behavior varies by drawing mode:
 * - Finger mode (fingerDrawingEnabled=true):
 *     Two-finger pinch to zoom, two-finger pan. Single finger draws.
 * - Pencil mode (fingerDrawingEnabled=false):
 *     Two-finger pinch to zoom, one-or-two-finger pan (center area only).
 *     Edge-zone finger touches fall through to page swipe. Stylus draws.
 */
const ZoomableInkViewport = forwardRef<ZoomableInkViewportRef, ZoomableInkViewportProps>(
  (props, ref) => {
    const {
      children,
      style,
      minScale = 1,
      maxScale = 5,
      enabled = true,
      onZoomChange,
      onGestureStateChange,
      onMotionStateChange,
      contentWidth = 0,
      contentHeight = 0,
      contentPadding = 0,
      isLandscape = false,
      fingerDrawingEnabled = true,
      edgeExclusionWidth = 0,
      viewportRef,
      blockedTouchRects = [],
      enableMomentumScroll = false,
      panEnabled = true,
      lockHorizontalPanNearFit = false,
      onTransformChange,
      transformNotificationMinIntervalMs = 16,
      onContentTap,
    } = props;

    // Layout dimensions
    const containerWidth = useSharedValue(0);
    const containerHeight = useSharedValue(0);

    // Content dimensions (from props, stored as shared values for worklets)
    const contentW = useSharedValue(contentWidth);
    const contentH = useSharedValue(contentHeight);
    const contentPad = useSharedValue(contentPadding);
    const isLandscapeMode = useSharedValue(isLandscape);
    const lockHorizontalPanNearFitValue = useSharedValue(lockHorizontalPanNearFit);
    const blockedRects = useSharedValue<TouchExclusionRect[]>(blockedTouchRects);

    // Update content dimensions when props change
    React.useEffect(() => {
      contentW.value = contentWidth;
      contentH.value = contentHeight;
      contentPad.value = contentPadding;
      isLandscapeMode.value = isLandscape;
      // Note: Initial positioning is handled by CSS (flex-start in landscape, center in portrait)
      // translateY starts at 0 and user can pan to see more content
    }, [contentWidth, contentHeight, contentW, contentH, contentPadding, contentPad, isLandscape, isLandscapeMode]);

    React.useEffect(() => {
      blockedRects.value = blockedTouchRects;
    }, [blockedRects, blockedTouchRects]);

    React.useEffect(() => {
      lockHorizontalPanNearFitValue.value = lockHorizontalPanNearFit;
    }, [lockHorizontalPanNearFit, lockHorizontalPanNearFitValue]);

    // Transform state
    const scale = useSharedValue(1);
    const translateX = useSharedValue(0);
    const translateY = useSharedValue(0);

    // Saved state for gesture continuity
    const savedScale = useSharedValue(1);
    const savedTranslateX = useSharedValue(0);
    const savedTranslateY = useSharedValue(0);

    // Focal point for pinch, captured at gesture start.
    // event.focalX/focalY are in screen (GestureDetector view) coords --
    // the GestureDetector wraps a stable, untransformed wrapper, so
    // RNGH does NOT apply an inverse content transform here.
    // Treating these as content coords gives wrong math whenever
    // translateX/Y is non-zero.
    const focalX = useSharedValue(0);
    const focalY = useSharedValue(0);

    // Track if we're currently gesturing (for blocking canvas)
    const isGesturingRef = useSharedValue(false);

    // Track if pinch is *actively scaling* (not just "fingers down").
    // Pan yields to pinch only while the scale is changing this frame
    // -- otherwise a 2-finger drag after a zoom would be permanently
    // suppressed because pinch's onEnd never fires until the fingers
    // lift. We update this every pinch.onUpdate based on the per-frame
    // delta of event.scale.
    const isPinchActive = useSharedValue(false);
    const lastPinchEventScale = useSharedValue(1);
    // Per-frame translation deltas for pan: RNGH gives us cumulative
    // translationX since gesture start, but we need per-frame delta so
    // we don't undo pinch's focal-point translateX adjustments.
    const lastPanTranslationX = useSharedValue(0);
    const lastPanTranslationY = useSharedValue(0);
    const isDecayingX = useSharedValue(false);
    const isDecayingY = useSharedValue(false);
    const isClampingX = useSharedValue(false);
    const isClampingY = useSharedValue(false);
    const isMotionActive = useSharedValue(false);
    const lastTransformNotificationTs = useSharedValue(0);

    const notifyGestureStart = useCallback(() => {
      onGestureStateChange?.(true);
    }, [onGestureStateChange]);

    const notifyGestureEnd = useCallback(() => {
      onGestureStateChange?.(false);
    }, [onGestureStateChange]);

    const notifyMotionChange = useCallback((isMoving: boolean) => {
      onMotionStateChange?.(isMoving);
    }, [onMotionStateChange]);

    const notifyTransformChange = useCallback((
      nextScale: number,
      nextTranslateX: number,
      nextTranslateY: number,
      nextContainerWidth: number,
      nextContainerHeight: number
    ) => {
      onTransformChange?.({
        scale: nextScale,
        translateX: nextTranslateX,
        translateY: nextTranslateY,
        containerWidth: nextContainerWidth,
        containerHeight: nextContainerHeight,
      });
    }, [onTransformChange]);

    const handleLayout = useCallback((event: LayoutChangeEvent) => {
      const { width, height } = event.nativeEvent.layout;
      const prevHeight = containerHeight.value;
      containerWidth.value = width;
      containerHeight.value = height;

      // On orientation change, reset pan/zoom to defaults
      // Use 200px threshold to avoid false positives from toolbar show/hide (52px)
      // Actual orientation changes cause hundreds of pixels of difference
      const isOrientationChange = prevHeight > 0 && Math.abs(height - prevHeight) > 200;
      if (isOrientationChange) {
        // Reset to initial state - CSS handles positioning (flex-start in landscape, center in portrait)
        translateX.value = 0;
        translateY.value = 0;
        savedTranslateX.value = 0;
        savedTranslateY.value = 0;
        scale.value = 1;
        savedScale.value = 1;
      }
    }, [containerWidth, containerHeight, translateX, translateY, savedTranslateX, savedTranslateY, scale, savedScale]);

    const clamp = (value: number, min: number, max: number): number => {
      'worklet';
      return Math.min(Math.max(value, min), max);
    };

    const isTouchBlocked = (x: number, y: number): boolean => {
      'worklet';
      const currentScale = scale.value > 0 ? scale.value : 1;
      const contentX = (x - translateX.value) / currentScale;
      const contentY = (y - translateY.value) / currentScale;

      for (const rect of blockedRects.value) {
        if (
          contentX >= rect.left &&
          contentX <= rect.right &&
          contentY >= rect.top &&
          contentY <= rect.bottom
        ) {
          return true;
        }
      }

      return false;
    };

    /**
     * Calculate pan bounds based on content vs container size
     * Allows panning when content exceeds container (e.g., landscape mode)
     *
     * When zoomed, allows enough range to see any corner of the content.
     * This enables proper focal-point zooming where the pinch point stays under fingers.
     *
     * Layout differences:
     * - Portrait: content is centered (justifyContent: center) - symmetric bounds
     * - Landscape: content is top-aligned (justifyContent: flex-start) - asymmetric bounds
     *
     * Content has padding (e.g., 16px from canvasShell) that must be accounted for.
     */
    const getTranslationBounds = (
      currentScale: number,
      containerW: number,
      containerH: number,
      contentWidth: number,
      contentHeight: number,
      padding: number,
      landscape: boolean
    ): { minX: number; maxX: number; minY: number; maxY: number } => {
      'worklet';
      // Scaled content dimensions. The actual page stack is centered
      // horizontally in the gesture viewport and top-aligned with
      // padding vertically in continuous mode.
      const scaledContentW = contentWidth * currentScale;
      const scaledContentH = contentHeight * currentScale;
      const scaledPadding = padding * currentScale;
      const totalVisualH = scaledContentH + scaledPadding * 2;

      // For X, allow the page edge nearest the pinch focal point to
      // remain anchored as long as the page itself stays inside the
      // viewport. When zoomed beyond the viewport, the same formula
      // becomes the usual "no blank outside the document" range.
      const maxTxForHorizontalEdges =
        containerW > 0 && scaledContentW > 0
          ? Math.abs(scaledContentW - containerW) / 2
          : 0;

      let minX = -maxTxForHorizontalEdges;
      let maxX = maxTxForHorizontalEdges;
      let minY = 0;
      let maxY = 0;

      if (landscape) {
        // LANDSCAPE: content is TOP-ALIGNED (flex-start)
        // Keep the page stack edges, not the shell edges, pinned to the
        // same padding margin. This lets top-edge pinches zoom into the
        // top of the page instead of being clamped inward immediately.
        if (totalVisualH > containerH) {
          const centerY = containerH / 2;
          const topPinnedY =
            padding - centerY - currentScale * (padding - centerY);
          const bottomPinnedY =
            containerH -
            padding -
            centerY -
            currentScale * (padding + contentHeight - centerY);
          minY = Math.min(bottomPinnedY, topPinnedY);
          maxY = Math.max(bottomPinnedY, topPinnedY);
        }
      } else {
        // PORTRAIT: content is CENTERED
        // Symmetric bounds around center
        const overflow = Math.max(0, totalVisualH - containerH);
        minY = -overflow / 2;
        maxY = overflow / 2;
      }

      return { minX, maxX, minY, maxY };
    };

    const clampTranslation = (
      tx: number,
      ty: number,
      currentScale: number,
      containerW: number,
      containerH: number,
      contentWidth: number,
      contentHeight: number,
      padding: number,
      landscape: boolean
    ): { x: number; y: number } => {
      'worklet';
      const bounds = getTranslationBounds(
        currentScale,
        containerW,
        containerH,
        contentWidth,
        contentHeight,
        padding,
        landscape
      );

      return {
        x: clamp(tx, bounds.minX, bounds.maxX),
        y: clamp(ty, bounds.minY, bounds.maxY),
      };
    };

    const shouldLockHorizontalTranslation = (currentScale: number): boolean => {
      'worklet';
      const scaledContentWidth = contentW.value * currentScale;
      return (
        lockHorizontalPanNearFitValue.value &&
        isLandscapeMode.value &&
        (
          currentScale <= HORIZONTAL_LOCK_MAX_SCALE ||
          scaledContentWidth <= containerWidth.value
        )
      );
    };

    const stopMomentumAnimations = () => {
      'worklet';
      cancelAnimation(scale);
      cancelAnimation(translateX);
      cancelAnimation(translateY);
      isDecayingX.value = false;
      isDecayingY.value = false;
      isClampingX.value = false;
      isClampingY.value = false;
      savedScale.value = scale.value;
      savedTranslateX.value = translateX.value;
      savedTranslateY.value = translateY.value;
    };

    const syncMotionState = () => {
      'worklet';
      const nextIsMoving =
        isGesturingRef.value ||
        isDecayingX.value ||
        isDecayingY.value ||
        isClampingX.value ||
        isClampingY.value;

      if (isMotionActive.value === nextIsMoving) {
        return;
      }

      isMotionActive.value = nextIsMoving;
      runOnJS(notifyMotionChange)(nextIsMoving);
    };

    useAnimatedReaction(
      () => ({
        scale: scale.value,
        translateX: translateX.value,
        translateY: translateY.value,
        containerWidth: containerWidth.value,
        containerHeight: containerHeight.value,
      }),
      (nextTransform, previousTransform) => {
        const containerChanged =
          !previousTransform ||
          nextTransform.containerWidth !== previousTransform.containerWidth ||
          nextTransform.containerHeight !== previousTransform.containerHeight;

        if (
          previousTransform &&
          nextTransform.scale === previousTransform.scale &&
          nextTransform.translateX === previousTransform.translateX &&
          nextTransform.translateY === previousTransform.translateY &&
          !containerChanged
        ) {
          return;
        }

        if (!containerChanged && transformNotificationMinIntervalMs > 0) {
          const now = Date.now();
          if (now - lastTransformNotificationTs.value < transformNotificationMinIntervalMs) {
            return;
          }
          lastTransformNotificationTs.value = now;
        } else {
          lastTransformNotificationTs.value = Date.now();
        }

        runOnJS(notifyTransformChange)(
          nextTransform.scale,
          nextTransform.translateX,
          nextTransform.translateY,
          nextTransform.containerWidth,
          nextTransform.containerHeight
        );
      }
    );

    /**
     * Pinch gesture for zooming - always requires 2 fingers
     */
    const pinchGesture = useMemo(() => {
      return Gesture.Pinch()
        .enabled(enabled)
        .onTouchesDown(() => {
          'worklet';
          stopMomentumAnimations();
          syncMotionState();
        })
        .onStart((event) => {
          'worklet';
          stopMomentumAnimations();
          // Don't claim "actively scaling" yet -- the gesture just
          // started, scale ratio is 1.0. We only mark active in
          // onUpdate when scale actually deviates between frames.
          isPinchActive.value = false;
          lastPinchEventScale.value = 1;
          isGesturingRef.value = true;
          runOnJS(notifyGestureStart)();
          syncMotionState();
          savedScale.value = scale.value;
          savedTranslateX.value = translateX.value;
          savedTranslateY.value = translateY.value;
          focalX.value = event.focalX;
          focalY.value = event.focalY;
        })
        .onUpdate((event) => {
          'worklet';
          // Per-frame scale delta. If fingers aren't actually
          // changing distance this frame, the user is dragging --
          // unlock pan and skip translating from pinch.
          const scaleDelta = Math.abs(event.scale - lastPinchEventScale.value);
          if (scaleDelta < 0.0005) {
            isPinchActive.value = false;
            lastPinchEventScale.value = event.scale;
            return;
          }

          isPinchActive.value = true;

          // Per-frame scale ratio (relative to last frame, NOT to
          // gesture start). Composes correctly with pan, which also
          // updates per-frame deltas.
          const frameRatio = event.scale / lastPinchEventScale.value;
          const prevScale = scale.value;
          const newScale = clamp(prevScale * frameRatio, minScale, maxScale);
          const centerX = containerWidth.value / 2;
          const centerY = containerHeight.value / 2;

          // Focal point zoom -- keep the content point currently under
          // the fingers anchored to the same SCREEN position across
          // this single frame.
          //
          // event.focalX/Y are reported in screen (GestureDetector
          // view) coordinates from a stable, untransformed wrapper, so
          // RNGH does NOT apply an inverse transform to the focal
          // coords. Treating them as content coords gives wrong math
          // whenever translateX/Y is non-zero (i.e. as soon as the
          // user has scrolled or panned anywhere, which is common in
          // continuous PDF mode).
          //
          // Derivation:
          //   screen = center + translate + scale * (content - center)
          //   focalContent = (focalScreen - center - prevTX) / prevScale + center
          //   For focalContent to stay at focalScreen after the zoom:
          //     focalScreen = center + newTX + newScale * (focalContent - center)
          //   Solve for newTX, simplify:
          //     newTX = prevTX + ((prevScale - newScale) / prevScale)
          //                       * (focalScreen - center - prevTX)
          //
          // The previous formula was missing both the / prevScale
          // ratio AND the - prevTX correction, which made every zoom
          // jump the view (proportional to the current pan offset).
          const dxFromCenter = event.focalX - centerX - translateX.value;
          const dyFromCenter = event.focalY - centerY - translateY.value;
          const scaleRatio = (prevScale - newScale) / prevScale;
          let newTranslateX = translateX.value + scaleRatio * dxFromCenter;
          let newTranslateY = translateY.value + scaleRatio * dyFromCenter;

          const clampedTranslation = clampTranslation(
            newTranslateX,
            newTranslateY,
            newScale,
            containerWidth.value,
            containerHeight.value,
            contentW.value,
            contentH.value,
            contentPad.value,
            isLandscapeMode.value
          );
          const isZoomingOut = newScale < prevScale;
          newTranslateX = shouldLockHorizontalTranslation(newScale) && isZoomingOut
            ? 0
            : clampedTranslation.x;
          newTranslateY = clampedTranslation.y;

          scale.value = newScale;
          // Clamp during pinch so zoom-out cannot expose empty space
          // around the document. While zooming in, fitting content can
          // still slide within the viewport so edge pinches stay
          // anchored to the page edge instead of correcting inward.
          translateX.value = newTranslateX;
          translateY.value = newTranslateY;

          lastPinchEventScale.value = event.scale;
        })
        .onEnd(() => {
          'worklet';
          isPinchActive.value = false;
          isGesturingRef.value = false;
          runOnJS(notifyGestureEnd)();

          // No release clamp animation: pinch updates already keep the
          // transform inside document bounds, so an extra animation here
          // would feel like a second, user-visible correction after the
          // fingers lift.

          savedScale.value = scale.value;
          savedTranslateX.value = translateX.value;
          savedTranslateY.value = translateY.value;

          syncMotionState();

          if (onZoomChange) {
            runOnJS(onZoomChange)(scale.value);
          }
        })
        .onFinalize(() => {
          'worklet';
          if (isGesturingRef.value) {
            isGesturingRef.value = false;
            runOnJS(notifyGestureEnd)();
            syncMotionState();
          }
        });
    }, [enabled, minScale, maxScale, onZoomChange, notifyGestureStart, notifyGestureEnd, notifyMotionChange]);

    /**
     * Pan gesture for panning zoomed/overflowing content
     *
     * - Finger mode (fingerDrawingEnabled=true): 2-finger pan only, 1-finger draws
     * - Pencil mode (fingerDrawingEnabled=false): 1-finger pan in center area,
     *   edge-zone touches rejected (fall through to page swipe), stylus draws
     */
    const panGesture = useMemo(() => {
      const gesture = Gesture.Pan()
        .enabled(enabled && panEnabled)
        .minDistance(0)
        .onTouchesDown((event, stateManager) => {
          'worklet';
          stopMomentumAnimations();
          syncMotionState();

          if (!fingerDrawingEnabled) {
            // Reject stylus touches - let them reach the drawing canvas
            // PointerType.STYLUS = 1
            if (event.pointerType === 1) {
              stateManager.fail();
              return;
            }

            // Reject touches in edge zones - let them reach page swipe handler
            if (edgeExclusionWidth > 0) {
              const touch = event.allTouches[0];
              if (touch && (
                touch.x < edgeExclusionWidth ||
                touch.x > containerWidth.value - edgeExclusionWidth
              )) {
                stateManager.fail();
                return;
              }
            }

            const touch = event.allTouches[0];
            if (touch && isTouchBlocked(touch.x, touch.y)) {
              stateManager.fail();
              return;
            }

            // Valid center-area finger touch — DO NOT activate yet. Defer activation
            // until the user actually starts moving (handled in onTouchesMove below).
            // If we activate here, a pure tap (touch + release with no movement) is
            // claimed by this gesture and never propagates to the inner
            // <TouchableWithoutFeedback onPress={handleCanvasPress}>, which is what
            // clears figure/text-box selection on tap-empty-canvas. The user-visible
            // bug was "I can't tap to deselect, I have to use pencil."
          }
        })
        .onTouchesMove((_event, stateManager) => {
          'worklet';
          // First movement after a valid touch-down promotes this pan to ACTIVE.
          // (Stylus / edge / blocked touches were already failed in onTouchesDown,
          // so we never reach here in those cases.)
          if (!fingerDrawingEnabled) {
            stateManager.activate();
          }
        });

      if (fingerDrawingEnabled) {
        // Finger mode: only 2-finger pan (single finger is for drawing)
        gesture.minPointers(2).maxPointers(2);
      } else {
        // Pencil mode: 1 or 2 finger pan (stylus is for drawing)
        gesture
          .minPointers(1)
          .maxPointers(2)
          .manualActivation(true);
      }

      gesture
        .onStart(() => {
          'worklet';
          stopMomentumAnimations();
          isGesturingRef.value = true;
          runOnJS(notifyGestureStart)();
          syncMotionState();
          savedTranslateX.value = translateX.value;
          savedTranslateY.value = translateY.value;
          lastPanTranslationX.value = 0;
          lastPanTranslationY.value = 0;
        })
        .onUpdate((event) => {
          'worklet';
          // Yield to pinch only while it is *actively scaling* this
          // frame. (isPinchActive is now per-frame -- it goes false
          // during a pure 2-finger drag, so pan can run.)
          if (isPinchActive.value) {
            // Even though we're not applying translation, keep the
            // per-frame baseline current so when pan resumes next
            // frame we don't double-apply accumulated delta.
            lastPanTranslationX.value = event.translationX;
            lastPanTranslationY.value = event.translationY;
            return;
          }

          // Use per-frame delta (translationX since last frame) instead
          // of cumulative. Cumulative would conflict with pinch's
          // focal-point adjustments to translateX/Y on prior frames --
          // pan would snap the content back to (savedTranslate +
          // cumulative) and undo the zoom translation.
          const dx = event.translationX - lastPanTranslationX.value;
          const dy = event.translationY - lastPanTranslationY.value;
          lastPanTranslationX.value = event.translationX;
          lastPanTranslationY.value = event.translationY;
          const newTranslateX = translateX.value + dx;
          const newTranslateY = translateY.value + dy;

          const bounds = getTranslationBounds(
            scale.value,
            containerWidth.value,
            containerHeight.value,
            contentW.value,
            contentH.value,
            contentPad.value,
            isLandscapeMode.value
          );

          // If we're entering this frame already out of bounds (which
          // happens after pinch-zoom near the top of the screen leaves
          // translateY > maxY=0 in landscape continuous mode), snapping
          // to bounds NOW would yank the content the user just zoomed
          // into off-screen ("snaps down" in the user's words). Allow
          // free movement back TOWARD bounds; only clamp the AXIS that's
          // moving further out of bounds.
          let nextX = newTranslateX;
          let nextY = newTranslateY;
          const xWasInBounds =
            translateX.value >= bounds.minX && translateX.value <= bounds.maxX;
          const yWasInBounds =
            translateY.value >= bounds.minY && translateY.value <= bounds.maxY;

          if (shouldLockHorizontalTranslation(scale.value)) {
            nextX = 0;
          } else if (xWasInBounds) {
            nextX = clamp(newTranslateX, bounds.minX, bounds.maxX);
          } else if (translateX.value > bounds.maxX) {
            // Currently overshooting maxX. Allow movement toward bounds
            // (smaller X), block movement further past bounds.
            nextX = Math.min(newTranslateX, translateX.value);
          } else {
            // Currently below minX. Allow movement up, block movement down.
            nextX = Math.max(newTranslateX, translateX.value);
          }

          if (yWasInBounds) {
            nextY = clamp(newTranslateY, bounds.minY, bounds.maxY);
          } else if (translateY.value > bounds.maxY) {
            nextY = Math.min(newTranslateY, translateY.value);
          } else {
            nextY = Math.max(newTranslateY, translateY.value);
          }

          translateX.value = nextX;
          translateY.value = nextY;
        })
        .onEnd((event) => {
          'worklet';
          isGesturingRef.value = false;
          runOnJS(notifyGestureEnd)();

          if (enableMomentumScroll && !isPinchActive.value) {
            const bounds = getTranslationBounds(
              scale.value,
              containerWidth.value,
              containerHeight.value,
              contentW.value,
              contentH.value,
              contentPad.value,
              isLandscapeMode.value
            );

            // If translateY is currently out of bounds (e.g. because
            // the user just finished a pinch zoom that anchored
            // content above the maxY=0 ceiling in landscape continuous
            // mode), withDecay's clamp would IMMEDIATELY snap Y to the
            // nearest bound -- visible to the user as "the view jumps
            // away when I let go." Skip decay in that case so the
            // value stays where the pinch placed it; the user's next
            // pan will gently bring it back via per-frame clamping.
            const yIsOutOfBounds =
              translateY.value > bounds.maxY || translateY.value < bounds.minY;
            const xIsOutOfBounds =
              translateX.value > bounds.maxX || translateX.value < bounds.minX;

            const shouldDecayX =
              !shouldLockHorizontalTranslation(scale.value) &&
              !xIsOutOfBounds &&
              Math.abs(event.velocityX) > 20 &&
              bounds.minX !== bounds.maxX;
            const shouldDecayY =
              !yIsOutOfBounds &&
              Math.abs(event.velocityY) > 20 &&
              bounds.minY !== bounds.maxY;

            if (shouldDecayX) {
              isDecayingX.value = true;
              syncMotionState();
              translateX.value = withDecay({
                velocity: event.velocityX,
                clamp: [bounds.minX, bounds.maxX],
              }, () => {
                'worklet';
                isDecayingX.value = false;
                syncMotionState();
              });
            }

            if (shouldDecayY) {
              isDecayingY.value = true;
              syncMotionState();
              translateY.value = withDecay({
                velocity: event.velocityY,
                clamp: [bounds.minY, bounds.maxY],
              }, () => {
                'worklet';
                isDecayingY.value = false;
                syncMotionState();
              });
            }
          }

          savedTranslateX.value = translateX.value;
          savedTranslateY.value = translateY.value;
          syncMotionState();
        })
        .onFinalize(() => {
          'worklet';
          // Ensure we always notify end, even on cancel
          if (isGesturingRef.value) {
            isGesturingRef.value = false;
            runOnJS(notifyGestureEnd)();
            syncMotionState();
          }
        });

      return gesture;
    }, [enableMomentumScroll, enabled, fingerDrawingEnabled, edgeExclusionWidth, notifyGestureStart, notifyGestureEnd, notifyMotionChange, panEnabled]);

    const notifyContentTap = useCallback((locationX: number, locationY: number) => {
      onContentTap?.({
        nativeEvent: {
          locationX,
          locationY,
          isZoomableContentTap: true,
        },
      });
    }, [onContentTap]);

    const tapGesture = useMemo(() => {
      return Gesture.Tap()
        .enabled(enabled && !!onContentTap)
        .maxDistance(10)
        .maxDuration(350)
        .onTouchesDown((event, stateManager) => {
          'worklet';
          if (event.pointerType === 1) {
            stateManager.fail();
          }
        })
        .onEnd((event, success) => {
          'worklet';
          if (!success) {
            return;
          }

          const currentScale = scale.value > 0 ? scale.value : 1;
          const locationX = (event.x - translateX.value) / currentScale;
          const locationY = (event.y - translateY.value) / currentScale;
          runOnJS(notifyContentTap)(locationX, locationY);
        });
    }, [enabled, notifyContentTap, onContentTap, scale, translateX, translateY]);

    /**
     * Compose gestures - pinch and pan run simultaneously
     * No double-tap (it delays stylus input by 250ms)
     */
    const composedGesture = useMemo(() => {
      return Gesture.Simultaneous(pinchGesture, panGesture, tapGesture);
    }, [pinchGesture, panGesture, tapGesture]);

    const resetZoom = useCallback(() => {
      cancelAnimation(scale);
      cancelAnimation(translateX);
      cancelAnimation(translateY);
      scale.value = 1;
      translateX.value = 0;
      translateY.value = 0;
      savedScale.value = 1;
      savedTranslateX.value = 0;
      savedTranslateY.value = 0;
    }, [scale, translateX, translateY, savedScale, savedTranslateX, savedTranslateY]);

    const resetZoomAnimated = useCallback(() => {
      cancelAnimation(scale);
      cancelAnimation(translateX);
      cancelAnimation(translateY);
      scale.value = withTiming(1, { duration: 150 });
      translateX.value = withTiming(0, { duration: 150 });
      translateY.value = withTiming(0, { duration: 150 });
      savedScale.value = 1;
      savedTranslateX.value = 0;
      savedTranslateY.value = 0;
    }, [scale, translateX, translateY, savedScale, savedTranslateX, savedTranslateY]);

    const isZoomed = useCallback(() => {
      return scale.value > 1.05;
    }, [scale]);

    useImperativeHandle(ref, () => ({
      resetZoom,
      resetZoomAnimated,
      isZoomed,
      getScale: () => scale.value,
      getTransform: () => ({
        scale: scale.value,
        translateX: translateX.value,
        translateY: translateY.value,
        containerWidth: containerWidth.value,
        containerHeight: containerHeight.value,
      }),
      setTransform: ({ scale: nextScale, translateX: nextTranslateX, translateY: nextTranslateY, animated = false }) => {
        cancelAnimation(scale);
        cancelAnimation(translateX);
        cancelAnimation(translateY);
        const resolvedScale = nextScale ?? scale.value;
        const shouldLockResolvedX =
          lockHorizontalPanNearFit &&
          isLandscape &&
          (
            resolvedScale <= HORIZONTAL_LOCK_MAX_SCALE ||
            contentWidth * resolvedScale <= containerWidth.value
          );
        const resolvedTranslateX =
          shouldLockResolvedX
            ? 0
            : nextTranslateX ?? translateX.value;
        const resolvedTranslateY = nextTranslateY ?? translateY.value;

        if (animated) {
          scale.value = withTiming(resolvedScale, { duration: 150 });
          translateX.value = withTiming(resolvedTranslateX, { duration: 150 });
          translateY.value = withTiming(resolvedTranslateY, { duration: 150 });
        } else {
          scale.value = resolvedScale;
          translateX.value = resolvedTranslateX;
          translateY.value = resolvedTranslateY;
        }

        savedScale.value = resolvedScale;
        savedTranslateX.value = resolvedTranslateX;
        savedTranslateY.value = resolvedTranslateY;
      },
    }), [resetZoom, resetZoomAnimated, isZoomed, scale, translateX, translateY, containerWidth, containerHeight, savedScale, savedTranslateX, savedTranslateY, lockHorizontalPanNearFit, isLandscape, contentWidth]);

    const animatedStyle = useAnimatedStyle(() => {
      return {
        transform: [
          { translateX: translateX.value },
          { translateY: translateY.value },
          { scale: scale.value },
        ],
      };
    });

    // When disabled, render without gesture handling
    if (!enabled) {
      return (
        <View style={[styles.container, style]} onLayout={handleLayout}>
          {children}
        </View>
      );
    }

    // Attach the gesture handler to an untransformed wrapper, and transform an
    // inner view. Pinch focal coordinates are then measured in stable viewport
    // coordinates instead of a view whose own transform is changing every frame.
    return (
      <View ref={viewportRef} style={[styles.clipContainer, style]} collapsable={false}>
        <GestureDetector gesture={composedGesture}>
          <View style={styles.container} onLayout={handleLayout}>
            <Animated.View style={[styles.transformedContent, animatedStyle]}>
              {children}
            </Animated.View>
          </View>
        </GestureDetector>
      </View>
    );
  }
);

const styles = StyleSheet.create({
  clipContainer: {
    flex: 1,
    overflow: 'hidden', // Clip transformed content
  },
  container: {
    flex: 1,
  },
  transformedContent: {
    ...StyleSheet.absoluteFillObject,
  },
});

ZoomableInkViewport.displayName = 'ZoomableInkViewport';

export default ZoomableInkViewport;
