"""Author /Game/Voxel/T_VoxelBathyInfo -- the ASSET HANDLE that
UVoxelBathyFieldSubsystem overwrites the pixels of at runtime.

This script authors one texture and then PROVES it is the exact texture the C++
will accept. It creates no material, no actor and no content: the pixels it
ships are all zeroes and are meant to be replaced.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript
      -script=ue-project/Tools/create_bathy_info_texture.py -unattended -nop4 -nosplash

Verify an existing asset without recreating it (no import, no save -- just the
read-back below, which is the part with teeth):
  set VOXEL_BATHY_VERIFY_ONLY=1        (or pass --verify-only in the script args)


================================================================================
WHAT THE ASSET IS FOR
================================================================================

VoxelBathyField.h is the long version. The short version: bake_ver 27 ships two
int16 planes per fine tile (lake depth and a signed shoreline distance), a
material cannot read a streamed CPU plane, and this project cannot use a
UMaterialInstanceDynamic to hand it one -- the far-field lake sheet deliberately
assigns the SHARED /Game/Voxel/M_WaterVoxel to every section with no MID at all
(VoxelWaterSheetActor.h:47-52), and a Material Parameter Collection, which is
this project's existing CPU->material channel, cannot hold a texture.

So the material samples a NAMED ASSET, this one, and UVoxelBathyFieldSubsystem
overwrites its pixels every time the camera-centred window recentres:

    UTexture2D::UpdateTextureRegions(0, 1, &Region, Pitch, /*bpp=*/8, Data)

8 bytes per pixel is FFloat16Color, i.e. PF_FloatRGBA. The subsystem REFUSES to
write -- permanently, for that world -- unless the asset it finds reports
GetPixelFormat() == PF_FloatRGBA and both dimensions are exactly 512. That guard
is the reason this script exists, and satisfying it is the whole job:

    512 x 512, PF_FloatRGBA, NO MIPS, non-sRGB, clamp, bilinear, never streamed.

CHANNELS -- WRITTEN BY C++, READ BY THE MATERIAL. This table is the one place
the two agree, so change it here and nowhere else:

    R  lake water depth at this cell, METRES. 0 where dry.
    G  SIGNED distance to the nearest shoreline, METRES. POSITIVE inside water,
       NEGATIVE on land, saturating at +/-100.
    B  validity. 1.0 where the bake answered, 0.0 where there is no data --
       no tile resident, or a world baked before bake_ver 27.
    A  reserved. 0.0.

THE SHIPPED PIXELS ARE ALL ZEROES, AND THAT IS A DELIBERATE VALUE, NOT A
PLACEHOLDER FOR ONE. All zeroes means B = 0 everywhere, which means "no baked
data here", which is the conservative answer every consumer must already handle
(VoxelBathyField.h: "A CONSUMER MUST HONOUR BOTH BathyFieldValid AND the
texture's B CHANNEL"). So in the material editor's preview, in a run with no
fine tier, and in any frame before the subsystem's first publish, the field
reads as absent and every lake falls back to its screen-space depth path. The
alternative -- shipping a plausible depth -- would make an unpublished field
look like a working one, which is the failure this project keeps paying for.


================================================================================
WHERE THIS SITS IN THE ASSET REGENERATION ORDER
================================================================================

INDEPENDENT of the sky chain. It reads no MaterialParameterCollection, samples
no texture, and nothing in create_sky_material.py's mandated 1-2-3 ordering
(sky -> dome -> water, see that file's header) touches it. It can be run at any
time, in any editor session, on its own.

BUT IT MUST RUN BEFORE Tools/create_water_voxel_material.py. That material's
bathymetry texture-sample parameter defaults to THIS asset, and a texture
parameter whose default asset does not exist is not a warning there -- the
generator raises rather than emitting a sampler bound to None, for the same
reason create_sky_material.py raises on an unresolved CollectionParameter: an
unbound sampler does not fail to compile, it compiles to something (zero, or the
engine's default grey), so the symptom would be every lake reading depth 0 and
validity 0 forever, with no diagnostic. The full order when regenerating
everything is therefore:

    0. Tools/create_bathy_info_texture.py           (T_VoxelBathyInfo)  <- HERE
    1. Tools/create_sky_material.py                 (MPC_VoxelSky + M_NightSky)
    2. Tools/create_sky_atmosphere_dome_material.py (M_SkyAtmosphereDome)
    3. Tools/create_water_voxel_material.py         (M_WaterVoxel)

tools/voxel-water-star-regen.ps1 drives 1-3. It does not yet drive 0; until it
does, run this by hand in the same editor session, BEFORE the water script.

RE-RUNNING THIS SCRIPT DELETES AND RECREATES THE ASSET (same delete-then-create
discipline as create_sky_material.py). A hard reference from an ALREADY-LOADED
M_WaterVoxel would be nulled in memory by that delete even though it re-resolves
by path on the next load, so do not re-run this in a session that has already
built the water material -- re-run both, in the order above.


================================================================================
HOW THE ASSET IS CREATED, AND WHY IT IS A RADIANCE .HDR FILE
================================================================================

UE's Python cannot construct a float texture from raw bytes: there is no
scripting-exposed equivalent of FTextureSource::Init, and asset_tools has no
"make me an RGBA16F Texture2D" factory. What Python CAN do is drive
UTextureFactory over a file on disk. So this script writes a temporary Radiance
RGBE image and imports it.

WHY .HDR SPECIFICALLY. It is the only float format this repo can author with
the standard library alone -- UE 5.8's bundled Python has neither numpy nor
Pillow (gen_terrain_textures.py's header states the same constraint, which is
why that file runs under SYSTEM Python instead) -- and its import path lands on
exactly the settings needed. Verified in the engine source on this box
(D:/UE_5.8), not assumed:

  * FHdrImageWrapper's only raw format is BGRE8
    (HdrImageWrapper.cpp:139-142).
  * ERawImageFormat::IsHDR(BGRE8) is true (ImageCore.cpp:1477-1480), and
    UTextureFactory's generic 2D image path sets CompressionSettings = TC_HDR
    and asserts SRGB == false for any HDR raw format
    (EditorFactories.cpp:3030-3034).
  * TC_HDR resolves to the texture format name RGBA16F
    (Texture.cpp:4289-4292), i.e. PF_FloatRGBA -- the format the C++ guard
    demands.

THE FILE THIS WRITES. UE's own HDR writer emits precisely this header
(HdrImageWrapper.cpp:177), and its reader is strict about all of it -- the magic
line must be "#?RADIANCE" or "#?RGBE" (:46-50), some header line must say
FORMAT=32-bit_rle_rgbe (:69-76), and the resolution line must be the "-Y H +X W"
form (:401-402):

    #?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 512 +X 512\n

followed by 512*512 flat, non-RLE, 4-byte RGBE pixels, all zero.

ALL-ZERO BYTES CANNOT BE MISREAD AS A RUN. Both of the decoder's
run-length forms are gated on a specific first byte that zero is not:
new-style adaptive RLE requires the scanline to begin with 2,2
(HdrImageWrapper.cpp:452-477), and old-style RLE requires the pixel to be
literally 1,1,1,count (:586-598). A 0,0,0,0 pixel takes neither branch and is
copied as a literal, so a flat file decodes to a flat image. And RGBE (0,0,0,0)
decodes to radiance (0,0,0) -- an exponent byte of 0 is the format's zero, so
the all-zeroes-means-no-data contract survives the round trip intact.

THE LONGLAT CUBEMAP TRAP, WHICH IS REAL AND IS NOT ABOUT ASPECT RATIO. It is
tempting to write "512x512 is 1:1 so UE cannot mistake it for an equirect
cubemap". That is FALSE on UE 5.8: EditorFactories.cpp:3505-3593 asks the
question for EVERY .hdr file regardless of its dimensions, and answers it from
config or a modal dialog. The two answers that matter here:

  * With -unattended (what every script in this directory uses), FApp::IsUnattended()
    is true, so FMessageDialog::Open returns the DEFAULT for a YesNoYesAllNoAll
    prompt without showing anything -- and that default is NO
    (MessageDialog.cpp:109-111, :157-166). No cubemap. We get a UTexture2D.
  * With -RUNNINGUNATTENDEDSCRIPT, GIsRunningUnattendedScript is true
    (LaunchEngineLoop.cpp:6857-6860) and EditorFactories.cpp:3540-3542 hard-codes
    "just default to legacy behavior = yes latlong cubemap". The import then
    produces a UTextureCube, this script's asset is the wrong CLASS, and the
    subsystem's UTexture2D cast fails.

So: do NOT add -RUNNINGUNATTENDEDSCRIPT to this script's command line. If it
ever has to be there, set [TextureImporter] LoadHdrAsLongLatCubemap=0 in the
editor ini first (EditorFactories.cpp:3523-3531). The read-back below asserts
the class, so that mistake fails loudly with this paragraph's name on it rather
than producing an asset that merely does nothing.

THE TEMPORARY FILE IS DELETED AFTER IMPORT, which leaves the texture's
AssetImportData pointing at a path that no longer exists. That is intentional
and costs nothing: "Reimport" is not the maintenance path for this asset --
re-running this script is -- and reimporting an all-zero file would be a no-op
anyway. It is also why nothing is added to Content/Voxel/TextureSource/: unlike
gen_terrain_textures.py's PNGs, there is no authored content here to keep under
version control, only a megabyte of zeroes that this file regenerates in a
millisecond.


================================================================================
THE LOD GROUP, AND THE ORDER THE PROPERTIES ARE SET IN
================================================================================

LOD GROUP: TEXTUREGROUP_EffectsNotFiltered. Chosen after checking that it
overrides nothing this asset depends on:

  * FILTER. An explicitly-set Filter always wins over the group --
    UTextureLODSettings::GetSamplerFilter switches on Texture->Filter and only
    consults TextureLODGroups[LODGroup].Filter in the TF_Default case
    (TextureLODSettings.cpp:505-527). Our TF_Bilinear is explicit, so the
    group's name notwithstanding, this texture is bilinear. (And even the
    fallback would be harmless: the group's configured filter is aniso, not
    point -- BaseDeviceProfiles.ini:198.)
  * ADDRESSING. Groups carry no address mode at all; AddressX/Y come straight
    off the texture.
  * RESOLUTION. The group's MaxLODSize is 16384 and its LODBias is 0
    (BaseDeviceProfiles.ini:198), so it cannot downsize the 512x512 -- which
    would otherwise be a silent way to fail the C++ size guard.
  * MIPS. The group's MipGenSettings only applies to textures left on
    TMGS_FromTextureGroup. Ours says TMGS_NoMipmaps explicitly.

The alternative was to leave the default TEXTUREGROUP_World. Either works; the
effects group is the more honest label for a runtime-written scratch surface,
and it keeps this texture out of the world-texture streaming pool's budget
accounting even for anyone who later clears never_stream.

ORDER MATTERS, AND NOT COSMETICALLY. lod_group is set FIRST and everything else
after it, because UTexture::PostEditChangeProperty CLOBBERS
CompressionSettings, SRGB, Filter and MipGenSettings when the LODGroup property
changes to TEXTUREGROUP_8BitData or TEXTUREGROUP_16BitData
(Texture.cpp:868-886). Our group is neither of those, so today the order is
belt-and-braces -- but "today" is doing a lot of work in that sentence, and the
failure it prevents (Filter silently reset to TF_Default, MipGenSettings
silently reset to TMGS_FromTextureGroup, i.e. MIPS BACK ON) is exactly the
class the read-back below exists to catch. Cheaper to order it correctly.


================================================================================
THE READ-BACK, WHICH IS THE POINT OF THE SCRIPT
================================================================================

A generator here proves what it produced (create_sky_material.py's round-trip
and zero-guid checks; create_water_voxel_material.py's .uasset byte read).
Every assertion in verify_asset() below names the thing that breaks:

  * WRONG CLASS (a UTextureCube)  -> the subsystem's cast fails, nothing is
    ever published.
  * WRONG SIZE or WRONG FORMAT    -> the subsystem refuses to publish the field,
    and every lake silently falls back to screen-space depth. "Silently" is the
    operative word: the fallback renders, so a screenshot looks like a tuning
    problem rather than a missing asset.
  * MIPS PRESENT                  -> distant water samples a mip that C++ never
    writes (UpdateTextureRegions is called for mip 0 only), so shorelines
    dissolve with distance and near water looks correct -- the worst possible
    split, because the near shot passes review.
  * STREAMED                      -> the streamer may evict or reload mip 0 out
    from under a pending UpdateTextureRegions.
  * sRGB ON                       -> every value is pushed through a colour
    curve; a 3.2 m depth reads as something else entirely.
  * WRAP instead of CLAMP         -> a sample just outside the camera window
    wraps to the far side of it, painting a lake from a kilometre away onto the
    edge of the screen.
  * NEAREST instead of BILINEAR   -> the shoreline steps on the 1.875 m source
    raster, which throws away the entire reason the bake ships a signed distance
    field rather than a mask (VoxelBathyField.h:43-48).

PIXEL FORMAT IS THE ONE THING THIS SCRIPT CANNOT ASSERT DIRECTLY.
UTexture2D::GetPixelFormat is not a UFUNCTION (Texture2D.h:161 -- only
Blueprint_GetSizeX/Y at :360/:367 are exposed), so there is no Python accessor
for it. verify_asset() tries the plausible binding names anyway and uses one if
it exists; when none does, the check falls back to its two authored causes --
compression_settings == TC_HDR and srgb == False, which Texture.cpp:4289-4292
maps to RGBA16F -- and the format assertion itself is COMPLETED AT RUNTIME by
the C++ guard in UVoxelBathyFieldSubsystem::Initialize, which reads the real
GetPixelFormat() and disarms with a message. That is a real check in a real
place, not a hole: what this script guarantees is the authored intent, and the
subsystem guarantees the built artefact.

There is also a byte-level read of the saved .uasset, the same trick
create_water_voxel_material.py uses at its end and for the same reason: UE
serialises enum properties BY NAME, so the package's name table literally
contains "TC_HDR" and "TMGS_NoMipmaps" when those values took. That reads the
artefact on disk rather than the UObject this script just configured in memory.
It is advisory when unreadable and fatal when it positively disagrees.
"""

