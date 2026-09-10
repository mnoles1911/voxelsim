# 1080p shell captures, 2026-09-08 (ADR-0011 decision 4)

Headless `tools/voxel-ui-capture.ps1 -Shot Screen -Panel <journal|inventory> -ResX 1920 -ResY 1080`
(the -ResX/-ResY pair was inert before this day: borderless mode ignored it and every
off-size capture was 1440p; the tool now measures and prints the PNG size).

| file | conditions | reading |
|---|---|---|
| journal-1440p-before.png | 2560x1440, Menu Size 1.00, before the trim | the reference the owner accepted |
| journal-1080p-before.png | 1920x1080, Menu Size 1.00, before the trim | same layout, 53.3% of width at both sizes: the shell sees 1080 units tall at every landscape resolution |
| journal-1080p-after-menusize100.png | 1920x1080, Menu Size 1.00, after | pixel-diff vs before: 6 px of 2 M (0.0003%), bottom row only; nothing trims at the default |
| journal-1080p-after-menusize150.png | 1920x1080, Menu Size forced to 1.50 via `[VoxelGraphics] ScreenShellScale` | the trim engaged: chrome shrinks 26 units first, then the frame clamps to 704 units and the body scrolls; owner judges the proportions |
| inventory-1080p-after-menusize150.png | same, inventory | same |

Mechanism: `FVoxelMenuLayout::ShellChromeForViewport(ViewportUnits, MenuScale)` in
`VoxelUITheme.h`; tests `VoxelEarth.FrontEnd.ScreenShell.Chrome`.
