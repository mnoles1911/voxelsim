# VFX and atmosphere asset credits and provenance

Fetched by `tools/fetch-vfx-assets.ps1` into `tools/vfx-assets/` (gitignored --
329 MB against a repo whose largest tracked binary is 385 KB). This file is the
reproducibility and licence record: source page, exact file, pinned byte size,
and sha256 once downloaded. Same role `SKY_ASSET_CREDITS.md` plays for the sky
textures.

**Nothing has been downloaded yet.** URLs and sizes below were verified
2026-08-24 by HEAD request from this machine; every one returned HTTP 200. The
sha256 column fills in when somebody runs the fetch.

## Attribution: none required

All eight are **CC0 / public domain**. None of these sources asks for
attribution, and that is why this set was chosen -- the alternative catalogue
(OpenGameArt) is a per-asset mix of CC0, CC-BY and GPL that has to be checked
upload by upload.

- **Poly Haven** -- entire library CC0, no signup, no attribution.
- **ambientCG** -- entire library CC0 1.0 Universal, explicitly "even in
  commercial circumstances".
- **Kenney** -- both packs tagged CC0 on their asset pages.

"Was CC0 in August 2026" is a fact with a date on it. Re-check the licence badge
before shipping if this is revisited years later.

**This is NOT the case for the sky textures.** Those carry a NASA/ESA
attribution obligation recorded in
`../Voxel/TextureSource/SKY_ASSET_CREDITS.md`, and that obligation ships with
any build that renders them. Do not conflate the two sets.

## Rights: unchanged by the 2026-08-25 clearance, and one obligation still ships

The owner cleared the project's OWN generated assets (menu art, music, SFX) for
commercial release on 2026-08-25. That statement does not apply here and does
not need to: these eight are third-party CC0, which already permits commercial
use with no attribution.

THE SKY TEXTURES ARE THE EXCEPTION AND THE CLEARANCE DOES NOT TOUCH THEM. The
NASA/ESA credit recorded in `../Voxel/TextureSource/SKY_ASSET_CREDITS.md` is an
attribution REQUIREMENT of that source, not a permission question -- being
cleared to use something is not the same as being free of the condition
attached to it. That credit line still has to appear in any build that renders
those textures, and as of today it appears in no shipping surface: there is no
credits screen wired to it (the menu's CREDITS panel is a placeholder). Whoever
builds that screen owns this.

## Assets

| File | Source page | Feeds | Bytes | sha256 |
|---|---|---|---|---|
| `kloofendal_48d_partly_cloudy_puresky_4k.exr` | [polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky) | SkyLight + reflection capture, clear day | 75,640,460 | not fetched |
| `kloppenheim_06_4k.hdr` | [polyhaven.com/a/kloppenheim_06](https://polyhaven.com/a/kloppenheim_06) | SkyLight + reflection, overcast | 23,526,002 | not fetched |
| `belfast_sunset_puresky_4k.exr` | [polyhaven.com/a/belfast_sunset_puresky](https://polyhaven.com/a/belfast_sunset_puresky) | SkyLight + reflection, dusk | 73,122,844 | not fetched |
| `rocky_terrain_02_nor_gl_4k.jpg` | [polyhaven.com/a/rocky_terrain_02](https://polyhaven.com/a/rocky_terrain_02) | Wet-rock detail normal; coarse water ripple layer | 16,219,215 | not fetched |
| `kenney_particle-pack.zip` | [kenney.nl/assets/particle-pack](https://www.kenney.nl/assets/particle-pack) | Niagara sprites: splash, spark, glow, embers (80) | 15,001,764 | not fetched |
| `kenney_smoke-particles.zip` | [kenney.nl/assets/smoke-particles](https://www.kenney.nl/assets/smoke-particles) | Niagara sprites: smoke, fog wisps, dust, haze (70) | 6,019,666 | not fetched |
| `Rock023_2K-PNG.zip` | [ambientcg.com/view?id=Rock023](https://ambientcg.com/view?id=Rock023) | Cliff/stone detail normal, wet sheen | 63,106,399 | not fetched |
| `Ground037_2K-PNG.zip` | [ambientcg.com/view?id=Ground037](https://ambientcg.com/view?id=Ground037) | Ground detail normal + roughness under rain | 72,439,572 | not fetched |

## Three things the source document gets wrong for this repo

`docs/vfx-and-atmosphere-assets.md` was written 2026-06-16 as research for a
UE5 port that did not exist yet. Its asset list holds up; three of its
recommendations do not.

1. **"Minimal, high-value" is a count, not a download.** It recommends six
   files without giving figures. Measured, those six are 200 MB and all eight
   are 329 MB.
2. **The water-material advice is superseded.** It recommends Single Layer
   Water; voxelsim ported SLW and rebuilt water rendering in August 2026. The
   normal-map suggestions may still be useful, the architecture section is
   history.
3. **The HDRI recommendation runs into the sky chain.** voxelsim renders sky
   through `M_SkyAtmosphereDome` / `M_NightSky` with `MPC_VoxelSky` driving
   them, and regenerating that MPC breaks dependents. Treat the three skies as
   candidate SkyLight sources to evaluate against the existing chain, not as a
   drop-in.

## What to grab first

The **Kenney packs** (20 MB for both) are the genuinely new material and the
only entries with no equivalent already in the repo. `-SkipGroundTextures`
fetches the six non-ambientCG files for 200 MB; the sprite packs alone are
what a first Niagara emitter needs.

## UE5 import notes

`nor_gl` is the **OpenGL-convention** normal and the right one for UE5 -- do
not substitute the `nor_dx` variant on the same asset page. Import normals with
sRGB **off** and compression set to Normalmap; import HDRIs as HDR/long-lat
cubemaps for the SkyLight source.
