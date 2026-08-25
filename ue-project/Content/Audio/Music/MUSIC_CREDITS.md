# Music: provenance

**This directory is EMPTY of audio as of 2026-08-24.** It is the permanent home
for the game's music, created ahead of the tracks so there is one answer to
"where does a soundtrack go" instead of a decision made again per track.

The first consumer is the main menu. `docs/adr/0009` records menu music as
deliberately absent: the three source WAVs from the Mira-Thal / *Voxelmark*
Godot build are ~124 MB and one of them exceeds GitHub's 100 MB per-file wall,
so they never crossed. The handoff contract is already decided -- adopt at
BeginLoad, fade to -40 dB over `FVoxelUITheme::MusicFadeOut` (1.5 s) at
hand-off -- and `VoxelUITheme.h` already carries that number.

## Why here and not under Content/UI/

`Content/UI/` is scoped to the front end's art and typeface, and
`FVoxelUIAssetLibrary` hardcodes `UI/Backgrounds` as a subdir path. The menu is
the first consumer of music but will not be the only one; in-world ambient or
exploration tracks under a `UI/` folder would be wrong within a year. This
directory sits under `FPaths::ProjectContentDir()` either way, which is what the
Slate front end resolves against.

## Rules for anything committed here

1. **COMPRESSED SOURCE, NOT WAV.** This repo has no LFS. Two baked terrain tiles
   at 192 MB each blocked 112 commits once already (see the 100 MB wall in
   `.gitignore`), and the menu art was re-encoded from 30.6 MB to 2 MB for the
   same reason. Commit OGG (or MP3); keep the WAV masters outside the repo and
   name them below.
2. **RECORD THE SOURCE sha256, not just the output's.** The table below is
   identified by the sha256 of the ORIGINAL file so a re-encode is reproducible
   and a swapped source shows up in the diff rather than only in the ears.
3. **The `.uasset` siblings are ignored, deliberately.** An interactive editor
   session auto-imports loose media it finds under `Content/`, so `.uasset`
   files will appear next to these. Nothing references them. See the matching
   rule in `.gitignore` alongside `Content/UI/Backgrounds/*.uasset`.

## The prompts that make these files

`docs/music-prompts.md` -- the Suno portfolio (59 cues, style prompt + structure
prompt each, with explicit key, BPM and lead instrument), copied from the Godot
checkout so the recipe sits with the output. Its header records what it does NOT
cover: the three tracks that exist in that repo today predate it and their
prompts were never written down anywhere.

## Tracks

_None yet._

| File | Source | Output | sha256 (source) |
|---|---|---|---|
| -- | -- | -- | -- |
