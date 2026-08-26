import { LoadingQuip } from '@voxelmark/design-system';

const Stage = ({ children }: { children: React.ReactNode }) => (
  <div style={{ background: '#0a0a0f', padding: 24 }}>{children}</div>
);

export const ShortLine = () => (
  <Stage>
    <LoadingQuip>Apologizing to the goats...</LoadingQuip>
  </Stage>
);

// The two-line reservation is the whole point: this wraps, and the block does
// not change height when it does.
export const WrapsToTwoLines = () => (
  <Stage>
    <LoadingQuip>
      Inviting pirates to the royal feast, and counting the crown treasury twice...
    </LoadingQuip>
  </Stage>
);
