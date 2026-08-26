import { Hourglass } from '@voxelmark/design-system';

const Stage = ({ children }: { children: React.ReactNode }) => (
  <div style={{ background: '#0a0a0f', padding: '24px 32px', display: 'flex', gap: 40, alignItems: 'flex-start' }}>
    {children}
  </div>
);

const Labelled = ({ label, children }: { label: string; children: React.ReactNode }) => (
  <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 10 }}>
    <div style={{ width: 96, height: 144 }}>{children}</div>
    <span style={{ color: '#7a6a4e', fontSize: 12 }}>{label}</span>
  </div>
);

export const Draining = () => (
  <Stage>
    <Labelled label="0%">
      <Hourglass progress={0} animated={false} />
    </Labelled>
    <Labelled label="25%">
      <Hourglass progress={0.25} animated={false} />
    </Labelled>
    <Labelled label="60%">
      <Hourglass progress={0.6} animated={false} />
    </Labelled>
    <Labelled label="100%">
      <Hourglass progress={1} animated={false} />
    </Labelled>
  </Stage>
);

export const MidLoad = () => (
  <Stage>
    <Labelled label="falling grains, mid-load">
      <Hourglass progress={0.42} />
    </Labelled>
  </Stage>
);
