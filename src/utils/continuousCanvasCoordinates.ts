export interface ContinuousTapInput {
  rawLocationX: number;
  rawLocationY: number;
  isZoomableContentTap: boolean;
  canvasWrapperOffset: {
    x: number;
    y: number;
  };
  canvasHeight: number;
  totalPages: number;
}

export interface ContinuousTapCoordinates {
  stackLocationX: number;
  stackLocationY: number;
  pageIndex: number;
  localX: number;
  localY: number;
}

export function resolveContinuousTapCoordinates({
  rawLocationX,
  rawLocationY,
  isZoomableContentTap,
  canvasWrapperOffset,
  canvasHeight,
  totalPages,
}: ContinuousTapInput): ContinuousTapCoordinates | null {
  if (canvasHeight <= 0 || totalPages <= 0) {
    return null;
  }

  const stackLocationX = isZoomableContentTap
    ? rawLocationX - canvasWrapperOffset.x
    : rawLocationX;
  const stackLocationY = isZoomableContentTap
    ? rawLocationY - canvasWrapperOffset.y
    : rawLocationY;

  const pageIndex = Math.max(
    0,
    Math.min(totalPages - 1, Math.floor(stackLocationY / canvasHeight)),
  );

  return {
    stackLocationX,
    stackLocationY,
    pageIndex,
    localX: stackLocationX,
    localY: stackLocationY - pageIndex * canvasHeight,
  };
}
