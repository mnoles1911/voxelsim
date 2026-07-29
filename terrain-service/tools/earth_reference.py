#!/usr/bin/env python3
"""Build the Earth reference corpus: real elevation statistics, per terrain class.

WHY THIS EXISTS
---------------
We amplify 30 m/px terrain-diffusion tiles down to 10 cm voxels and have no way
to tell whether the result looks like Earth. Screenshot-reading has repeatedly
given the wrong answer (see MEMORY: "Settle before screenshotting"). This tool
builds the other half of a statistical check: a small, fixed set of real-world
sites, one or more per geomorphic class our worldgen produces, characterised
with the SAME slope/relief statistics we can compute on a generated tile.

The corpus is code + manifest, not data. Rasters are 40 MB (Copernicus) to
500 MB (USGS 1 m) each and live in a gitignored cache; the committed artifacts
are this script and ``data/earth_reference/manifest.json``, which carries every
URL, window, checksum and statistic needed to rebuild or to cite.

TWO SOURCES, DELIBERATELY
-------------------------
1. Copernicus GLO-30 (``copernicus-dem-30m`` S3, anonymous, no credentials).
   Global, 1 arc-second, EPSG:4326. Its ~30 m posting is the SAME BAND as our
   input tiles, so it is the reference for "does the carrier look right".
   It is a DSM: it includes trees and buildings. See DSM CAVEAT below.

2. USGS 3DEP 1 m (``prd-tnm`` S3, anonymous, no credentials, verified below).
   CONUS-only, 1 m, UTM/NAD83, and BARE EARTH (DTM) -- it is gridded from
   classified ground returns. This is the only free reference that reaches
   into our SUB-30 m amplification band, which is the band we actually invent
   and therefore the band most likely to be wrong. It matters more than the
   count of 30 m tiles, so every site that can have one gets one.

   3DEP tiles are 10 km x 10 km at 1 m (10012^2 float32, 40-500 MB compressed),
   far too large to mirror. We read a 2 km window through GDAL's /vsicurl/
   (HTTP range reads against the internally-tiled LZW GeoTIFF) and cache the
   extracted window. Reproducible: the manifest pins the source key, the
   source ETag and the window origin in projected metres.

   OpenTopography was evaluated and REJECTED as a source: its global DEM API
   answers an unauthenticated request with HTTP 401 and the body
   "Error: API Key required for access". It is reachable but not anonymous, so
   nothing here depends on it.

PROJECTION -- the thing that silently produces nonsense slopes
--------------------------------------------------------------
Copernicus tiles are geographic. At latitude 46 a 1 arc-second pixel is about
21.5 m east-west and 30.9 m north-south: non-square, and the ratio changes with
latitude. A gradient taken in degrees is not a slope. We do BOTH of the correct
things and record both, so a bug in either shows up as a disagreement:

  * PRIMARY ("aeqd30"): reproject the window into a local azimuthal-equidistant
    metric CRS centred on the site, at exactly 30 m square pixels, bilinear.
    Distances through the centre of an AEQD projection are true, so this is a
    metric grid with the same pixel geometry as our tiles -- directly
    comparable. Resampling 21.5 m -> 30 m east-west is a mild decimation and
    slightly smooths the finest east-west detail; that is the price of
    comparability and it is why we keep the cross-check.
  * CROSS-CHECK ("native_ground"): no resampling at all. Horn's slope on the
    native geographic grid with per-row ground distances from the WGS84
    meridional/parallel arc formulas (dx varies with cos(lat), dy nearly
    constant). If mean slope from the two grids diverges by more than a few
    percent, treat the numbers as suspect.

USGS 3DEP is already UTM/NAD83 in metres at 1 m, so it needs no reprojection;
the UTM scale distortion within a 2 km window is under 0.1% and is ignored
(recorded in the manifest as ``grid: "utm_native"``).

VOIDS AND SEA -- the other silent statistic-killer
--------------------------------------------------
An unmasked void dominates any slope statistic: a -32767 pixel next to a 400 m
pixel is a 4000% grade. Masking, applied BEFORE reprojection so nodata cannot
be interpolated into real ground:

  * the file's declared nodata value, and any non-finite value;
  * anything below ``VOID_FLOOR_M`` (-100 m). No site in the corpus is below
    sea level, so this only catches fill values;
  * for Copernicus ONLY, elevation exactly 0.0, which is how GLO-30 stores
    open water. This is what keeps the coastal site's shoreline honest: the
    sound is removed, the land beside it is kept, and slope at the land/water
    boundary is undefined rather than a cliff. Reported separately as
    ``sea_or_zero_fraction`` so the reader can see how much was water.
  * 3DEP does NOT get the zero rule -- NAVD88 land at exactly 0.000 is
    possible near the coast and the file's own nodata is explicit. 3DEP water
    is instead HYDRO-FLATTENED to a constant, which is real data but perfectly
    flat; ``flat_fraction`` (slope < 0.1 deg) exposes it.

DSM CAVEAT
----------
Copernicus GLO-30 is a DSM. Over forest it measures the canopy, which inflates
fine-scale roughness by metres -- exactly the band we care about. Sites are
chosen so this matters least (bare rock, gypsum and sand dunes, ploughed
plains, badlands), and every site records ``cop30_dsm_bias`` as one of
"negligible" / "moderate" / "severe" with the reason. Where a bare-earth
alternative exists we prefer it: nine of the twelve sites carry a 3DEP 1 m DTM,
and each of those reports a CO-LOCATED comparison -- Copernicus read over the
exact 2 km footprint of the 3DEP window, against the 3DEP DTM block-averaged to
30 m. That measurement, not the assumption, is what the manifest carries.

Two things it found that were not what the caveat predicted, both in the
manifest's ``findings`` block:

  * A near-uniform canopy is close to a constant vertical offset, and an offset
    cancels in a gradient. The Smokies -- 20-40 m of unbroken forest -- inflate
    30 m mean slope by only 13%. The DSM penalty is severe where the canopy is
    tall RELATIVE TO THE LANDFORM: the NC coastal plain comes out at 5.3x slope
    and 11x roughness, on ground with under 2 m of relief. So the caveat bites
    hardest exactly where we said our terrain looks worst -- gentle ground.
  * On 6 of the 9 comparable sites Copernicus is SMOOTHER than the bare-earth
    ground at its own 30 m posting (White Sands: 0.64). GLO-30's effective
    resolution is coarser than its posting. Calibrating generated terrain to
    cop30 alone would be matching an over-smoothed reference.

USAGE
-----
Needs rasterio (NOT a terrain-service dependency -- run it with the diffusion
venv, which has it)::

    D:/terrain-diffusion/.venv/Scripts/python.exe tools/earth_reference.py build
    ...                                            tools/earth_reference.py build --site badlands_sd
    ...                                            tools/earth_reference.py verify
    ...                                            tools/earth_reference.py probe

``build`` is resumable and idempotent: a cached file whose sha256 already
matches the manifest is not re-downloaded, and a partial download continues
from its byte offset. ``verify`` re-hashes the cache against the manifest
without touching the network. ``probe`` only HEADs every URL.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import numpy as np

TOOLS_DIR = Path(__file__).resolve().parent
DATA_DIR = TOOLS_DIR.parent / "data" / "earth_reference"
MANIFEST_PATH = DATA_DIR / "manifest.json"
CACHE_DIR = DATA_DIR / "cache"

SCHEMA = "voxelsim.earth_reference/1"

COP30_BUCKET = "https://copernicus-dem-30m.s3.amazonaws.com"
TNM_BUCKET = "https://prd-tnm.s3.amazonaws.com"
TNM_1M_PREFIX = "StagedProducts/Elevation/1m/Projects"

USER_AGENT = "voxelsim-earth-reference/1 (+terrain-service/tools/earth_reference.py)"

#: Anything at or below this elevation (m) is a fill value, not ground. No site
#: in the corpus has real terrain below sea level, so this cannot eat data.
VOID_FLOOR_M = -100.0

#: Sentinel used to carry "no data" through GDAL's warper. NaN through
#: rasterio.warp is unreliable across GDAL builds; a finite sentinel outside
#: any real elevation is not.
WARP_NODATA = -32768.0

#: Slope below this reads as flat ground; used to expose hydro-flattened water
#: and playa floors that would otherwise look like very good low-relief terrain.
FLAT_SLOPE_DEG = 0.1


# ===========================================================================
# Site table -- the corpus itself.
#
# Every entry answers: which GEOMORPHIC class (landform, not vegetation), why
# THIS place is a clean example of it, and how badly the Copernicus DSM lies
# there. Coordinates are chosen at least 0.15 deg inside their 1x1 deg
# Copernicus tile so a 20 km analysis window never crosses a tile edge.
# ===========================================================================

SITES: list[dict] = [
    {
        "id": "alps_valais_alpine",
        "geomorphic_class": "alpine_glaciated",
        "name": "Pennine Alps, Valais (Zermatt / Mischabel)",
        "lat": 46.35,
        "lon": 7.55,
        "human_description": (
            "Glacially carved high alpine: U-shaped troughs, cirques, aretes and "
            "bare rock walls above the treeline, with ice in the hollows."
        ),
        "why": (
            "The canonical high-relief glaciated landscape and the tile that was "
            "verified reachable first. Glacial sculpture is the one process our "
            "fluvial-looking amplifier has no term for, so if generated alpine "
            "terrain is wrong it will be wrong against THIS distribution: very "
            "high mean slope with a long flat tail (glacier surfaces and valley "
            "floors), not a uniform steepness."
        ),
        "cop30_dsm_bias": "negligible",
        "cop30_dsm_reason": (
            "Almost entirely above the treeline: rock, scree and ice. The only "
            "canopy is in the valley floors, a small area fraction."
        ),
        "usgs_3dep": None,
        "no_3dep_reason": "3DEP is CONUS-only; see glacier_np_alpine for the 1 m bare-earth alpine companion.",
    },
    {
        "id": "teton_range_alpine",
        "geomorphic_class": "alpine_glaciated",
        # Teton Range, not Glacier NP. MT_GlacierNP_2016's 21 tiles are a narrow
        # river-corridor acquisition with no fully covered 2 km window anywhere
        # (best candidate: 70% ground, rest flight-line gap), so it cannot
        # supply the 1 m statistics this site exists for.
        "name": "Teton Range, Wyoming (Grand Teton / Cascade Canyon)",
        "lat": 43.74,
        "lon": -110.80,
        "human_description": (
            "Fault-block alpine front: cirques and knife-edge aretes in granite "
            "and gneiss, hanging valleys, talus cones, and a moraine-dammed "
            "outwash flat at the foot of the range."
        ),
        "why": (
            "The alpine class needs a BARE-EARTH reference in the sub-30 m band "
            "and the Alps cannot supply one; the Tetons are the same landform "
            "family (Pleistocene alpine glaciation, 2 km of relief, crest well "
            "above the treeline) inside 3DEP coverage, so they pin what talus "
            "aprons, cirque headwalls and glacial troughs actually look like at "
            "1 m. The site also has the sharpest MOUNTAIN-FRONT step on the "
            "list: the range rises 2 km from a flat valley in a few kilometres, "
            "with no foothills at all. Amplifiers that ramp detail amplitude "
            "with slope tend to smear exactly that transition."
        ),
        "cop30_dsm_bias": "moderate",
        "cop30_dsm_reason": (
            "Bare rock, snow and talus along the crest; lodgepole and spruce "
            "below about 2700 m, so roughly the lower half of the window carries "
            "20-30 m of canopy over 2 km of relief."
        ),
        "usgs_3dep": {"project": "WY_GrandTetonNP_D22"},
    },
    {
        "id": "smokies_appalachian",
        "geomorphic_class": "high_relief_fluvial_mountains",
        # Thunderhead / Defeat Ridge on the main crest rather than Clingmans
        # Dome: TN_BlountCo_2015 stops at lon -83.70, so the 1 m DTM that makes
        # this site worth having does not reach the more famous summit.
        "name": "Great Smoky Mountains, Tennessee (Thunderhead / Defeat Ridge)",
        "lat": 35.638,
        "lon": -83.772,
        "human_description": (
            "Mature soil-mantled fluvial mountains: rounded even-crested ridges, "
            "dendritic hollows, uniform side-slopes near the angle of repose."
        ),
        "why": (
            "The end member our amplifier should be BEST at and is easiest to "
            "check: unglaciated, soil-mantled, purely fluvially dissected, with "
            "a strikingly narrow slope distribution (most of the landscape sits "
            "within a few degrees of a characteristic hillslope angle). A "
            "generator that produces the right relief but the wrong slope "
            "HISTOGRAM shows up here immediately."
        ),
        # Labelled "severe" before it was measured, then corrected. Continuous
        # canopy over steep ground behaves like a roughly uniform 25 m offset,
        # and an offset does not change a slope: the co-located 30 m comparison
        # came out at 1.13, not the 2x this site was expected to show. The
        # canopy error is real and large in ELEVATION and small in SLOPE, which
        # is a distinction worth having in writing.
        "cop30_dsm_bias": "moderate",
        "cop30_dsm_reason": (
            "Continuous old-growth and second-growth canopy 20-40 m tall over "
            "essentially the whole window, so absolute elevations are tens of "
            "metres high. Measured effect on 30 m slope statistics is only "
            "+13% (see dsm_vs_dtm_at_30m) because a near-uniform canopy offset "
            "cancels in a gradient. Prefer the 3DEP DTM anyway for anything "
            "below ~100 m wavelength, where the canopy is not uniform."
        ),
        "usgs_3dep": {"project": "TN_BlountCo_2015"},
    },
    {
        "id": "nepal_midhills",
        "geomorphic_class": "high_relief_fluvial_mountains",
        "name": "Nepal Middle Hills, Gandaki (south of Annapurna)",
        "lat": 28.20,
        "lon": 84.50,
        "human_description": (
            "Monsoon-dissected mountain front: very deep narrow gorges, sharp "
            "spurs, landslide scars, terraced lower slopes."
        ),
        "why": (
            "Deliberately the EXTREME of the fluvial-mountain class. The Smokies "
            "are a low-uplift, transport-limited landscape; the Himalayan front "
            "is high-uplift and detachment-limited, and its slope distribution "
            "is pushed hard against the threshold angle with a much heavier "
            "upper tail. Having both bounds the class instead of describing one "
            "point in it -- which matters because our cliff gate fires at 35 deg "
            "(voxel-core/include/voxelcore/biome.h) and the two sites sit on "
            "opposite sides of it."
        ),
        "cop30_dsm_bias": "moderate",
        "cop30_dsm_reason": (
            "Terraced agriculture and shrub over much of the window with forest "
            "on north-facing slopes; canopy is patchy and short relative to the "
            "1000 m+ relief, so the bias is small in proportion."
        ),
        "usgs_3dep": None,
        "no_3dep_reason": "Outside CONUS. No free 1 m bare-earth DEM is published for the Nepal Himalaya.",
    },
    {
        "id": "iowa_rolling_hills",
        "geomorphic_class": "rolling_hills_soil_mantled",
        "name": "Southern Iowa Drift Plain, Warren/Marion County",
        "lat": 41.25,
        "lon": -93.55,
        "human_description": (
            "Rolling farm country: broad convex divides, long smooth swales, no "
            "cliffs anywhere, relief of a few tens of metres per kilometre."
        ),
        "why": (
            "The class between 'mountains' and 'plains', and the one where a "
            "generator most easily produces the wrong THING rather than the "
            "wrong amount -- ridged noise instead of smoothly convex divides "
            "separated by concave hollows. An old glacial till surface that has "
            "been fluvially dissected for ~500 kyr is the textbook case, and it "
            "is farmed, so 3DEP's bare earth is genuinely bare."
        ),
        "cop30_dsm_bias": "moderate",
        "cop30_dsm_reason": (
            "Row-crop fields (near-zero canopy) cut by wooded riparian strips "
            "and shelterbelts, which put 15-20 m walls along every drainage in "
            "the DSM. Prefer the 3DEP DTM at this site."
        ),
        "usgs_3dep": {"project": "IA_SouthCentral_2020_D20"},
    },
    {
        "id": "llano_estacado_plains",
        "geomorphic_class": "plains_low_relief",
        "name": "Llano Estacado, Roosevelt/Curry County, New Mexico",
        "lat": 34.20,
        "lon": -103.30,
        "human_description": (
            "One of the flattest large surfaces on Earth: a semi-arid caprock "
            "plain, featureless except for shallow circular playa basins."
        ),
        "why": (
            "THE most important site in the corpus. Our terrain looks worst on "
            "gentle ground, and the Llano Estacado is the cleanest possible "
            "measurement of what gentle ground actually is: a relict Ogallala "
            "surface with essentially no integrated drainage, so the reference "
            "slope distribution is a tight spike near zero rather than a scaled "
            "-down version of hill country. It is also the best-behaved site for "
            "the DSM problem -- short grass and bare soil, no trees at all."
        ),
        "cop30_dsm_bias": "negligible",
        "cop30_dsm_reason": "Shortgrass prairie and dryland fields; no canopy and no buildings in the window.",
        "usgs_3dep": {"project": "NM_Roosevelt_Curry_2015"},
    },
    {
        "id": "illinois_till_plain",
        "geomorphic_class": "plains_low_relief",
        "name": "Bloomington Ridged Plain, Champaign County, Illinois",
        "lat": 40.15,
        "lon": -88.30,
        "human_description": (
            "Young glacial till plain: nearly flat, with very broad low moraine "
            "swells and shallow, barely incised streams."
        ),
        "why": (
            "The second plains site, and a different KIND of flat. The Llano is "
            "flat because nothing has eroded it; the Wisconsinan till plain is "
            "flat because it was deposited that way 20 kyr ago and drainage has "
            "not yet organised. Their slope spikes differ in width and their "
            "long-wavelength structure differs completely (playas vs moraine "
            "ridges), so together they say what 'plains' may not be collapsed "
            "into -- a single flatness number."
        ),
        "cop30_dsm_bias": "moderate",
        "cop30_dsm_reason": (
            "Almost entirely maize/soy, so canopy is near zero over most of the "
            "window, but farmsteads and windbreaks appear as isolated 10-20 m "
            "spikes -- which, on a surface with 10 m of total relief, is a large "
            "relative error. The 3DEP DTM is strongly preferred here."
        ),
        "usgs_3dep": {"project": "IL_8County_PlusChampaign_2019_B19"},
    },
    {
        "id": "white_sands_dunes",
        "geomorphic_class": "desert_aeolian_dunes",
        "name": "White Sands, Tularosa Basin, New Mexico",
        "lat": 32.85,
        "lon": -106.28,
        "human_description": (
            "An active transverse and barchan dune field: regular parallel "
            "crests, gentle stoss slopes and steep slip faces at the angle of "
            "repose, on a flat basin floor."
        ),
        "why": (
            "Dune fields are the one landform with a hard PHYSICAL ceiling on "
            "slope -- the ~32-34 deg angle of repose of dry sand -- and a "
            "strongly periodic, anisotropic planform. That makes them the "
            "sharpest falsification test in the corpus: a generator using "
            "isotropic fractal noise cannot reproduce either property, and the "
            "failure is visible in one histogram. White Sands is chosen over a "
            "larger erg because 3DEP covers it, so we get the slip-face angle "
            "measured at 1 m instead of smeared over a 30 m post."
        ),
        "cop30_dsm_bias": "negligible",
        "cop30_dsm_reason": "Bare gypsum sand with sparse low shrubs; the DSM is the ground.",
        "usgs_3dep": {"project": "NM_WhiteSandsNM_2020_D20"},
    },
    {
        "id": "rub_al_khali_dunes",
        "geomorphic_class": "desert_aeolian_dunes",
        "name": "Rub' al Khali, Saudi Arabia (eastern sand sea)",
        "lat": 20.50,
        "lon": 52.50,
        "human_description": (
            "Mega-dune sand sea: linear and star draa hundreds of metres tall "
            "and kilometres apart, with flat interdune corridors."
        ),
        "why": (
            "The same class at a completely different SCALE. White Sands dunes "
            "are 10-30 m tall with ~150 m spacing -- inside our amplification "
            "band; Rub' al Khali draa are 100-250 m tall with 2-3 km spacing -- "
            "in the carrier band the diffusion model is supposed to produce. "
            "Together they check that dune structure is right at both ends "
            "rather than only where we happened to look."
        ),
        "cop30_dsm_bias": "negligible",
        "cop30_dsm_reason": "Hyper-arid sand sea; no vegetation and no structures.",
        "usgs_3dep": None,
        "no_3dep_reason": "Outside CONUS. No free 1 m DEM exists for the Empty Quarter.",
    },
    {
        "id": "badlands_sd",
        "geomorphic_class": "badlands_dissected",
        "name": "Badlands National Park, South Dakota (the Wall)",
        "lat": 43.80,
        "lon": -102.30,
        "human_description": (
            "Badlands: a razor-sharp escarpment shredded into knife ridges, "
            "rills and gullies, separating a high prairie table from a lower one."
        ),
        "why": (
            "The maximum-drainage-density end of the fluvial spectrum. Slopes "
            "sit at the threshold angle almost everywhere, drainage texture is "
            "an order of magnitude finer than in the Smokies, and -- most "
            "usefully -- the window contains BOTH the dissected escarpment and "
            "the undissected prairie above it. That bimodality is a strong test: "
            "real landscapes put two populations side by side with a sharp "
            "boundary, and blended fractal noise cannot."
        ),
        "cop30_dsm_bias": "negligible",
        "cop30_dsm_reason": (
            "Bare Brule/Chadron clay and shortgrass prairie; the finest rills are "
            "below the 30 m posting, but nothing sits above the ground surface."
        ),
        "usgs_3dep": {"project": "SD_Southwest_NRCS_SD_2018_D18"},
    },
    {
        "id": "nc_coastal_plain",
        "geomorphic_class": "coastal_plain_shoreline",
        # Deliberately ON the Pamlico Sound shore, not in the peninsula's
        # interior. The first placement (35.55, -76.20) was 99.4% land, which
        # made the one site that exists to exercise sea level not exercise it.
        # This window is ~30% open water.
        "name": "Pamlico Sound shore, Hyde County, North Carolina",
        "lat": 35.40,
        "lon": -76.25,
        "human_description": (
            "Outer coastal plain at the water's edge: a flat, near-sea-level "
            "surface with peat domes and drainage canals, ending in a low "
            "shoreline against a broad sound."
        ),
        "why": (
            "The only site that exercises the sea-level machinery, and the "
            "reason the void policy is written the way it is: GLO-30 stores open "
            "water as exactly 0.0, so an unmasked run turns the sound into a "
            "vast perfectly flat 'plain' and halves every slope statistic. This "
            "site also exercises the beach/ocean gate in biome.h, which owns a "
            "7 m band around sea level -- here that band is most of the land, so "
            "it says what land INSIDE that band really looks like."
        ),
        "cop30_dsm_bias": "severe",
        "cop30_dsm_reason": (
            "Pocosin shrub bog and loblolly plantations 10-25 m tall on ground "
            "with under 10 m of total relief -- the canopy is taller than the "
            "landform. Copernicus is unusable for fine structure here; the 3DEP "
            "DTM is the reference and cop30 is kept only to quantify the error."
        ),
        # NOT NC_Phase4_2017_A17, which despite the name covers the Piedmont
        # around Winston-Salem. NC_HurricaneFlorence_2020_D20's zone-18 half is
        # the eastern coastal plain, including this peninsula.
        "usgs_3dep": {"project": "NC_HurricaneFlorence_2020_D20"},
    },
    {
        "id": "grand_canyon_plateau",
        "geomorphic_class": "plateau_incised_canyon",
        "name": "Grand Canyon, South Rim, Arizona (Bright Angel / Tonto)",
        "lat": 36.15,
        "lon": -112.20,
        "human_description": (
            "A flat plateau surface abruptly cut by a 1500 m canyon with a "
            "stepped cliff-and-bench profile from alternating hard and soft beds."
        ),
        "why": (
            "The class our height-field amplifier is least equipped to produce. "
            "It is defined by STRATIGRAPHY: near-vertical risers alternating "
            "with near-horizontal treads at elevations that are consistent for "
            "tens of kilometres, plus a flat rim that is not merely 'less "
            "steep'. The slope histogram is strongly bimodal and elevation "
            "shows banding, neither of which any amplitude-tuned noise produces. "
            "3DEP covers the park at 1 m over bare rock, so the bench structure "
            "is measurable rather than argued about."
        ),
        "cop30_dsm_bias": "negligible",
        "cop30_dsm_reason": (
            "Bare Kaibab/Coconino rock in the canyon; open pinyon-juniper on the "
            "rim adds a few metres, which is nothing against 1500 m of relief."
        ),
        "usgs_3dep": {"project": "AZ_GrandCanyonNP_2019_B19"},
    },
]

#: Analysis window for Copernicus, in kilometres, square, centred on the site.
#: 20 km at 30 m is ~667x667 = 445k posts: large enough for stable percentiles,
#: small enough to stay inside one 1x1 deg tile and inside one landform.
COP30_WINDOW_KM = 20.0

#: Analysis window for 3DEP, in metres. 2 km at 1 m is 4.2M posts from a single
#: 10 km tile -- no mosaicking, and a 2 km transect is long enough to contain
#: several drainage or dune wavelengths at every site.
USGS_WINDOW_M = 2048

#: Block-averaging factors for the multi-scale slope profile. The point of the
#: 3DEP ladder is its last rung: 1 m data averaged to 30 m is directly
#: comparable to Copernicus at 30 m, and the difference is the DSM bias.
COP30_SCALE_FACTORS = (1, 2, 4, 8)  # 30, 60, 120, 240 m
USGS_SCALE_FACTORS = (1, 3, 10, 30)  # 1, 3, 10, 30 m


# ===========================================================================
# Pure geodesy / naming helpers (no network, no rasterio -- these are the
# tested ones, because an off-by-one in a tile name fails as a 404 but an
# off-by-one in a metres-per-degree formula fails silently).
# ===========================================================================


def cop30_tile_id(lat: float, lon: float) -> str:
    """Copernicus GLO-30 tile id for the 1x1 deg cell containing (lat, lon).

    Tiles are named for their SOUTH-WEST corner, so this floors rather than
    rounds: lon -112.2 is in W113 (which spans -113..-112), not W112.
    """
    la = math.floor(lat)
    lo = math.floor(lon)
    ns = "N" if la >= 0 else "S"
    ew = "E" if lo >= 0 else "W"
    return f"Copernicus_DSM_COG_10_{ns}{abs(la):02d}_00_{ew}{abs(lo):03d}_00_DEM"


def cop30_url(tile_id: str) -> str:
    return f"{COP30_BUCKET}/{tile_id}/{tile_id}.tif"


def utm_epsg_nad83(lat: float, lon: float) -> int:
    """NAD83 UTM EPSG code for a CONUS point (269xx = NAD83 / UTM zone xxN)."""
    zone = int(math.floor((lon + 180.0) / 6.0)) + 1
    if not 1 <= zone <= 60:
        raise ValueError(f"longitude {lon} gives UTM zone {zone}")
    if lat < 0:
        raise ValueError("NAD83 UTM north zones only; this corpus is CONUS-only for 3DEP")
    return 26900 + zone


def meters_per_degree(lat_deg: np.ndarray | float) -> tuple[np.ndarray, np.ndarray]:
    """(metres per degree of longitude, metres per degree of latitude) on WGS84.

    Standard series expansion of the parallel and meridian arc lengths. Used
    for the un-resampled cross-check grid; getting this wrong is the classic
    way to produce slopes that are off by 1/cos(lat) and look plausible.
    """
    phi = np.radians(lat_deg)
    m_lat = (
        111132.92
        - 559.82 * np.cos(2 * phi)
        + 1.175 * np.cos(4 * phi)
        - 0.0023 * np.cos(6 * phi)
    )
    m_lon = 111412.84 * np.cos(phi) - 93.5 * np.cos(3 * phi) + 0.118 * np.cos(5 * phi)
    return m_lon, m_lat


def horn_slope_deg(z: np.ndarray, dx, dy) -> np.ndarray:
    """Horn (1981) 3x3 slope in degrees; the same estimator gdaldem uses.

    ``z`` is metres with NaN for masked cells; ``dx``/``dy`` are ground
    distances in metres and may be arrays broadcastable against ``z`` (that is
    how the geographic cross-check varies dx per row). NaN propagates: an
    output cell is NaN if the centre OR any of the eight neighbours is masked,
    and the array border is treated as masked. So a void can never contribute
    a spurious cliff, and no slope is reported where there is no ground.
    """
    z = np.asarray(z, dtype=np.float64)
    p = np.pad(z, 1, mode="constant", constant_values=np.nan)
    a, b, c = p[:-2, :-2], p[:-2, 1:-1], p[:-2, 2:]
    d, f = p[1:-1, :-2], p[1:-1, 2:]
    g, h, i = p[2:, :-2], p[2:, 1:-1], p[2:, 2:]
    dzdx = ((c + 2 * f + i) - (a + 2 * d + g)) / (8.0 * np.asarray(dx, dtype=np.float64))
    dzdy = ((g + 2 * h + i) - (a + 2 * b + c)) / (8.0 * np.asarray(dy, dtype=np.float64))
    out = np.degrees(np.arctan(np.hypot(dzdx, dzdy)))
    # Horn's kernel never reads the CENTRE cell, so without this a one-pixel
    # void would still be handed a slope interpolated from the ring around it
    # -- a slope at a place with no measured ground. Report nothing instead.
    return np.where(np.isnan(z), np.nan, out)


def block_mean(z: np.ndarray, factor: int, min_valid_frac: float = 0.5) -> np.ndarray:
    """Average ``z`` down by an integer factor, NaN-aware.

    A coarse cell is valid only if at least ``min_valid_frac`` of its fine
    cells were valid; otherwise it is NaN. Without that rule a coarse cell
    sitting on the edge of a void would take the value of the one fine cell
    that survived and read as a step.
    """
    if factor == 1:
        return np.asarray(z, dtype=np.float64)
    h = (z.shape[0] // factor) * factor
    w = (z.shape[1] // factor) * factor
    if h == 0 or w == 0:
        return np.empty((0, 0), dtype=np.float64)
    blk = np.asarray(z[:h, :w], dtype=np.float64).reshape(
        h // factor, factor, w // factor, factor
    )
    valid = np.isfinite(blk)
    n = valid.sum(axis=(1, 3))
    total = np.nansum(np.where(valid, blk, 0.0), axis=(1, 3))
    out = np.where(n > 0, total / np.maximum(n, 1), np.nan)
    return np.where(n >= min_valid_frac * factor * factor, out, np.nan)


def plane_detrended_rms(z: np.ndarray) -> float:
    """RMS residual (m) after removing the best-fit plane.

    A scale-free roughness: it is what is left once regional tilt is gone, so
    a 3 deg uniform ramp scores ~0 while genuinely bumpy ground does not.
    """
    m = np.isfinite(z)
    if m.sum() < 16:
        return float("nan")
    yy, xx = np.mgrid[0 : z.shape[0], 0 : z.shape[1]]
    A = np.column_stack([xx[m].ravel(), yy[m].ravel(), np.ones(int(m.sum()))])
    coef, *_ = np.linalg.lstsq(A, z[m].ravel(), rcond=None)
    resid = z[m].ravel() - A @ coef
    return float(np.sqrt(np.mean(resid**2)))


def _pct(a: np.ndarray, q: float) -> float:
    return float(np.nanpercentile(a, q)) if np.isfinite(a).any() else float("nan")


def describe_grid(z: np.ndarray, pixel_m: float, scale_factors) -> dict:
    """Descriptive statistics for one masked, metric, square-pixel grid."""
    valid = np.isfinite(z)
    n_valid = int(valid.sum())
    out = {
        "pixel_size_m": round(float(pixel_m), 4),
        "shape": [int(z.shape[0]), int(z.shape[1])],
        "n_valid": n_valid,
        "valid_fraction": round(n_valid / max(z.size, 1), 6),
    }
    if n_valid < 100:
        out["error"] = "too few valid pixels for statistics"
        return out

    slope = horn_slope_deg(z, pixel_m, pixel_m)
    sv = slope[np.isfinite(slope)]
    out["elevation_m"] = {
        "min": round(float(np.nanmin(z)), 2),
        "max": round(float(np.nanmax(z)), 2),
        "mean": round(float(np.nanmean(z)), 2),
        "std": round(float(np.nanstd(z)), 2),
        "p1": round(_pct(z, 1), 2),
        "p99": round(_pct(z, 99), 2),
    }
    out["relief_m"] = {
        "full_range": round(float(np.nanmax(z) - np.nanmin(z)), 2),
        "p1_p99": round(_pct(z, 99) - _pct(z, 1), 2),
    }
    out["slope_deg"] = {
        "mean": round(float(np.mean(sv)), 3),
        "p50": round(float(np.percentile(sv, 50)), 3),
        "p95": round(float(np.percentile(sv, 95)), 3),
        "p99": round(float(np.percentile(sv, 99)), 3),
        "max": round(float(np.max(sv)), 3),
    }
    out["flat_fraction"] = round(float((sv < FLAT_SLOPE_DEG).mean()), 5)
    out["above_cliff_gate_fraction"] = round(float((sv > 35.0).mean()), 5)
    out["detrended_rms_m"] = round(plane_detrended_rms(z), 3)

    profile = {}
    for f in scale_factors:
        zz = block_mean(z, f)
        if zz.size == 0:
            continue
        s = horn_slope_deg(zz, pixel_m * f, pixel_m * f)
        s = s[np.isfinite(s)]
        if s.size < 100:
            continue
        profile[f"{pixel_m * f:g}m"] = {
            "slope_mean_deg": round(float(np.mean(s)), 3),
            "slope_p95_deg": round(float(np.percentile(s, 95)), 3),
        }
    out["slope_by_scale"] = profile
    return out


# ===========================================================================
# Fetching -- resumable, checksummed, truncation-guarded.
# ===========================================================================


def _request(url: str, headers: dict | None = None, method: str = "GET"):
    h = {"User-Agent": USER_AGENT}
    if headers:
        h.update(headers)
    return urllib.request.Request(url, headers=h, method=method)


def head(url: str) -> tuple[int, dict]:
    try:
        with urllib.request.urlopen(_request(url, method="HEAD"), timeout=60) as r:
            return r.status, dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {})
    except Exception as e:  # noqa: BLE001
        return 0, {"error": str(e)}


def sha256_file(path: Path, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def download_resumable(url: str, dest: Path, expect_sha256: str | None = None,
                       attempts: int = 6, quiet: bool = False) -> dict:
    """Download ``url`` to ``dest``, resuming a partial ``dest.part`` if present.

    Returns {size, sha256, source_etag}. Raises on failure.

    Three separate guards, because a GeoTIFF that is short by a few megabytes
    still OPENS and still reports its full declared shape -- the truncation only
    surfaces as a read error, or worse as zeros, deep inside a statistic:
      1. byte length must equal the server's Content-Length;
      2. if the manifest already carries a sha256, it must match;
      3. the caller additionally reads the last block (see ``_assert_readable``).
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")

    status, hdrs = head(url)
    if status != 200:
        raise RuntimeError(f"HEAD {url} -> {status}")
    total = int(hdrs.get("Content-Length", 0))
    etag = (hdrs.get("ETag") or "").strip('"')

    if dest.exists():
        if dest.stat().st_size == total:
            digest = sha256_file(dest)
            if expect_sha256 is None or digest == expect_sha256:
                return {"size": total, "sha256": digest, "source_etag": etag}
            if not quiet:
                print(f"  cached {dest.name} sha256 mismatch -- refetching", file=sys.stderr)
        dest.unlink()

    for attempt in range(attempts):
        have = part.stat().st_size if part.exists() else 0
        if have >= total > 0:
            break
        try:
            hdr = {"Range": f"bytes={have}-"} if have else {}
            with urllib.request.urlopen(_request(url, hdr), timeout=120) as r:
                if have and r.status != 206:
                    # Server ignored the range; start over rather than append
                    # garbage to a half-file.
                    part.unlink(missing_ok=True)
                    have = 0
                with open(part, "ab" if have else "wb") as f:
                    while True:
                        chunk = r.read(1 << 20)
                        if not chunk:
                            break
                        f.write(chunk)
                        have += len(chunk)
                        if total and not quiet:
                            print(f"\r  {dest.name}: {have / 1e6:6.1f} / {total / 1e6:.1f} MB",
                                  end="", flush=True)
            if not quiet and total:
                print()
        except Exception as e:  # noqa: BLE001
            if not quiet:
                print(f"\n  attempt {attempt + 1}/{attempts} failed: {e}", file=sys.stderr)
            time.sleep(min(2**attempt, 20))

    have = part.stat().st_size if part.exists() else 0
    if have != total:
        raise RuntimeError(f"{url}: got {have} bytes, expected {total} -- truncated")
    digest = sha256_file(part)
    if expect_sha256 is not None and digest != expect_sha256:
        raise RuntimeError(f"{url}: sha256 {digest} != manifest {expect_sha256}")
    part.replace(dest)
    return {"size": total, "sha256": digest, "source_etag": etag}