import os
import sys
import tempfile

import unreal

PACKAGE_PATH = "/Game/Voxel"
ASSET_NAME = "T_VoxelBathyInfo"
ASSET_PATH = PACKAGE_PATH + "/" + ASSET_NAME
OBJECT_PATH = ASSET_PATH + "." + ASSET_NAME

# MUST MATCH UVoxelBathyFieldSubsystem::kSize (VoxelBathyField.h:137) EXACTLY.
# The C++ refuses to write to a texture of any other size, so a change on one
# side and not the other does not produce a differently-sized field -- it
# produces no field at all. Both sides are literals on purpose: there is no
# shared header a Python commandlet could read, and a "flexible" reader here
# would only hide the mismatch it is supposed to surface.
SIZE = 512

# --verify-only: read an existing asset back and say whether it would be
# accepted, without importing or saving anything. Cheap, and it is the right
# thing to run after a checkout, after a merge that touched Content/, or when a
# run reports zero published windows and the question is whether the asset or
# the subsystem is at fault. Accepted as either an env var or a script arg
# because a -run=pythonscript commandlet makes argument passing awkward.
VERIFY_ONLY_ENV = "VOXEL_BATHY_VERIFY_ONLY"


def want_verify_only():
    if "--verify-only" in sys.argv:
        return True
    return os.environ.get(VERIFY_ONLY_ENV, "").strip().lower() in ("1", "true", "yes", "on")


