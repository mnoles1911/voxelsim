// The VoxelMark design system.
//
// A React port of the game's front end (ue-project/Source/VoxelEarthUI): the
// main menu, the loading screen, and the panel/button/typography vocabulary
// they are built from. Colours and layout numbers are generated from the game's
// own C++ header, so they cannot drift.

export { VoxelMarkRoot } from './components/VoxelMarkRoot';
export type { VoxelMarkRootProps } from './components/VoxelMarkRoot';

export { Text } from './components/Text';
export type { TextProps, TextTone, TextSize, TextShadow } from './components/Text';

export { MenuButton } from './components/MenuButton';
export type { MenuButtonProps } from './components/MenuButton';

export { Panel, Divider } from './components/Panel';
export type { PanelProps, DividerProps } from './components/Panel';

export { MenuTitle, MenuSubtitle, VersionStamp } from './components/MenuHeader';
export type {
  MenuTitleProps,
  MenuSubtitleProps,
  VersionStampProps,
} from './components/MenuHeader';

export { MenuColumn, MenuColumnGap } from './components/MenuColumn';
export type { MenuColumnProps, MenuColumnGapProps } from './components/MenuColumn';

export { SaveRow, SaveList } from './components/SaveList';
export type { SaveRowProps, SaveListProps } from './components/SaveList';

export { MessagePanel } from './components/MessagePanel';
export type { MessagePanelProps } from './components/MessagePanel';

export { ScreenFrame } from './components/ScreenFrame';
export type { ScreenFrameProps } from './components/ScreenFrame';

export { MainMenuScreen } from './components/MainMenuScreen';
export type { MainMenuScreenProps } from './components/MainMenuScreen';

export { Hourglass } from './components/Hourglass';
export type { HourglassProps } from './components/Hourglass';

export { ProgressBar, ProgressPercent } from './components/ProgressBar';
export type { ProgressBarProps, ProgressPercentProps } from './components/ProgressBar';

export { LoadingQuip } from './components/LoadingQuip';
export type { LoadingQuipProps } from './components/LoadingQuip';

export { TipFooter } from './components/TipFooter';
export type { TipFooterProps } from './components/TipFooter';

export { FpsReadout } from './components/FpsReadout';
export type { FpsReadoutProps } from './components/FpsReadout';

export { LoadingScreen } from './components/LoadingScreen';
export type { LoadingScreenProps } from './components/LoadingScreen';

// --- data ------------------------------------------------------------------

export { colors, layout } from './tokens';
export type { VoxelMarkColor } from './tokens';

export { backgrounds, backgroundNames } from './backgrounds';
export type { BackgroundName } from './backgrounds';

export {
  TITLE,
  SUBTITLE,
  VERSION_STAMP,
  LOADING_TITLE,
  TIP_PREFIX,
  MENU_ITEMS,
  LOADING_QUIPS,
  GAMEPLAY_TIPS,
} from './strings';