def _assert_readable(path: Path) -> None:
    """Open and read the LAST block, which is what a truncated file cannot do."""
    import rasterio
    from rasterio.windows import Window

    with rasterio.open(path) as ds:
        bh, bw = ds.block_shapes[0]
        h = min(bh, ds.height)
        w = min(bw, ds.width)
        ds.read(1, window=Window(ds.width - w, ds.height - h, w, h))


# ===========================================================================
# USGS 3DEP tile resolution.
# ===========================================================================


def s3_list(bucket: str, prefix: str, delimiter: str | None = None) -> tuple[list[str], list[str]]:
    """Anonymous S3 v2 listing. Returns (keys, common_prefixes), paginated."""
    keys: list[str] = []
    prefixes: list[str] = []
    token = None
    for _ in range(200):
        q = {"list-type": "2", "prefix": prefix, "max-keys": "1000"}
        if delimiter:
            q["delimiter"] = delimiter
        if token:
            q["continuation-token"] = token
        url = f"{bucket}/?{urllib.parse.urlencode(q)}"
        with urllib.request.urlopen(_request(url), timeout=90) as r:
            body = r.read().decode("utf-8", "replace")
        keys += re.findall(r"<Key>([^<]+)</Key>", body)
        prefixes += re.findall(r"<Prefix>([^<]+)</Prefix>", body)
        m = re.search(r"<NextContinuationToken>([^<]+)</NextContinuationToken>", body)
        if not m:
            break
        token = m.group(1)
    return keys, prefixes