def write_zero_hdr(path):
    """Write a 512x512 all-zero Radiance RGBE file. Standard library only.

    See "HOW THE ASSET IS CREATED" in the module docstring for why this is a
    .hdr, why flat zeroes are unambiguous to the decoder, and where each of
    those claims was checked in the engine source.
    """
    # Byte-for-byte the header UE's own HDR writer emits
    # (HdrImageWrapper.cpp:177). The reader is strict about every part of it;
    # do not reformat, do not add a comment line, do not use CRLF.
    header = ("#?RADIANCE\n"
              "FORMAT=32-bit_rle_rgbe\n"
              "\n"
              "-Y %d +X %d\n" % (SIZE, SIZE)).encode("ascii")

    # 512*512 pixels, 4 bytes each (R, G, B, shared exponent), every one zero.
    # bytes(n) is n zero bytes -- 1 MB, allocated once.
    body = bytes(SIZE * SIZE * 4)

    with open(path, "wb") as fh:
        fh.write(header)
        fh.write(body)

    written = os.path.getsize(path)
    expected = len(header) + len(body)
    if written != expected:
        # A short write here would be decoded as a truncated scanline and the
        # import would fail with a message about the FILE, several steps away
        # from the cause.
        raise RuntimeError(
            "wrote %d bytes to %s but expected %d -- the .hdr is truncated and "
            "the import would fail on a malformed scanline"
            % (written, path, expected))
    return path


