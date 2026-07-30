"""Author /Game/Voxel/M_SkyAtmosphereDome -- the canonical IsSky dome that PAINTS
THE MAIN-VIEW SKY, so that a later phase can feed the star map into the
SkyLight's real-time capture without the visible sky going dark.

Phase S1 of docs/sky-and-local-light-plan.md. Pattern:
Tools/create_sky_material.py, which is this file's template -- same
checked-connect discipline, same checked MPC bindings, and the star-map subgraph
literally shared through Tools/sky_star_graph.py rather than copied.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=Tools/create_sky_atmosphere_dome_material.py -unattended -nop4 -nosplash


================================================================================
RUN ORDER IS MANDATORY
================================================================================

    1. Tools/import_sky_textures.py                 (T_SkyStarmap, T_MoonColor)
    2. Tools/create_sky_material.py                 (MPC_VoxelSky + M_NightSky)
    3. Tools/create_sky_atmosphere_dome_material.py (this file)

create_sky_material.py is the SOLE AUTHOR of MPC_VoxelSky and it DELETES AND
RECREATES the asset every run (create_sky_material.py's create_collection). This
script only READS the collection, and the scalar it depends on -- StarAmbientGain
-- is declared in create_sky_material.py's SCALAR_PARAMS table for exactly that
reason: if this script added the parameter instead, the next run of (2) would
silently drop it and this material's binding would unbind.

That failure is silent by construction:
UMaterialExpressionCollectionParameter::Compile falls through to emitting a
CONSTANT when GetParameterIndex misses (MaterialExpressions.cpp:17179-17193), so
an unbound parameter is not a compile error, not a warning and not a log line.
Hence the guard: every collection_param() call here checks NAME MEMBERSHIP
against the collection AS READ BACK, and raises. Running this script against a
stale MPC fails loudly on the first binding instead of shipping a wrong sky.


================================================================================
WHY THIS MATERIAL HAS TO EXIST AT ALL, AND WHY IT IS THE RISKY ONE
================================================================================

FScene::AllocateAndCaptureFrameSkyEnvMap
(Renderer/Private/ReflectionEnvironmentRealTimeCapture.cpp:343) renders exactly
four things into the SkyLight's cubemap: the SkyAtmosphere raymarch, volumetric
clouds, height fog, and meshes whose material has bIsSky. M_NightSky sets
is_sky = False deliberately (create_sky_material.py, "WHY NOT bIsSky"), so today
SkyMeshBatches is empty, the capture takes the else branch at
ReflectionEnvironmentRealTimeCapture.cpp:743-746 -- whose engine comment says it
outright: "If there are any mesh tagged as IsSky then we render them only,
otherwise we simply render the sky atmosphere itself" -- and THE STAR DOME
CONTRIBUTES EXACTLY ZERO AMBIENT LIGHT.

The obvious fix is a capture-only IsSky dome. It breaks the visible sky. The
moment ANY primitive uses a sky material, View.bSceneHasSkyMaterial goes true
(SceneVisibility.cpp:2096, fed by ViewRelevance.bUsesSkyMaterial at
SceneVisibility.cpp:1970-1977) and the main view's atmosphere pass STOPS PAINTING
SKY PIXELS: SkyAtmosphereRendering.cpp:2214 sets
    SkyRC.bRenderSkyPixel = !View.bSceneHasSkyMaterial
and with RENDERSKY_ENABLED==0 the shader clips far-depth pixels out entirely
(SkyAtmosphere.usf:982-992). The engine ships an editor warning for precisely
this misconfiguration (ReflectionEnvironmentRealTimeCapture.cpp:313-316).

    ==> Once one IsSky mesh exists, an IsSky mesh must paint the main-view sky.

So this material's FIRST job is to be a pixel-for-pixel faithful stand-in for
what the atmosphere pass paints today. That is the whole of S1, and it is a
REGRESSION risk rather than research: the failure mode is a plausible image, not
an error. S1's gate is a dome-on/off A/B within ONE process (the screenshot floor
is 0.00% within a session and 1.81% between -- VoxelGpuVerify.cpp:2074-2084),
|delta mean luma| < 2/255 at every rung, twilight rungs specifically.

WHICH IS WHY THE STAR BRANCH SHIPS GAINED TO ZERO. StarAmbientGain defaults to
0.0 and voxel.Sky.StarAmbientGain defaults to 0. S2 flips the cvar; nothing here
needs regenerating for it.


================================================================================
THE GRAPH
================================================================================

    EmissiveColor = SkyAtmosphereViewLuminance                 // the sky itself
                  + SkyAtmosphereLightDiskLuminance(light 0)   // the sun disc
                  + ReflectionCapturePassSwitch(
                        Default    = (0,0,0),                  // main view
                        Reflection = StarMap * StarBrightness
                                     * StarHorizonFade
                                     * StarAmbientGain)        // capture only

Three engine expressions do the work. Class and pin names below were read out of
D:\\UE_5.8 rather than remembered (NOTE: on 5.8 these headers live under
Runtime/Engine/PUBLIC/Materials/, not the Classes/Materials/ path older notes
give -- Classes/ has no Materials directory at all any more):

  UMaterialExpressionSkyAtmosphereViewLuminance
      Public/Materials/MaterialExpressionSkyAtmosphereViewLuminance.h
      One optional input, WorldDirection, LEFT UNCONNECTED here: the translator
      substitutes -CameraVector when it is absent
      (HLSLMaterialTranslator.cpp:12369-12377), which is the gaze direction and
      exactly what is wanted. Compiles to
      MaterialExpressionSkyAtmosphereViewLuminance (MaterialTemplate.ush:2369),
      a sample of View.SkyViewLutTexture scaled by SkyAtmosphereSkyLuminanceFactor
      and OneOverPreExposure -- i.e. the same LUT the full-screen sky pass uses.
      The capture deliberately swaps in its own LUT, built with a constant
      referential for the six cube faces
      (ReflectionEnvironmentRealTimeCapture.cpp:583-597).

  UMaterialExpressionSkyAtmosphereLightDiskLuminance
      Public/Materials/MaterialExpressionSkyAtmosphereLightIlluminance.h:70
      (it shares a header with the Illuminance expressions -- there is no
      MaterialExpressionSkyAtmosphereLightDiskLuminance.h).
      Property LightIndex (int32, EditAnywhere) -> python light_index. Optional
      input DiskAngularDiameterOverride, left unconnected so the disc size comes
      from the DirectionalLight, as it does today.
      IT SELF-SUPPRESSES IN THE CAPTURE: MaterialTemplate.ush:2346-2348 wraps the
      whole body in `if (ResolvedView.RenderingReflectionCaptureMask == 0.0f)`
      with the comment "Do not render light disk when in reflection capture in
      order to avoid double specular." So the sun disc needs no gating of its own
      and cannot double-count against directional light 0 -- and the same
      mechanism is why the plan's claim that "the atmosphere's own moon disc is
      already black" in the capture holds for light index 1 too.

  UMaterialExpressionReflectionCapturePassSwitch
      Public/Materials/MaterialExpressionReflectionCapturePassSwitch.h
      Pins "Default" and "Reflection" (property names; the class does not
      override GetInputName, so UMaterialExpression::GetInputName returns the
      FExpressionInput property name verbatim -- MaterialExpressions.cpp:1821-1856).
      BOTH ARE REQUIRED: Compile errors with "Missing input Default" /
      "Missing input Reflection" if either is unconnected
      (MaterialExpressions.cpp:21612-21629), which is why Default is wired to an
      explicit black constant rather than left dangling.

      THE LOAD-BEARING NODE. It compiles to
      GetReflectionCapturePassSwitchState(), which is
      `View.RenderingReflectionCaptureMask > 0.0f` (Common.ush:2306-2309), and
      the sky capture sets that mask to 1.0
      (ReflectionEnvironmentRealTimeCapture.cpp:570). That is what keeps the stars
      out of the main view while feeding them to the capture.

      IT IS NOT SKY-SPECIFIC. RenderingReflectionCaptureMask is set for ORDINARY
      reflection captures too, so the Reflection branch also fires for any
      UReflectionCaptureComponent (box/sphere captures) that anyone adds later.
      This project ships none today. Whoever adds the first one should know that
      it will pick up starlight from this material, scaled by StarAmbientGain --
      harmless at gain 0, and at gain > 0 the fix is a second switch on
      View.RealTimeReflectionCapture (which the sky capture also sets to 1.0,
      ReflectionEnvironmentRealTimeCapture.cpp:574, and a box capture does not),
      not a change to this node.


================================================================================
DOMAIN / BLEND / SHADING MODEL, AND THE TRAP THAT TURNED OUT NOT TO BE ONE
================================================================================

    MaterialDomain  MD_Surface
    ShadingModel    MSM_Unlit
    BlendMode       BLEND_Opaque      -- required: Material.h:1088 says "Unlit and
                                         Opaque materials can be used as sky
                                         material on a sky dome mesh"
    TwoSided        true              -- stock /Engine/BasicShapes/Sphere, viewed
                                         from inside
    bIsSky          TRUE              -- the entire point of this material

create_sky_material.py:52-59 rejected bIsSky partly on the grounds that "an
OPAQUE dome is still eligible for the depth prepass" and would therefore risk
suppressing the atmosphere's twilight gradient. MEASURED AGAINST 5.8, THAT
PARTICULAR FEAR IS UNFOUNDED, and it is worth writing down because it is the one
thing that makes this material safe:

    inline bool ShouldIncludeMaterialInDefaultOpaquePass(const FMaterial& M)
    { return !M.IsSky() && !M.GetShadingModels().HasShadingModel(MSM_SingleLayerWater); }
                                            -- MaterialShared.h:3717-3721

and that predicate gates the depth prepass (DepthRendering.cpp:999), the default
opaque base pass (BasePassRendering.cpp:2040), shadow depth
(ShadowDepthRendering.cpp:2311) and velocity (VelocityRendering.cpp:939). So an
IsSky material renders in EMeshPass::SkyPass ONLY, and that pass is created with
depth writes explicitly masked off (CreateSkyPassProcessor,
SkyPassRendering.cpp:341-348). The dome writes NO depth, appears in NO prepass,
casts no shadow and emits no velocity. Sky depth stays at the far plane, so
height fog, aerial perspective on real geometry and TSR behave as they do today.
(Material.h:1088's other half -- "these meshes will not receive any contribution
from the aerial perspective" -- is implemented at
BasePassPixelShader.usf:1577's `MATERIAL_IS_SKY==0` guard and is correct here:
SkyAtmosphereViewLuminance already integrates the whole view ray.)

DEPTH TESTING IS STILL ON, though, and that gives this dome the SAME hard radius
requirement the star dome has, for a different reason. SkyPass uses
CF_DepthNearOrEqual, so a dome NEARER than a distant mountain passes the test and
PAINTS SKY OVER THE MOUNTAIN in the base-pass colour target. AVoxelSkyDomeActor
shares one radius (voxel.Sky.DomeRadiusUU, default 2e7 UU) between both domes and
BeginPlay measures it against AVoxelClipmapActor's real corner distance -- 92.7
km at the shipped 4096 m cascade, not the 50 km the older docs claim.

NO "APPLY FOGGING" DECISION TO MAKE: bUseTranslucencyVertexFog is a translucency
setting and this material is opaque.


================================================================================
WHAT COULD NOT BE VERIFIED WITHOUT THE EDITOR
================================================================================

The editor is single-occupancy and this task was write-only, so nothing below was
run:

1. That SkyAtmosphereViewLuminance's LUT sample matches the full-screen
   raymarched sky to under 2/255 AT TWILIGHT. This is the one uncertainty S1
   exists to settle (docs/sky-and-local-light-plan.md sec 7.2) and the gate is a
   live dome-on/off ladder, not a reading of this file.
2. That the three expression classes are constructible from Python under these
   exact names. The names come from the 5.8 headers; MaterialEditingLibrary
   creates any UMaterialExpression subclass, and every connect below raises on
   failure with the node's real pin list attached, so a wrong name fails on one
   line rather than shipping.
3. Whether SkyAtmosphereViewLuminance returns black when the SkyAtmosphere
   component has "Render in Main Pass" off: MaterialTemplate.ush:2372-2375 early-
   returns 0 unless RenderingReflectionCaptureMask != 0 or
   IsSkyAtmosphereRenderedInMain(EnvironmentComponentsFlags). UVoxelSkySubsystem
   never clears that flag, but if a future change does, this dome goes black
   while also suppressing the atmosphere pass -- the exact silent failure mode 1
   in the plan.
"""

