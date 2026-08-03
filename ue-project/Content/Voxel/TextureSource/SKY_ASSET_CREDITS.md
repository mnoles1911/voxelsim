# Sky asset credits and provenance

Fetched by `tools/fetch-sky-assets.ps1` into `tools/sky-assets/` (gitignored --
385 KB is the largest binary this repo has ever checked in; a 34+ MB EXR does
not belong in git history). This file is the reproducibility record: source
page, exact file, and sha256 needed to fetch byte-identical assets again, the
same role `terrain-service/data/earth_reference/manifest.json` plays for the
Copernicus/3DEP cache.

## Required attribution

> Star map: NASA/Goddard Space Flight Center Scientific Visualization Studio.
> Gaia DR2: ESA/Gaia/DPAC.
>
> Moon: NASA's Scientific Visualization Studio.

This attribution must ship with any build that renders these textures (about
page / credits screen / README -- wherever the project's other third-party
notices live).

## Assets

| Asset | Source page | File fetched | Size (bytes) | sha256 |
|---|---|---|---|---|
| Star map (4k, equirectangular, celestial coords) | https://svs.gsfc.nasa.gov/4851 | `starmap_2020_4k.exr` (4096x2048) | 35997085 | `69b841cd048c2ef35543eb1c5819b322530e78aa4b0cab97ec1de3ef56a4ddcd` |
| Star map (8k option, `-EightK`) | https://svs.gsfc.nasa.gov/4851 | `starmap_2020_8k.exr` (8192x4096) | 130530278 | not fetched -- default is the 4k map; run `fetch-sky-assets.ps1 -EightK` and record the hash here if this is pinned |
| Moon colour | https://svs.gsfc.nasa.gov/4720 | `lroc_color_poles_4k.tif` (4096x2048) | 13095388 | `918649a7f8ed2f1329b2cd95bb0d25483befdcb60ae1a66db681a637cc21344f` |
| Moon displacement | https://svs.gsfc.nasa.gov/4720 | `ldem_4_uint.tif` (1440x720) | 2076866 | `e6668bec27fc9b8fbb02d198c7ddfb08eedeeb790167b494f95e6b34201da05e` |

All four URLs were verified before pinning by HEAD request (`curl -I`, no
bytes downloaded to test) on 2026-07-29, confirming HTTP 200 and the
Content-Length shown above. The three fetched rows were then actually
downloaded and hashed with `Get-FileHash -Algorithm SHA256`
(cross-checked with `sha256sum`) to populate this table -- the sizes above
are what landed on disk, not just the HEAD-reported Content-Length.

## Status

**These textures now render, and the import step is REQUIRED.** Superseded on
2026-08-02; this section previously said there was no sky material and that the
assets had "nowhere to render". That stopped being true in `befb438`/`2284c5e`,
which shipped `M_NightSky` and `M_SkyAtmosphereDome` (built by
`ue-project/Tools/create_sky_material.py` and
`create_sky_atmosphere_dome_material.py`). Both carry hard package references to
`/Game/Voxel/T_SkyStarmap`, and `M_NightSky` also to `/Game/Voxel/T_MoonColor`.

Getting the sky on screen is therefore TWO steps, and the second one is easy to
skip because nothing enforces it:

```powershell
.\tools\fetch-sky-assets.ps1                      # 1. download into tools/sky-assets/
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
    .\ue-project\VoxelEarth.uproject -run=pythonscript `
    -script=.\ue-project\Tools\import_sky_textures.py -unattended -nop4 -nosplash
```

The `.uasset` outputs are gitignored build artifacts (`.gitignore:82-84`) --
`T_SkyStarmap` alone is 38.8 MB against a repo whose largest tracked binary is
385 KB -- so **a fresh clone has the materials but not the textures**, and the
import has to be re-run per checkout.

**What skipping step 2 looks like**, observed on 2026-08-02 in a checkout where
only step 1 had been run. The sky subsystem initialises normally and
`VoxelSky clock RESOLVED` still appears, so the log looks healthy; the tell is:

```
LogMaterial: Warning: M_NightSky: Requesting an invalid TextureIndex! (1 / 1)
LoadErrors: While trying to load package /Game/Voxel/M_NightSky, a dependent
            package /Game/Voxel/T_SkyStarmap was not available.
LoadErrors: While trying to load package /Game/Voxel/M_SkyAtmosphereDome, a
            dependent package /Game/Voxel/T_SkyStarmap was not available.
```

Note this is NOT caught by the material guard in `VoxelSkyDomeActor.cpp:135-141`:
that only tests whether the material object loads, and a material with an
unresolved texture reference still loads. `bAtmosphereMaterialValid` stays true,
so the IsSky dome is still shown -- i.e. a missing texture degrades silently,
unlike a missing material, which is refused loudly. Grep the log for
`invalid TextureIndex` or `T_SkyStarmap ... was not available` to tell the two
apart.
