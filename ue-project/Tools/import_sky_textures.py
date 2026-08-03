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

THIS SCRIPT IS NOW LOAD-BEARING, AND IT WAS NOT WHEN IT WAS WRITTEN. The
original text here said there was no sky material and that these textures had
"nowhere to render once imported". befb438/2284c5e made that false: they shipped
M_NightSky and M_SkyAtmosphereDome (built by create_sky_material.py and
create_sky_atmosphere_dome_material.py), and both hold hard package references
to /Game/Voxel/T_SkyStarmap -- M_NightSky also to /Game/Voxel/T_MoonColor.

So skipping this script no longer costs nothing; it costs the stars and the
moon. And because the .uassets this writes are gitignored build output
(.gitignore:82-84 -- T_SkyStarmap alone is 38.8 MB), EVERY checkout starts with
the materials present and the textures absent. Observed 2026-08-02: a checkout
where only fetch-sky-assets.ps1 had been run rendered a starless night sky while
logging "VoxelSky clock RESOLVED" perfectly happily. The signal was:

    LogMaterial: Warning: M_NightSky: Requesting an invalid TextureIndex! (1 / 1)
    LoadErrors: ... /Game/Voxel/M_NightSky, a dependent package
                /Game/Voxel/T_SkyStarmap was not available.

WHY NOTHING CATCHES THAT FOR YOU. VoxelSkyDomeActor.cpp:135-141 guards the
MATERIALS -- a missing material sets bAtmosphereMaterialValid false and
ApplyDomeCvars then refuses to show the dome at all (:341), loudly. That guard
does not extend to textures: a material with an unresolved texture reference
still loads as an object, so the guard passes and the IsSky dome is shown
anyway. A missing texture is therefore a SILENT degrade, where a missing
material is a refusal. Grep for 'invalid TextureIndex' to tell them apart.
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
        "import_sky_textures: all %d textures imported. These ARE rendered: "
        "M_NightSky samples T_SkyStarmap and T_MoonColor, M_SkyAtmosphereDome "
        "samples T_SkyStarmap. If a run logs 'M_NightSky: Requesting an "
        "invalid TextureIndex' or 'T_SkyStarmap ... was not available', the "
        "import did not reach that checkout -- the .uassets are gitignored "
        "build output, so re-run this script there." % len(SPECS))


main()
