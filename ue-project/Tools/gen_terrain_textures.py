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
precision, so remapping p1..p99 onto 0..255 recovers the signal.

HISTORICAL NOTE, corrected at worldgen v8: this docstring used to add that the
same narrow window "lands the entire world below voxel-core biome.h's
kBiomePrecipAridU8 (60) -- which is why every land voxel currently classifies
DESERT -> MAT_SAND and the world renders as one flat beige". That was true and
is now fixed. biome.h states its thresholds physically and converts through
voxelcore/climate.h, so kBiomePrecipAridU8 is 9 (= 400 mm/yr) and the Whittaker
table spreads properly. The REMAP still earns its place -- it is a display
stretch that uses the full LUT axis for a narrow region -- but it is no longer
compensating for a broken classifier.

These two constants are duplicated in VoxelClimateProbe.h and MUST match it
exactly. Since v8 that header DERIVES them from climate.h's physical constants
(-8.6 C, +19.2 C, 659 mm/yr, 1506 mm/yr) and static_asserts that they still come
out 100/189/14/32, so the C++ side cannot drift silently. This copy is still a
copy: if that assert ever fires, change both files and regenerate the LUT in the
same commit.
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


BIOMELUT_PROBE = "vxc_biomelut"


def _classified_grid(size):
    """(size, size) of material ids, from the ENGINE's own biome classifier.

    Shells out to vxc_biomelut rather than porting classifyBiome to Python.
    That classifier is worldgen -- integer-only, and mirrored bit-for-bit in
    worldgen.ush under the CPU/GPU contract -- so a Python transcription would
    be a third copy of the biome gates, which is the failure this codebase has
    already paid for twice (material ids, then colour).
    """
    import shutil
    import subprocess

    candidates = [
        os.path.join(os.path.dirname(__file__), "..", "..", "build", "voxel-core",
                     "bench", BIOMELUT_PROBE),
        os.path.join(os.path.dirname(__file__), "..", "..", "build", "bench",
                     BIOMELUT_PROBE),
        shutil.which(BIOMELUT_PROBE) or "",
    ]
    exe = next((os.path.abspath(c) for c in candidates if c and os.path.exists(c)), None)
    if exe is None:
        raise SystemExit(
            f"{BIOMELUT_PROBE} not found. T_VoxelBiomeLUT is painted from the "
            "engine's own biome classifier (ADR-0009), so it has to be built:\n"
            "    cmake -S voxel-core -B build/voxel-core -G Ninja\n"
            "    cmake --build build/voxel-core --target vxc_biomelut")

    out = subprocess.run([exe, str(size)], capture_output=True, text=True, check=True)
    grid = np.zeros((size, size), dtype=np.int32)
    seen = 0
    for line in out.stdout.splitlines():
        if line.startswith("#"):
            continue
        u, v, _biome, mat = (int(t) for t in line.split())
        grid[v, u] = mat
        seen += 1
    if seen != size * size:
        raise SystemExit(f"{BIOMELUT_PROBE} returned {seen} cells, expected {size * size}")
    return grid


def _box_blur(img, radius):
    """Edge-clamped box blur, applied `radius` times at width 3.

    Repeated 3-wide passes rather than one wide kernel: it stays a couple of
    lines, it is separable and cheap at 64x64, and the result is close enough to
    a Gaussian for a texture whose job is to have no hard edges.
    """
    out = img.astype(np.float64)
    for _ in range(radius):
        for axis in (0, 1):
            padded = np.concatenate(
                [np.take(out, [0], axis=axis), out, np.take(out, [-1], axis=axis)],
                axis=axis)
            lo = np.take(padded, range(0, out.shape[axis]), axis=axis)
            mid = np.take(padded, range(1, out.shape[axis] + 1), axis=axis)
            hi = np.take(padded, range(2, out.shape[axis] + 2), axis=axis)
            out = (lo + mid + hi) / 3.0
    return out


def write_biome_lut(path, size=64, ecotone=3):
    """Whittaker-style biome diagram: U = precipitation, V = temperature.

    Both axes are ALREADY remapped to this world's p1..p99 (see module
    docstring), so u=0 is 659 mm/yr and u=1 is 1506 mm/yr; v=0 is -8.6 degC and
    v=1 is +19.3 degC. Every texel is therefore reachable by real data, which is
    the whole point of remapping rather than using raw u8.

    GENERATED FROM THE PALETTE SINCE ADR-0009, not authored. Each cell is
    classified by the engine's own classifyBiome, mapped through its own
    biomeSurfaceMaterial, and painted with that material's colour from
    kMaterialPalette. It used to interpolate six hand-picked corner anchors --
    "cold_dry = bare tundra, mild_wet = temperate broadleaf" and so on -- which
    was a second, independent answer to "what colour is a grassland".

    WHY THAT MATTERED ENOUGH TO CHANGE. ADR-0009 makes the NEAR FIELD purely
    material-led, and the clipmap keeps this LUT because a heightmap vertex has
    no material id to look one up with. Two authorities over one seam is exactly
    the arrangement terrain_material_common.py's header describes going wrong
    before -- "the vista was pale green, the ground was beige". Painted from the
    palette, the vista is the same colours smoothed across climate space, and a
    retune reaches both from one table.

    `ecotone` blurs the classified grid. Biome membership is a step function and
    the colours are discrete, so without it every biome boundary in the 50 km
    vista is a hard colour edge. Three passes over 64 texels is a soft band a few
    per cent of the climate range wide -- an ecotone, which is what the real
    thing looks like.
    """
    grid = _classified_grid(size)

    # Material id -> LINEAR albedo, from the generated palette table. PALETTE is
    # indexed by material id and its RGB column is generated from
    # materialpalette.h, so this cannot drift from what the near field draws.
    rgb = np.zeros((size, size, 3), dtype=np.float64)
    for mat in np.unique(grid):
        entry = PALETTE[int(mat)]
        rgb[grid == mat] = (entry[1], entry[2], entry[3])

    rgb = _box_blur(rgb, ecotone)

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