def import_texture(hdr_path):
    """Delete any existing asset and import the .hdr in its place.

    Delete-then-create, matching create_sky_material.py: re-running this script
    must produce the same asset from scratch rather than layering settings onto
    whatever was there, because "whatever was there" includes every state a
    half-finished earlier run could have left.
    """
    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        unreal.EditorAssetLibrary.delete_asset(ASSET_PATH)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", hdr_path)
    task.set_editor_property("destination_path", PACKAGE_PATH)
    task.set_editor_property("destination_name", ASSET_NAME)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    task.set_editor_property("factory", unreal.TextureFactory())

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    # load_object, not EditorAssetLibrary.load_asset: a -run=pythonscript
    # commandlet does not wait for the asset-registry scan, and load_asset goes
    # through the registry. import_terrain_textures.py:71-74 and
    # import_sky_textures.py:246-249 document the same trap.
    tex = unreal.load_object(None, OBJECT_PATH)
    if tex is None:
        raise RuntimeError(
            "import produced no loadable asset at %s. TextureFactory did not "
            "accept %s -- check the log above for an FHdrImageWrapper error "
            "(malformed header / malformed scanline). Without this asset the "
            "bathymetry field has no handle to write into and every lake falls "
            "back to screen-space depth." % (ASSET_PATH, hdr_path))
    return tex


