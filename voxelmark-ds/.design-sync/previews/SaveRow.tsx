import { SaveRow } from '@voxelmark/design-system';

export const Loadable = () => (
  <div style={{ background: '#4a2f1a', padding: 16, width: 620 }}>
    <SaveRow name="Ashfall Hold" detail="2026-08-24 19:12   X -61440  Y -61440  Z 402" />
  </div>
);

export const Unloadable = () => (
  <div style={{ background: '#4a2f1a', padding: 16, width: 620 }}>
    <SaveRow
      name="Drun-Khazad, deep run"
      loadable={false}
      disabledReason="Saved by an older build - world format 19"
    />
  </div>
);
