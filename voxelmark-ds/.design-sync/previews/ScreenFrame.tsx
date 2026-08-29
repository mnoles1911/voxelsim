import { ScreenFrame, Panel, Text } from '@voxelmark/design-system';

const Stage = ({ children }: { children: React.ReactNode }) => (
  <div style={{ width: 640, height: 360, position: 'relative', overflow: 'hidden' }}>{children}</div>
);

const Centre = ({ children }: { children: React.ReactNode }) => (
  <div style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
    {children}
  </div>
);

export const MenuWash = () => (
  <Stage>
    <ScreenFrame background="castleFeast" wash="menu">
      <Centre>
        <Panel style={{ width: 320 }}>
          <Text tone="title" size="panel-title" as="div">55% night-black</Text>
        </Panel>
      </Centre>
    </ScreenFrame>
  </Stage>
);

export const LoadingWash = () => (
  <Stage>
    <ScreenFrame background="castleFeast" wash="loading">
      <Centre>
        <Panel style={{ width: 320 }}>
          <Text tone="title" size="panel-title" as="div">62% pure black</Text>
        </Panel>
      </Centre>
    </ScreenFrame>
  </Stage>
);

// No background means no wash, deliberately -- washing the flat backdrop would
// put near-black on near-black.
export const NoArtNoWash = () => (
  <Stage>
    <ScreenFrame>
      <Centre>
        <Panel style={{ width: 320 }}>
          <Text tone="title" size="panel-title" as="div">Flat backdrop</Text>
        </Panel>
      </Centre>
    </ScreenFrame>
  </Stage>
);
