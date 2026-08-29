import { LoadingScreen } from '@voxelmark/design-system';

const Screen = ({ children }: { children: React.ReactNode }) => (
  <div style={{ width: 900, height: 506, position: 'relative', overflow: 'hidden' }}>{children}</div>
);

export const MidLoad = () => (
  <Screen>
    <LoadingScreen
      progress={0.42}
      background="cave"
      quip="Stoking the volcano under Drûn-Khazad..."
      tip="Water flows. If you carve under a pond, expect a small flood."
      animated={false}
    />
  </Screen>
);

export const NearlyDone = () => (
  <Screen>
    <LoadingScreen
      progress={0.93}
      background="sailing"
      quip="Counting the king's gold (twice)..."
      tip="The compass points north. The sun rises east. The map is hand-drawn."
      animated={false}
    />
  </Screen>
);

export const WithFrameCounter = () => (
  <Screen>
    <LoadingScreen
      progress={0.18}
      background="battle"
      quip="Organizing goblin bands..."
      showFps
      fps={11}
      worstMs={104}
      animated={false}
    />
  </Screen>
);

export const FlatBackdrop = () => (
  <Screen>
    <LoadingScreen progress={0.5} quip="Apologizing to the goats..." animated={false} />
  </Screen>
);
