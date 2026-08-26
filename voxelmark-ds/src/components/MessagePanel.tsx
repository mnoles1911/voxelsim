import type { CSSProperties, ReactNode } from 'react';
import { Panel } from './Panel';
import { MenuButton } from './MenuButton';

export interface MessagePanelProps {
  /** Panel heading -- HELP, CREDITS, SETTINGS. */
  title: ReactNode;
  /**
   * Body copy. Newlines are preserved, and the body scrolls when it outgrows
   * the panel, which the credits do.
   */
  children?: ReactNode;
  /** Label of the footer button. BACK by default; the load panel uses CANCEL. */
  actionLabel?: ReactNode;
  onAction?: () => void;
  className?: string;
  style?: CSSProperties;
}

/**
 * The shape every sub-screen shares: a gold heading, a scrolling body, and one
 * button that takes you back.
 *
 * HELP, CREDITS and SETTINGS are all this component in the game -- three copies
 * of a panel is how three panels start disagreeing. The frame is a fixed
 * 720x560, so long bodies scroll rather than growing the panel.
 *
 * @example
 * <MessagePanel title="HELP">Help content coming soon.</MessagePanel>
 */
export function MessagePanel({
  title,
  children,
  actionLabel = 'BACK',
  onAction,
  className,
  style,
}: MessagePanelProps) {
  return (
    <Panel size="sub" className={className} style={style}>
      <div className="vm-panel__content">
        <h2 className="vm-message-panel__title">{title}</h2>
        <div className="vm-message-panel__body">{children}</div>
        <div className="vm-message-panel__footer">
          <MenuButton size="dialog" onClick={onAction}>
            {actionLabel}
          </MenuButton>
        </div>
      </div>
    </Panel>
  );
}
