import { MenuColumn, MenuColumnGap, MenuButton } from '@voxelmark/design-system';

export const FullMenu = () => (
  <div style={{ background: '#0a0a0f', padding: 24 }}>
    <MenuColumn>
      <MenuButton disabled>CONTINUE</MenuButton>
      <MenuButton>NEW GAME</MenuButton>
      <MenuButton disabled>LOAD GAME</MenuButton>
      <MenuButton>SETTINGS</MenuButton>
      <MenuButton>HELP</MenuButton>
      <MenuButton>CREDITS</MenuButton>
      <MenuColumnGap variant="quit" />
      <MenuButton>QUIT</MenuButton>
    </MenuColumn>
  </div>
);

export const WithSaves = () => (
  <div style={{ background: '#0a0a0f', padding: 24 }}>
    <MenuColumn>
      <MenuButton>CONTINUE</MenuButton>
      <MenuButton>NEW GAME</MenuButton>
      <MenuButton>LOAD GAME</MenuButton>
    </MenuColumn>
  </div>
);
