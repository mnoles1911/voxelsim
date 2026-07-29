"""Import the NASA sky-dome reference textures fetched by
tools/fetch-sky-assets.ps1 as /Game/Voxel Texture2D assets.

Run AFTER tools/fetch-sky-assets.ps1 has populated tools/sky-assets/ (source
files live outside Content/ and are gitignored -- they are large NASA
downloads, not checked-in generated content like gen_terrain_textures.py's
PNGs, so this script reads from tools/sky-assets/ rather than
Content/Voxel/TextureSource/):

  UnrealEditor-Cmd.exe <uproject> -run=pythonscript
      -script=ue-project/Tools/import_sky_textures.py -unattended -nop4 -nosplash

Settings matter as much as the pixels here:

  T_SkyStarmap        NASA SVS equirectangular star map (celestial/ICRF
                       coords), half-float EXR. sRGB OFF -- this is linear
                       HDR data, not display-referred colour; sRGB-decoding
                       it would darken and hue-shift every star. TC_HDR keeps
                       the half-float dynamic range (an ordinary BC1/BC7
                       colour format would clip it to LDR). Address: wrap in
                       U (right ascension wraps around the sky), clamp in V
                       (declination does not wrap over the poles -- wrapping
                       V would smear the north/south pole texels across the
                       seam). Mips ON: it's a distant background plate,
                       minification is expected, and mips suppress star-field
                       aliasing/shimmer at grazing angles.

  T_MoonColor          LROC colour-poles albedo mosaic. sRGB ON (ordinary
                       display-referred colour), TC_DEFAULT (regular BCn
                       colour compression is fine for an albedo map -- unlike
                       the star map there's no HDR range to preserve), wrap U
                       / clamp V for the same equirect-sphere reason as the
                       star map.

  T_MoonDisplacement   LDEM elevation data (ldem_4_uint.tif). sRGB OFF -- it
                       is a height field, not colour, and decoding it through
                       a colour curve would corrupt every sample. TC_DISPLACEMENTMAP
                       so it is NOT block-compressed: BC formats are lossy
                       per-4x4-pixel-block, which would introduce terracing
                       into whatever displaces/parallax-samples this map.
                       Wrap U / clamp V, matching the other two.

KNOWN RISK, unproven in this repo: EXR -> Texture2D import through
unreal.TextureFactory has never been exercised here before (PNG import via
the same factory already works, see import_terrain_textures.py). This script
does NOT paper over an EXR import failure. If import_asset_tasks() produces
no loadable asset at /Game/Voxel/T_SkyStarmap, this script raises
RuntimeError after logging exactly which asset(s) failed -- it does not
report success with 0 textures imported, and it does not silently skip the
star map and continue. Doctrine: a gate that no-ops and exits 0 is not a
pass.

The documented (not automatic) fallback if EXR import fails: convert the EXR
to a format TextureFactory definitely imports (e.g. 16-bit-float TIFF or
.hdr) using SYSTEM Python -- numpy + Pillow (with the OpenEXR plugin or
imageio's freeimage/openexr backend) for the EXR read/write -- then re-run
this script against the converted file. This mirrors gen_terrain_textures.py
/ import_terrain_textures.py's split: UE 5.8's bundled Python has neither
numpy nor Pillow, so any pixel-format conversion has to happen outside the
editor's Python before this script runs.

NOTE: there is no sky material yet, and none of this repo's
create_*_material.py scripts (create_voxel_material.py, create_ocean_material.py,
create_water_voxel_material.py, create_clipmap_material.py -- all terrain/water)
makes one. These textures have nowhere to render once imported. If nothing
shows up on screen after running this script, that is expected -- it is not
evidence the import failed. Check the log output (or the asset registry)
for the actual pass/fail signal, not the viewport.
"""

import os

import unreal

PACKAGE_PATH = "/Game/Voxel"
SOURCE_DIR = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "sky-assets")
)

