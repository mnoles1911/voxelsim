import { VersionStamp } from '@voxelmark/design-system';

// It is absolutely positioned against the screen corner, so the cell provides
// the positioned box it expects.
export const InTheCorner = () => (
  <div style={{ position: 'relative', width: 480, height: 160, background: '#0a0a0f' }}>
    <VersionStamp />
  </div>
);
