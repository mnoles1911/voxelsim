import type { CSSProperties, ReactNode } from 'react';
import { MenuButton } from './MenuButton';

export interface SaveRowProps {
  /** The save's display name. */
  name: ReactNode;
  /** The second line: timestamp and world coordinates in game, free text here. */
  detail?: ReactNode;
  /**
   * A save the build cannot open (a world from an incompatible version, a
   * missing tile set). It stays visible and stays listed -- hiding it is how a
   * player concludes their save was deleted.
   */
  loadable?: boolean;
  /** Shown in place of `detail` when the save is not loadable. */
  disabledReason?: ReactNode;
  onLoad?: () => void;
  onDelete?: () => void;
  className?: string;
  style?: CSSProperties;
}

/**
 * One row of the LOAD GAME list: name and detail on the left, LOAD and DELETE
 * on the right.
 *
 * DELETE carries the destructive red label at rest. An unloadable save keeps
 * its row and its DELETE button but loses LOAD, and its second line explains
 * why instead of showing the timestamp.
 *
 * @example
 * <SaveRow name="Ashfall Hold" detail="2026-08-24 19:12   X -61440  Y -61440  Z 402" />
 * <SaveRow name="Old Run" loadable={false} disabledReason="Saved by an older build" />
 */
export function SaveRow({
  name,
  detail,
  loadable = true,
  disabledReason,
  onLoad,
  onDelete,
  className,
  style,
}: SaveRowProps) {
  const classes = ['vm-save-row', loadable ? null : 'vm-save-row--unloadable', className]
    .filter(Boolean)
    .join(' ');
  return (
    <div className={classes} style={style}>
      <div className="vm-save-row__text">
        <span className="vm-save-row__name">{name}</span>
        <span
          className={['vm-save-row__detail', loadable ? null : 'vm-save-row__detail--disabled']
            .filter(Boolean)
            .join(' ')}
        >
          {loadable ? detail : disabledReason}
        </span>
      </div>
      <div className="vm-save-row__actions">
        <MenuButton size="compact" disabled={!loadable} onClick={onLoad}>
          LOAD
        </MenuButton>
        <MenuButton size="compact" destructive onClick={onDelete}>
          DELETE
        </MenuButton>
      </div>
    </div>
  );
}

export interface SaveListProps {
  /** `SaveRow` children. Leave empty to show the empty state. */
  children?: ReactNode;
  /** Shown when there are no rows. */
  emptyMessage?: ReactNode;
  className?: string;
  style?: CSSProperties;
}

/**
 * The LOAD GAME list: save rows at 6px separation, or the empty message when
 * there are none.
 *
 * When this list is empty the menu's CONTINUE and LOAD GAME buttons are
 * disabled -- the two go together.
 */
export function SaveList({
  children,
  emptyMessage = 'No saves yet. Start a New Game to begin.',
  className,
  style,
}: SaveListProps) {
  const hasRows = Array.isArray(children) ? children.length > 0 : Boolean(children);
  return (
    <div className={['vm-save-list', className].filter(Boolean).join(' ')} style={style}>
      {hasRows ? children : <p className="vm-save-list__empty">{emptyMessage}</p>}
    </div>
  );
}
