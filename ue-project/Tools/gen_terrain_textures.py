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

from terrain_palette import PALETTE, PALETTE_WIDTH, biome_tinted_runs

# MUST MATCH VoxelClimateProbe.h's kTempU8Lo/Hi and kPrecipU8Lo/Hi exactly.
TEMP_U8_LO, TEMP_U8_HI = 100, 189
PRECIP_U8_LO, PRECIP_U8_HI = 14, 32

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Content", "Voxel", "TextureSource")

# Deterministic: the textures are content, and a texture that changes every time
# it is regenerated would make every screenshot comparison meaningless.
SEED = 20260722


# --- material palette -------------------------------------------------------
#
# The table itself now lives in terrain_palette.py, imported above: the material
# graph needs the same data (to decide which ids take the biome colour) and runs
# under UE's Python, which has no numpy/Pillow. One table, two consumers.


def linear_to_srgb(c):
    """Encode a LINEAR colour for storage in an sRGB texture.

    T_VoxelPalette and T_VoxelBiomeLUT are imported with srgb=True, so UE decodes
    sRGB->linear when sampling. The colours in terrain_palette.py and in the
    biome diagram below are authored as LINEAR albedo, so they must be
    sRGB-ENCODED on the way out or every colour comes back darker and more
    saturated than authored.

    That was a real, measured bug, not a theoretical one: the first render of
    this material came back with linear channel ratios (1, 0.71, 0.43) where the
    authored sand is (1, 0.89, 0.65) -- exactly the signature of a missing
    linear->sRGB encode, and the thing that identified which colour was actually
    reaching the screen.
    """
    c = np.clip(np.asarray(c, dtype=np.float64), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def write_palette(path):
    """16x1 RGB, LINEAR albedo sRGB-encoded for storage.

    The biome-tint weight is NOT in this texture. It briefly lived in the alpha
    channel and then in a second row; both read back as ~0 in the shader no
    matter how the texture was configured (verified 16x2, TF_NEAREST, TA_CLAMP,
    uncompressed, no mips -- and an unlit probe still showed 0.004 where the
    texel was 255). Rather than keep guessing at UE sampler semantics, the
    weight is now computed arithmetically from the material id in the material
    graph, generated from terrain_palette.biome_tinted_runs(). That is exact,
    costs one fewer sampler, and has no texture-format failure mode at all.
    """
    img = np.zeros((1, PALETTE_WIDTH, 3), dtype=np.uint8)
    for i, (_name, r, g, b, _tint) in enumerate(PALETTE):
        img[0, i] = np.rint(linear_to_srgb([r, g, b]) * 255.0).astype(np.uint8)
    Image.fromarray(img, "RGB").save(path)
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

    # Corner anchors of the diagram, LINEAR albedo.
    #
    # These are deliberately DARKER and more saturated than they "look right" in
    # isolation. The terrain is lit by a full-strength sun plus sky, and the
    # first tuning pass -- anchors around 0.5 luminance -- came back as a washed
    # pale olive on screen: a 0.5 linear albedo under this lighting tonemaps to
    # nearly white. Real vegetation albedo is 0.10-0.25, so these now sit in
    # that range and the greens survive the exposure.
    cold_dry = (0.34, 0.32, 0.29)   # bare tundra / frost-shattered fellfield
    cold_wet = (0.24, 0.27, 0.25)   # wet tundra, mossy grey-green
    mild_dry = (0.36, 0.31, 0.15)   # golden steppe grass
    mild_wet = (0.11, 0.21, 0.08)   # temperate broadleaf forest
    warm_dry = (0.40, 0.34, 0.17)   # dry grassland
    warm_wet = (0.08, 0.22, 0.07)   # lush temperate rainforest

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
    rgb = _mix(rgb, (0.07, 0.13, 0.08), taiga)

    # sRGB-encoded on the way out: the LUT is imported with srgb=True, so UE
    # decodes on sample. See linear_to_srgb's docstring.
    out = np.clip(np.rint(linear_to_srgb(rgb) * 255.0), 0, 255).astype(np.uint8)
    Image.fromarray(out, "RGB").save(path)
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
    print("T_VoxelPalette.png   16x1   materials=%d biome-tinted-runs=%s"
          % (len(PALETTE), biome_tinted_runs()))

    b = write_biome_lut(os.path.join(out, "T_VoxelBiomeLUT.png"))
    print("T_VoxelBiomeLUT.png  %dx%d  temp u8 %d..%d, precip u8 %d..%d"
          % (b.shape[0], b.shape[1], TEMP_U8_LO, TEMP_U8_HI, PRECIP_U8_LO, PRECIP_U8_HI))

    d = write_detail(os.path.join(out, "T_VoxelDetail.png"))
    print("T_VoxelDetail.png    %dx%d  tiling fBm, mean=%.3f"
          % (d.shape[0], d.shape[1], d.mean() / 255.0))

    print("wrote to", out)


if __name__ == "__main__":
    main()
