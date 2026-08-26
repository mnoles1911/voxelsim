import type { CSSProperties, ReactNode } from 'react';

export interface VoxelMarkRootProps {
  /** The screens, panels and controls to render inside the theme. */
  children?: ReactNode;
  /**
   * Stretch to fill the space the root is placed in. On by default, because
   * both screens are full-bleed compositions; turn it off to drop a panel or a
   * button into a page that manages its own layout.
   */
  fill?: boolean;
  className?: string;
  style?: CSSProperties;
}

/**
 * The theme root. **Every VoxelMark component must be rendered inside one.**
 *
 * This is where the Macondo Swash Caps face, the parchment ink colour and the
 * square-cornered baseline are applied. A component rendered outside it still
 * renders, but in the host page's font and colour -- which is by far the most
 * common way a VoxelMark screen ends up looking wrong, because nothing errors.
 *
 * @example
 * <VoxelMarkRoot>
 *   <MainMenuScreen background="castleFeast" />
 * </VoxelMarkRoot>
 */
export function VoxelMarkRoot({ children, fill = true, className, style }: VoxelMarkRootProps) {
  const classes = ['vm-root', className].filter(Boolean).join(' ');
  return (
    <div
      className={classes}
      style={{ ...(fill ? { width: '100%', height: '100%' } : null), ...style }}
    >
      {children}
    </div>
  );
}