import os
import sys

import unreal

# Shared sky graph, same directory. A -run=pythonscript commandlet does not put
# the script's own directory on sys.path, so add it explicitly (same as
# create_voxel_material.py:56-58).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sky_star_graph import (  # noqa: E402
    COLLECTION_PATH,
    PACKAGE_PATH,
    SkyGraphBuilder,
    build_horizon_fade,
    build_star_uv,
    build_view_direction,
    load_collection,
    sample_starmap,
)

MATERIAL_NAME = "M_SkyAtmosphereDome"
MATERIAL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME

# The atmosphere light index the SUN is on. UVoxelSkySubsystem puts the sun on 0
# and the moon on 1 (VoxelSkySubsystem.cpp:922-923 / :962-963), and the moon is
# deliberately NOT drawn here: its flat untextured atmosphere disc is suppressed
# with SetAtmosphereSunDiskColorScale(Black) at spawn and the textured, phased
# moon lives on M_NightSky. Adding it here would put two moons on screen.
SUN_ATMOSPHERE_LIGHT_INDEX = 0

# The one parameter this material needs that M_NightSky does not. Checked up
# front, by name, with an error that names the fix -- see "RUN ORDER IS
# MANDATORY" above. collection_param() would catch it too, but the message here
# can say WHY it is missing.
REQUIRED_NEW_PARAM = "StarAmbientGain"


