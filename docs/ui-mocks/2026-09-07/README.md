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

## What is ported, and where

| Mock | Slate |
|------|-------|
| Main Menu | `SVoxelMainMenu` |
| Loading Screen | `SVoxelLoadingScreen`, `SVoxelHourglass` |
| Pause Menu / Settings / Save / Load | `SVoxelPauseMenu` + `SVoxelSettingsPanel` / `SVoxelSaveDialog` / `SVoxelLoadDialog` |
| Inventory / Map / Journal / Player / Codex | one `SVoxelScreenShell` hosting `SVoxelInventoryScreen`, `SVoxelMapScreen`, `SVoxelJournalScreen`, `SVoxelPlayerScreen`, `SVoxelCodexScreen` |
| Death Screen | `SVoxelDeathScreen` |
| Dialogue | `SVoxelDialogueOverlay` |
| HUD v2 | `SVoxelGameHud` |

All eight wave-2 screens are owned by `UVoxelScreensUISubsystem`.

## Two things the mocks reference that do not exist

* **`design/CONVERSATION_SYSTEM.md`.** `menus_shared.css` names it as the
  dialogue layer's spec and "Voxelmark Dialogue.html" cites it plus
  `SKILLS_AND_PROGRESSION.md` and a `SpeechCheckBroker` for every number in the
  node. None of them is in this repository and `git log` finds no deleted one,
  so the mock's own markup and comments are the whole spec the port had.
* **`--blood` and `--blood-bright`.** "Voxelmark Death Screen.html" paints its
  heading and rule with both; neither is declared in the `:root` block or in
  the mock's own `<style>`, so in a browser they resolve to nothing. The port
  chooses values and records the choice at `VoxelUITheme::DeathBlood`.

## Systems the mocks assume that this game does not have

Recorded here because it is the single biggest gap between these pictures and
the port, and because each one is a decision a reader will otherwise re-derive:
**rarity, durability, equipment, crafting recipes, quests/journal, codex/lore,
skills, perks, factions, XP/levels, health, hunger, death/respawn, and
conversations.** A case-sensitive grep for each across both modules on
2026-09-07 returned nothing. `VoxelScreenData.h` is the interface every screen
draws through, and `VoxelScreenData.cpp`'s `Seed*()` functions hold the mock's
own placeholder content until those systems arrive.

The one exception is the **inventory**, which is real:
`UVoxelInventoryComponent` and `FVoxelItemRegistry` back the pack, the hotbar
and the HUD dock live.

**Scaling doctrine: [ADR-0011](../../adr/0011-scale-tolerant-ui.md).** These mocks are
authored at 1920x1080 and each `.html` carries its own `zoom:1.33333` to preview as
1440p. The Slate port stores the **1080p** figures and lets the engine's continuous
`ShortestSide` curve supply the rest. Do NOT re-author these at 2560x1440: it moves
the design above the legibility floor and makes every 1080p player receive a
downscale. Any absolute pixel figure measured from a capture taken before
2026-09-07 afternoon was measured at 1.333 and must be divided before comparison.
