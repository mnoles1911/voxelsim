"""Author M_Ocean (water track W1, docs/voxel-earth-implementation-plan.md
SS3.7 "Implicit static" water: shader surface + underwater fog, ZERO voxel
water data). Same headless-Python pattern as create_voxel_material.py
(checked connections -- see that file's comment on why: a silently-failed
pin connect produced an invisible-terrain material once).

Basic translucent water: flat deep blue-green tint, ~0.8 opacity, low
roughness, and a small tangent-space normal perturbation driven by two
independent panning sine waves sampled from absolute world position (NOT
mesh-local vertex UVs -- see AVoxelOceanActor.cpp's UpdateFollowPlane comment
on why that keeps the follow-plane recenter from causing texture swim).
Deliberately basic -- real wave shading/foam/caustics are W5 polish.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash
"""

import unreal

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_Ocean"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME


def main():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
        unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material, factory)
    if material is None:
        raise RuntimeError("Failed to create material asset at " + FULL_PATH)

    # Translucent so the ocean plane reads as water rather than an opaque
    # sheet; two-sided so it's visible both from above AND from below once
    # swimming (docs spec: "camera-following ocean surface").
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("two_sided", True)

    mel = unreal.MaterialEditingLibrary

    # --- Base color / opacity / roughness ---------------------------------
    base_color = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -500, -300)
    base_color.set_editor_property("constant", unreal.LinearColor(0.015, 0.12, 0.16, 1.0))  # deep blue-green
    if not mel.connect_material_property(base_color, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect base_color -> BaseColor failed")

    opacity = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -500, -150)
    opacity.set_editor_property("r", 0.8)
    if not mel.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY):
        raise RuntimeError("connect opacity -> Opacity failed")

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -500, -50)
    roughness.set_editor_property("r", 0.05)
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness -> Roughness failed")

    # --- Slight normal perturbation: two independent panning sine waves ---
    # Each channel: mask one world-position axis -> scale by a frequency ->
    # add a panning (Time * speed) offset -> Sine -> scale by a small
    # strength. The two channels use different masks/frequencies/speeds so
    # the result isn't a single uniform ripple. Not run through Normalize
    # afterwards: the perturbation is small relative to a Z of 1.0, so the
    # slight off-unit length is imperceptible for this placeholder.
    world_pos = mel.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -900, 250)
    time_node = mel.create_material_expression(material, unreal.MaterialExpressionTime, -900, 550)

    def make_wave_channel(mask_r, mask_g, freq, speed, y):
        mask = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -700, y)
        mask.set_editor_property("r", mask_r)
        mask.set_editor_property("g", mask_g)
        mask.set_editor_property("b", False)
        mask.set_editor_property("a", False)
        # "" targets the expression's first (and, for ComponentMask, only)
        # input pin regardless of its display name (MaterialEditingLibrary
        # treats an empty ToInputName as "first input" -- see
        # GetExpressionInputByName in MaterialEditingLibrary.cpp).
        if not mel.connect_material_expressions(world_pos, "", mask, ""):
            raise RuntimeError("connect world_pos -> mask.Input failed")

        freq_const = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -700, y + 90)
        freq_const.set_editor_property("r", freq)
        scaled = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -550, y)
        if not mel.connect_material_expressions(mask, "", scaled, "A"):
            raise RuntimeError("connect mask -> scaled.A failed")
        if not mel.connect_material_expressions(freq_const, "", scaled, "B"):
            raise RuntimeError("connect freq_const -> scaled.B failed")

        speed_const = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -550, y + 170)
        speed_const.set_editor_property("r", speed)
        panned_time = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -400, y + 170)
        if not mel.connect_material_expressions(time_node, "", panned_time, "A"):
            raise RuntimeError("connect time -> panned_time.A failed")
        if not mel.connect_material_expressions(speed_const, "", panned_time, "B"):
            raise RuntimeError("connect speed_const -> panned_time.B failed")

        panned = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -250, y)
        if not mel.connect_material_expressions(scaled, "", panned, "A"):
            raise RuntimeError("connect scaled -> panned.A failed")
        if not mel.connect_material_expressions(panned_time, "", panned, "B"):
            raise RuntimeError("connect panned_time -> panned.B failed")

        sine = mel.create_material_expression(material, unreal.MaterialExpressionSine, -100, y)
        if not mel.connect_material_expressions(panned, "", sine, ""):  # single input, see mask.Input comment above
            raise RuntimeError("connect panned -> sine.Input failed")

        strength_const = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -100, y + 90)
        strength_const.set_editor_property("r", 0.04)
        scaled_sine = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 60, y)
        if not mel.connect_material_expressions(sine, "", scaled_sine, "A"):
            raise RuntimeError("connect sine -> scaled_sine.A failed")
        if not mel.connect_material_expressions(strength_const, "", scaled_sine, "B"):
            raise RuntimeError("connect strength_const -> scaled_sine.B failed")

        return scaled_sine

    # X-perturbation driven by world Y; Y-perturbation driven by world X --
    # different frequency/speed per channel so the two waves aren't in lockstep.
    wave_x = make_wave_channel(False, True, 0.015, 0.15, 100)
    wave_y = make_wave_channel(True, False, 0.021, -0.11, 400)

    append_xy = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, 220, 100)
    if not mel.connect_material_expressions(wave_x, "", append_xy, "A"):
        raise RuntimeError("connect wave_x -> append_xy.A failed")
    if not mel.connect_material_expressions(wave_y, "", append_xy, "B"):
        raise RuntimeError("connect wave_y -> append_xy.B failed")

    z_const = mel.create_material_expression(material, unreal.MaterialExpressionConstant, 220, 400)
    z_const.set_editor_property("r", 1.0)

    append_xyz = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, 400, 250)
    if not mel.connect_material_expressions(append_xy, "", append_xyz, "A"):
        raise RuntimeError("connect append_xy -> append_xyz.A failed")
    if not mel.connect_material_expressions(z_const, "", append_xyz, "B"):
        raise RuntimeError("connect z_const -> append_xyz.B failed")

    if not mel.connect_material_property(append_xyz, "", unreal.MaterialProperty.MP_NORMAL):
        raise RuntimeError("connect append_xyz -> Normal failed")

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("M_Ocean created and saved at " + FULL_PATH)


main()
