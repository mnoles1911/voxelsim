# design-sync notes — @voxelmark/design-system

Read this before a re-sync. It holds the things that cost time on the first
run and are not visible from the config.

## What this package is

A React port of the game's Slate front end (`ue-project/Source/VoxelEarthUI`),
built specifically so Claude Design can compose VoxelMark screens. It is **not**
a pre-existing library that was imported — it was authored during the first sync
(2026-08-26) from the C++ as the specification.

That has one consequence worth internalising: **the C++ is upstream.** If
`VoxelUITheme.h` changes, this package changes with it and the change is
mechanical (`npm run tokens`). If a component's behaviour changes in
`SVoxel*.cpp`, nothing here notices — that is a manual port.

## Build

```sh
npm install
npm run backgrounds   # needs Python + Pillow; only when the art changes
npm run build         # tokens -> esbuild -> tsc -> css
```

- `npm run build` does **not** run `backgrounds` — that step needs Python and is
  slow. Its output `src/backgrounds.ts` **is committed**, so a fresh clone builds
  with Node alone; run `backgrounds` only when the art itself changes.
- Nothing under `assets/` is source. It is regenerated from
  `ue-project/Content/UI/`. The package must sit beside `ue-project/` in the
  voxelsim checkout; both generators fail loudly if it does not.

## Generated files — never hand-edit

| File | Generator | Source of truth |
|---|---|---|
| `src/styles/tokens.css`, `src/tokens.ts` | `tools/gen-tokens.mjs` | `ue-project/Source/VoxelEarthUI/VoxelUITheme.h` |
| `src/styles/utilities.css` | `tools/gen-utilities.mjs` | `src/styles/tokens.css` |
| `src/backgrounds.ts` | `tools/gen-backgrounds.mjs` | `assets/backgrounds/web/` |
| `assets/backgrounds/web/` | `tools/prepare-backgrounds.py` | `ue-project/Content/UI/Backgrounds` |

`gen-utilities.mjs` exists so that every class named in `conventions.md`
provably resolves. If you add a palette colour, re-run `npm run tokens` and the
`vm-bg-*` / `vm-text-*` / `vm-border-*` classes appear with it.

## Environment gotchas on this box

- **No Playwright browsers are installed and none are needed.** Point the render
  check at the system Chrome:
  `DS_CHROMIUM_PATH="/c/Program Files/Google/Chrome/Application/chrome.exe"`.
  `playwright` is installed in `.ds-sync/` with
  `PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1`; Chrome 151 drives it fine. This saves a
  ~200 MB download on every fresh clone.
- **The Bash tool mangles heredocs here.** Multi-line Python/CSS written via
  `cat <<'EOF'` loses backslash escapes and trips on quotes (`\n` inside an
  f-string became a real newline and broke the file). Write source files with
  the Write tool, or via a script file — not inline heredocs.
- **`cd` inside a Bash call persists to the next call.** A `cd ds-bundle` that
  "fails" often means you were already there.

## Sync-specific findings

- **`cfg.provider` wraps every preview**, so a preview cell demonstrating
  "outside the provider" is impossible — both cells render wrapped and come out
  identical (this trips `variantsIdentical`). `VoxelMarkRoot.tsx` was rewritten
  to show what the root *carries* instead. Don't reintroduce the contrast cell.
- **The card viewport caps story height at 620px** and content width at ~900px;
  anything larger is **cropped, not scaled**. `MainMenuScreen` needs
  `overrides.MainMenuScreen.viewport = "960x900"` because the menu column is
  ~704px tall — without it the title is clipped and QUIT vanishes entirely.
  Loading-screen stages are sized 900 wide for the same reason (960 cropped the
  FPS readout off the right edge).
- **Changing a component's `viewport` requires a full `package-build.mjs`** —
  `preview-rebuild.mjs` refuses with `[CONFIG_STALE]`, and a full build **resets
  `_screenshots/review/`**, so re-run the full capture afterwards.
- **17 of 22 components carry `cardMode: "column"`.** This DS is inherently wide
  (520px button columns, 600px loading column, full screens); column cards are
  correct here, not a workaround. Expect `[GRID_OVERFLOW]` on any new component
  wider than a grid cell and apply the same override.
