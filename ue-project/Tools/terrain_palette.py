"""The per-material-id palette table. Single source of truth, NO dependencies.

Imported by BOTH gen_terrain_textures.py (which runs under system Python with
numpy/Pillow and bakes the RGB column into T_VoxelPalette.png) and
terrain_material_common.py (which runs under UE's bundled Python, has neither
numpy nor Pillow, and turns the BIOME_TINT column into material graph
arithmetic). Keeping it dependency-free is what lets one file serve both.

Indexed by vxc::MaterialId -- voxel-core/include/voxelcore/core.h. Those ids are
append-only (renumbering invalidates saved edit logs), so this table's order is
equally fixed.

RGB is LINEAR albedo. gen_terrain_textures.py sRGB-encodes it on the way into
the texture, because the texture is imported with srgb=True and UE decodes on
sample; skipping that encode was a real bug that made every colour come back
darker and more saturated than authored.

BIOME_TINT says whether the surface biome colour replaces this material's own
albedo:
  False -- subsurface strata. What you see when you dig or stand in a cave.
  True  -- a surface material. Takes the T_VoxelBiomeLUT colour for its column's
           climate.
This one column is what lets a single material graph make a cave read as rock
and the hillside above it read as grassland. It also makes the whole scheme
robust to voxel-core's classifier: on real diffusion tiles that classifier
currently labels every land surface voxel MAT_SAND (its precipitation thresholds
are calibrated for the synthetic sampler, not for WorldClim's 0..12000 mm/yr
quantization -- see VoxelClimateProbe.h), so today the biome path effectively
owns all outdoor appearance. If the classifier is later fixed to emit varied
surface ids, this table is already correct for them.
"""

# (name, linear R, G, B, biome_tint)
PALETTE = [
    ("AIR",            0.00, 0.00,  0.00,  False),
    ("BEDROCK",        0.19, 0.18,  0.175, False),
    ("ROCK",           0.38, 0.355, 0.325, False),
    ("GRAVEL",         0.45, 0.42,  0.385, False),
    ("SAND",           0.74, 0.66,  0.48,  True),
    ("SUBSOIL",        0.34, 0.26,  0.185, False),
    ("TOPSOIL",        0.24, 0.175, 0.115, True),
    ("SNOW",           0.90, 0.925, 0.96,  False),
    ("GRASS",          0.26, 0.38,  0.16,  True),
    ("JUNGLE_SOIL",    0.20, 0.30,  0.12,  True),
    ("SAVANNA_GRASS",  0.52, 0.46,  0.24,  True),
    ("PODZOL",         0.22, 0.20,  0.16,  True),
    ("PERMAFROST",     0.52, 0.51,  0.49,  True),
    ("MUD",            0.22, 0.19,  0.155, False),
    ("CLAY",           0.44, 0.34,  0.26,  False),
    # DEBUG INSTRUMENT, not world content -- vxc::MAT_WATERMARK. Solid voxels
    # standing where the bake says water is, so the water model can be judged at
    # full clipmap range instead of through the near-field renderer's 25.6 m
    # bubble. Only ever produced under -VoxelWaterMarker=1.
    #
    # BIOME_TINT IS FALSE AND THAT IS THE POINT: a tinted marker would take the
    # surrounding biome colour and stop being legible as a marker. Magenta
    # because nothing in the natural palette above is anywhere near it, so a
    # single glance separates "water goes here" from ground.
    ("WATERMARK",      0.90, 0.00,  0.90,  False),
]

PALETTE_WIDTH = 16
assert len(PALETTE) == PALETTE_WIDTH


def biome_tinted_runs():
    """Contiguous runs of biome-tinted ids, as [(first, last), ...].

    The material builds one band test per RUN rather than one per id, so the
    graph stays small (today: ids {4} and {6..12} -> two runs, ~8 nodes) while
    still being generated from this table instead of hardcoded in the shader.
    """
    runs = []
    for i, entry in enumerate(PALETTE):
        if not entry[4]:
            continue
        if runs and runs[-1][1] == i - 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    return [(a, b) for a, b in runs]
