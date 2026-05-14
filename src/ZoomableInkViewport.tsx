import React, { forwardRef, useImperativeHandle, useCallback, useMemo } from 'react';
import { StyleSheet, View, LayoutChangeEvent } from 'react-native';
import { GestureDetector } from 'react-native-gesture-handler';
import Animated, {
  useAnimatedStyle,
  useSharedValue,
  withTiming,
  cancelAnimation,
} from 'react-native-reanimated';
import type {
  TouchExclusionRect,
  ZoomableInkViewportProps,
  ZoomableInkViewportRef,
} from './zoomable-ink-viewport/types';
import {
  shouldLockHorizontalTranslation as shouldLockHorizontalTranslationForLayout,
} from './zoomable-ink-viewport/geometry';
import { useZoomableViewportGestures } from './zoomable-ink-viewport/useZoomableViewportGestures';

export type {
  TouchExclusionRect,
  ZoomableInkViewportProps,
  ZoomableInkViewportRef,
} from './zoomable-ink-viewport/types';

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

    const composedGesture = useZoomableViewportGestures({
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
    });

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

    const contentFrameStyle = useMemo(() => ({
      height: Math.max(0, contentHeight + contentPadding * 2),
    }), [contentHeight, contentPadding]);

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
        const shouldLockResolvedX = shouldLockHorizontalTranslationForLayout(
          resolvedScale,
          contentWidth,
          containerWidth.value,
          lockHorizontalPanNearFit,
          isLandscape,
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
        transformOrigin: [
          containerWidth.value / 2,
          containerHeight.value / 2,
          0,
        ],
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
          <View style={contentFrameStyle}>
            {children}
          </View>
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
            <Animated.View
              collapsable={false}
              style={[styles.transformedContent, contentFrameStyle, animatedStyle]}
            >
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
    position: 'absolute',
    top: 0,
    left: 0,
    right: 0,
  },
});

ZoomableInkViewport.displayName = 'ZoomableInkViewport';

export default ZoomableInkViewport;
