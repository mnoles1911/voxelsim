"""E1 probe: what does a SECOND texture coordinate actually carry on the
COMPONENT render path?

docs/gpu-g4-parity-plan.md item 2 and docs/gpu-waves-plan.md E1 both assert:

    "The component path's vertex factory supplies only a float2 texture
     coordinate, so TexCoord0.zw arrives as zero there no matter what the
     graph does."

and the whole "encode identity as ZERO" safety argument rests on that.

This script edits M_VoxelTerrain so that EmissiveColor = abs(TexCoord1) * 0.05.
BaseColor is untouched, so the scene stays recognisable and the emissive term
is purely additive:

  * if TexCoord1 really is zero on the component path -> emissive is black and
    the frame is indistinguishable from the control.
  * if TexCoord1 is anything else -> the frame is visibly brighter/banded.

Run:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> \
    -unattended -nop4 -nosplash

Revert afterwards with: git checkout -- ue-project/Content/Voxel/M_VoxelTerrain.uasset
"""

import unreal

FULL_PATH = "/Game/Voxel/M_VoxelTerrain"

material = unreal.load_object(None, FULL_PATH + "." + "M_VoxelTerrain")
if material is None:
    raise RuntimeError("failed to load " + FULL_PATH)

mel = unreal.MaterialEditingLibrary

tc1 = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, 900)
tc1.set_editor_property("coordinate_index", 1)

zero = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -900, 1000)
zero.set_editor_property("r", 0.0)

append = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, -700, 900)
if not mel.connect_material_expressions(tc1, "", append, "A"):
    raise RuntimeError("connect tc1 -> append.A failed")
if not mel.connect_material_expressions(zero, "", append, "B"):
    raise RuntimeError("connect zero -> append.B failed")

abs_node = mel.create_material_expression(material, unreal.MaterialExpressionAbs, -550, 900)
if not mel.connect_material_expressions(append, "", abs_node, ""):
    raise RuntimeError("connect append -> abs failed")

scale = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -550, 1000)
scale.set_editor_property("r", 0.05)

mul = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -400, 900)
if not mel.connect_material_expressions(abs_node, "", mul, "A"):
    raise RuntimeError("connect abs -> mul.A failed")
if not mel.connect_material_expressions(scale, "", mul, "B"):
    raise RuntimeError("connect scale -> mul.B failed")

if not mel.connect_material_property(mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
    raise RuntimeError("connect mul -> EmissiveColor failed")

mel.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log("E1 PROBE: M_VoxelTerrain EmissiveColor = abs(TexCoord1) * 0.05")
