export const computeDataSignature = (data: string): string => {
  const len = data.length;
  if (len <= 128) return `${len}:${data}`;
  return `${len}:${data.slice(0, 64)}:${data.slice(-64)}`;
};
