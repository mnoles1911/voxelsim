import { Text } from '@voxelmark/design-system';

export const Tones = () => (
  <div style={{ display: 'flex', flexDirection: 'column', gap: 8, background: '#0a0a0f', padding: 16 }}>
    <Text tone="title" size="panel-title">
      Gold — headings and the title
    </Text>
    <Text tone="body" size="body">
      Body — parchment ink, the default for reading copy
    </Text>
    <Text tone="dim" size="body">
      Dim — subtitles, quips, secondary detail
    </Text>
    <Text tone="mute" size="body">
      Mute — the build stamp and anything that must not compete
    </Text>
    <Text tone="danger" size="body">
      Danger — the DELETE label, and nothing else
    </Text>
  </div>
);

export const TypeScale = () => (
  <div style={{ display: 'flex', flexDirection: 'column', gap: 10, background: '#0a0a0f', padding: 16 }}>
    <Text tone="title" size="loading" as="div">
      L O A D I N G
    </Text>
    <Text tone="title" size="panel-title" as="div">
      CREDITS
    </Text>
    <Text tone="body" size="button" as="div">
      NEW GAME
    </Text>
    <Text tone="dim" size="quip" as="div">
      Teaching wolves to read maps...
    </Text>
    <Text tone="body" size="body" as="div">
      Settlements are protected.
    </Text>
    <Text tone="mute" size="stamp" as="div">
      Milestone 5-3D — dev build
    </Text>
  </div>
);

export const OverArtwork = () => (
  <div
    style={{
      background: 'linear-gradient(140deg, #6b4520, #2e1b0d 60%, #4a2f1a)',
      padding: 24,
      display: 'flex',
      flexDirection: 'column',
      gap: 10,
    }}
  >
    <Text tone="title" size="panel-title" shadow="title" as="div">
      Shadowed heading
    </Text>
    <Text tone="dim" size="quip" shadow="body" as="div">
      Pouring mead for the long-dead...
    </Text>
    <Text tone="body" size="body" as="div">
      No shadow — legible on flat panels, muddy over art
    </Text>
  </div>
);
