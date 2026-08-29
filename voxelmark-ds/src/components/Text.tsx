import type { CSSProperties, ElementType, ReactNode } from 'react';

/** The label variants, as UIStyles.gd names them. */
export type TextTone = 'title' | 'body' | 'dim' | 'mute' | 'gold' | 'parchment' | 'danger';

/** The type scale the front end actually uses. Named by role, not by size. */
export type TextSize =
  | 'title'
  | 'loading'
  | 'panel-title'
  | 'button'
  | 'quip'
  | 'body'
  | 'row'
  | 'tip'
  | 'stamp';

/** The three drop shadows in the front end; `none` is the default. */
export type TextShadow = 'none' | 'title' | 'body' | 'tight';

export interface TextProps {
  children?: ReactNode;
  /** Ink colour. `body` (parchment ink) by default. */
  tone?: TextTone;
  /** Size step from the type scale. Inherits when omitted. */
  size?: TextSize;
  /**
   * Hard drop shadow. Text over the photographic backdrops needs one to stay
   * legible -- the screens use `title` for headings and `body` for everything
   * else on art.
   */
  shadow?: TextShadow;
  /** Element to render. `span` by default. */
  as?: ElementType;
  className?: string;
  style?: CSSProperties;
}

/**
 * Typography in the VoxelMark voice: the serif face, one of the four ink
 * colours, and optionally one of the hard drop shadows the screens use over
 * background art.
 *
 * Prefer this over a bare `<span>` so the tone comes from the palette rather
 * than an invented hex.
 *
 * @example
 * <Text as="h2" tone="title" size="panel-title">CREDITS</Text>
 * <Text tone="dim" size="quip" shadow="body">Pillaging villages...</Text>
 */
export function Text({
  children,
  tone = 'body',
  size,
  shadow = 'none',
  as: Tag = 'span',
  className,
  style,
}: TextProps) {
  const classes = [
    'vm-text',
    `vm-text--${tone}`,
    size ? `vm-type-${size}` : null,
    shadow !== 'none' ? `vm-shadow-${shadow}` : null,
    className,
  ]
    .filter(Boolean)
    .join(' ');
  return (
    <Tag className={classes} style={style}>
      {children}
    </Tag>
  );
}