def build_capture_only_stars(b, view, view_z, horizon_fade):
    """The star branch: the SHARED star map, gained for the SkyLight capture.

    Returns an RGB expression. It is wired to the Reflection pin of a
    ReflectionCapturePassSwitch by the caller, so it can never reach the main
    view -- doubled stars over the additive dome is silent failure mode 3 in the
    plan, and the switch is what makes it unreachable rather than merely unlikely.

    THE GAIN CHAIN, and every factor earns its place:

      StarBrightness    the SUNRISE FADE, driven every frame from sun altitude
                        by UVoxelSkySubsystem (VoxelSkySubsystem.cpp's
                        StarBrightnessForSunAltitude) and already folded with
                        voxel.Sky.StarGain's measured 0.15. Sharing it means the
                        ambient starlight fades out at dawn on exactly the same
                        curve the visible stars do, rather than on a second curve
                        that would have to be kept in sync.
      StarHorizonFade   see sky_star_graph.build_horizon_fade -- in the capture it
                        stops the cubemap integrating a star field over the LOWER
                        hemisphere, which would SH-project into light arriving
                        from under the ground.
      StarAmbientGain   the S1/S2 switch. 0.0 in S1.
    """
    uv, ddx_fixed, ddy_fixed = build_star_uv(b, view, view_z)
    star = sample_starmap(b, uv, ddx_fixed, ddy_fixed)

    gain = b.mul(
        b.mul(b.collection_param("StarBrightness"), horizon_fade),
        b.collection_param(REQUIRED_NEW_PARAM))
    return b.mul(star, gain, "RGB", "")