- **`cfg.tokensGlob` is inert without `cfg.tokensPkg`** (it resolves inside
  `node_modules/<tokensPkg>`), so `tokens/` in the bundle is empty. That is
  fine: the token block is inlined at the top of `_ds_bundle.css`, which
  `styles.css` imports, so designs do receive it. `conventions.md` points the
  agent at those files rather than at `tokens/`.
- **Named OS fallback fonts trip `[FONT_MISSING]`.** `'Papyrus', 'Luminari'` in
  the font stack read to validate as families the DS expects and cannot ship.
  The stack is now `'Macondo Swash Caps', serif`. Don't add named fallbacks back.
- **Known render warns: none.** Validate exits 0 clean. Any warn on a re-sync is
  new — look at it.

## Fidelity decisions (don't silently revert these)

- **Colours are the authored sRGB hexes** from `Colors.gd` /
  `menus_shared.css`, which is what the FColor literals hold. Whether Slate
  re-encodes them identically on screen is unresolved upstream
  (`voxel.UI.SRGBTint`, default 1) and `VoxelUITheme.h` says outright that
  nothing in the port should be called pixel-exact. The design system is
  deliberately not modelling that question.
- **`FLinearColor` literals pack straight to bytes**, so the loading bar's "dark
  leather" track is `#0a0604` — *not* the ~`#382b22` an sRGB-encode assumption
  gives. Getting this wrong is invisible until someone samples a pixel.
- **The panel drop shadow is a hard rectangle**, inflated 8px and offset 4px
  down, because `FSlateBrush` has no blur and that is what the game draws.
- **The loading wash was measured, not eyeballed**: 0.377–0.385 multiplier
  against an unwashed render of the same crop, matching 62% black. An eyeball
  read called it "too light" and was wrong.
- **Button labels are flex-centred.** `display: block` rides them ~4px high
  against the engine's `VAlign_Center`.
- **Keyboard focus styling is new**, not ported — the Godot build has no
  keyboard/gamepad navigation at all; the Slate port added it.

## Re-sync risks — what can silently go stale

1. **The C++ is upstream and nothing enforces it.** `VoxelUITheme.h` changes are
   picked up only when someone runs `npm run tokens`; `SVoxel*.cpp` behaviour
   changes are never picked up. Before a re-sync, diff the header against
   `src/styles/tokens.css` and skim `git log ue-project/Source/VoxelEarthUI`.
2. **Menu copy is duplicated, not generated.** `src/strings.ts` was transcribed
   from `VoxelUIStrings.cpp` (24 quips, 13 tips, the credits body). Nothing
   re-checks it. The credits block in particular is a shipping obligation —
   third-party notices — and the preview version is deliberately **shortened to
   fit the panel**, so do not treat it as the authoritative text.
3. **`assets/` is gitignored and regenerated; `src/backgrounds.ts` is not.**
   The rule is: generated *source* is committed (`tokens.ts`, `tokens.css`,
   `utilities.css`, `backgrounds.ts`), generated *intermediates* are not (the
   re-encoded JPEGs). So a clone builds with only Node — Python/Pillow is needed
   only to re-encode the art. The cost is that `src/backgrounds.ts` is 845 KB of
   base64 in git and its diff is unreadable; regenerate it, never hand-edit it.
4. **Preview stage sizes are tuned to the card's crop behaviour** (900 wide,
   620 tall unless overridden). If the product changes those dimensions, several
   previews will crop and it will look like a component regression.
5. **The grain animation is presentational, not the ported physics.** The engine
   runs a fixed-timestep simulation (gravity 0.012/tick, 24 grains, spawn
   jitter); the web version lays grains along a `t²` path with CSS keyframes.
   Visually equivalent at any instant, but it is not the same code and will not
   track changes to `SVoxelHourglass.cpp`.
6. **Only the two screens that exist were ported.** SETTINGS is a placeholder in
   the game too (`docs/front-end-plan.md` R8). Pause menu, HUD and inventory are
   not here.
7. **Verification state lives in the uploaded `_ds_sync.json`**, not in git. A
   re-sync must fetch it to `.design-sync/.cache/remote-sync.json` or everything
   re-verifies from scratch.
