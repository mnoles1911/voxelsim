"""Generate the terrain appearance textures as PNGs (system Python, numpy+Pillow).

Companion to import_terrain_textures.py, which imports these PNGs into UE and
sets the compression/mip/filter settings each one needs. They are split because
UE 5.8's bundled Python has neither numpy nor Pillow, and pure-Python fBm over
a 512x512 image is unreasonably slow.

Run:
    python ue-project/Tools/gen_terrain_textures.py
    <UnrealEditor-Cmd> <uproject> -run=pythonscript -script=ue-project/Tools/import_terrain_textures.py ...

Outputs (PNG, checked in under Content/Voxel/TextureSource/ so the .uasset is
always regenerable -- doctrine: no binary-only assets):

  T_VoxelPalette.png    16x1  RGBA  per-material-id albedo LUT, A = biome-tint weight
  T_VoxelBiomeLUT.png   64x64 RGBA  Whittaker diagram, U = precipitation, V = temperature
  T_VoxelDetail.png     512x512 RGB tiling fBm detail (R fine / G medium / B coarse)

WHY THESE AXES ARE REMAPPED
---------------------------
Measured over all 25 real diffusion tiles in
tile-cache/terrain-diffusion-unlabeled-3e11cf157a836c70/000000000135276f/s1
(6.55M land pixels, elevation > 0):

    channel        u8 p1..p99     physical (per terrain_service EXPECTED_CHANNELS)
    temperature    100 .. 189     -8.6 .. +19.3 degC   (median +3.3)
    precipitation   14 ..  32      659 .. 1506 mm/yr   (median 1271)
    seasonality     67 .. 140      bio_4 788 .. 1647
    precip_var      88 .. 151      CV 69 .. 118 %

Precipitation only occupies u8 13..33 because terrain-service quantizes bio_12
against a 0..12000 mm/yr full scale while this world is cool-temperate maritime
(611..1647 mm/yr). Consuming the raw u8 throws away ~92% of the available
precision AND lands the entire world below voxel-core biome.h's kBiomePrecipAridU8
(60) -- which is why every land voxel currently classifies DESERT -> MAT_SAND and
the world renders as one flat beige. Remapping p1..p99 onto 0..255 recovers the
signal. These two constants are duplicated in VoxelClimateProbe.h and MUST match
it exactly; that header documents this file as the other copy.
"""

import os

import numpy as np
from PIL import Image

# MUST MATCH VoxelClimateProbe.h's kTempU8Lo/Hi and kPrecipU8Lo/Hi exactly.
TEMP_U8_LO, TEMP_U8_HI = 100, 189
PRECIP_U8_LO, PRECIP_U8_HI = 14, 32

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Content", "Voxel", "TextureSource")

# Deterministic: the textures are content, and a texture that changes every time
# it is regenerated would make every screenshot comparison meaningless.
SEED = 20260722


# --- material palette -------------------------------------------------------
#
# Indexed by vxc::MaterialId (voxel-core/include/voxelcore/core.h). RGB is the
# material's own albedo in sRGB; A is how much the surface-biome colour takes
# over (255 = fully biome-driven, 0 = pure material albedo).
#
# The A column is what makes a cave look like rock while the hillside above it
# looks like grassland, from ONE material graph: subsurface strata (bedrock,
# rock, gravel, subsoil, mud, clay) are never biome-tinted, surface materials
# always are. It also makes this robust to voxel-core's classifier: today every
# land surface voxel is MAT_SAND, so the biome path effectively owns all outdoor
# appearance; if the classifier is later fixed to emit varied surface ids, this
# table is already correct for them.
PALETTE = [
    # (name,             R,    G,    B,    biome_tint)
    ("AIR",            0.00, 0.00, 0.00,   0),
    ("BEDROCK",        0.19, 0.18, 0.175,  0),
    ("ROCK",           0.38, 0.355, 0.325, 0),
    ("GRAVEL",         0.45, 0.42, 0.385,  0),
    ("SAND",           0.74, 0.66, 0.48, 255),
    ("SUBSOIL",        0.34, 0.26, 0.185,  0),
    ("TOPSOIL",        0.24, 0.175, 0.115, 255),
    ("SNOW",           0.90, 0.925, 0.96,  0),
    ("GRASS",          0.26, 0.38, 0.16, 255),
    ("JUNGLE_SOIL",    0.20, 0.30, 0.12, 255),
    ("SAVANNA_GRASS",  0.52, 0.46, 0.24, 255),
    ("PODZOL",         0.22, 0.20, 0.16, 255),
    ("PERMAFROST",     0.52, 0.51, 0.49, 255),
    ("MUD",            0.22, 0.19, 0.155, 0),
    ("CLAY",           0.44, 0.34, 0.26,  0),
    ("_RESERVED",      0.50, 0.45, 0.40,  0),
]


