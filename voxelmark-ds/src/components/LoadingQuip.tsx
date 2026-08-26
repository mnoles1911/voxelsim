import type { CSSProperties, ReactNode } from 'react';
import { LOADING_QUIPS } from '../strings';

export interface LoadingQuipProps {
  /** The line to show. Defaults to the first of the game's own quips. */
  children?: ReactNode;
  /**
   * Run the crossfade cycle (2.5s, matching the game's rotation). The text
   * itself does not change -- rotating the copy is the host app's job; this
   * only carries the fade.
   */
  cycling?: boolean;
  className?: string;
  style?: CSSProperties;
}

/**
 * The flavour line under the loading heading -- 20px dim ink over a hard
 * shadow, centred in a 600px column.
 *
 * **It reserves room for two lines even when it shows one.** Without that, a
 * quip that wraps shoves the bar and the percentage down and back up every 2.5
 * seconds, which is far more distracting than the wrap itself.
 *
 * The game ships 24 of these and shuffles them per load; they are exported as
 * `LOADING_QUIPS` if you want the real copy.
 *
 * @example
 * <LoadingQuip>Counting the king's gold (twice)...</LoadingQuip>
 */
export function LoadingQuip({
  children = LOADING_QUIPS[0],
  cycling = false,
  className,
  style,
}: LoadingQuipProps) {
  const classes = ['vm-quip', cycling ? 'vm-quip--cycling' : null, className]
    .filter(Boolean)
    .join(' ');
  return (
    <p className={classes} style={style}>
      {children}
    </p>
  );
}
