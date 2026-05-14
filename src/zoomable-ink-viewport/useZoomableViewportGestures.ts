import { useCallback, useMemo } from "react";
import { Gesture } from "react-native-gesture-handler";
import {
  cancelAnimation,
  runOnJS,
  useAnimatedReaction,
  withDecay,
} from "react-native-reanimated";
import type { SharedValue } from "react-native-reanimated";
import {
  clamp,
  clampTranslation,
  getTranslationBounds,
  shouldLockHorizontalTranslation as shouldLockHorizontalTranslationForLayout,
} from "./geometry";
import type {
  TouchExclusionRect,
  ZoomableInkViewportProps,
} from "./types";

type UseZoomableViewportGesturesParams = {
  enabled: boolean;
  minScale: number;
  maxScale: number;
  onZoomChange?: ZoomableInkViewportProps["onZoomChange"];
  onGestureStateChange?: ZoomableInkViewportProps["onGestureStateChange"];
  onMotionStateChange?: ZoomableInkViewportProps["onMotionStateChange"];
  fingerDrawingEnabled: boolean;
  edgeExclusionWidth: number;
  enableMomentumScroll: boolean;
  panEnabled: boolean;
  onTransformChange?: ZoomableInkViewportProps["onTransformChange"];
  transformNotificationMinIntervalMs: number;
  onContentTap?: ZoomableInkViewportProps["onContentTap"];
  containerWidth: SharedValue<number>;
  containerHeight: SharedValue<number>;
  contentW: SharedValue<number>;
  contentH: SharedValue<number>;
  contentPad: SharedValue<number>;
  isLandscapeMode: SharedValue<boolean>;
  lockHorizontalPanNearFitValue: SharedValue<boolean>;
  blockedRects: SharedValue<TouchExclusionRect[]>;
  scale: SharedValue<number>;
  translateX: SharedValue<number>;
  translateY: SharedValue<number>;
  savedScale: SharedValue<number>;
  savedTranslateX: SharedValue<number>;
  savedTranslateY: SharedValue<number>;
  focalX: SharedValue<number>;
  focalY: SharedValue<number>;
  isGesturingRef: SharedValue<boolean>;
  isPinchActive: SharedValue<boolean>;
  lastPinchEventScale: SharedValue<number>;
  lastPanTranslationX: SharedValue<number>;
  lastPanTranslationY: SharedValue<number>;
  isDecayingX: SharedValue<boolean>;
  isDecayingY: SharedValue<boolean>;
  isClampingX: SharedValue<boolean>;
  isClampingY: SharedValue<boolean>;
  isMotionActive: SharedValue<boolean>;
  lastTransformNotificationTs: SharedValue<number>;
};

export const useZoomableViewportGestures = ({
  enabled,
  minScale,
  maxScale,
  onZoomChange,
  onGestureStateChange,
  onMotionStateChange,
  fingerDrawingEnabled,
  edgeExclusionWidth,
  enableMomentumScroll,
  panEnabled,
  onTransformChange,
  transformNotificationMinIntervalMs,
  onContentTap,
  containerWidth,
  containerHeight,
  contentW,
  contentH,
  contentPad,
  isLandscapeMode,
  lockHorizontalPanNearFitValue,
  blockedRects,
  scale,
  translateX,
  translateY,
  savedScale,
  savedTranslateX,
  savedTranslateY,
  focalX,
  focalY,
  isGesturingRef,
  isPinchActive,
  lastPinchEventScale,
  lastPanTranslationX,
  lastPanTranslationY,
  isDecayingX,
  isDecayingY,
  isClampingX,
  isClampingY,
  isMotionActive,
  lastTransformNotificationTs,
}: UseZoomableViewportGesturesParams) => {
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

  const notifyContentTap = useCallback((locationX: number, locationY: number) => {
    onContentTap?.({
      nativeEvent: {
        locationX,
        locationY,
        isZoomableContentTap: true,
      },
    });
  }, [onContentTap]);

  const isTouchBlocked = (x: number, y: number): boolean => {
    "worklet";
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

  const shouldLockHorizontalTranslation = (currentScale: number): boolean => {
    "worklet";
    return shouldLockHorizontalTranslationForLayout(
      currentScale,
      contentW.value,
      containerWidth.value,
      lockHorizontalPanNearFitValue.value,
      isLandscapeMode.value,
    );
  };

  const stopMomentumAnimations = () => {
    "worklet";
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
    "worklet";
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

  const pinchGesture = useMemo(() => {
    return Gesture.Pinch()
      .enabled(enabled)
      .onTouchesDown(() => {
        "worklet";
        stopMomentumAnimations();
        syncMotionState();
      })
      .onStart((event) => {
        "worklet";
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
        "worklet";
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
        "worklet";
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
        "worklet";
        if (isGesturingRef.value) {
          isGesturingRef.value = false;
          runOnJS(notifyGestureEnd)();
          syncMotionState();
        }
      });
  }, [enabled, minScale, maxScale, onZoomChange, notifyGestureStart, notifyGestureEnd, notifyMotionChange]);

  const panGesture = useMemo(() => {
    const gesture = Gesture.Pan()
      .enabled(enabled && panEnabled)
      .minDistance(0)
      .onTouchesDown((event, stateManager) => {
        "worklet";
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

          // Valid center-area finger touch - DO NOT activate yet. Defer activation
          // until the user actually starts moving (handled in onTouchesMove below).
          // If we activate here, a pure tap (touch + release with no movement) is
          // claimed by this gesture and never propagates to the inner
          // <TouchableWithoutFeedback onPress={handleCanvasPress}>, which is what
          // clears figure/text-box selection on tap-empty-canvas. The user-visible
          // bug was "I can't tap to deselect, I have to use pencil."
        }
      })
      .onTouchesMove((_event, stateManager) => {
        "worklet";
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
        "worklet";
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
        "worklet";
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
        "worklet";
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
              "worklet";
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
              "worklet";
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
        "worklet";
        // Ensure we always notify end, even on cancel
        if (isGesturingRef.value) {
          isGesturingRef.value = false;
          runOnJS(notifyGestureEnd)();
          syncMotionState();
        }
      });

    return gesture;
  }, [enableMomentumScroll, enabled, fingerDrawingEnabled, edgeExclusionWidth, notifyGestureStart, notifyGestureEnd, notifyMotionChange, panEnabled]);

  const tapGesture = useMemo(() => {
    return Gesture.Tap()
      .enabled(enabled && !!onContentTap)
      .maxDistance(10)
      .maxDuration(350)
      .onTouchesDown((event, stateManager) => {
        "worklet";
        if (event.pointerType === 1) {
          stateManager.fail();
        }
      })
      .onEnd((event, success) => {
        "worklet";
        if (!success) {
          return;
        }

        const currentScale = scale.value > 0 ? scale.value : 1;
        const locationX = (event.x - translateX.value) / currentScale;
        const locationY = (event.y - translateY.value) / currentScale;
        runOnJS(notifyContentTap)(locationX, locationY);
      });
  }, [enabled, notifyContentTap, onContentTap, scale, translateX, translateY]);

  return useMemo(() => {
    return Gesture.Simultaneous(pinchGesture, panGesture, tapGesture);
  }, [pinchGesture, panGesture, tapGesture]);
};
