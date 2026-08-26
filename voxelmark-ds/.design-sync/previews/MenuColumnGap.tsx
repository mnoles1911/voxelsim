import { MenuColumn, MenuColumnGap, MenuButton, Text } from '@voxelmark/design-system';

// The gaps are invisible on their own, so both cells show them doing their job
// between real buttons.
export const QuitGap = () => (
  <div style={{ background: '#0a0a0f', padding: 24 }}>
    <Text tone="mute" size="stamp" as="div" style={{ marginBottom: 8 }}>
      14px between entries, 80px above QUIT
    </Text>
    <MenuColumn>
      <MenuButton>HELP</MenuButton>
      <MenuButton>CREDITS</MenuButton>
      <MenuColumnGap variant="quit" />
      <MenuButton>QUIT</MenuButton>
    </MenuColumn>
  </div>
);

export const TitleGap = () => (
  <div style={{ background: '#0a0a0f', padding: 24 }}>
    <Text tone="title" size="panel-title" as="div" style={{ textAlign: 'center', width: 520 }}>
      VOXELMARK
    </Text>
    <MenuColumnGap variant="title" />
    <MenuColumn>
      <MenuButton>NEW GAME</MenuButton>
    </MenuColumn>
  </div>
);
