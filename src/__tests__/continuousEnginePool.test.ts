import {
  CONTINUOUS_ENGINE_POOL_SIZE,
  getContinuousEnginePoolRange,
} from "../utils/continuousEnginePool";

describe("getContinuousEnginePoolRange", () => {
  it("keeps the pool fixed at three pages when enough pages exist", () => {
    const range = getContinuousEnginePoolRange(10, 20);

    expect(range).toEqual({ startIndex: 9, endIndex: 11 });
    expect(range.endIndex - range.startIndex + 1).toBe(CONTINUOUS_ENGINE_POOL_SIZE);
  });

  it("clamps to the start and end of the notebook", () => {
    expect(getContinuousEnginePoolRange(0, 20)).toEqual({
      startIndex: 0,
      endIndex: 2,
    });
    expect(getContinuousEnginePoolRange(19, 20)).toEqual({
      startIndex: 17,
      endIndex: 19,
    });
  });

  it("uses fewer slots only when the notebook has fewer than three pages", () => {
    expect(getContinuousEnginePoolRange(1, 2)).toEqual({
      startIndex: 0,
      endIndex: 1,
    });
  });
});
