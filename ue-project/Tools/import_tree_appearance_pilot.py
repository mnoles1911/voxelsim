"""Import the isolated 24-model appearance comparison; no world placement changes.

Run in a free Unreal editor/commandlet session after the GLB preparation tool.
Only /Game/Voxel/AppearancePilot/V1 assets are created or updated.
The imported models require visual color-space and performance verification.
"""
from pathlib import Path
import json
import os
import hashlib
import unreal
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))

ROOT=Path(unreal.Paths.project_dir()).resolve().parent
SOURCE=Path(os.environ.get('TREE_APPEARANCE_EXPORT_DIR',str(ROOT/'asset-forge/out/tree-appearance-pilot/game-v2')))
DEST='/Game/Voxel/AppearancePilot/V2'
from tree_foliage_mask import SHAPED_MASK_BODY
MASK=SHAPED_MASK_BODY+"\nreturn FoliageCoverage;"

def material(name,needle=None):
    path=DEST+'/'+name
    m=unreal.load_asset(path)
    if not m:m=unreal.AssetToolsHelpers.get_asset_tools().create_asset(name,DEST,unreal.Material,unreal.MaterialFactoryNew())
    if not m:raise RuntimeError('Could not create '+path)
    mel=unreal.MaterialEditingLibrary;mel.delete_all_material_expressions(m)
    def node(cls):return mel.create_material_expression(m,cls)
    color=node(unreal.MaterialExpressionVertexColor)
    # GLTFMeshFactory.cpp counter-converts colors before StaticMeshBuilder's
    # sRGB encoding, preserving the glTF linear COLOR_0 values in vertex bytes.
    # An extra gamma decode here incorrectly darkens the imported foliage.
    assert mel.connect_material_property(color,'',unreal.MaterialProperty.MP_BASE_COLOR)
    rough=node(unreal.MaterialExpressionConstant);rough.set_editor_property('r',.9)
    assert mel.connect_material_property(rough,'',unreal.MaterialProperty.MP_ROUGHNESS)
    m.set_editor_property('used_with_instanced_static_meshes',True)
    m.set_editor_property('used_with_nanite',True)
    m.set_editor_property('two_sided',needle is not None)
    m.set_editor_property('blend_mode',unreal.BlendMode.BLEND_OPAQUE if needle is None else unreal.BlendMode.BLEND_MASKED)
    if needle is not None:
        uv=node(unreal.MaterialExpressionTextureCoordinate)
        opening=node(unreal.MaterialExpressionScalarParameter);opening.set_editor_property('parameter_name','Opening');opening.set_editor_property('default_value',.45)
        n=node(unreal.MaterialExpressionConstant);n.set_editor_property('r',float(needle))
        mask=node(unreal.MaterialExpressionCustom);mask.set_editor_property('code',MASK);mask.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        inputs=[]
        for key in ('UV','Needle','Opening'):
            item=unreal.CustomInput();item.set_editor_property('input_name',key);inputs.append(item)
        mask.set_editor_property('inputs',inputs)
        for src,key in ((uv,'UV'),(n,'Needle'),(opening,'Opening')):assert mel.connect_material_expressions(src,'',mask,key)
        assert mel.connect_material_property(mask,'',unreal.MaterialProperty.MP_OPACITY_MASK)
        m.set_editor_property('opacity_mask_clip_value',.5)
    mel.layout_material_expressions(m);mel.recompile_material(m)
    assert unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m

def main():
    manifest=json.loads((SOURCE/'manifest.json').read_text())
    assert len(manifest['models'])==24
    wood=material('M_Solid');leaf=material('M_Leaf',False);needle=material('M_Needle',True)
    report=[]
    for row in manifest['models']:
        only=os.environ.get('TREE_APPEARANCE_ONLY','')
        if only and row['id'] not in only.split(','):continue
        source=SOURCE/(row['id']+'.glb')
        assert hashlib.sha256(source.read_bytes()).hexdigest()==row['sha256'], 'Stale export '+row['id']
        task=unreal.AssetImportTask()
        task.set_editor_property('filename',str(SOURCE/(row['id']+'.glb')))
        task.set_editor_property('destination_path',DEST+'/'+row['id'].replace('-','_'))
        task.set_editor_property('automated',True);task.set_editor_property('replace_existing',True);task.set_editor_property('save',True)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        meshes=[]
        slots=[]
        for path in task.get_editor_property('imported_object_paths'):
            mesh=unreal.load_asset(path)
            if not isinstance(mesh,unreal.StaticMesh):continue
            for i,slot in enumerate(mesh.get_editor_property('static_materials')):
                slot_name=str(slot.get_editor_property('material_slot_name')).lower()
                slots.append(slot_name)
                mesh.set_material(i,(needle if row['needle'] else leaf) if 'foliage' in slot_name else wood)
            assert unreal.EditorAssetLibrary.save_loaded_asset(mesh)
            meshes.append(path)
        if not meshes:raise RuntimeError('No imported static mesh for '+row['id'])
        assert any('foliage' in s for s in slots), 'No foliage material slot: '+str(slots)
        assert any('solidwood' in s for s in slots), 'No wood material slot: '+str(slots)
        report.append(dict(id=row['id'],meshes=meshes,slots=slots,sha256=row['sha256']))
        (SOURCE/'unreal-import-progress.json').write_text(json.dumps(dict(models=report),indent=2))
    (SOURCE/'unreal-import-report.json').write_text(json.dumps(dict(status='imported; visual and GPU verification pending',models=report),indent=2))
    unreal.log('TREE_APPEARANCE_IMPORT_COMPLETE:'+str(len(report)))

if __name__=='__main__':main()
