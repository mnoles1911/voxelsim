# Visual effects

**Empty as of 2026-08-24.** The home for Niagara systems, their materials, and
the sprite sheets they sample.

## There is no VFX in this project yet, and that is a fact worth stating

`VoxelExplosive.cpp` says "no particles yet" in two places. Nothing in
`Source/` references Niagara or Cascade. The PBF water particles are a GPU
compute system of our own that renders through `M_WaterVoxel`, not a Niagara
emitter -- so "we have particles" is false in the sense that matters here.

The Godot build's VFX were `GPUParticles3D` nodes built in code
(`scenes/vfx/BloodBurst.tscn`, `BloodDrip.tscn`, `DustBurst.tscn`), and its
asset-pipeline doc says so explicitly: *"Particle systems / VFX -- these are
runtime, not asset pipeline. Don't try to generate blood particles."* So there
is no VFX asset list to port for gameplay effects; there is a list of
**sprite and atmosphere sources** they will sample, which is
`docs/vfx-and-atmosphere-assets.md`.

## What lives here vs. what gets fetched

**Committed here:** the Niagara systems and materials we author, as `.uasset`,
the same way `Content/Voxel/M_*.uasset` are committed. These are small and are
the deliverable.

**NOT committed:** the third-party CC0 sprite packs and HDRIs. Follow the
pattern `Content/Voxel/TextureSource/SKY_ASSET_CREDITS.md` already sets --
a fetch script writes into a gitignored directory and a credits file records
source page, exact filename and sha256 so the fetch is reproducible and the
licence is auditable. The largest binary this repo has ever tracked is 385 KB;
a Kenney pack or a 4K EXR does not belong in git history.

**Already done, do not re-fetch:** the sky half of that list. `T_SkyStarmap`,
`T_MoonColor` and `T_MoonDisplacement` are fetched by
`tools/fetch-sky-assets.ps1` and recorded in `SKY_ASSET_CREDITS.md`. The
atmosphere doc was written for the Godot-era UE5 port plan and does not know
that.

## Naming

Follow `Content/Voxel`'s existing prefixes, because they are already the
project's convention and a second scheme helps nobody: `NS_` Niagara system,
`NE_` emitter, `M_` material, `MI_` material instance, `T_` texture.
