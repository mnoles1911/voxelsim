import { Panel, Divider, Text } from '@voxelmark/design-system';

export const BetweenSections = () => (
  <div style={{ background: '#1a1410', padding: 28 }}>
    <Panel style={{ width: 400 }}>
      <Text tone="body" size="body" as="div">Graphics</Text>
      <Divider />
      <Text tone="body" size="body" as="div" style={{ marginTop: 8 }}>Audio</Text>
      <Divider />
      <Text tone="body" size="body" as="div" style={{ marginTop: 8 }}>Controls</Text>
    </Panel>
  </div>
);
