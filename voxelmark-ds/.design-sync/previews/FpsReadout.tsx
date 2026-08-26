import { FpsReadout } from '@voxelmark/design-system';

// Pinned to the top-right of a positioned box, and shown over art because the
// black outline exists for exactly that case.
const Corner = ({ children }: { children: React.ReactNode }) => (
  <div
    style={{
      position: 'relative',
      width: 420,
      height: 120,
      background: 'linear-gradient(120deg, #6b4520, #8a5a28 50%, #4a2f1a)',
    }}
  >
    {children}
  </div>
);

export const Healthy = () => (
  <Corner>
    <FpsReadout fps={58} worstMs={19} />
  </Corner>
);

// Past 33 ms the worst-frame figure goes red -- the same threshold the in-game
// perf HUD calls a hitch.
export const Hitching = () => (
  <Corner>
    <FpsReadout fps={11} worstMs={104} />
  </Corner>
);