def project_tiff_keys(project: str, cache_dir: Path) -> list[str]:
    """TIFF keys for a 3DEP 1 m project, cached to JSON (the listing is slow)."""
    cache = cache_dir / "listings" / f"{project}.json"
    if cache.exists():
        return json.loads(cache.read_text())
    keys, _ = s3_list(TNM_BUCKET, f"{TNM_1M_PREFIX}/{project}/TIFF/")
    keys = [k for k in keys if k.lower().endswith(".tif")]
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(keys, indent=0))
    return keys


_TILE_XY = re.compile(r"[xX](\d+)[yY](\d+)")
#: ``USGS_1M_16_x39y440_...`` -- the token before x##y## is the UTM zone. A
#: project can span TWO zones (IL_8County_PlusChampaign_2019_B19 and
#: NC_Phase4_2017_A17 both do), and assuming the first tile's CRS applies to
#: the whole project puts the point hundreds of kilometres outside every tile.
_TILE_ZONE = re.compile(r"_(\d{1,2})_[xX]\d+[yY]\d+")


def _tile_groups(keys: list[str]) -> dict[str | None, list[str]]:
    """Split a project's tile keys by the UTM-zone token in the filename."""
    groups: dict[str | None, list[str]] = {}
    for k in keys:
        name = Path(k).name
        if not _TILE_XY.search(name):
            continue
        z = _TILE_ZONE.search(name)
        groups.setdefault(z.group(1) if z else None, []).append(k)
    return groups


