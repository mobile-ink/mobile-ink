export const CONTINUOUS_ENGINE_POOL_SIZE = 3;

export const getContinuousEnginePoolRange = (
  currentPageIndex: number,
  totalPages: number,
  poolSize: number = CONTINUOUS_ENGINE_POOL_SIZE,
) => {
  if (totalPages <= 0 || poolSize <= 0) {
    return { startIndex: 0, endIndex: -1 };
  }

  const boundedPoolSize = Math.min(poolSize, totalPages);
  const boundedPageIndex = Math.max(
    0,
    Math.min(totalPages - 1, currentPageIndex),
  );
  const maxStart = Math.max(0, totalPages - boundedPoolSize);
  const centeredStart = boundedPageIndex - Math.floor(boundedPoolSize / 2);
  const startIndex = Math.max(0, Math.min(maxStart, centeredStart));

  return {
    startIndex,
    endIndex: startIndex + boundedPoolSize - 1,
  };
};