# (asset name, source file, srgb, compression, mips, filter, address_u, address_v)
SPECS = [
    ("T_SkyStarmap", "starmap_2020_4k.exr", False, unreal.TextureCompressionSettings.TC_HDR,
     unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP, unreal.TextureFilter.TF_DEFAULT,
     unreal.TextureAddress.TA_WRAP, unreal.TextureAddress.TA_CLAMP),
    ("T_MoonColor", "lroc_color_poles_4k.tif", True, unreal.TextureCompressionSettings.TC_DEFAULT,
     unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP, unreal.TextureFilter.TF_DEFAULT,
     unreal.TextureAddress.TA_WRAP, unreal.TextureAddress.TA_CLAMP),
    # TC_DISPLACEMENTMAP: pythonized from the C++ enumerator TC_Displacementmap.
    # This repo has no prior use of this particular enum value to copy (only
    # TC_EDITOR_ICON / TC_DEFAULT / TC_HDR appear elsewhere), and this script
    # was authored without a live editor session available to confirm it
    # (see report). If it raises AttributeError, check
    # `unreal.TextureCompressionSettings` in an interactive editor console
    # for the exact spelling and fix this one line.
    ("T_MoonDisplacement", "ldem_4_uint.tif", False, unreal.TextureCompressionSettings.TC_DISPLACEMENTMAP,
     unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP, unreal.TextureFilter.TF_DEFAULT,
     unreal.TextureAddress.TA_WRAP, unreal.TextureAddress.TA_CLAMP),
]


def main():
    tasks = []
    for (name, src, _srgb, _comp, _mips, _filt, _addr_u, _addr_v) in SPECS:
        path = os.path.join(SOURCE_DIR, src)
        if not os.path.isfile(path):
            raise RuntimeError(
                "missing %s -- run `tools/fetch-sky-assets.ps1` first "
                "(pass -EightK if this spec is repointed at the 8k star map)"
                % path)

        full = PACKAGE_PATH + "/" + name
        if unreal.EditorAssetLibrary.does_asset_exist(full):
            unreal.EditorAssetLibrary.delete_asset(full)

        task = unreal.AssetImportTask()
        task.set_editor_property("filename", path)
        task.set_editor_property("destination_path", PACKAGE_PATH)
        task.set_editor_property("destination_name", name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        task.set_editor_property("factory", unreal.TextureFactory())
        tasks.append(task)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    failures = []
    for (name, src, srgb, comp, mips, filt, addr_u, addr_v) in SPECS:
        full = PACKAGE_PATH + "/" + name
        # load_object, not EditorAssetLibrary.load_asset: a -run=pythonscript
        # commandlet does not wait for the asset-registry scan, and load_asset
        # goes through the registry (import_terrain_textures.py documents the
        # same trap).
        tex = unreal.load_object(None, full + "." + name)
        if tex is None:
            # Loud failure, not a silent no-op. This is the EXR-import risk
            # called out in the module docstring for T_SkyStarmap, but the
            # check is unconditional -- any of the three failing to produce
            # an asset is a real failure, not a "0 imported, 0 failed" pass.
            failures.append(name)
            unreal.log_error(
                "FAILED to import %s from %s -- TextureFactory/import_asset_tasks "
                "produced no loadable asset. If this is T_SkyStarmap: EXR import "
                "through TextureFactory is unproven in this repo (see this "
                "script's header) -- the fallback is to convert the EXR to a "
                "format TextureFactory definitely imports (16-bit-float TIFF "
                "or .hdr) with system Python (numpy + Pillow/OpenEXR or "
                "imageio; UE's bundled Python has neither) and re-run against "
                "the converted file. This is NOT done automatically."
                % (full, src))
            continue

        tex.set_editor_property("srgb", srgb)
        tex.set_editor_property("compression_settings", comp)
        tex.set_editor_property("mip_gen_settings", mips)
        tex.set_editor_property("filter", filt)
        tex.set_editor_property("address_x", addr_u)
        tex.set_editor_property("address_y", addr_v)
        # No post_edit_change() -- Texture2D does not expose it to Python
        # (5.8); set_editor_property already routes through
        # PostEditChangeProperty (see import_terrain_textures.py).
        unreal.EditorAssetLibrary.save_loaded_asset(tex)
        unreal.log("imported %s  srgb=%s comp=%s mips=%s filter=%s addr=%s/%s"
                   % (full, srgb, comp, mips, filt, addr_u, addr_v))

    if failures:
        raise RuntimeError(
            "import_sky_textures: %d of %d textures FAILED to import: %s -- "
            "see the log_error lines above for the specific cause. This is a "
            "hard failure, not a partial success."
            % (len(failures), len(SPECS), ", ".join(failures)))

    unreal.log(
        "import_sky_textures: all %d textures imported. NOTE -- there is no "
        "sky material yet (none of create_voxel_material.py / "
        "create_ocean_material.py / create_water_voxel_material.py / "
        "create_clipmap_material.py makes one), so these textures have "
        "nowhere to render. That is expected." % len(SPECS))


main()