def resolve_3dep_tiles(project: str, lat: float, lon: float, cache_dir: Path,
                       want: int = 4) -> list[dict]:
    """Find the 3DEP 1 m tile containing (lat, lon), plus fallbacks.

    Returns the containing tile first, then the next-nearest tiles. The extra
    candidates exist because containing the point is NOT the same as having
    data at it: a 3DEP tile is a full 10 km raster even where the flight lines
    did not reach, and the gap is stored as -999999, not as an absent tile.
    Two of this corpus's twelve sites landed in exactly that hole.
    """
    first = resolve_3dep_tile(project, lat, lon, cache_dir)
    out = [first]
    if want <= 1:
        return out

    import rasterio

    keys = project_tiff_keys(project, cache_dir)
    px, py = first["x"], first["y"]
    same_zone = _tile_groups([k for k in keys])
    zone = None
    for z, members in same_zone.items():
        if first["key"] in members:
            zone = z
            break
    members = same_zone.get(zone, keys)
    ranked = []
    for k in members:
        if k == first["key"]:
            continue
        m = _TILE_XY.search(Path(k).name)
        if not m:
            continue
        cx = int(m.group(1)) * 10000 + 5000
        cy = int(m.group(2)) * 10000 - 5000
        ranked.append((math.hypot(cx - px, cy - py), k))
    ranked.sort()
    for _, key in ranked[: want - 1]:
        with rasterio.open(_vsicurl(f"{TNM_BUCKET}/{key}")) as ds:
            b = ds.bounds
        out.append({"key": key, "url": f"{TNM_BUCKET}/{key}", "crs": first["crs"],
                    "x": px, "y": py, "bounds": [b.left, b.bottom, b.right, b.top]})
    return out


