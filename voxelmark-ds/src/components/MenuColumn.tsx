import type { CSSProperties, ReactNode } from 'react';

export interface MenuColumnProps {
  /** The buttons, in order. */
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
}

/**
 * The 520px-wide column the main menu's buttons stack in, at the game's 14px
 * separation.
 *
 * The title block sits **above** this rather than inside it, because the title
 * is wider than the column and must not inherit its width.
 *
 * @example
 * <MenuColumn>
 *   <MenuButton disabled>CONTINUE</MenuButton>
 *   <MenuButton>NEW GAME</MenuButton>
 *   <MenuColumnGap variant="quit" />
 *   <MenuButton>QUIT</MenuButton>
 * </MenuColumn>
 */
export function MenuColumn({ children, className, style }: MenuColumnProps) {
  return (
    <div className={['vm-menu-column', className].filter(Boolean).join(' ')} style={style}>
      {children}
    </div>
  );
}

export interface MenuColumnGapProps {
  /**
   * `title` is the 36px break between the title block and the first button;
   * `quit` is the 80px break that sets QUIT apart from everything above it.
   */
  variant?: 'title' | 'quit';
  className?: string;
  style?: CSSProperties;
}

/**
 * The two deliberate breaks in the menu's rhythm. Both are ported values, not
 * taste: the 80px gap above QUIT is what stops a misclick from quitting the
 * game.
 */
export function MenuColumnGap({ variant = 'quit', className, style }: MenuColumnGapProps) {
  const classes = [
    variant === 'title' ? 'vm-menu-column__title-gap' : 'vm-menu-column__quit-gap',
    className,
  ]
    .filter(Boolean)
    .join(' ');
  return <div className={classes} style={style} aria-hidden="true" />;
}
