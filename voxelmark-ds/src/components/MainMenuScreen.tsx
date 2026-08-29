import type { CSSProperties, ReactNode } from 'react';
import type { BackgroundName } from '../backgrounds';
import { ScreenFrame } from './ScreenFrame';
import { MenuTitle, MenuSubtitle, VersionStamp } from './MenuHeader';
import { MenuColumn, MenuColumnGap } from './MenuColumn';
import { MenuButton } from './MenuButton';

export interface MainMenuScreenProps {
  /** Which backdrop to show behind the 55% wash. Omit for the flat backdrop. */
  background?: BackgroundName;
  /**
   * Whether there is anything to continue or load. When false -- the state a
   * fresh install is in -- CONTINUE and LOAD GAME are disabled together.
   */
  hasSaves?: boolean;
  /** Replaces the whole centre block, for showing a sub-panel over the backdrop. */
  children?: ReactNode;
  /** Build stamp in the corner. Pass `null` to hide it. */
  version?: ReactNode;
  onContinue?: () => void;
  onNewGame?: () => void;
  onLoadGame?: () => void;
  onSettings?: () => void;
  onHelp?: () => void;
  onCredits?: () => void;
  onQuit?: () => void;
  className?: string;
  style?: CSSProperties;
}

/**
 * The main menu, whole: backdrop and wash, the title block, the seven-entry
 * button column, and the build stamp in the corner.
 *
 * Two details worth keeping when you adapt it. CONTINUE and LOAD GAME are
 * **disabled rather than hidden** when there are no saves, so the menu a player
 * sees is always the same menu. And QUIT sits behind an 80px gap -- every other
 * entry is 14px apart, and that break is what stops a misclick from quitting
 * the game.
 *
 * Pass `children` to show a sub-panel (LOAD GAME, HELP, CREDITS) over the same
 * backdrop instead of the column; exactly one is on screen at a time.
 *
 * @example
 * <MainMenuScreen background="castleFeast" hasSaves />
 *
 * @example
 * <MainMenuScreen background="cave">
 *   <MessagePanel title="CREDITS">{creditsBody}</MessagePanel>
 * </MainMenuScreen>
 */
export function MainMenuScreen({
  background,
  hasSaves = false,
  children,
  version,
  onContinue,
  onNewGame,
  onLoadGame,
  onSettings,
  onHelp,
  onCredits,
  onQuit,
  className,
  style,
}: MainMenuScreenProps) {
  return (
    <ScreenFrame background={background} wash="menu" className={className} style={style}>
      <div className="vm-center" style={{ width: '100%', height: '100%' }}>
        {children ?? (
          <div>
            <MenuTitle />
            <MenuSubtitle />
            <MenuColumnGap variant="title" />
            <MenuColumn>
              <MenuButton disabled={!hasSaves} onClick={onContinue}>
                CONTINUE
              </MenuButton>
              <MenuButton onClick={onNewGame}>NEW GAME</MenuButton>
              <MenuButton disabled={!hasSaves} onClick={onLoadGame}>
                LOAD GAME
              </MenuButton>
              <MenuButton onClick={onSettings}>SETTINGS</MenuButton>
              <MenuButton onClick={onHelp}>HELP</MenuButton>
              <MenuButton onClick={onCredits}>CREDITS</MenuButton>
              <MenuColumnGap variant="quit" />
              <MenuButton onClick={onQuit}>QUIT</MenuButton>
            </MenuColumn>
          </div>
        )}
      </div>
      {version === null ? null : <VersionStamp>{version ?? undefined}</VersionStamp>}
    </ScreenFrame>
  );
}