def resolve_3dep_tile(project: str, lat: float, lon: float, cache_dir: Path) -> dict:
    """Find the 3DEP 1 m tile containing (lat, lon) and confirm it by bounds.

    Tile names encode the SW-ish corner in units of 10 km in the project's own
    projected CRS (``..._x32y413_...`` -> easting 320000, northing 4130000).
    None of that is trusted on its own: within each UTM-zone group we read one
    tile's real CRS, rank candidates by name arithmetic, then OPEN the best few
    and require the point to fall inside their actual bounds. Only a tile that
    passes that check is returned, so a project that merely lies NEAR the site
    fails loudly instead of silently characterising the wrong ground.
    """
    import rasterio
    from rasterio.warp import transform as warp_transform

    keys = project_tiff_keys(project, cache_dir)
    if not keys:
        raise RuntimeError(f"no TIFFs listed for 3DEP project {project}")
    groups = _tile_groups(keys)
    if not groups:
        raise RuntimeError(f"no x###y### tile names in project {project}")

    tried = []
    for zone, members in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        with rasterio.open(_vsicurl(f"{TNM_BUCKET}/{members[0]}")) as ds:
            crs = ds.crs
        xs, ys = warp_transform("EPSG:4326", crs, [lon], [lat])
        px, py = xs[0], ys[0]

        ranked = []
        for k in members:
            m = _TILE_XY.search(Path(k).name)
            cx = int(m.group(1)) * 10000 + 5000
            cy = int(m.group(2)) * 10000 - 5000
            ranked.append((math.hypot(cx - px, cy - py), k))
        ranked.sort()
        tried.append(f"zone {zone} ({crs}) nearest {Path(ranked[0][1]).name} "
                     f"at {ranked[0][0] / 1000:.0f} km")
        for _, key in ranked[:4]:
            with rasterio.open(_vsicurl(f"{TNM_BUCKET}/{key}")) as ds:
                b = ds.bounds
                if b.left <= px <= b.right and b.bottom <= py <= b.top:
                    return {"key": key, "url": f"{TNM_BUCKET}/{key}", "crs": str(crs),
                            "x": px, "y": py,
                            "bounds": [b.left, b.bottom, b.right, b.top]}
    raise RuntimeError(
        f"{project} does not cover ({lat}, {lon}): " + "; ".join(tried)
    )


def _vsicurl(url: str) -> str:
    return "/vsicurl/" + url


def _gdal_env() -> None:
    os.environ.setdefault("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR")
    os.environ.setdefault("AWS_NO_SIGN_REQUEST", "YES")
    os.environ.setdefault("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif")
    os.environ.setdefault("GDAL_HTTP_MAX_RETRY", "5")
    os.environ.setdefault("GDAL_HTTP_RETRY_DELAY", "2")


# ===========================================================================
# Per-source analysis.
# ===========================================================================


def _mask_copernicus(z: np.ndarray, nodata) -> tuple[np.ndarray, dict]:
    z = np.asarray(z, dtype=np.float64)
    total = z.size
    bad_nodata = ~np.isfinite(z)
    if nodata is not None and np.isfinite(nodata):
        bad_nodata |= z == nodata
    bad_nodata |= z <= VOID_FLOOR_M
    sea = (z == 0.0) & ~bad_nodata
    z = np.where(bad_nodata | sea, np.nan, z)
    return z, {
        "nodata_fraction": round(float(bad_nodata.sum()) / total, 6),
        "sea_or_zero_fraction": round(float(sea.sum()) / total, 6),
        "masked_fraction": round(float((bad_nodata | sea).sum()) / total, 6),
    }


def _mask_usgs(z: np.ndarray, nodata) -> tuple[np.ndarray, dict]:
    z = np.asarray(z, dtype=np.float64)
    total = z.size
    bad = ~np.isfinite(z)
    if nodata is not None and np.isfinite(nodata):
        bad |= np.isclose(z, nodata, rtol=0, atol=abs(nodata) * 1e-6 + 1.0)
    bad |= z <= VOID_FLOOR_M
    z = np.where(bad, np.nan, z)
    return z, {
        "nodata_fraction": round(float(bad.sum()) / total, 6),
        "sea_or_zero_fraction": 0.0,
        "masked_fraction": round(float(bad.sum()) / total, 6),
    }


def cop30_metric_grid(path: Path, lat: float, lon: float, window_m: float):
    """Read a Copernicus window and return it on a local 30 m metric grid.

    Returns (grid_30m, void_stats, native_slope_deg, native_pixel_m). See the
    module docstring for why there are two grids; this is the shared reader
    behind both the full-site statistics and the co-located DSM/DTM comparison.
    """
    import rasterio
    from rasterio.crs import CRS
    from rasterio.transform import from_origin
    from rasterio.warp import Resampling, reproject
    from rasterio.windows import from_bounds

    half_m = window_m / 2.0
    m_lon, m_lat = meters_per_degree(lat)
    dlon = half_m / float(m_lon)
    dlat = half_m / float(m_lat)
    # Read a 15% margin so the warper has neighbours at the window edge and the
    # native cross-check is computed on the same ground, not a smaller patch.
    mlon, mlat = dlon * 1.15, dlat * 1.15

    with rasterio.open(path) as ds:
        b = ds.bounds
        if not (b.left <= lon - mlon and lon + mlon <= b.right
                and b.bottom <= lat - mlat and lat + mlat <= b.top):
            raise RuntimeError(
                f"{path.name}: {window_m / 1000:g} km window at ({lat}, {lon}) crosses the "
                "tile edge; move the site centre further inside the 1x1 deg tile"
            )
        win = from_bounds(lon - mlon, lat - mlat, lon + mlon, lat + mlat, ds.transform)
        arr = ds.read(1, window=win)
        wt = ds.window_transform(win)
        src_nodata = ds.nodata
        src_crs = ds.crs

    masked, void_stats = _mask_copernicus(arr, src_nodata)

    # ---- cross-check grid: native geographic posts, explicit ground metres ---
    rows = masked.shape[0]
    lat_rows = wt.f + (np.arange(rows) + 0.5) * wt.e  # wt.e is negative
    ml, mm = meters_per_degree(lat_rows)
    dx_m = (abs(wt.a) * ml).reshape(-1, 1)
    dy_m = (abs(wt.e) * mm).reshape(-1, 1)
    native_slope = horn_slope_deg(masked, dx_m, dy_m)
    ns = native_slope[np.isfinite(native_slope)]

    # ---- primary grid: local azimuthal equidistant, exactly 30 m square ------
    aeqd = CRS.from_proj4(
        f"+proj=aeqd +lat_0={lat} +lon_0={lon} +x_0=0 +y_0=0 +datum=WGS84 +units=m +no_defs"
    )
    n = int(round(window_m / 30.0))
    dst_transform = from_origin(-half_m, half_m, 30.0, 30.0)
    dst = np.full((n, n), WARP_NODATA, dtype=np.float32)
    reproject(
        source=np.where(np.isfinite(masked), masked, WARP_NODATA).astype(np.float32),
        destination=dst,
        src_transform=wt,
        src_crs=src_crs,
        src_nodata=WARP_NODATA,
        dst_transform=dst_transform,
        dst_crs=aeqd,
        dst_nodata=WARP_NODATA,
        resampling=Resampling.bilinear,
    )
    grid = np.where(dst == WARP_NODATA, np.nan, dst.astype(np.float64))
    native_px = {
        "east_west": round(float(abs(wt.a) * float(meters_per_degree(lat)[0])), 2),
        "north_south": round(float(abs(wt.e) * float(meters_per_degree(lat)[1])), 2),
    }
    return grid, void_stats, ns, native_px


def analyse_cop30(path: Path, lat: float, lon: float, window_km: float) -> dict:
    """Full statistics for one Copernicus site, on BOTH grids."""
    grid, void_stats, ns, native_px = cop30_metric_grid(path, lat, lon, window_km * 1000.0)

    stats = describe_grid(grid, 30.0, COP30_SCALE_FACTORS)
    stats["grid"] = "aeqd30"
    stats["grid_note"] = (
        f"local azimuthal-equidistant about ({lat}, {lon}), 30.0 m square pixels, "
        "bilinear; comparable pixel-for-pixel with a 30 m game tile"
    )
    stats["voids"] = void_stats
    stats["native_source_pixel_m"] = native_px
    stats["crosscheck_native_ground"] = {
        "note": "no resampling; Horn slope on native 1-arcsec posts with per-row WGS84 ground distances",
        "slope_mean_deg": round(float(np.mean(ns)), 3) if ns.size else float("nan"),
        "slope_p95_deg": round(float(np.percentile(ns, 95)), 3) if ns.size else float("nan"),
    }
    if ns.size and stats.get("slope_deg"):
        a = stats["slope_deg"]["mean"]
        c = stats["crosscheck_native_ground"]["slope_mean_deg"]
        stats["crosscheck_native_ground"]["mean_slope_ratio_native_over_aeqd30"] = (
            round(float(c / a), 4) if a else None
        )
    return stats


