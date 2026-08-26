import { SaveList, SaveRow, Panel, Text } from '@voxelmark/design-system';

const OnArt = ({ children }: { children: React.ReactNode }) => (
  <div style={{ background: 'linear-gradient(150deg, #3a2410, #0d0a07 60%, #2e1b0d)', padding: 32 }}>
    {children}
  </div>
);

export const WithSaves = () => (
  <OnArt>
  <Panel style={{ width: 660 }}>
    <Text tone="title" size="panel-title" as="div" style={{ textAlign: 'center', marginBottom: 10 }}>
      LOAD GAME
    </Text>
    <SaveList>
      <SaveRow name="Ashfall Hold" detail="2026-08-24 19:12   X -61440  Y -61440  Z 402" />
      <SaveRow name="Riverwatch" detail="2026-08-22 08:41   X -46080  Y -30720  Z 118" />
      <SaveRow
        name="Drun-Khazad, deep run"
        loadable={false}
        disabledReason="Saved by an older build - world format 19"
      />
    </SaveList>
  </Panel>
  </OnArt>
);

export const Empty = () => (
  <OnArt>
  <Panel style={{ width: 660 }}>
    <Text tone="title" size="panel-title" as="div" style={{ textAlign: 'center', marginBottom: 10 }}>
      LOAD GAME
    </Text>
    <SaveList />
  </Panel>
  </OnArt>
);
