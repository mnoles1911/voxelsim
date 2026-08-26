import { Panel, Divider, Text, MenuButton } from '@voxelmark/design-system';

// Panels are built to sit on photographic art -- the hard drop shadow is what
// separates them from it, so every cell here puts one on a backdrop.
const OnArt = ({ children }: { children: React.ReactNode }) => (
  <div style={{ background: 'linear-gradient(150deg, #6b4520, #1a1410 55%, #2e1b0d)', padding: 32 }}>
    {children}
  </div>
);

export const OakPanel = () => (
  <OnArt>
    <Panel style={{ width: 420 }}>
      <Text tone="title" size="panel-title" as="div">The Ash Throne</Text>
      <Text tone="body" size="body" as="p" style={{ marginTop: 10 }}>
        Oak fill, a hard 2px black border, and the offset shadow beneath it.
      </Text>
    </Panel>
  </OnArt>
);

export const IronPanel = () => (
  <OnArt>
    <Panel tone="iron" style={{ width: 420 }}>
      <Text tone="body" size="body" as="div">
        The iron treatment: cooler and darker, for surfaces that should recede.
      </Text>
    </Panel>
  </OnArt>
);

export const WithDivider = () => (
  <OnArt>
    <Panel style={{ width: 420 }}>
      <Text tone="title" size="panel-title" as="div">SETTINGS</Text>
      <Divider />
      <Text tone="dim" size="body" as="p" style={{ margin: '10px 0' }}>
        Sections are separated by the same black the border uses.
      </Text>
      <Divider />
      <div style={{ marginTop: 10, display: 'flex', justifyContent: 'center' }}>
        <MenuButton size="dialog">BACK</MenuButton>
      </div>
    </Panel>
  </OnArt>
);