def apply_settings(tex):
    """Set the properties the C++ guard and the material both depend on.

    lod_group FIRST -- see "ORDER MATTERS" in the module docstring
    (UTexture::PostEditChangeProperty, Texture.cpp:868-886).
    """
    tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_EFFECTS_NOT_FILTERED)

    # RGBA16F, uncompressed. The import already chose TC_HDR for us (an HDR raw
    # format forces it, EditorFactories.cpp:3030-3034); set it anyway so the
    # asset states its own requirement instead of inheriting it.
    tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_HDR)
    # Data, not colour. A depth in metres pushed through an sRGB curve is a
    # different number, and nothing downstream would say so.
    tex.set_editor_property("srgb", False)
    # C++ writes mip 0 only. A mip chain would be built once, from zeroes, and
    # then never updated -- distant water would sample it forever.
    tex.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    # 2 MB, resident, always. The streamer must never evict or reload the
    # mip the render thread is being told to overwrite.
    tex.set_editor_property("never_stream", True)
    # Bilinear is what turns the signed distance field's 100 mm LSB into a
    # sub-texel waterline; nearest would put the shoreline back on the 1.875 m
    # source raster (VoxelBathyField.h:43-48).
    tex.set_editor_property("filter", unreal.TextureFilter.TF_BILINEAR)
    # Clamp, both axes. The window is finite and camera-centred; a wrapped
    # sample would paint the far side of the window onto its near edge.
    tex.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)

    # No post_edit_change() -- Texture2D does not expose it to Python (5.8);
    # set_editor_property already routes through PostEditChangeProperty, so the
    # texture rebuilds on save (import_terrain_textures.py:85-87).
    unreal.EditorAssetLibrary.save_loaded_asset(tex)


