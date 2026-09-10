"""Add the opt-in surface support collection without replacing existing assets/IDs.
Run before rebuilding the two consuming materials in a coordinated editor slot.
"""
import unreal
from voxel_surface_lighting_common import validate_collection
PATH='/Game/Voxel/MPC_VoxelSurfaceLighting'
collection=unreal.load_asset(PATH)
if not collection:
    collection=unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'MPC_VoxelSurfaceLighting','/Game/Voxel',unreal.MaterialParameterCollection,
        unreal.MaterialParameterCollectionFactoryNew())
if not collection: raise RuntimeError('Cannot create surface lighting collection')
scalars,vectors=validate_collection(collection,require_complete=False)
snames={str(p.get_editor_property('parameter_name')).casefold() for p in scalars}
vnames={str(p.get_editor_property('parameter_name')).casefold() for p in vectors}
changed=False
for p in scalars:
    if str(p.get_editor_property('parameter_name')).casefold()=='enabled' and p.get_editor_property('default_value')!=0.:
        p.set_editor_property('default_value',0.);changed=True
if 'enabled' not in snames:
    p=unreal.CollectionScalarParameter();p.set_editor_property('parameter_name','Enabled');p.set_editor_property('default_value',0.)
    scalars.append(p);changed=True
for name,value in {'AmbientSkyAndGround':(0,0,0,0),'SunDirAndWrapFloor':(0,0,1,-1),'WrapColorAndSkyBoost':(0,0,0,0)}.items():
    if name.casefold() in snames: raise RuntimeError(name+' must be vector')
    if name.casefold() not in vnames:
        p=unreal.CollectionVectorParameter();p.set_editor_property('parameter_name',name);p.set_editor_property('default_value',unreal.LinearColor(*value))
        vectors.append(p);changed=True
if changed:
    collection.set_editor_property('scalar_parameters',scalars)
    collection.set_editor_property('vector_parameters',vectors)
    validate_collection(collection)
    if not unreal.EditorAssetLibrary.save_loaded_asset(collection):
        raise RuntimeError('Failed to save surface lighting collection')
validate_collection(collection)
unreal.log('Surface lighting collection maintained; existing parameter IDs preserved')