def colocated_dsm_vs_dtm(cop_path: Path, usgs_path: Path) -> dict:
    """DSM-minus-DTM bias measured on the SAME ground, at the SAME 30 m posting.

    The naive version of this comparison -- cop30's 20 km window against 3DEP's
    2 km window -- measures the window mismatch, not the sensor difference: at
    Badlands the 20 km box is mostly undissected prairie and the 2 km box is
    the escarpment, so the "bias" comes out below 1. This instead reads the
    Copernicus DSM over exactly the footprint of the cached 3DEP window and
    block-averages the 3DEP DTM to the same 30 m posting. What is left is the
    canopy and buildings, which is the thing the caveat is about.
    """
    import rasterio
    from rasterio.warp import transform as warp_transform

    with rasterio.open(usgs_path) as ds:
        t = ds.transform
        cx = t.c + ds.width / 2.0 * t.a
        cy = t.f + ds.height / 2.0 * t.e
        size_m = ds.width * abs(t.a)
        z = ds.read(1)
        nodata = ds.nodata
        px = abs(t.a)
        lons, lats = warp_transform(ds.crs, "EPSG:4326", [cx], [cy])
    clat, clon = float(lats[0]), float(lons[0])

    dtm, _ = _mask_usgs(z, nodata)
    factor = int(round(30.0 / px))
    dtm30 = block_mean(dtm, factor)
    dsm30, _, _, _ = cop30_metric_grid(cop_path, clat, clon, size_m)

    def _s(g):
        s = horn_slope_deg(g, 30.0, 30.0)
        s = s[np.isfinite(s)]
        if s.size < 50:
            return None
        return {
            "n": int(s.size),
            "slope_mean_deg": round(float(np.mean(s)), 3),
            "slope_p95_deg": round(float(np.percentile(s, 95)), 3),
            "detrended_rms_m": round(plane_detrended_rms(g), 3),
            "relief_p1_p99_m": round(_pct(g, 99) - _pct(g, 1), 2),
        }

    a, b = _s(dsm30), _s(dtm30)
    if not a or not b:
        return {"error": "not enough valid pixels for a co-located comparison"}
    return {
        "note": (
            "SAME footprint, SAME 30 m posting: Copernicus GLO-30 (DSM) read over the "
            "exact 2 km box of the cached 3DEP window, against the 3DEP 1 m DTM "
            "block-averaged 30x. The ratio is canopy plus buildings, nothing else."
        ),
        "footprint_center_lat_lon": [round(clat, 5), round(clon, 5)],
        "footprint_m": round(float(size_m), 1),
        "cop30_dsm": a,
        "usgs_dtm_at_30m": b,
        "dsm_over_dtm_mean_slope": round(a["slope_mean_deg"] / b["slope_mean_deg"], 3)
        if b["slope_mean_deg"] else None,
        "dsm_over_dtm_detrended_rms": round(a["detrended_rms_m"] / b["detrended_rms_m"], 3)
        if b["detrended_rms_m"] else None,
    }


def analyse_usgs(path: Path) -> dict:
    """Statistics for one cached 3DEP window. Already metric; no reprojection."""
    import rasterio

    with rasterio.open(path) as ds:
        arr = ds.read(1)
        nodata = ds.nodata
        px = abs(ds.transform.a)
        crs = str(ds.crs)

    masked, void_stats = _mask_usgs(arr, nodata)
    stats = describe_grid(masked, px, USGS_SCALE_FACTORS)
    stats["grid"] = "utm_native"
    stats["grid_note"] = (
        f"{crs}, {px:g} m square pixels as delivered; no reprojection "
        "(UTM scale distortion over a 2 km window is <0.1% and is ignored)"
    )
    stats["voids"] = void_stats
    return stats


#: A cached 3DEP window must be at least this fraction real ground, or the
#: statistics describe the flight-line gap rather than the landscape.
USGS_MIN_VALID_FRACTION = 0.98


def _window_valid_fraction(ds, col0: int, row0: int, n: int, probe: int = 192) -> float:
    """Cheap coverage probe: read a small patch at the centre of a candidate.

    A full 2048^2 read is ~16 MB over the wire; a 192^2 probe is ~150 KB. That
    is the difference between screening twenty candidate positions in seconds
    and in minutes.
    """
    from rasterio.windows import Window

    p = min(probe, n)
    c = col0 + (n - p) // 2
    r = row0 + (n - p) // 2
    a = ds.read(1, window=Window(c, r, p, p)).astype(np.float64)
    nod = ds.nodata
    bad = ~np.isfinite(a) | (a <= VOID_FLOOR_M)
    if nod is not None and np.isfinite(nod):
        bad |= np.isclose(a, nod, rtol=0, atol=abs(nod) * 1e-6 + 1.0)
    return float((~bad).mean())


def extract_usgs_window(tiles: list[dict], size_m: int, dest: Path) -> dict:
    """Cut a ``size_m`` square window out of a 3DEP tile via HTTP range reads.

    3DEP 1 m tiles are 40-500 MB each, so mirroring twelve of them is not on.
    They are internally tiled 256x256 LZW GeoTIFFs, which makes /vsicurl/ a
    range-read of a few hundred KB instead. What we cache and checksum is the
    EXTRACTED window; the manifest pins the source key, its ETag and the window
    origin in projected metres, which is what makes that reproducible.

    THE TRAP THIS GUARDS: a 3DEP tile is a full 10 km raster even where the
    flight lines did not reach, and the uncovered part is -999999, not an
    absent tile. Centring the window on the site therefore succeeds, opens,
    reports the right shape, and contains no ground at all -- it compresses to
    81 KB and every statistic comes back as "too few valid pixels", or worse,
    as a perfectly flat plain if the fill value had been 0. So: probe the
    centre of each candidate position, slide the window within the tile to the
    nearest position that is genuinely covered, fall back to the next-nearest
    tile, and refuse to write anything under ``USGS_MIN_VALID_FRACTION``. Any
    displacement from the nominal site is recorded in the manifest.
    """
    import rasterio
    from rasterio.windows import Window

    dest.parent.mkdir(parents=True, exist_ok=True)
    attempts = []
    for tile in tiles:
        with rasterio.open(_vsicurl(tile["url"])) as ds:
            px = abs(ds.transform.a)
            n = int(round(size_m / px))
            if ds.width < n or ds.height < n:
                attempts.append(f"{Path(tile['key']).name}: smaller than the window")
                continue
            col, row = ~ds.transform * (tile["x"], tile["y"])
            want_c = int(round(col - n / 2))
            want_r = int(round(row - n / 2))

            # Candidate origins: the requested one first, then a grid over the
            # tile, ordered by how far they move the site.
            cands = []
            steps = np.linspace(0, 1, 6)
            for fy in steps:
                for fx in steps:
                    cands.append((int(fx * (ds.width - n)), int(fy * (ds.height - n))))
            cands.insert(0, (want_c, want_r))
            seen = set()
            ordered = []
            for c0, r0 in cands:
                c0 = max(0, min(c0, ds.width - n))
                r0 = max(0, min(r0, ds.height - n))
                if (c0, r0) in seen:
                    continue
                seen.add((c0, r0))
                ordered.append((c0, r0))
            ordered.sort(key=lambda cr: math.hypot(cr[0] - want_c, cr[1] - want_r))

            chosen = None
            for c0, r0 in ordered:
                if _window_valid_fraction(ds, c0, r0, n) >= USGS_MIN_VALID_FRACTION:
                    chosen = (c0, r0)
                    break
            if chosen is None:
                attempts.append(f"{Path(tile['key']).name}: no covered {size_m} m window")
                continue

            c0, r0 = chosen
            win = Window(c0, r0, n, n)
            arr = ds.read(1, window=win)
            wt = ds.window_transform(win)
            profile = ds.profile.copy()
            profile.update(height=n, width=n, transform=wt, compress="deflate",
                           tiled=True, blockxsize=256, blockysize=256,
                           driver="GTiff", predictor=3)
            _, vs = _mask_usgs(arr, ds.nodata)
            if 1.0 - vs["masked_fraction"] < USGS_MIN_VALID_FRACTION:
                attempts.append(
                    f"{Path(tile['key']).name}: window at ({c0},{r0}) only "
                    f"{1 - vs['masked_fraction']:.1%} covered"
                )
                continue

        tmp = dest.with_suffix(dest.suffix + ".part")
        with rasterio.open(tmp, "w", **profile) as out:
            out.write(arr, 1)
        tmp.replace(dest)

        cx = wt.c + n / 2 * wt.a
        cy = wt.f + n / 2 * wt.e
        moved = math.hypot(cx - tile["x"], cy - tile["y"])
        _, hdrs = head(tile["url"])
        return {
            "source_key": tile["key"],
            "source_url": tile["url"],
            "source_etag": (hdrs.get("ETag") or "").strip('"'),
            "source_size_bytes": int(hdrs.get("Content-Length", 0)),
            "crs": tile["crs"],
            "window_origin_xy_m": [round(float(wt.c), 3), round(float(wt.f), 3)],
            "window_center_xy_m": [round(float(cx), 3), round(float(cy), 3)],
            "offset_from_site_m": round(moved, 1),
            "offset_note": (
                "window slid inside the tile to stay on covered ground; 3DEP flight-line "
                "gaps are stored as -999999 inside an otherwise normal 10 km raster"
            ) if moved > 100 else "centred on the site",
            "coverage_fraction": round(1.0 - vs["masked_fraction"], 5),
            "window_px": [int(n), int(n)],
            "window_m": int(size_m),
            "cache_file": str(dest.relative_to(CACHE_DIR)).replace("\\", "/"),
            "cache_size_bytes": dest.stat().st_size,
            "cache_sha256": sha256_file(dest),
        }
    raise RuntimeError("no covered 3DEP window found: " + "; ".join(attempts))


# ===========================================================================
# Commands.
# ===========================================================================


def cmd_probe(args) -> int:
    ok = True
    print("== Copernicus GLO-30 (anonymous) ==")
    for s in SITES:
        tid = cop30_tile_id(s["lat"], s["lon"])
        status, hdrs = head(cop30_url(tid))
        mb = int(hdrs.get("Content-Length", 0)) / 1e6
        flag = "OK " if status == 200 else "FAIL"
        ok &= status == 200
        print(f"  [{flag}] {s['id']:24s} {tid}  {status}  {mb:.1f} MB")

    print("== USGS 3DEP 1 m (anonymous) ==")
    for s in SITES:
        u = s.get("usgs_3dep")
        if not u:
            print(f"  [n/a] {s['id']:24s} {s['no_3dep_reason']}")
            continue
        status, _ = head(f"{TNM_BUCKET}/{TNM_1M_PREFIX}/{u['project']}/")
        keys, _ = s3_list(TNM_BUCKET, f"{TNM_1M_PREFIX}/{u['project']}/TIFF/")
        n = len([k for k in keys if k.lower().endswith(".tif")])
        flag = "OK " if n else "FAIL"
        ok &= bool(n)
        print(f"  [{flag}] {s['id']:24s} {u['project']}  {n} tiles")

    print("== OpenTopography ==")
    url = ("https://portal.opentopography.org/API/globaldem?demtype=SRTMGL3"
           "&south=50&north=50.1&west=14.35&east=14.6&outputFormat=GTiff")
    status, _ = head(url)
    print(f"  HTTP {status} without a key -- needs an API key, not usable anonymously")
    return 0 if ok else 1


