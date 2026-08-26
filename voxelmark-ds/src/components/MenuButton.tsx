import type { CSSProperties, MouseEventHandler, ReactNode } from 'react';

export interface MenuButtonProps {
  /** The label. Menu labels are upper case throughout the game. */
  children?: ReactNode;
  /**
   * `default` is the full-width 56px-tall menu entry. `compact` is the 96x44
   * row action (LOAD / DELETE in the save list); `dialog` is the 160x44
   * confirm/cancel width.
   */
  size?: 'default' | 'compact' | 'dialog';
  /**
   * Red resting label, for destructive actions -- this is exactly what DELETE
   * uses. Only the resting state changes: a hovered destructive button goes
   * gold like every other one.
   */
  destructive?: boolean;
  disabled?: boolean;
  onClick?: MouseEventHandler<HTMLButtonElement>;
  className?: string;
  style?: CSSProperties;
  type?: 'button' | 'submit' | 'reset';
}

/**
 * The oak menu button -- the only button in the game's front end.
 *
 * Four states, all ported from `UIStyles.menu_button_styles()`: dark oak on
 * black at rest, lighter oak with a gold border and gold label on hover, a
 * darkened fill with deep-gold trim while pressed, and a flattened fill with
 * iron trim and muted text when disabled. Square corners, 2px border, no
 * transition -- the state change is instant, as it is in game.
 *
 * CONTINUE and LOAD GAME are disabled whenever there are no saves; that is the
 * state to reach for rather than hiding them.
 *
 * @example
 * <MenuButton onClick={startGame}>NEW GAME</MenuButton>
 * <MenuButton disabled>CONTINUE</MenuButton>
 * <MenuButton size="compact" destructive>DELETE</MenuButton>
 */
export function MenuButton({
  children,
  size = 'default',
  destructive = false,
  disabled = false,
  onClick,
  className,
  style,
  type = 'button',
}: MenuButtonProps) {
  const classes = [
    'vm-menu-button',
    size !== 'default' ? `vm-menu-button--${size}` : null,
    destructive ? 'vm-menu-button--danger' : null,
    className,
  ]
    .filter(Boolean)
    .join(' ');
  return (
    <button type={type} className={classes} disabled={disabled} onClick={onClick} style={style}>
      {children}
    </button>
  );
}