def build_atmosphere_sky(b):
    """SkyAtmosphereViewLuminance -- the sky this dome exists to keep painting.

    WorldDirection is left UNCONNECTED on purpose. FHLSLMaterialTranslator::
    SkyAtmosphereViewLuminance substitutes Mul(Constant3(-1,-1,-1), CameraVector)
    when the input is absent (HLSLMaterialTranslator.cpp:12369-12377), which is
    the gaze direction -- the same thing sky_star_graph.build_view_direction
    computes for the star UV. Wiring it explicitly would only create a second
    place for that sign to be wrong.
    """
    return b.node(unreal.MaterialExpressionSkyAtmosphereViewLuminance)


def build_sun_disk(b):
    """SkyAtmosphereLightDiskLuminance(light 0) -- the sun's own disc.

    Needed because the atmosphere's full-screen pass drew it and will not any
    more: with bRenderSkyPixel false, SkyAtmosphere.usf clips far-depth pixels
    before the disc is added (SkyAtmosphere.usf:970-992). Without this node the
    A/B ladder loses the sun from every daytime rung, which is the loudest
    possible version of this failure and still worth not having.

    No gating needed for the capture: the expression's own shader body is wrapped
    in `if (RenderingReflectionCaptureMask == 0.0f)`
    (MaterialTemplate.ush:2346-2348), so it is already black there.
    """
    n = b.node(unreal.MaterialExpressionSkyAtmosphereLightDiskLuminance)
    n.set_editor_property("light_index", SUN_ATMOSPHERE_LIGHT_INDEX)
    # Read back rather than assume. LightIndex is EditAnywhere/BlueprintReadWrite
    # on 5.8, but a silently-refused write here would draw the MOON's disc where
    # the sun should be -- which at night is nothing at all, i.e. invisible.
    got = int(n.get_editor_property("light_index"))
    if got != SUN_ATMOSPHERE_LIGHT_INDEX:
        raise RuntimeError(
            "SkyAtmosphereLightDiskLuminance.light_index did not round-trip: wrote %d, "
            "read back %d. Index 0 is the SUN (VoxelSkySubsystem.cpp:922-923); index 1 is "
            "the moon, whose disc must stay black here."
            % (SUN_ATMOSPHERE_LIGHT_INDEX, got))
    return n


