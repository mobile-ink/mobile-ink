import type { ReactNode, RefObject } from "react";
import type { View, ViewStyle } from "react-native";

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
  children: ReactNode;
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
