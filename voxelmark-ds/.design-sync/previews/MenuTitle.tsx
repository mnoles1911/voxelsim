import { MenuTitle, ScreenFrame } from '@voxelmark/design-system';

export const OnBackdrop = () => (
  <div style={{ width: 760, height: 200, position: 'relative', overflow: 'hidden' }}>
    <ScreenFrame background="fortressBattles" wash="menu">
      <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
        <MenuTitle />
      </div>
    </ScreenFrame>
  </div>
);

export const Flat = () => (
  <div style={{ background: '#0a0a0f', padding: '24px 0' }}>
    <MenuTitle />
  </div>
);