def read_pixel_format(tex):
    """The engine's real pixel format, or None if this build will not say.

    UTexture2D::GetPixelFormat is not a UFUNCTION (Texture2D.h:161), so there is
    normally no Python accessor. Try the plausible spellings rather than
    assume: if a future binding exposes it, this check becomes the direct one
    and nobody has to notice. None means UNKNOWN and is deliberately not
    "wrong" -- reporting a bindings gap as a format failure would abort a
    correct regeneration, which is the same mistake create_water_voxel_material.py
    calls out when it returns -1 for an unreadable arm instead of 0.
    """
    for attr in ("get_pixel_format", "blueprint_get_pixel_format", "get_platform_pixel_format"):
        fn = getattr(tex, attr, None)
        if fn is None:
            continue
        try:
            return fn()
        except Exception:  # noqa: BLE001 -- a signature mismatch is not a failure of the asset
            continue
    return None


def verify_package_bytes():
    """Read the enum values back out of the SAVED .uasset, not out of memory.

    Same trick and same justification as create_water_voxel_material.py's
    package read-back: UE serialises enum properties BY NAME, so a package whose
    CompressionSettings really is TC_HDR has the string "TC_HDR" in its name
    table. This is the only check in this file that looks at the artefact rather
    than at the UObject this script just configured.

    Returns True (agrees), False (positively disagrees) or None (unreadable).
    """
    uasset = os.path.join(unreal.Paths.project_content_dir(), "Voxel", ASSET_NAME + ".uasset")
    try:
        with open(uasset, "rb") as fh:
            blob = fh.read().decode("latin-1")
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "%s: could not read the saved package back (%r), so the format and mip settings "
            "on DISK are unverified. The in-memory checks above still passed; treat this as "
            "one fewer proof, not as a failure." % (ASSET_NAME, exc))
        return None

    found = {name: (name in blob) for name in (
        "TC_HDR", "TMGS_NoMipmaps", "TF_Bilinear", "TA_Clamp", "TextureCube")}
    unreal.log("%s PACKAGE READ-BACK: %s"
               % (ASSET_NAME, ", ".join("%s=%s" % (k, v) for k, v in sorted(found.items()))))
    # TC_HDR_Compressed and TC_HDR_F32 both contain "TC_HDR" as a substring, so
    # rule them out explicitly before trusting the positive.
    hdr_ok = found["TC_HDR"] and "TC_HDR_Compressed" not in blob and "TC_HDR_F32" not in blob
    return bool(hdr_ok and found["TMGS_NoMipmaps"] and not found["TextureCube"])


