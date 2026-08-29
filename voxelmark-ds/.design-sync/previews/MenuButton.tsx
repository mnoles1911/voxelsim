import { MenuButton } from '@voxelmark/design-system';

export const MainMenuEntries = () => (
  <div style={{ display: 'flex', flexDirection: 'column', gap: 14, width: 520 }}>
    <MenuButton>NEW GAME</MenuButton>
    <MenuButton>SETTINGS</MenuButton>
    <MenuButton>CREDITS</MenuButton>
  </div>
);

export const Disabled = () => (
  <div style={{ display: 'flex', flexDirection: 'column', gap: 14, width: 520 }}>
    <MenuButton disabled>CONTINUE</MenuButton>
    <MenuButton disabled>LOAD GAME</MenuButton>
  </div>
);

export const RowActions = () => (
  <div style={{ display: 'flex', gap: 6 }}>
    <MenuButton size="compact">LOAD</MenuButton>
    <MenuButton size="compact" destructive>
      DELETE
    </MenuButton>
  </div>
);

export const DialogWidth = () => (
  <div style={{ display: 'flex', gap: 10 }}>
    <MenuButton size="dialog">BACK</MenuButton>
    <MenuButton size="dialog">CANCEL</MenuButton>
  </div>
);
