export interface PollDelayOptions {
  baseS: number;
  maxS: number;
  jitterS: number;
  random?: () => number;
}

export function nextPollDelayS(
  consecutiveFailures: number,
  options: PollDelayOptions,
): number {
  const failureExponent = Math.max(0, Math.trunc(consecutiveFailures) - 1);
  const baseDelay = Math.min(options.maxS, options.baseS * (2 ** failureExponent));
  const random = Math.min(1, Math.max(0, (options.random ?? Math.random)()));
  const jitter = (random * 2 - 1) * Math.max(0, options.jitterS);
  return Math.max(20, Math.min(options.maxS, Math.round(baseDelay + jitter)));
}
