import type { CSSProperties, ReactNode } from 'react';
import { GAMEPLAY_TIPS, TIP_PREFIX } from '../strings';

export interface TipFooterProps {
  /** The tip text. Defaults to the first of the game's own tips. */
  children?: ReactNode;
  /** The gold prefix word. */
  prefix?: ReactNode;
  className?: string;
  style?: CSSProperties;
}

/**
 * The tip line pinned to the bottom of the loading screen: a gold TIP prefix,
 * then the tip in dim ink at 55% opacity.
 *
 * Absolutely positioned against the screen, 60px in from each side and 16px up
 * from the bottom -- so its parent must be positioned. `LoadingScreen` already
 * is.
 *
 * The game rotates 13 of these on a hard cut every 8 seconds (no fade, unlike
 * the quips); they are exported as `GAMEPLAY_TIPS`.
 *
 * @example
 * <TipFooter>Water flows. If you carve under a pond, expect a small flood.</TipFooter>
 */
export function TipFooter({
  children = GAMEPLAY_TIPS[0],
  prefix = TIP_PREFIX,
  className,
  style,
}: TipFooterProps) {
  return (
    <div className={['vm-tip-footer', className].filter(Boolean).join(' ')} style={style}>
      <span className="vm-tip-footer__prefix">{prefix}</span>
      <span className="vm-tip-footer__text">{children}</span>
    </div>
  );
}