def write_palette(path):
    img = np.zeros((1, 16, 4), dtype=np.uint8)
    for i, (_name, r, g, b, a) in enumerate(PALETTE):
        img[0, i] = (
            int(round(r * 255)),
            int(round(g * 255)),
            int(round(b * 255)),
            int(a),
        )
    Image.fromarray(img, "RGBA").save(path)
    return img


# --- biome LUT --------------------------------------------------------------


def _mix(a, b, t):
    t = np.clip(t, 0.0, 1.0)[..., None]
    return np.asarray(a) * (1.0 - t) + np.asarray(b) * t


def write_biome_lut(path, size=64):
    """Whittaker-style biome diagram: U = precipitation, V = temperature.

    Both axes are ALREADY remapped to this world's p1..p99 (see module docstring),
    so u=0 is 659 mm/yr and u=1 is 1506 mm/yr; v=0 is -8.6 degC and v=1 is
    +19.3 degC. Every texel of this texture is therefore reachable by real data,
    which is the whole point of remapping rather than using raw u8.

    Colours are chosen for a cool-temperate maritime world, NOT the tropical/
    desert palette a naive biome table would use: nothing here is a sand desert,
    the dry end is golden steppe grass at ~660 mm/yr.
    """
    u = np.linspace(0.0, 1.0, size)[None, :].repeat(size, axis=0)  # precipitation
    v = np.linspace(0.0, 1.0, size)[:, None].repeat(size, axis=1)  # temperature

    # Corner anchors of the diagram (linear-ish sRGB values).
    cold_dry = (0.46, 0.44, 0.40)   # bare tundra / frost-shattered fellfield
    cold_wet = (0.40, 0.42, 0.42)   # wet tundra, mossy grey-green
    mild_dry = (0.52, 0.47, 0.26)   # golden steppe grass
    mild_wet = (0.22, 0.34, 0.16)   # temperate broadleaf forest
    warm_dry = (0.56, 0.50, 0.28)   # dry grassland
    warm_wet = (0.17, 0.36, 0.13)   # lush temperate rainforest

    cold = _mix(cold_dry, cold_wet, u)
    mild = _mix(mild_dry, mild_wet, u)
    warm = _mix(warm_dry, warm_wet, u)

    # Temperature is the dominant axis in this world (it spans a full 28 degC
    # while precipitation spans a modest 850 mm/yr), so it gets the two-segment
    # ramp. The cold->mild knee sits at v=0.38, which is ~+2 degC -- roughly the
    # treeline temperature, and close to the measured land median (+3.3 degC),
    # so half the land falls on each side and the map actually uses its range.
    knee = 0.38
    lower = _mix(cold, mild, v / knee)
    upper = _mix(mild, warm, (v - knee) / (1.0 - knee))
    rgb = np.where((v < knee)[..., None], lower, upper)

    # A taiga wedge: cold AND wet reads as dark conifer rather than grey tundra.
    # Without this the whole cold half is a flat grey and the uplands look dead.
    taiga = np.clip((0.52 - v) / 0.22, 0.0, 1.0) * np.clip((v - 0.16) / 0.16, 0.0, 1.0) * np.clip((u - 0.35) / 0.4, 0.0, 1.0)
    rgb = _mix(rgb, (0.15, 0.23, 0.16), taiga)

    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[..., :3] = np.clip(np.rint(rgb * 255.0), 0, 255).astype(np.uint8)
    out[..., 3] = 255
    Image.fromarray(out, "RGBA").save(path)
    return out


