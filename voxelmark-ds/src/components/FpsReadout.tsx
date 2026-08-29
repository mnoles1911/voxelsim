import type { CSSProperties } from 'react';

/** Past this, the worst-frame figure goes red. The same threshold the in-game perf HUD calls a hitch. */
const HITCH_MS = 33;

export interface FpsReadoutProps {
  /** Frames per second, as a whole number. */
  fps?: number;
  /** Worst frame in the recent window, in milliseconds. */
  worstMs?: number;
  className?: string;
  style?: CSSProperties;
}

/**
 * The two-line frame counter in the loading screen's top-right corner, outlined
 * in black so it stays readable over any background art.
 *
 * The worst-frame figure turns red past 33 ms, which is the same threshold the
 * in-game performance HUD uses -- the two readouts agree about what counts as a
 * bad frame on purpose.
 *
 * Absolutely positioned, so its parent must be positioned.
 *
 * @example
 * <FpsReadout fps={58} worstMs={19} />
 * <FpsReadout fps={11} worstMs={104} />
 */
export function FpsReadout({ fps = 60, worstMs = 16, className, style }: FpsReadoutProps) {
  const bad = worstMs > HITCH_MS;
  const classes = ['vm-fps-readout', bad ? 'vm-fps-readout--bad' : null, className]
    .filter(Boolean)
    .join(' ');
  return (
    <p className={classes} style={style}>
      {`FPS: ${Math.round(fps)}\nworst: ${Math.round(worstMs)} ms`}
    </p>
  );
}
