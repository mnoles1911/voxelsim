import type { CSSProperties, ReactNode } from 'react';

export interface PanelProps {
  children?: ReactNode;
  /**
   * `sub` is the fixed 720x560 frame every sub-screen uses (LOAD GAME, HELP,
   * CREDITS, SETTINGS). Omit it for a panel that sizes to its content.
   */
  size?: 'auto' | 'sub';
  /** Iron fill instead of oak -- the darker, cooler panel treatment. */
  tone?: 'oak' | 'iron';
  className?: string;
  style?: CSSProperties;
}

/**
 * The oak body panel: oak fill, a hard 2px black border, 16px of inner
 * padding, and the drop shadow beneath it.
 *
 * The shadow is a **hard-edged rectangle**, not a blur -- offset 4px down and
 * spread 8px. That is what the game draws (Slate brushes carry no blur
 * parameter), and matching it is what keeps a design made here looking like the
 * shipping screen. Panels are meant to sit on the photographic backdrops; the
 * shadow is what separates them from the art.
 *
 * @example
 * <Panel size="sub">
 *   <div className="vm-panel__content">
 *     <Text as="h2" tone="title" size="panel-title">HELP</Text>
 *   </div>
 * </Panel>
 */
export function Panel({ children, size = 'auto', tone = 'oak', className, style }: PanelProps) {
  const classes = [
    'vm-panel',
    size === 'sub' ? 'vm-panel--sub' : null,
    tone === 'iron' ? 'vm-panel--iron' : null,
    className,
  ]
    .filter(Boolean)
    .join(' ');
  return (
    <div className={classes} style={style}>
      {children}
    </div>
  );
}

export interface DividerProps {
  className?: string;
  style?: CSSProperties;
}

/**
 * The 1px black rule the panels use to separate sections -- the same black as
 * the panel border, so the two read as one frame.
 */
export function Divider({ className, style }: DividerProps) {
  return <hr className={['vm-divider', className].filter(Boolean).join(' ')} style={style} />;
}
