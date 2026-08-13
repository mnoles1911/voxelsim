"""The per-material-id palette table. NO dependencies, and half of it GENERATED.

Imported by BOTH gen_terrain_textures.py (which runs under system Python with
numpy/Pillow and bakes the RGB column into T_VoxelPalette.png) and
terrain_material_common.py (which runs under UE's bundled Python, has neither
numpy nor Pillow, and turns the BIOME_TINT column into material graph
arithmetic). Keeping it dependency-free is what lets one file serve both -- and
it is why the RGB column is written INTO this file by a generator rather than
computed here at import time.

THIS IS NO LONGER A SECOND PALETTE, and the header of this file used to claim it
was the first. `vxc::kMaterialPalette`
(voxel-core/include/voxelcore/materialpalette.h) is the one definition of what a
material looks like (ADR-0008). The RGB column below is generated from it by

    python ue-project/Tools/gen_material_palette_ush.py

and tools/compile-shaders.ps1 runs that generator with --check, so a hand edit
here fails CI with the line number. Edit the header, not this table.

BIOME_TINT IS STILL AUTHORED HERE, and that is the whole split. It is not a
statement about what a material looks like, it is a statement about who decides
its colour on the UE side, which is this project's policy and not the engine
core's business.

WHICH FACE THE RGB COMES FROM: the TOP face. This table is indexed by material
id alone with no face information -- it is what T_VoxelPalette holds, one texel
per id -- and terrain is looked at from above. ADR-0008's three face classes
exist precisely because one colour cannot serve all three, so where they differ
this column is lossy BY CONSTRUCTION: MAT_BARK's top face is the cut end grain
of a trunk, not the bark down its sides. Anything that needs a material's real
appearance should read the per-face path (VoxelMaterialPalette.ush, which
M_VoxelTerrain gets through TexCoords[3]/[4]), not this.

Indexed by vxc::MaterialId -- voxel-core/include/voxelcore/core.h. Those ids are
append-only (renumbering invalidates saved edit logs), so this table's order is
equally fixed. There is one entry per material, checked against the enum by the
generator, which mirrors the static_assert the C++ table carries: a material
added without an appearance is a failure rather than a hole nobody notices.

RGB is LINEAR albedo; the header authors sRGB and the generator converts.
gen_terrain_textures.py sRGB-ENCODES it again on the way into the texture,
because the texture is imported with srgb=True and UE decodes on sample;
skipping that encode was a real bug that made every colour come back darker and
more saturated than authored. The two conversions are exact inverses over all
26 x 3 authored channels (checked: every byte comes back the byte the header
authored), so what a designer picks is what the texture holds.

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
    ("AIR",               0.000000, 0.000000, 0.000000, False),
    ("BEDROCK",           0.057805, 0.057805, 0.068478, False),
    ("ROCK",              0.201556, 0.187821, 0.165132, False),
    ("GRAVEL",            0.266356, 0.238398, 0.198069, False),
    ("SAND",              0.686685, 0.577580, 0.323143, True),
    ("SUBSOIL",           0.187821, 0.116971, 0.064803, False),
    ("TOPSOIL",           0.107023, 0.061246, 0.031896, True),
    ("SNOW",              0.896269, 0.921582, 0.964686, False),
    ("GRASS",             0.122139, 0.270498, 0.042311, True),
    ("JUNGLE_SOIL",       0.088656, 0.051269, 0.025187, True),
    ("SAVANNA_GRASS",     0.401978, 0.341914, 0.102242, True),
    ("PODZOL",            0.076185, 0.051269, 0.036889, True),
    ("PERMAFROST",        0.401978, 0.456411, 0.502886, True),
    ("MUD",               0.068478, 0.048172, 0.031896, False),
    ("CLAY",              0.313989, 0.181164, 0.111932, False),
    # DEBUG INSTRUMENT, not world content -- vxc::MAT_WATERMARK. Solid voxels
    # standing where the bake says water is, so the water model can be judged at
    # full clipmap range instead of through the near-field renderer's 25.6 m
    # bubble. Only ever produced under -VoxelWaterMarker=1.
    #
    # BIOME_TINT IS FALSE AND THAT IS THE POINT: a tinted marker would take the
    # surrounding biome colour and stop being legible as a marker. Magenta
    # because nothing in the natural palette above is anywhere near it, so a
    # single glance separates "water goes here" from ground.
    ("WATERMARK",         1.000000, 0.000000, 1.000000, False),
    # --- ASSET MATERIALS ---------------------------------------------------
    # Wood and foliage from asset-forge. The terrain generator cannot emit any
    # of these; they arrive only from a baked asset, which is why core.h puts
    # them after everything the amplifier can produce.
    #
    # BIOME_TINT IS FALSE FOR ALL TEN, and this is the only real decision in
    # these rows. A biome tint answers "what colour is the GROUND here, given
    # this column's climate" -- it is a property of a place. An oak's bark is
    # not a property of a place; tinting it would make the same tree change
    # colour as it crossed a climate boundary, and it would take the colour of
    # the dirt. The species already carries its own answer, chosen in the forge
    # and baked into the material id.
    ("BARK",              0.304987, 0.162029, 0.068478, False),
    ("HEARTWOOD",         0.341914, 0.181164, 0.076185, False),
    ("DEADWOOD",          0.254152, 0.208637, 0.144128, False),
    ("LEAF_BROADLEAF",    0.076185, 0.187821, 0.036889, False),
    ("LEAF_NEEDLE",       0.036889, 0.097587, 0.051269, False),
    ("LEAF_JUNGLE",       0.042311, 0.162029, 0.031896, False),
    ("LEAF_DRY",          0.270498, 0.238398, 0.084376, False),
    ("BARK_PALE",         0.434154, 0.391572, 0.313989, False),
    ("LEAF_BLOSSOM",      0.822786, 0.514918, 0.577580, False),
    ("LEAF_AUTUMN",       0.527115, 0.181164, 0.031896, False),
    # --- CREATURE MATERIALS ------------------------------------------------
    # Skin colours for the environment animals from asset-forge. Like the wood
    # and foliage above, the terrain generator cannot emit any of these.
    #
    # BIOME_TINT IS FALSE FOR ALL TEN, and for a stronger reason than it is
    # false for bark. A biome tint answers "what colour is the GROUND here,
    # given this column's climate": it is a property of a PLACE. Bark is at
    # least attached to a place. An animal is not attached to anything -- it
    # swims through columns -- so a tinted fish would change colour as it
    # crossed a climate boundary while nothing about the fish had changed.
    #
    # These rows also sit outside what this column is FOR. It is indexed by
    # material id with no face information and it holds the TOP face, because
    # terrain is looked at from above; an animal is looked at from the side,
    # and MAT_SKIN_SILVER is the one material in the whole table whose sides
    # are deliberately brighter than its top. Anything that needs a fish's real
    # appearance must read VoxelMaterialPalette.ush, not this.
    ("SKIN_DARK",         0.027321, 0.029557, 0.039546, False),
    ("SKIN_PALE",         0.806952, 0.760525, 0.658375, False),
    ("SKIN_SILVER",       0.434154, 0.491021, 0.552011, False),
    ("SKIN_OLIVE",        0.093059, 0.116971, 0.036889, False),
    ("SKIN_BROWN",        0.155926, 0.084376, 0.034340, False),
    ("SKIN_ORANGE",       0.760525, 0.181164, 0.015996, False),
    ("SKIN_YELLOW",       0.806952, 0.539479, 0.036889, False),
    ("SKIN_RED",          0.401978, 0.027321, 0.021219, False),
    ("SKIN_BLUE",         0.027321, 0.116971, 0.391572, False),
    ("SKIN_GREEN",        0.042311, 0.296138, 0.107023, False),
    # --- PLUMAGE MATERIALS -------------------------------------------------
    # Feather and bill colours for the asset-forge bird kind. Like the wood,
    # the foliage and the skins above, the terrain generator cannot emit any of
    # these -- they arrive only from a baked asset.
    #
    # BIOME_TINT IS FALSE FOR ALL ELEVEN, for exactly the reason it is false for
    # the skins: a biome tint answers "what colour is the GROUND here, given
    # this column's climate", and it is a property of a PLACE. A bird is the
    # least attached thing in the library -- it flies across climate boundaries
    # -- so a tinted one would change colour in mid-air while nothing about the
    # bird had changed. A tinted MAT_PLUME_CYAN would also destroy the single
    # most stylised entry in the palette, which exists precisely because the
    # kingfisher is not the colour of anything around it.
    #
    # These rows also sit outside what this column is FOR. It holds the TOP
    # face, because terrain is looked at from above, and a bird is looked at
    # from the side -- and MAT_PLUME_IRIDESCENT is, with MAT_SKIN_SILVER, one
    # of only two materials in the whole table whose sides are deliberately
    # brighter than its top. Anything that needs a bird's real appearance must
    # read VoxelMaterialPalette.ush, not this.
    ("PLUME_WHITE",       0.921582, 0.921582, 0.887923, False),
    ("PLUME_GREY",        0.304987, 0.332452, 0.371238, False),
    ("PLUME_SLATE",       0.076185, 0.107023, 0.162029, False),
    ("PLUME_BUFF",        0.630757, 0.434154, 0.181164, False),
    ("PLUME_RUFOUS",      0.456411, 0.088656, 0.017642, False),
    ("PLUME_CRIMSON",     0.630757, 0.021219, 0.039546, False),
    ("PLUME_LIME",        0.313989, 0.590619, 0.061246, False),
    ("PLUME_CYAN",        0.034340, 0.479320, 0.617207, False),
    ("PLUME_LILAC",       0.381326, 0.230740, 0.630757, False),
    ("PLUME_IRIDESCENT",  0.011612, 0.076185, 0.061246, False),
    ("BEAK_HORN",         0.116971, 0.097587, 0.072272, False),
]

# Derived, not asserted against a literal. The old form was `PALETTE_WIDTH = 16`
# with an assert beside it, which is a constant that has to be edited in step
# with the table -- and the table is now generated from an enum that grows.
PALETTE_WIDTH = len(PALETTE)


def biome_tinted_runs():
    """Contiguous runs of biome-tinted ids, as [(first, last), ...].

    The material builds one band test per RUN rather than one per id, so the
    graph stays small while still being generated from this table instead of
    hardcoded in the shader. Today: [(4, 4), (6, 6), (8, 12)] -- three runs,
    because MAT_SNOW (7) sits between two tinted stretches and is not tinted.
    (The count here read "two runs" for a while, which was never true of any
    version of the table; MAT_SNOW has always been False.)
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
