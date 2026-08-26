import { MessagePanel } from '@voxelmark/design-system';

const OnArt = ({ children }: { children: React.ReactNode }) => (
  <div style={{ background: 'linear-gradient(150deg, #3a2410, #0d0a07 60%, #2e1b0d)', padding: 36 }}>
    {children}
  </div>
);

export const Help = () => (
  <OnArt>
    <MessagePanel title="HELP">Help content coming soon.</MessagePanel>
  </OnArt>
);

export const Credits = () => (
  <OnArt>
    <MessagePanel title="CREDITS">
      {`VOXELMARK
Mira-Thal Trilogy - Game One


THIRD-PARTY NOTICES

Moon surface and elevation
NASA's Scientific Visualization Studio.

Star map
NASA/Goddard Space Flight Center SVS.
Gaia DR2: ESA/Gaia/DPAC.

Macondo Swash Caps
Open Font Licence 1.1.`}
    </MessagePanel>
  </OnArt>
);