# --- tiling fBm detail ------------------------------------------------------


def _periodic_value_noise(rng, size, lattice):
    """Value noise that tiles exactly at `size`, on a `lattice`-cell grid."""
    grid = rng.random((lattice, lattice), dtype=np.float64)
    # Sample coordinates in lattice space; wrap indices so the result is periodic.
    t = np.arange(size, dtype=np.float64) * (lattice / size)
    i0 = np.floor(t).astype(np.int64) % lattice
    i1 = (i0 + 1) % lattice
    f = t - np.floor(t)
    f = f * f * (3.0 - 2.0 * f)  # smoothstep

    a = grid[np.ix_(i0, i0)]
    b = grid[np.ix_(i0, i1)]
    c = grid[np.ix_(i1, i0)]
    d = grid[np.ix_(i1, i1)]
    fx = f[None, :]
    fy = f[:, None]
    return (a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy)


def _fbm(rng, size, lattices):
    total = np.zeros((size, size), dtype=np.float64)
    norm = 0.0
    amp = 1.0
    for lat in lattices:
        total += amp * _periodic_value_noise(rng, size, lat)
        norm += amp
        amp *= 0.5
    out = total / norm
    # Normalize to full range so the 8-bit encoding isn't wasted.
    return (out - out.min()) / max(out.max() - out.min(), 1e-9)


def write_detail(path, size=512):
    """Tiling three-octave-band detail. LINEAR data, not sRGB.

    R = fine grain      (lattice 64..256) -- per-surface tooth
    G = medium mottle   (lattice 16..64)  -- patchiness within a biome
    B = coarse blotches (lattice  4..16)  -- large-scale colour break-up

    This texture is the whole reason the material has something to mip and
    anisotropically filter at all; M_VoxelTerrain/M_VoxelClipmap previously had
    no textures and no meaningful UVs, so mip/aniso had nothing to work on.
    """
    rng = np.random.default_rng(SEED)
    fine = _fbm(rng, size, [64, 128, 256])
    med = _fbm(rng, size, [16, 32, 64])
    coarse = _fbm(rng, size, [4, 8, 16])

    img = np.zeros((size, size, 3), dtype=np.uint8)
    for i, ch in enumerate((fine, med, coarse)):
        img[..., i] = np.clip(np.rint(ch * 255.0), 0, 255).astype(np.uint8)
    Image.fromarray(img, "RGB").save(path)
    return img


def main():
    out = os.path.normpath(OUT_DIR)
    os.makedirs(out, exist_ok=True)

    p = write_palette(os.path.join(out, "T_VoxelPalette.png"))
    print("T_VoxelPalette.png   16x1   materials=%d biome-tinted=%d"
          % (len(PALETTE), int((p[0, :, 3] > 0).sum())))

    b = write_biome_lut(os.path.join(out, "T_VoxelBiomeLUT.png"))
    print("T_VoxelBiomeLUT.png  %dx%d  temp u8 %d..%d, precip u8 %d..%d"
          % (b.shape[0], b.shape[1], TEMP_U8_LO, TEMP_U8_HI, PRECIP_U8_LO, PRECIP_U8_HI))

    d = write_detail(os.path.join(out, "T_VoxelDetail.png"))
    print("T_VoxelDetail.png    %dx%d  tiling fBm, mean=%.3f"
          % (d.shape[0], d.shape[1], d.mean() / 255.0))

    print("wrote to", out)


if __name__ == "__main__":
    main()
