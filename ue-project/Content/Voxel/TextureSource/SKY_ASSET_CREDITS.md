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

Textures are fetched and importable (see `ue-project/Tools/import_sky_textures.py`),
but **there is no sky material yet** -- none of this repo's
`create_*_material.py` scripts builds one. These assets currently have
nowhere to render; that is expected, not a bug.
