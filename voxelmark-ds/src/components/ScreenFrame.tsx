import type { CSSProperties, ReactNode } from 'react';
import { backgrounds, type BackgroundName } from '../backgrounds';

export interface ScreenFrameProps {
  children?: ReactNode;
  /**
   * Which of the six menu backdrops to show. Omit for the flat #0a0a0f
   * backdrop -- and note that omitting it also removes the wash, deliberately
   * (see below).
   */
  background?: BackgroundName;
  /**
   * `menu` is the main menu's 55% wash in night-black; `loading` is the
   * loading screen's heavier 62% pure black. The wash is what makes text over
   * photographic art readable.
   */
  wash?: 'menu' | 'loading';
  className?: string;
  style?: CSSProperties;
}

/**
 * The full-bleed screen frame both screens are built on: flat backdrop,
 * optional photographic art, a wash over the art, then content.
 *
 * **The wash goes away with the art, on purpose.** With no background image
 * the screen sits on its flat near-black backdrop, and laying the wash over
 * that anyway would put near-black buttons on near-black -- which is how a
 * graceful fallback turns into an unreadable screen. Pass no `background` and
 * you get no wash.
 *
 * @example
 * <ScreenFrame background="cave" wash="loading">
 *   <LoadingScreen progress={0.42} />
 * </ScreenFrame>
 */
export function ScreenFrame({
  children,
  background,
  wash = 'menu',
  className,
  style,
}: ScreenFrameProps) {
  const art = background ? backgrounds[background] : undefined;
  const washColor =
    wash === 'loading' ? 'var(--vm-wash-loading)' : 'rgba(13, 10, 7, var(--vm-background-tint-alpha))';
  return (
    <div className={['vm-screen', className].filter(Boolean).join(' ')} style={style}>
      {art ? <img className="vm-screen__art" src={art} alt="" /> : null}
      {art ? <div className="vm-screen__wash" style={{ background: washColor }} /> : null}
      <div className="vm-screen__content">{children}</div>
    </div>
  );
}
