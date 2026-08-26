import type { CSSProperties, ReactNode } from 'react';
import { TITLE, SUBTITLE, VERSION_STAMP } from '../strings';

export interface MenuTitleProps {
  /** Defaults to the game's own title. */
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
}

/**
 * The game title: 84px gold, centred, on one line.
 *
 * It is deliberately allowed to be **wider than the 520px button column** --
 * inheriting that width is what once clipped "VOXELMARK" to "OXELMAR" in the
 * engine, losing a letter off each end. If a longer title is passed, widen the
 * `--vm-title-box-width` token rather than letting it wrap.
 *
 * @example
 * <MenuTitle />
 */
export function MenuTitle({ children = TITLE, className, style }: MenuTitleProps) {
  return (
    <h1 className={['vm-menu-title', className].filter(Boolean).join(' ')} style={style}>
      {children}
    </h1>
  );
}

export interface MenuSubtitleProps {
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
}

/**
 * The line under the title -- 18px dim ink. Where the game names the trilogy
 * and which instalment this is.
 */
export function MenuSubtitle({ children = SUBTITLE, className, style }: MenuSubtitleProps) {
  return (
    <p className={['vm-menu-subtitle', className].filter(Boolean).join(' ')} style={style}>
      {children}
    </p>
  );
}

export interface VersionStampProps {
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
}

/**
 * The build stamp pinned to the bottom-left corner of the menu, 16px in from
 * both edges, in the mutest ink the palette has. It is meant to be readable
 * but never to compete with anything.
 *
 * Absolutely positioned, so its parent must be positioned too -- inside
 * `MainMenuScreen` that is already the case.
 */
export function VersionStamp({ children = VERSION_STAMP, className, style }: VersionStampProps) {
  return (
    <p className={['vm-version-stamp', className].filter(Boolean).join(' ')} style={style}>
      {children}
    </p>
  );
}