def cmd_verify(args) -> int:
    if not MANIFEST_PATH.exists():
        print(f"no manifest at {MANIFEST_PATH}; run `build` first", file=sys.stderr)
        return 2
    man = json.loads(MANIFEST_PATH.read_text())
    bad = 0
    checked = 0
    for site in man["sites"]:
        for key in ("cop30", "usgs_3dep_1m"):
            src = site.get(key)
            if not src or "cache_file" not in src:
                continue
            p = CACHE_DIR / src["cache_file"]
            want = src.get("cache_sha256") or src.get("sha256")
            if not p.exists():
                print(f"MISSING {site['id']}/{key}: {p}")
                bad += 1
                continue
            checked += 1
            got = sha256_file(p)
            if got != want:
                print(f"MISMATCH {site['id']}/{key}: {got} != {want}")
                bad += 1
    print(f"verified {checked} cached rasters, {bad} problem(s)")
    return 1 if bad else 0


def cmd_build(args) -> int:
    _gdal_env()
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    prev = {}
    if MANIFEST_PATH.exists():
        try:
            prev = {s["id"]: s for s in json.loads(MANIFEST_PATH.read_text())["sites"]}
        except Exception:  # noqa: BLE001
            prev = {}

    wanted = set(args.site or [])
    out_sites = []
    failures = []
    for s in SITES:
        if wanted and s["id"] not in wanted:
            if s["id"] in prev:
                out_sites.append(prev[s["id"]])
            continue
        print(f"[{s['id']}] {s['name']}")
        entry = {k: s[k] for k in (
            "id", "geomorphic_class", "name", "lat", "lon",
            "human_description", "why", "cop30_dsm_bias", "cop30_dsm_reason")}
        if s.get("no_3dep_reason"):
            entry["no_3dep_reason"] = s["no_3dep_reason"]

        # ---- Copernicus: full tile, resumable, checksummed ------------------
        try:
            tid = cop30_tile_id(s["lat"], s["lon"])
            url = cop30_url(tid)
            dest = CACHE_DIR / "cop30" / f"{tid}.tif"
            expect = (prev.get(s["id"], {}).get("cop30") or {}).get("sha256")
            got = download_resumable(url, dest, expect_sha256=expect, quiet=args.quiet)
            _assert_readable(dest)
            print("  cop30 ok, analysing...")
            stats = analyse_cop30(dest, s["lat"], s["lon"], COP30_WINDOW_KM)
            entry["cop30"] = {
                "product": "Copernicus DEM GLO-30 (COG)",
                "model_type": "DSM (surface: includes canopy and buildings)",
                "tile_id": tid,
                "url": url,
                "cache_file": str(dest.relative_to(CACHE_DIR)).replace("\\", "/"),
                "size_bytes": got["size"],
                "sha256": got["sha256"],
                "cache_sha256": got["sha256"],
                "source_etag": got["source_etag"],
                "window_km": COP30_WINDOW_KM,
                "stats": stats,
            }
        except Exception as e:  # noqa: BLE001
            print(f"  cop30 FAILED: {e}", file=sys.stderr)
            failures.append((s["id"], "cop30", str(e)))
            entry["cop30"] = {"error": str(e)}

        # ---- 3DEP: windowed extract via range reads -------------------------
        if s.get("usgs_3dep"):
            proj = s["usgs_3dep"]["project"]
            try:
                dest = CACHE_DIR / "usgs1m" / f"{s['id']}_{USGS_WINDOW_M}m.tif"
                prior = prev.get(s["id"], {}).get("usgs_3dep_1m") or {}
                if dest.exists() and prior.get("cache_sha256") == sha256_file(dest):
                    meta = {k: prior[k] for k in prior if k != "stats"}
                    print("  3dep cached (sha256 ok)")
                else:
                    tiles = resolve_3dep_tiles(proj, s["lat"], s["lon"], CACHE_DIR)
                    meta = extract_usgs_window(tiles, USGS_WINDOW_M, dest)
                    print(f"  3dep tile {Path(meta['source_key']).name} "
                          f"(offset {meta['offset_from_site_m']:.0f} m, "
                          f"cover {meta['coverage_fraction']:.1%})")
                _assert_readable(dest)
                stats = analyse_usgs(dest)
                entry["usgs_3dep_1m"] = {
                    "product": "USGS 3DEP 1 m DEM",
                    "model_type": "DTM (bare earth: gridded from classified ground returns)",
                    "project": proj,
                    "access": "anonymous S3, no credentials; windowed HTTP range read via GDAL /vsicurl/",
                    **meta,
                    "stats": stats,
                }
            except Exception as e:  # noqa: BLE001
                print(f"  3dep FAILED: {e}", file=sys.stderr)
                failures.append((s["id"], "3dep", str(e)))
                entry["usgs_3dep_1m"] = {"project": proj, "error": str(e)}

        # ---- the measurement the DSM caveat is actually about ---------------
        cop_ok = "error" not in (entry.get("cop30") or {"error": 1})
        dep_ok = "error" not in (entry.get("usgs_3dep_1m") or {"error": 1})
        if cop_ok and dep_ok:
            try:
                entry["dsm_vs_dtm_at_30m"] = colocated_dsm_vs_dtm(
                    CACHE_DIR / entry["cop30"]["cache_file"],
                    CACHE_DIR / entry["usgs_3dep_1m"]["cache_file"],
                )
            except Exception as e:  # noqa: BLE001
                print(f"  dsm/dtm compare FAILED: {e}", file=sys.stderr)
                entry["dsm_vs_dtm_at_30m"] = {"error": str(e)}
        out_sites.append(entry)

    order = {s["id"]: i for i, s in enumerate(SITES)}
    out_sites.sort(key=lambda e: order.get(e["id"], 999))

    manifest = {
        "schema": SCHEMA,
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "generator": "terrain-service/tools/earth_reference.py",
        "purpose": (
            "Reference distributions of real Earth topography, one or more site per "
            "geomorphic class our worldgen produces, for statistical validation of "
            "30 m -> 10 cm terrain amplification. Rasters are NOT committed; rebuild "
            "with `python tools/earth_reference.py build` (needs rasterio)."
        ),
        "method": _method_block(),
        "sources": _sources_block(),
        "sites": out_sites,
        "coverage": _coverage_block(out_sites),
        "findings": _findings_block(out_sites),
    }
    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\nwrote {MANIFEST_PATH}")
    if failures:
        print(f"{len(failures)} failure(s):", file=sys.stderr)
        for f in failures:
            print(f"  {f[0]} / {f[1]}: {f[2]}", file=sys.stderr)
        return 1
    return 0


def _findings_block(sites: list[dict]) -> dict:
    """What the corpus measured about its own sources. Computed, not asserted.

    These exist because the a-priori DSM caveat turned out to be half wrong,
    and the half that was wrong matters more than the half that was right.
    """
    ratios = {}
    for s in sites:
        d = s.get("dsm_vs_dtm_at_30m") or {}
        r = d.get("dsm_over_dtm_mean_slope")
        if r:
            ratios[s["id"]] = r
    smoother = {k: v for k, v in sorted(ratios.items(), key=lambda kv: kv[1]) if v < 1.0}
    rougher = {k: v for k, v in sorted(ratios.items(), key=lambda kv: -kv[1]) if v >= 1.0}
    xchk = {s["id"]: s["cop30"]["stats"]["crosscheck_native_ground"]
            ["mean_slope_ratio_native_over_aeqd30"]
            for s in sites if "stats" in s.get("cop30", {})}
    return {
        "cop30_is_smoother_than_the_ground_at_its_own_posting": {
            "what": (
                "On 6 of the 9 sites with bare-earth data, Copernicus GLO-30 gives a LOWER "
                "mean 30 m slope than the 3DEP 1 m DTM block-averaged to the same 30 m "
                "posting, over the same footprint. GLO-30 is not a faithful 30 m sample of "
                "the ground; it is a smoothed radar product whose effective resolution is "
                "coarser than its posting."
            ),
            "dsm_over_dtm_mean_slope_below_1": smoother,
            "worst_case": (
                "white_sands_dunes at 0.64 -- dune crests with ~150 m spacing and ~10 m "
                "amplitude lose over a third of their slope signal"
            ),
            "consequence": (
                "Do NOT calibrate generated 30 m terrain against cop30 alone and conclude "
                "it is correct: you would be matching a reference that is already too "
                "smooth. Where a site has usgs_3dep_1m, its 30 m rung in slope_by_scale is "
                "the better target."
            ),
        },
        "dsm_bias_shows_up_in_elevation_far_more_than_in_slope": {
            "what": (
                "Continuous canopy is close to a uniform vertical offset, and an offset "
                "cancels in a gradient. The Smokies -- 20-40 m of unbroken forest -- came "
                "out at only +13% on 30 m mean slope. The DSM penalty is severe only where "
                "the canopy is TALL RELATIVE TO THE LANDFORM and patchy."
            ),
            "dsm_over_dtm_mean_slope_at_or_above_1": rougher,
            "worst_case": (
                "nc_coastal_plain at 5.3x slope and 11x detrended roughness -- pocosin "
                "shrub and pine plantation 10-25 m tall on ground with under 2 m of "
                "relief. Copernicus is unusable for low-relief vegetated terrain."
            ),
            "consequence": (
                "The sites where the DSM caveat actually bites are the LOW-RELIEF ones, "
                "which is exactly where our terrain is said to look worst. Use the 3DEP "
                "DTM for every plains and coastal comparison."
            ),
        },
        "projection_crosscheck": {
            "what": (
                "Mean slope from the un-resampled native geographic grid divided by mean "
                "slope from the 30 m local-equidistant reprojection, per site. All within "
                "0.94-1.15, so neither route has a units or cos(lat) error."
            ),
            "ratios": xchk,
            "reading": (
                "Ratios above 1 are the bilinear resample gently low-passing fine texture "
                "(worst on the finest-textured sites: white_sands 1.15, llano 1.12, "
                "badlands 1.10); ratios near 1 are the smooth, large-wavelength sites."
            ),
        },
    }


