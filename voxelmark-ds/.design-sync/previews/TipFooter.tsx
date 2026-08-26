import { TipFooter } from '@voxelmark/design-system';

// Absolutely positioned against the screen's bottom edge, so the cell supplies
// a positioned box the size of a screen footer.
const Footer = ({ children }: { children: React.ReactNode }) => (
  <div style={{ position: 'relative', width: 760, height: 90, background: '#0a0a0f' }}>{children}</div>
);

export const Default = () => (
  <Footer>
    <TipFooter />
  </Footer>
);

export const LongerTip = () => (
  <Footer>
    <TipFooter>Roland flinches when low. The HUD rarely lies, but his body never does.</TipFooter>
  </Footer>
);
