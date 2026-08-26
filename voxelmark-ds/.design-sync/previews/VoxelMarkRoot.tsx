import { VoxelMarkRoot, MenuButton, MenuTitle, MenuSubtitle, Panel, Text } from '@voxelmark/design-system';

// NOTE: an "outside the root" contrast cell is not expressible here -- the sync
// config wraps every preview in VoxelMarkRoot as its provider, so an unwrapped
// cell renders wrapped anyway and the two come out identical. What the root
// does is shown by what it carries instead.

export const WhatTheRootProvides = () => (
  <VoxelMarkRoot fill={false} style={{ padding: 24, width: 560 }}>
    <MenuTitle />
    <MenuSubtitle />
    <div style={{ marginTop: 20, display: 'flex', flexDirection: 'column', gap: 14 }}>
      <MenuButton>NEW GAME</MenuButton>
      <MenuButton disabled>CONTINUE</MenuButton>
    </div>
  </VoxelMarkRoot>
);

export const FillsItsContainer = () => (
  <div style={{ width: 560, height: 240 }}>
    <VoxelMarkRoot style={{ display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
      <Panel style={{ width: 340 }}>
        <Text tone="title" size="panel-title" as="div">
          Backdrop, edge to edge
        </Text>
        <Text tone="dim" size="body" as="p" style={{ marginTop: 8 }}>
          With fill on, the root takes the whole box and paints the menu backdrop.
        </Text>
      </Panel>
    </VoxelMarkRoot>
  </div>
);
