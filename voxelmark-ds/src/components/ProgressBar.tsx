import type { CSSProperties } from 'react';

export interface ProgressBarProps {
  /** 0 to 1. Values outside that range are clamped. */
  progress?: number;
  className?: string;
  style?: CSSProperties;
}

/**
 * The loading bar: a 520x8 dark-leather track with a bright sand fill running
 * left to right. No border, no radius, no gloss -- it is a flat two-colour bar.
 *
 * @example
 * <ProgressBar progress={0.42} />
 */
export function ProgressBar({ progress = 0, className, style }: ProgressBarProps) {
  const p = Math.min(Math.max(progress, 0), 1);
  return (
    <div
      className={['vm-progress', className].filter(Boolean).join(' ')}
      style={style}
      role="progressbar"
      aria-valuenow={Math.floor(p * 100)}
      aria-valuemin={0}
      aria-valuemax={100}
    >
      <div className="vm-progress__fill" style={{ width: `${p * 100}%` }} />
    </div>
  );
}

export interface ProgressPercentProps {
  /** 0 to 1. */
  progress?: number;
  className?: string;
  style?: CSSProperties;
}

/**
 * The percentage under the bar.
 *
 * It **floors** rather than rounding, deliberately: 99.6% must read 99%,
 * because a bar that says 100% while the world is still landing is the one lie
 * this whole progress model exists to avoid.
 */
export function ProgressPercent({ progress = 0, className, style }: ProgressPercentProps) {
  const p = Math.min(Math.max(progress, 0), 1);
  return (
    <p className={['vm-loading-percent', className].filter(Boolean).join(' ')} style={style}>
      {Math.floor(p * 100)}%
    </p>
  );
}