def verify_asset():
    """Load the asset back and RAISE if it is not one C++ will write into.

    Every message names the consequence, because the consequences here are all
    silent: nothing in a frame says "the bathymetry field was refused", it just
    is not there, and the water still renders.
    """
    tex = unreal.load_object(None, OBJECT_PATH)
    if tex is None:
        raise RuntimeError(
            "no asset at %s. UVoxelBathyFieldSubsystem has nothing to write into, so it "
            "disarms at Initialize and no bathymetry field is ever published -- every lake "
            "falls back to screen-space depth, with no error in the frame."
            % ASSET_PATH)

    # --- class ---------------------------------------------------------------
    #
    # A UTextureCube here is not a near miss, it is the documented outcome of
    # running the import with -RUNNINGUNATTENDEDSCRIPT (EditorFactories.cpp:3540-3542).
    # See "THE LONGLAT CUBEMAP TRAP" in the module docstring.
    if not isinstance(tex, unreal.Texture2D):
        raise RuntimeError(
            "%s is a %s, not a Texture2D. The .hdr was imported as a longlat CUBEMAP -- that "
            "happens when the editor runs with -RUNNINGUNATTENDEDSCRIPT, which hard-codes "
            "'yes latlong cubemap' for every HDR import. UVoxelBathyFieldSubsystem's "
            "UTexture2D cast fails and it never publishes a field. Drop that flag (plain "
            "-unattended answers NO), or set [TextureImporter] LoadHdrAsLongLatCubemap=0 in "
            "the editor ini, and re-run." % (ASSET_PATH, type(tex).__name__))

    # --- size ----------------------------------------------------------------
    size_x = int(tex.blueprint_get_size_x())
    size_y = int(tex.blueprint_get_size_y())
    if size_x != SIZE or size_y != SIZE:
        raise RuntimeError(
            "%s is %dx%d, not %dx%d. UVoxelBathyFieldSubsystem checks the dimensions before "
            "its first UpdateTextureRegions and REFUSES to write anything if they disagree, "
            "so the bathymetry field is never published and every lake silently falls back "
            "to screen-space depth. If this reads as a smaller power of two, a texture LOD "
            "group's MaxLODSize capped it."
            % (ASSET_PATH, size_x, size_y, SIZE, SIZE))

    # --- format --------------------------------------------------------------
    #
    # The direct assertion if the bindings allow it; otherwise the two authored
    # properties that produce it, with the real check deferred to the C++ guard.
    # See "PIXEL FORMAT IS THE ONE THING THIS SCRIPT CANNOT ASSERT DIRECTLY".
    pixel_format = read_pixel_format(tex)
    comp = tex.get_editor_property("compression_settings")
    srgb = bool(tex.get_editor_property("srgb"))

    if pixel_format is not None:
        expected = getattr(unreal.PixelFormat, "PF_FLOAT_RGBA", None)
        if expected is not None and pixel_format != expected:
            raise RuntimeError(
                "%s built as %s, not PF_FloatRGBA. C++ writes 8 bytes per pixel "
                "(FFloat16Color) through UpdateTextureRegions; against any other format that "
                "is either refused by the guard -- no field, every lake on screen-space depth "
                "-- or, if the guard were ever removed, a write past the end of the "
                "allocation." % (ASSET_PATH, pixel_format))

    if comp != unreal.TextureCompressionSettings.TC_HDR:
        raise RuntimeError(
            "%s has compression_settings=%s, not TC_HDR. Only TC_HDR builds to RGBA16F "
            "(Texture.cpp:4289-4292); anything else -- including TC_HDR_Compressed, which is "
            "BC6H -- gives the subsystem a pixel format it refuses to write, so no "
            "bathymetry field is published and every lake falls back to screen-space depth."
            % (ASSET_PATH, comp))

    if srgb:
        raise RuntimeError(
            "%s has sRGB ON. R and G are metres and B is a validity flag, not colour: an sRGB "
            "decode bends every one of them through a gamma curve, so depths and shoreline "
            "distances come out wrong by a factor that varies with the value itself -- and "
            "the water still renders, so nothing reports it." % ASSET_PATH)

    # --- mips ----------------------------------------------------------------
    mips = tex.get_editor_property("mip_gen_settings")
    if mips != unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS:
        raise RuntimeError(
            "%s has mip_gen_settings=%s, not TMGS_NoMipmaps. C++ calls UpdateTextureRegions "
            "for MIP 0 ONLY, so every lower mip stays frozen at the all-zero import content: "
            "distant water samples a mip C++ never writes and reads validity 0 (or worse, a "
            "half-valid average across the shoreline), while near water looks perfect. That "
            "split passes a close-up screenshot review." % (ASSET_PATH, mips))

    # --- streaming -----------------------------------------------------------
    if not bool(tex.get_editor_property("never_stream")):
        raise RuntimeError(
            "%s is STREAMED (never_stream is False). The streaming system may evict or "
            "reload the very mip the render thread has been handed a region update for, so "
            "the field flickers between the live window and the all-zero import content -- "
            "intermittently, under memory pressure, which is the hardest possible version of "
            "this bug to reproduce." % ASSET_PATH)

    # --- sampler -------------------------------------------------------------
    #
    # Not the C++ guard's business, but the material's, and both failures are
    # visible-but-plausible rather than obvious.
    filt = tex.get_editor_property("filter")
    if filt != unreal.TextureFilter.TF_BILINEAR:
        raise RuntimeError(
            "%s has filter=%s, not TF_Bilinear. The waterline is found by looking for the "
            "zero crossing of the signed distance field, and point sampling snaps that "
            "crossing to the 1.875 m source raster -- which discards the whole reason the "
            "bake ships a signed distance rather than a mask (VoxelBathyField.h:43-48). "
            "Shorelines come back visibly stair-stepped." % (ASSET_PATH, filt))

    addr_x = tex.get_editor_property("address_x")
    addr_y = tex.get_editor_property("address_y")
    if addr_x != unreal.TextureAddress.TA_CLAMP or addr_y != unreal.TextureAddress.TA_CLAMP:
        raise RuntimeError(
            "%s has address %s/%s, not TA_Clamp on both axes. The window is a finite 960 m "
            "square that follows the camera; a wrapped sample just outside it returns the "
            "field from the OPPOSITE edge, painting a lake from up to a kilometre away onto "
            "the near edge of the screen." % (ASSET_PATH, addr_x, addr_y))

    unreal.log(
        "%s VERIFIED: %dx%d, comp=%s, srgb=%s, mips=%s, neverStream=True, filter=%s, "
        "address=%s/%s, pixelFormat=%s"
        % (ASSET_PATH, size_x, size_y, comp, srgb, mips, filt, addr_x, addr_y,
           pixel_format if pixel_format is not None else
           "UNKNOWN from Python -- asserted at runtime by the C++ guard"))
    if pixel_format is None:
        unreal.log(
            "%s: this engine build exposes no pixel-format accessor to Python "
            "(UTexture2D::GetPixelFormat is not a UFUNCTION), so the format assertion above "
            "is TC_HDR + sRGB-off, which Texture.cpp maps to RGBA16F. The format itself is "
            "checked at RUNTIME by UVoxelBathyFieldSubsystem::Initialize, which reads the "
            "real GetPixelFormat() and disarms with a message if it is not PF_FloatRGBA. If "
            "a run reports zero published windows, that message is the first thing to grep "
            "for." % ASSET_NAME)

    return tex


