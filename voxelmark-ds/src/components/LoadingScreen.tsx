import type { CSSProperties, ReactNode } from 'react';
import type { BackgroundName } from '../backgrounds';
import { LOADING_TITLE } from '../strings';
import { ScreenFrame } from './ScreenFrame';
import { Hourglass } from './Hourglass';
import { ProgressBar, ProgressPercent } from './ProgressBar';
import { LoadingQuip } from './LoadingQuip';
import { TipFooter } from './TipFooter';
import { FpsReadout } from './FpsReadout';

export interface LoadingScreenProps {
  /** How far through the load, 0 to 1. Drives both the bar and the hourglass. */
  progress?: number;
  /** The flavour line. Defaults to the game's first quip. */
  quip?: ReactNode;
  /** The footer tip. Defaults to the game's first tip. Pass `null` to hide the footer. */
  tip?: ReactNode;
  /** Which backdrop to show behind the 62% wash. Omit for the flat backdrop. */
  background?: BackgroundName;
  /** Show the frame counter in the corner. Off by default -- it is a dev readout. */
  showFps?: boolean;
  fps?: number;
  worstMs?: number;
  /** Animate the hourglass bob, the grains and the quip crossfade. */
  animated?: boolean;
  className?: string;
  style?: CSSProperties;
}

/**
 * The whole loading screen: backdrop and wash, then a centred column carrying
 * the hourglass, the L O A D I N G heading, the quip, the bar and the
 * percentage -- with the tip footer along the bottom.
 *
 * This is a **long-lived screen, not a flash**: a cold start measures tens of
 * seconds, which is why it carries rotating copy and an animated hourglass at
 * all. Designs built on it should assume the player reads it.
 *
 * The heading's spaced letters are literal -- neither Godot labels nor Slate
 * have letter-spacing, so the source fakes the tracking with spaces and every
 * port has kept the same trick.
 *
 * @example
 * <LoadingScreen progress={0.42} background="cave" />
 */
export function LoadingScreen({
  progress = 0,
  quip,
  tip,
  background,
  showFps = false,
  fps = 60,
  worstMs = 16,
  animated = true,
  className,
  style,
}: LoadingScreenProps) {
  return (
    <ScreenFrame background={background} wash="loading" className={className} style={style}>
      <div className="vm-loading-column">
        <div className="vm-hourglass-wrap">
          <Hourglass progress={progress} animated={animated} />
        </div>
        <h1 className="vm-loading-title">{LOADING_TITLE}</h1>
        <LoadingQuip cycling={animated}>{quip ?? undefined}</LoadingQuip>
        <ProgressBar progress={progress} />
        <ProgressPercent progress={progress} />
      </div>
      {tip === null ? null : <TipFooter>{tip ?? undefined}</TipFooter>}
      {showFps ? <FpsReadout fps={fps} worstMs={worstMs} /> : null}
    </ScreenFrame>
  );
}