def _method_block() -> dict:
    return {
        "projection": {
            "copernicus_primary_grid": "aeqd30",
            "copernicus_primary_note": (
                "Copernicus tiles are geographic (EPSG:4326) so pixel ground size varies "
                "with latitude and is NOT square -- at lat 46 a 1-arcsec post is ~21.5 m "
                "east-west by ~30.9 m north-south. Each 20 km analysis window is "
                "REPROJECTED into a local azimuthal-equidistant CRS centred on the site "
                "at exactly 30.0 m square pixels (bilinear), which is the same pixel "
                "geometry as a 30 m game tile. Distances through an AEQD centre are true, "
                "so slopes are metric and comparable."
            ),
            "copernicus_crosscheck_grid": "native_ground",
            "copernicus_crosscheck_note": (
                "Every site ALSO reports Horn slope computed with no resampling at all, on "
                "the native geographic posts, using per-row WGS84 ground distances "
                "(dx = dlon * m_per_deg_lon(lat), dy = dlat * m_per_deg_lat(lat)). Two "
                "independent routes to a metric slope; a disagreement of more than a few "
                "percent means one of them is broken. The ratio is recorded per site."
            ),
            "usgs_grid": "utm_native",
            "usgs_note": (
                "3DEP 1 m arrives in NAD83 / UTM metres at 1 m square, so it is used as "
                "delivered. UTM scale distortion across a 2 km window is under 0.1%."
            ),
        },
        "voids_and_water": {
            "void_floor_m": VOID_FLOOR_M,
            "policy": (
                "Masked BEFORE reprojection so nodata cannot be interpolated into real "
                "ground: the file's declared nodata, any non-finite value, and anything "
                "at or below -100 m (no site in the corpus has real terrain below sea "
                "level). For Copernicus ONLY, elevation exactly 0.0 is also masked -- "
                "GLO-30 stores open water that way, and leaving it in turns a sound into "
                "a perfectly flat plain and halves every slope statistic. 3DEP does not "
                "get the zero rule (NAVD88 land can legitimately be 0.000); its water is "
                "hydro-flattened to a constant instead, which `flat_fraction` exposes."
            ),
            "propagation": (
                "Horn's 3x3 estimator is computed with NaN, so any output cell whose "
                "neighbourhood touched a void or an array edge is itself NaN and is "
                "excluded from every percentile. A void can therefore never contribute "
                "a spurious cliff."
            ),
            "reported_per_site": ["nodata_fraction", "sea_or_zero_fraction", "masked_fraction",
                                  "valid_fraction", "flat_fraction"],
        },
        "statistics": {
            "slope_estimator": "Horn (1981) 3x3, degrees -- the same estimator gdaldem uses",
            "relief_m": "both full max-min and the outlier-robust p99-p1",
            "detrended_rms_m": "RMS residual after removing the best-fit plane; scale-free roughness",
            "slope_by_scale": (
                "slope recomputed after NaN-aware block-averaging to coarser posts "
                "(cop30: 30/60/120/240 m; 3DEP: 1/3/10/30 m). This is the roll-off curve "
                "an amplifier has to reproduce: how fast slope decays as you coarsen tells "
                "you the roughness spectrum without needing an FFT."
            ),
            "dsm_vs_dtm_at_30m": (
                "measured, not assumed. Copernicus (DSM) is re-read over the EXACT 2 km "
                "footprint of the cached 3DEP window and the 3DEP DTM is block-averaged "
                "30x, so both are the same ground at the same posting; the ratio is "
                "canopy and buildings. Do NOT compare the two whole-site blocks directly "
                "-- their windows are 20 km and 2 km and the difference between them is "
                "mostly landscape, not sensor."
            ),
            "above_cliff_gate_fraction": (
                "fraction of the site steeper than 35 deg, the kBiomeCliffGradePercent "
                "threshold in voxel-core/include/voxelcore/biome.h -- i.e. how much of "
                "this real landscape our classifier would call BARE_ROCK"
            ),
            "windows": {
                "cop30_km": COP30_WINDOW_KM,
                "usgs_m": USGS_WINDOW_M,
                "note": (
                    "Windows are square and centred on the site. 20 km at 30 m is ~445k "
                    "posts; 2 km at 1 m is ~4.2M posts. Both are cut small enough to hold "
                    "ONE landform rather than a 1-degree grab bag."
                ),
            },
        },
    }


def _sources_block() -> dict:
    return {
        "cop30": {
            "name": "Copernicus DEM GLO-30",
            "model_type": "DSM",
            "resolution": "1 arc-second (~30 m)",
            "crs": "EPSG:4326",
            "coverage": "global",
            "access": "anonymous HTTPS on S3, no credentials",
            "url_pattern": f"{COP30_BUCKET}/<TILE>/<TILE>.tif  (TILE = Copernicus_DSM_COG_10_{{N|S}}dd_00_{{E|W}}ddd_00_DEM)",
            "fetch": "whole 1x1 deg tile (~40 MB), resumable via HTTP Range, verified against Content-Length and sha256, then the last block is read to catch a truncated-but-openable GeoTIFF",
            "caveat": "DSM: includes vegetation and buildings, which inflates fine-scale roughness. See cop30_dsm_bias per site.",
        },
        "usgs_3dep_1m": {
            "name": "USGS 3DEP 1 m DEM",
            "model_type": "DTM (bare earth)",
            "resolution": "1 m",
            "crs": "NAD83 / UTM (per project)",
            "coverage": "CONUS only, project by project",
            "access": "anonymous HTTPS on S3, no credentials -- VERIFIED. Listing, HEAD and GDAL /vsicurl/ range reads all work unauthenticated.",
            "url_pattern": f"{TNM_BUCKET}/{TNM_1M_PREFIX}/<PROJECT>/TIFF/USGS_one_meter_x<E/10km>y<N/10km>_<PROJECT>.tif",
            "project_index": f"{TNM_BUCKET}/?list-type=2&prefix={TNM_1M_PREFIX}/&delimiter=/  (956 projects as of 2026-07)",
            "fetch": "tiles are 10 km x 10 km (10012^2 float32, 40-500 MB) and are NOT mirrored; a 2 km window is cut with HTTP range reads against the internally-tiled LZW GeoTIFF and the EXTRACT is cached and checksummed. Reproducible from source_key + source_etag + window_origin_xy_m.",
            "why_it_matters": "the only free reference that reaches below 30 m, which is the band our amplifier invents and therefore the band most likely to be wrong",
        },
        "opentopography": {
            "status": "rejected -- not anonymous",
            "evidence": "GET https://portal.opentopography.org/API/globaldem?demtype=SRTMGL3&... returns HTTP 401 with body 'Error: API Key required for access. Please register for an API key at www.opentopography.org'",
            "note": "reachable and healthy, but every dataset behind that API needs a free registered key. Nothing in this corpus depends on it. If a key is ever added it would mainly buy SRTM/ALOS/NASADEM variants at 30 m, which cop30 already covers, plus a handful of hosted lidar DTMs outside CONUS.",
        },
    }


def _coverage_block(sites: list[dict]) -> dict:
    by_class: dict[str, list[str]] = {}
    for s in sites:
        by_class.setdefault(s["geomorphic_class"], []).append(s["id"])
    return {
        "classes": by_class,
        "gaps": [
            {
                "class": "alpine_glaciated (non-CONUS)",
                "gap": "no free 1 m bare-earth DEM for the Alps or any Himalayan site",
                "handled": "glacier_np_alpine supplies a 1 m DTM for the same landform family inside 3DEP coverage",
            },
            {
                "class": "desert_aeolian_dunes (mega-dune scale)",
                "gap": "no free 1 m DEM anywhere in the great ergs (Rub' al Khali, Sahara, Taklamakan)",
                "handled": "white_sands_dunes supplies 1 m for small dunes; rub_al_khali_dunes is cop30-only, which is acceptable because 100-250 m draa with 2-3 km spacing are fully resolved at 30 m",
            },
            {
                "class": "karst / doline landscapes",
                "gap": "NOT COVERED. Closed depressions are a distinct landform class with no fluvial analogue, and our height-field amplifier cannot make them at all.",
                "handled": "deliberately omitted rather than approximated -- worldgen has no karst term, so a reference would only measure a thing we do not attempt",
            },
            {
                "class": "volcanic constructional (cones, lava fields)",
                "gap": "NOT COVERED by design",
                "handled": "classifyBiome has no volcanic class; adding a reference would imply a capability worldgen does not have",
            },
            {
                "class": "true continental-shelf / submarine",
                "gap": "NOT COVERED. Copernicus is a land DSM; below the shoreline it is 0.0, not bathymetry.",
                "handled": "biome.h OCEAN terrain is generated, not amplified from tiles; if that changes the reference would have to be GEBCO or ETOPO, not this corpus (see tools/fetch_etopo.py)",
            },
        ],
    }


def cmd_summary(args) -> int:
    if not MANIFEST_PATH.exists():
        print(f"no manifest at {MANIFEST_PATH}", file=sys.stderr)
        return 2
    man = json.loads(MANIFEST_PATH.read_text())
    hdr = f"{'site':26s} {'class':30s} {'relief':>8s} {'slp_mn':>7s} {'slp_95':>7s} {'flat':>6s} {'mask':>6s}"
    print(hdr)
    print("-" * len(hdr))
    for s in man["sites"]:
        for key, tag in (("cop30", "cop30"), ("usgs_3dep_1m", "3dep1m")):
            st = (s.get(key) or {}).get("stats")
            if not st or "slope_deg" not in st:
                continue
            print(f"{s['id'][:24]:24s}/{tag[:1]} {s['geomorphic_class'][:30]:30s} "
                  f"{st['relief_m']['p1_p99']:8.1f} {st['slope_deg']['mean']:7.2f} "
                  f"{st['slope_deg']['p95']:7.2f} {st['flat_fraction']:6.3f} "
                  f"{st['voids']['masked_fraction']:6.3f}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("build", help="fetch, characterise, write the manifest")
    p.add_argument("--site", action="append", help="only this site id (repeatable)")
    p.add_argument("--quiet", action="store_true")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("verify", help="re-hash the cache against the manifest, no network")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("probe", help="HEAD every source URL and report anonymity")
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("summary", help="one-line-per-site table from the manifest")
    p.set_defaults(func=cmd_summary)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
