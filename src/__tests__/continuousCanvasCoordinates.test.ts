import { resolveContinuousTapCoordinates } from '../utils/continuousCanvasCoordinates';

describe('continuousCanvasCoordinates', () => {
  it('resolves zoomable taps from the full page stack, not the rendered engine window', () => {
    const resolved = resolveContinuousTapCoordinates({
      rawLocationX: 156,
      rawLocationY: 2216,
      isZoomableContentTap: true,
      canvasWrapperOffset: { x: 36, y: 16 },
      canvasHeight: 1000,
      totalPages: 6,
    });

    expect(resolved).toEqual({
      stackLocationX: 120,
      stackLocationY: 2200,
      pageIndex: 2,
      localX: 120,
      localY: 200,
    });
  });

  it('leaves direct canvas taps as already stack-local coordinates', () => {
    const resolved = resolveContinuousTapCoordinates({
      rawLocationX: 80,
      rawLocationY: 3075,
      isZoomableContentTap: false,
      canvasWrapperOffset: { x: 36, y: 16 },
      canvasHeight: 1000,
      totalPages: 6,
    });

    expect(resolved).toEqual({
      stackLocationX: 80,
      stackLocationY: 3075,
      pageIndex: 3,
      localX: 80,
      localY: 75,
    });
  });

  it('keeps taps past the last page bounded to the last page', () => {
    const resolved = resolveContinuousTapCoordinates({
      rawLocationX: 80,
      rawLocationY: 6500,
      isZoomableContentTap: false,
      canvasWrapperOffset: { x: 0, y: 0 },
      canvasHeight: 1000,
      totalPages: 4,
    });

    expect(resolved?.pageIndex).toBe(3);
    expect(resolved?.localY).toBe(3500);
  });

  it('returns null for invalid page geometry', () => {
    expect(resolveContinuousTapCoordinates({
      rawLocationX: 80,
      rawLocationY: 100,
      isZoomableContentTap: false,
      canvasWrapperOffset: { x: 0, y: 0 },
      canvasHeight: 0,
      totalPages: 4,
    })).toBeNull();
    expect(resolveContinuousTapCoordinates({
      rawLocationX: 80,
      rawLocationY: 100,
      isZoomableContentTap: false,
      canvasWrapperOffset: { x: 0, y: 0 },
      canvasHeight: 1000,
      totalPages: 0,
    })).toBeNull();
  });
});
