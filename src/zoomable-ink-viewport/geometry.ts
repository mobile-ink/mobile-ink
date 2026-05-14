export const HORIZONTAL_LOCK_MAX_SCALE = 1.12;

export type TranslationBounds = {
  minX: number;
  maxX: number;
  minY: number;
  maxY: number;
};

export const clamp = (value: number, min: number, max: number): number => {
  "worklet";
  return Math.min(Math.max(value, min), max);
};

/**
 * Calculates pan bounds from the current content, viewport, zoom, and layout mode.
 */
export const getTranslationBounds = (
  currentScale: number,
  containerW: number,
  containerH: number,
  contentWidth: number,
  contentHeight: number,
  padding: number,
  landscape: boolean
): TranslationBounds => {
  "worklet";
  const scaledContentW = contentWidth * currentScale;
  const scaledContentH = contentHeight * currentScale;
  const scaledPadding = padding * currentScale;
  const totalVisualH = scaledContentH + scaledPadding * 2;

  const maxTxForHorizontalEdges =
    containerW > 0 && scaledContentW > 0
      ? Math.abs(scaledContentW - containerW) / 2
      : 0;

  let minX = -maxTxForHorizontalEdges;
  let maxX = maxTxForHorizontalEdges;
  let minY = 0;
  let maxY = 0;

  if (landscape) {
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
    const overflow = Math.max(0, totalVisualH - containerH);
    minY = -overflow / 2;
    maxY = overflow / 2;
  }

  return { minX, maxX, minY, maxY };
};

export const clampTranslation = (
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
  "worklet";
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

export const shouldLockHorizontalTranslation = (
  currentScale: number,
  contentWidth: number,
  containerWidth: number,
  lockHorizontalPanNearFit: boolean,
  isLandscape: boolean,
): boolean => {
  "worklet";
  const scaledContentWidth = contentWidth * currentScale;
  return (
    lockHorizontalPanNearFit &&
    isLandscape &&
    (
      currentScale <= HORIZONTAL_LOCK_MAX_SCALE ||
      scaledContentWidth <= containerWidth
    )
  );
};
