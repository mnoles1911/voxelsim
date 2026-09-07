"""Update vegetation materials in place without recreating the sky collection."""
from pathlib import Path
import runpy
import sys
import unreal

tools = Path(__file__).resolve().parent
sys.path.insert(0, str(tools))
for script in ('create_detail_asset_material.py', 'create_environment_lod_material.py'):
    runpy.run_path(str(tools / script), run_name='__main__')
unreal.log('VEGETATION_MATERIALS_COMPLETE')
runpy.run_path(str(tools / 'validate_vegetation_materials.py'), run_name='__main__')