def build_pass_switch(b, reflection_rgb):
    """ReflectionCapturePassSwitch(Default=black, Reflection=stars).

    Default is an EXPLICIT Constant3Vector(0,0,0) rather than an unconnected pin:
    UMaterialExpressionReflectionCapturePassSwitch::Compile emits
    Errorf("Missing input Default") when either input is dangling
    (MaterialExpressions.cpp:21612-21629), so leaving it out is a compile failure,
    not a zero.
    """
    n = b.node(unreal.MaterialExpressionReflectionCapturePassSwitch)
    b.link(b.const3(0.0, 0.0, 0.0), "", n, "Default")
    b.link(reflection_rgb, "", n, "Reflection")
    return n


def main():
    # READ the collection; do not author it. See "RUN ORDER IS MANDATORY".
    collection = load_collection()

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(MATERIAL_PATH)

    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material,
                                        unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("Failed to create material asset at " + MATERIAL_PATH)

    # --- domain / blend / shading ------------------------------------------
    #
    # OPAQUE + UNLIT + bIsSky is the combination Material.h:1088 names as the one
    # that may be used on a sky dome. See the module docstring for why the
    # depth-prepass fear that kept M_NightSky off this path does not apply:
    # ShouldIncludeMaterialInDefaultOpaquePass (MaterialShared.h:3717-3721)
    # excludes IsSky materials from the prepass, the opaque base pass, shadow
    # depth and velocity, leaving only EMeshPass::SkyPass with depth writes
    # masked.
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("is_sky", True)
    # Belt and braces: IsSky already keeps this out of ShadowDepthRendering
    # (ShadowDepthRendering.cpp:2311), and AVoxelSkyDomeActor calls
    # SetCastShadow(false) on the component. A 200 km sphere that ever did cast
    # would shadow the entire world, so all three say so.
    material.set_editor_property("cast_ray_traced_shadows", False)

    b = SkyGraphBuilder(material, collection)

    # Fail on the NEW parameter first, with the ordering error spelled out. Every
    # other name here has been on the MPC since M_NightSky shipped, so a miss on
    # this one specifically means create_sky_material.py is stale or was re-run
    # from an older revision -- which is a different fix from a typo.
    if REQUIRED_NEW_PARAM not in b.mpc_names():
        raise RuntimeError(
            "%s has no %r parameter. Present: %s. It is declared in "
            "Tools/create_sky_material.py's SCALAR_PARAMS (default 0.0) because that "
            "script DELETES AND RECREATES the collection, so it must own every "
            "parameter. Re-run Tools/create_sky_material.py, THEN this script. "
            "Without the parameter every CollectionParameter node compiles to a "
            "constant and nothing anywhere reports it "
            "(MaterialExpressions.cpp:17179-17193)."
            % (COLLECTION_PATH, REQUIRED_NEW_PARAM, sorted(b.mpc_names())))

    # --- the shared view basis ---------------------------------------------
    view, view_z = build_view_direction(b)
    horizon_fade = build_horizon_fade(b, view_z)

    # --- the three terms ----------------------------------------------------
    sky = build_atmosphere_sky(b)
    sun_disk = build_sun_disk(b)
    stars = build_capture_only_stars(b, view, view_z, horizon_fade)
    capture_only = build_pass_switch(b, stars)

    emissive = b.add(b.add(sky, sun_disk), capture_only)
    b.prop(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    b.mel.layout_material_expressions(material)
    b.mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_SkyAtmosphereDome created and saved at " + MATERIAL_PATH)
    unreal.log(
        "S1 SHIPS THE STAR BRANCH GAINED TO ZERO. %s.StarAmbientGain defaults to 0.0 and "
        "voxel.Sky.StarAmbientGain (which overwrites it every frame) defaults to 0, so this "
        "material's ONLY job right now is to paint the main-view sky exactly as the "
        "SkyAtmosphere pass does today -- because the moment this dome is in the scene, "
        "View.bSceneHasSkyMaterial goes true and that pass stops painting sky pixels "
        "(SkyAtmosphereRendering.cpp:2214). Verify with the dome-on/off ladder before "
        "believing anything else about it: -VoxelSkyLadder=N -VoxelSkyLadderAltCvar="
        "voxel.Sky.AtmosphereDome." % COLLECTION_PATH)


main()
