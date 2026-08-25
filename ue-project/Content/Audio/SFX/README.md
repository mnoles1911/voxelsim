# Game sound effects

**Empty as of 2026-08-24.** The sibling of `../Music/`, created ahead of the
audio so "where does a sound effect go" is answered once.

The inventory that fills this directory is `docs/sfx-library.md` (~1,930 entries
across 19 categories, ~300-400 of them for a playable slice) and the generation
prompts are `docs/sfx-prompts.md`. Both are ported from the Mira-Thal /
Voxelmark Godot checkout; see their headers for what is already rendered there.

## Folder plan

From `docs/sfx-library.md` §2, unchanged so the two docs keep matching:

    locomotion/   step_*, jump_*, climb_*, swim_*, armor_*
    combat/       cmb_*   (player + enemy + impacts)
    voxel/        vox_*   (dig/mine/chop/place/explosive/collapse)
    crafting/     craft_*
    items/        item_*
    environment/  wx_* (weather), water_*, fire_*
    ambience/     amb_<region>_<day|night>_loop   [stereo]
    creatures/    wld_*   (ambient fauna, non-combat)
    systems/      lock_*, minigame_*, invest_*
    ui/           ui_*, journal_*, map_*, save_*, skill_*
    npc/          npc_*   (non-verbal efforts, crowd)
    economy/      econ_*

Folders appear as sounds land -- an empty tree of twelve directories is noise.

## Conventions (from the library doc, so a file dropped here is already correct)

- **Mono, 44.1 kHz.** Ambient beds and long loops are the stereo exception.
- **`snake_case`**, `<group>_<thing>[_<qualifier>][_NN]`, where `_NN` is the
  variation index: `step_walk_stone_03`, `vox_mine_stone_pick_02`.
- **Variations exist to defeat the machine-gun repeat.** Footsteps and
  high-frequency impacts get 5, generic one-shots 3, unique one-shots 1,
  loops 1 (seamless).
- **Compressed source, never WAV**, for the reason in `../Music/MUSIC_CREDITS.md`:
  this repo has no LFS and has already been blocked by large binaries once.

## ONE THING IS NOT DECIDED, and it is not mine to decide silently

How the engine gets at these files is **open**. The menu art precedent does NOT
transfer: Slate decodes `Content/UI`'s JPEGs itself at runtime, which is why
those are committed loose and their auto-imported `.uasset` siblings are
gitignored. Gameplay audio has no equivalent path -- `UGameplayStatics::PlaySound`
wants a `USoundBase`, which means an imported `.uasset`. Three routes, and they
imply opposite gitignore rules:

1. **Commit the imported `.uasset`** and treat the loose file as source. Costs
   double storage (UE embeds the compressed audio in the asset).
2. **Commit loose source only** and import on each machine. Cheapest in git,
   but the import settings (loop flag, compression quality) then live nowhere.
3. **Runtime decode**, the way the front end handles JPEG. Most work, and only
   worth it if the front end ends up owning music playback anyway.

No `.uasset` ignore rule has been added for this directory precisely because
adding one would quietly pick route 2. Decide it when the first sound lands.
