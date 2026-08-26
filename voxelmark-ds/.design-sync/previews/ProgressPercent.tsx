import { ProgressPercent } from '@voxelmark/design-system';

// It floors rather than rounds: 99.6% must read 99, never 100, while the world
// is still landing.
export const Floors = () => (
  <div style={{ background: '#0a0a0f', padding: 24, display: 'flex', gap: 32, alignItems: 'center' }}>
    <ProgressPercent progress={0} />
    <ProgressPercent progress={0.427} />
    <ProgressPercent progress={0.996} />
    <ProgressPercent progress={1} />
  </div>
);