def main():
    verify_only = want_verify_only()

    if verify_only:
        unreal.log("%s: --verify-only -- checking the existing asset, importing nothing."
                   % ASSET_NAME)
        verify_asset()
        unreal.log("%s: verify-only passed. The asset on disk is one "
                   "UVoxelBathyFieldSubsystem will write into." % ASSET_PATH)
        return

    # The .hdr is scratch, not content -- see the module docstring for why it
    # does not live in Content/Voxel/TextureSource/ alongside the terrain PNGs.
    tmp_dir = tempfile.mkdtemp(prefix="voxel_bathy_")
    hdr_path = os.path.join(tmp_dir, ASSET_NAME + ".hdr")
    try:
        write_zero_hdr(hdr_path)
        unreal.log("%s: wrote a %dx%d all-zero Radiance RGBE scratch file (%d bytes) to %s"
                   % (ASSET_NAME, SIZE, SIZE, os.path.getsize(hdr_path), hdr_path))

        tex = import_texture(hdr_path)
        apply_settings(tex)
    finally:
        # Best effort. A leftover megabyte in the OS temp directory is not worth
        # failing an otherwise-good asset generation over.
        try:
            os.remove(hdr_path)
            os.rmdir(tmp_dir)
        except OSError:
            pass

    verify_asset()
    verify_package_bytes_ok = verify_package_bytes()
    if verify_package_bytes_ok is False:
        raise RuntimeError(
            "the saved %s.uasset does not name TC_HDR and TMGS_NoMipmaps (or it names "
            "TextureCube). The in-memory properties this script set did not survive into the "
            "package, so the asset the editor loads next session is NOT RGBA16F with no mips "
            "-- UVoxelBathyFieldSubsystem would refuse to publish and every lake would fall "
            "back to screen-space depth. See the PACKAGE READ-BACK line above for which name "
            "was missing." % ASSET_NAME)

    unreal.log(
        "%s created and saved. It holds ALL ZEROES, which means validity (B) = 0 everywhere, "
        "which means 'no baked bathymetry' -- that is the correct shipped state and an empty "
        "field after running this script is EXPECTED, not a failure. Real pixels arrive at "
        "runtime from UVoxelBathyFieldSubsystem. Run Tools/create_water_voxel_material.py "
        "AFTER this script: its bathymetry sampler defaults to this asset and raises if it "
        "is missing." % ASSET_PATH)


main()
