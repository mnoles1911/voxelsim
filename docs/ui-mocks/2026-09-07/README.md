# Voxelmark UI mocks — 2026-09-07 revision

Verbatim copies of the Claude Design project
`https://claude.ai/design/p/019dfd74-b8dc-70b1-8c2b-9312c0bc9f63`, pulled on
2026-09-07 via the design MCP, staged here so the Slate port is reproducible
from the repository alone and so sub-agents can read the source without design
authorization.

The existing front end (`ue-project/Source/VoxelEarthUI`) is already a port of
an EARLIER revision of these mocks (via the Godot build); `VoxelUITheme.h`'s
palette is the CSS `:root` block. This revision moved the title screen to a
right-aligned cartouche list with a logo top-right and a patch callout
top-left, shrank the loading hourglass, and added the overlay family (pause,
settings, save, load) plus eight in-game screens.

Fonts referenced by `menus_shared.css` are OFL faces from Google Fonts,
shipped under `ue-project/Content/UI/Fonts/`:

| CSS var    | Family               | File                          |
|------------|----------------------|-------------------------------|
| `--serif`  | Macondo Swash Caps   | `MacondoSwashCaps-Regular.ttf`|
| `--mono`   | VT323                | `VT323-Regular.ttf`           |
| `--hand`   | IM Fell English      | `IMFeENrm28P.ttf` (roman), `IMFeENit28P.ttf` (italic) |
| `--pixel`  | Press Start 2P       | not shipped (unused by the ported screens) |

Background art referenced as `assets/menu_backgrounds/*.jpg` already lives in
`ue-project/Content/UI/Backgrounds/` (`cave.jpg`, `battle.jpg`,
`castle_feast.jpg`, `forest_fight.jpg`).

Every mock is authored at 1920x1080 and shown at `zoom:1.33333` for 1440p;
all pixel numbers in the port are the 1080p figures.
