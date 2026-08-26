import { MainMenuScreen, MessagePanel } from '@voxelmark/design-system';

const Screen = ({ children }: { children: React.ReactNode }) => (
  <div style={{ width: 960, height: 820, position: 'relative', overflow: 'hidden' }}>{children}</div>
);

export const FreshInstall = () => (
  <Screen>
    <MainMenuScreen background="castleFeast" />
  </Screen>
);

export const WithSaves = () => (
  <Screen>
    <MainMenuScreen background="forestFight" hasSaves />
  </Screen>
);

export const ShowingCredits = () => (
  <Screen>
    <MainMenuScreen background="cave">
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
    </MainMenuScreen>
  </Screen>
);
