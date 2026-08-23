"""What each material looks like. GENERATED — do not edit.

Source: voxel-core/include/voxelcore/materialpalette.h
Regenerate: python tools/gen_palette.py

One flat colour per voxel face, varied per voxel. See ADR-0008 for why
this is not a texture and why the variation has two frequencies, and
ADR-0009 for the third scale (biome_tint) and the metric patch unit.

THE TABLE IS HERE, THE EVALUATION IS NOT. How these numbers turn into a
colour is voxelcore/materialcolor.h, mirrored in forge/render.py.
"""

# id: (top, side, bottom, voxel_jitter, voxel_hue, patch_strength,
#      patch_scale_dm, biome_tint)
# Colours are sRGB 0-255. voxel_jitter, voxel_hue, patch_strength and
# biome_tint are 1/255ths; patch_scale_dm is a WORLD wavelength in
# decimetres (10 cm), which is one level-0 voxel and half a detail-grid
# one -- it is not counted in whatever cells the grid at hand happens
# to use.
PALETTE = {
     0: ((0, 0, 0), (0, 0, 0), (0, 0, 0), 0, 0, 0, 0, 0),  # MAT_AIR
     1: ((70, 69, 74), (70, 69, 74), (63, 62, 67), 28, 7, 34, 24, 0),  # MAT_BEDROCK
     2: ((132, 126, 108), (125, 119, 102), (114, 109, 94), 42, 12, 48, 20, 0),  # MAT_ROCK
     3: ((150, 139, 119), (144, 133, 114), (134, 124, 107), 52, 14, 40, 12, 0),  # MAT_GRAVEL
     4: ((206, 186, 140), (200, 181, 136), (188, 170, 128), 30, 12, 34, 16, 0),  # MAT_SAND
     5: ((120, 96, 72), (115, 92, 69), (106, 85, 64), 34, 14, 40, 18, 0),  # MAT_SUBSOIL
     6: ((96, 74, 52), (90, 70, 49), (82, 63, 44), 38, 16, 46, 16, 0),  # MAT_TOPSOIL
     7: ((243, 246, 251), (238, 241, 247), (228, 232, 240), 10, 4, 16, 28, 0),  # MAT_SNOW
     8: ((76, 116, 54), (66, 96, 50), (88, 67, 48), 46, 34, 54, 14, 0),  # MAT_GRASS
     9: ((104, 58, 38), (98, 55, 36), (88, 49, 32), 36, 18, 44, 16, 0),  # MAT_JUNGLE_SOIL
    10: ((170, 158, 90), (154, 142, 82), (104, 84, 54), 44, 30, 52, 14, 0),  # MAT_SAVANNA_GRASS
    11: ((82, 78, 70), (78, 74, 66), (70, 66, 60), 34, 10, 42, 18, 0),  # MAT_PODZOL
    12: ((170, 180, 188), (164, 174, 182), (152, 162, 172), 26, 10, 32, 22, 0),  # MAT_PERMAFROST
    13: ((62, 66, 58), (59, 63, 55), (53, 57, 50), 28, 10, 38, 18, 0),  # MAT_MUD
    14: ((152, 118, 94), (146, 113, 90), (136, 105, 84), 24, 12, 34, 20, 0),  # MAT_CLAY
    15: ((255, 0, 255), (255, 0, 255), (255, 0, 255), 0, 0, 0, 0, 0),  # MAT_WATERMARK
    16: ((150, 112, 74), (86, 65, 47), (150, 112, 74), 30, 14, 40, 10, 0),  # MAT_BARK
    17: ((158, 118, 78), (152, 113, 75), (150, 112, 74), 22, 10, 28, 8, 0),  # MAT_HEARTWOOD
    18: ((138, 126, 106), (132, 120, 101), (124, 113, 95), 34, 12, 40, 10, 0),  # MAT_DEADWOOD
    19: ((78, 120, 54), (72, 111, 50), (62, 96, 44), 46, 40, 68, 12, 0),  # MAT_LEAF_BROADLEAF
    20: ((54, 88, 64), (50, 82, 59), (43, 71, 51), 44, 32, 62, 12, 0),  # MAT_LEAF_NEEDLE
    21: ((58, 112, 50), (53, 103, 46), (45, 88, 39), 48, 42, 70, 14, 0),  # MAT_LEAF_JUNGLE
    22: ((142, 134, 82), (134, 126, 77), (118, 111, 68), 46, 36, 64, 12, 0),  # MAT_LEAF_DRY
    23: ((176, 168, 152), (198, 194, 181), (176, 168, 152), 26, 10, 44, 12, 0),  # MAT_BARK_PALE
    24: ((234, 190, 200), (228, 182, 193), (214, 168, 180), 34, 30, 50, 12, 0),  # MAT_LEAF_BLOSSOM
    25: ((192, 118, 50), (184, 112, 47), (168, 101, 42), 50, 48, 68, 12, 0),  # MAT_LEAF_AUTUMN
    26: ((46, 48, 56), (44, 46, 54), (40, 42, 50), 12, 4, 10, 8, 0),  # MAT_SKIN_DARK
    27: ((232, 226, 212), (226, 220, 206), (216, 210, 198), 12, 5, 10, 8, 0),  # MAT_SKIN_PALE
    28: ((176, 186, 196), (186, 196, 206), (196, 204, 212), 16, 6, 12, 6, 0),  # MAT_SKIN_SILVER
    29: ((86, 96, 54), (82, 92, 52), (76, 85, 48), 14, 8, 12, 8, 0),  # MAT_SKIN_OLIVE
    30: ((110, 82, 52), (105, 78, 50), (97, 72, 46), 16, 8, 14, 8, 0),  # MAT_SKIN_BROWN
    31: ((226, 118, 34), (218, 113, 32), (204, 105, 30), 14, 8, 10, 8, 0),  # MAT_SKIN_ORANGE
    32: ((232, 194, 54), (224, 187, 52), (210, 175, 48), 14, 8, 10, 8, 0),  # MAT_SKIN_YELLOW
    33: ((170, 46, 40), (164, 44, 38), (152, 41, 35), 14, 8, 12, 8, 0),  # MAT_SKIN_RED
    34: ((46, 96, 168), (44, 92, 162), (40, 85, 150), 14, 6, 12, 8, 0),  # MAT_SKIN_BLUE
    35: ((58, 148, 92), (56, 142, 88), (51, 131, 81), 14, 8, 12, 8, 0),  # MAT_SKIN_GREEN
    36: ((246, 246, 242), (240, 240, 236), (230, 230, 226), 10, 4, 8, 8, 0),  # MAT_PLUME_WHITE
    37: ((150, 156, 164), (145, 151, 159), (134, 140, 147), 14, 6, 12, 8, 0),  # MAT_PLUME_GREY
    38: ((78, 92, 112), (75, 88, 107), (69, 81, 99), 14, 6, 12, 8, 0),  # MAT_PLUME_SLATE
    39: ((208, 176, 118), (201, 170, 114), (186, 158, 106), 16, 8, 14, 8, 0),  # MAT_PLUME_BUFF
    40: ((180, 84, 36), (174, 81, 35), (161, 75, 32), 16, 8, 12, 8, 0),  # MAT_PLUME_RUFOUS
    41: ((208, 40, 56), (201, 39, 54), (186, 36, 50), 14, 6, 10, 8, 0),  # MAT_PLUME_CRIMSON
    42: ((152, 202, 70), (147, 195, 68), (136, 181, 63), 14, 8, 10, 8, 0),  # MAT_PLUME_LIME
    43: ((52, 184, 206), (50, 178, 199), (46, 165, 185), 14, 6, 10, 8, 0),  # MAT_PLUME_CYAN
    44: ((166, 132, 208), (160, 128, 201), (149, 118, 186), 14, 8, 12, 8, 0),  # MAT_PLUME_LILAC
    45: ((28, 78, 70), (34, 88, 80), (24, 68, 62), 16, 14, 16, 6, 0),  # MAT_PLUME_IRIDESCENT
    46: ((96, 88, 76), (93, 85, 74), (86, 79, 68), 12, 6, 10, 6, 0),  # MAT_BEAK_HORN
}

FACE_TOP, FACE_SIDE, FACE_BOTTOM = 0, 1, 2

MATERIAL_COUNT = 47

# Column indices into a PALETTE row, so a reader does not have to count.
TOP, SIDE, BOTTOM = 0, 1, 2
JITTER, HUE, PATCH, PATCH_SCALE_DM, BIOME_TINT = 3, 4, 5, 6, 7


def entry(mat: int):
    """Appearance for a material id, magenta if it has none."""
    return PALETTE.get(int(mat), (((255, 0, 255),) * 3) + (0, 0, 0, 0, 0))
