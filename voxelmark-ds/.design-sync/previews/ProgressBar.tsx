import { ProgressBar, ProgressPercent } from '@voxelmark/design-system';

const Stage = ({ children }: { children: React.ReactNode }) => (
  <div
    style={{
      background: '#0a0a0f',
      padding: 28,
      display: 'flex',
      flexDirection: 'column',
      gap: 18,
      alignItems: 'center',
    }}
  >
    {children}
  </div>
);

export const Steps = () => (
  <Stage>
    <ProgressBar progress={0} />
    <ProgressBar progress={0.18} />
    <ProgressBar progress={0.42} />
    <ProgressBar progress={0.93} />
    <ProgressBar progress={1} />
  </Stage>
);

export const WithPercent = () => (
  <Stage>
    <ProgressBar progress={0.42} />
    <ProgressPercent progress={0.42} />
  </Stage>
);
