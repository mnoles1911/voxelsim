"""Import the PNGs from gen_terrain_textures.py as /Game/Voxel Texture2D assets.

Run AFTER gen_terrain_textures.py (system Python writes the PNGs; UE's bundled
Python has no numpy/Pillow, hence the split):

  UnrealEditor-Cmd.exe <uproject> -run=pythonscript
      -script=ue-project/Tools/import_terrain_textures.py -unattended -nop4 -nosplash

Settings matter as much as the pixels here:

  T_VoxelPalette   16x1 LUT indexed by material id. MUST be uncompressed
                   (a 16-entry palette through BC1 would bleed neighbouring
                   materials into each other), MUST have no mips (a mip would
                   average unrelated materials), MUST be TF_Nearest + clamp so
                   index i lands exactly on texel i.
  T_VoxelBiomeLUT  64x64 Whittaker diagram. Uncompressed + no mips for the same
                   reason, but TF_Bilinear -- biome transitions should be smooth.
  T_VoxelDetail    512x512 tiling fBm. sRGB OFF (it is data, not colour), mips ON
                   and wrapping -- this is the texture that finally gives the
                   terrain material something to mip and anisotropically filter.
"""

import os

import unreal

PACKAGE_PATH = "/Game/Voxel"
SOURCE_DIR = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Content", "Voxel", "TextureSource")
)

# (asset name, srgb, compression, mips, filter, address)
SPECS = [
    ("T_VoxelPalette", True, unreal.TextureCompressionSettings.TC_EDITOR_ICON,
     unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS, unreal.TextureFilter.TF_NEAREST,
     unreal.TextureAddress.TA_CLAMP),
    ("T_VoxelBiomeLUT", True, unreal.TextureCompressionSettings.TC_EDITOR_ICON,
     unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS, unreal.TextureFilter.TF_BILINEAR,
     unreal.TextureAddress.TA_CLAMP),
    ("T_VoxelDetail", False, unreal.TextureCompressionSettings.TC_DEFAULT,
     unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP, unreal.TextureFilter.TF_DEFAULT,
     unreal.TextureAddress.TA_WRAP),
    # Per-voxel jitter MUST be nearest + no mips + uncompressed: bilinear would
    # blend adjacent texels back into the smooth gradient whose absence is the
    # whole point, a mip chain would average it to grey at distance (distance is
    # where the corduroy is boldest, so the jitter must SURVIVE there), and DXT
    # blocks would correlate neighbouring texels.
    ("T_VoxelBlockNoise", False, unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP,
     unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS, unreal.TextureFilter.TF_NEAREST,
     unreal.TextureAddress.TA_WRAP),
]


def main():
    tasks = []
    for name, _srgb, _comp, _mips, _filt, _addr in SPECS:
        png = os.path.join(SOURCE_DIR, name + ".png")
        if not os.path.isfile(png):
            raise RuntimeError(
                "missing %s -- run `python ue-project/Tools/gen_terrain_textures.py` first" % png)
        full = PACKAGE_PATH + "/" + name
        if unreal.EditorAssetLibrary.does_asset_exist(full):
            unreal.EditorAssetLibrary.delete_asset(full)

        task = unreal.AssetImportTask()
        task.set_editor_property("filename", png)
        task.set_editor_property("destination_path", PACKAGE_PATH)
        task.set_editor_property("destination_name", name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        task.set_editor_property("factory", unreal.TextureFactory())
        tasks.append(task)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    for (name, srgb, comp, mips, filt, addr) in SPECS:
        full = PACKAGE_PATH + "/" + name
        # load_object, not EditorAssetLibrary.load_asset: a -run=pythonscript
        # commandlet does not wait for the asset registry scan, and load_asset
        # goes through the registry (create_voxel_material.py documents the same
        # trap for DitherTemporalAA).
        tex = unreal.load_object(None, full + "." + name)
        if tex is None:
            raise RuntimeError("import produced no asset at " + full)

        tex.set_editor_property("srgb", srgb)
        tex.set_editor_property("compression_settings", comp)
        tex.set_editor_property("mip_gen_settings", mips)
        tex.set_editor_property("filter", filt)
        tex.set_editor_property("address_x", addr)
        tex.set_editor_property("address_y", addr)
        # No post_edit_change() -- Texture2D does not expose it to Python (5.8);
        # set_editor_property already routes through PostEditChangeProperty, so
        # the texture rebuilds on save.
        unreal.EditorAssetLibrary.save_loaded_asset(tex)
        unreal.log("imported %s  srgb=%s comp=%s mips=%s filter=%s"
                   % (full, srgb, comp, mips, filt))


main()
