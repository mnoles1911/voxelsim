"""Exercise persisted decisions across fresh Python/server module sessions."""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import _path
from forge import inventory, server, spec

root=Path(__file__).resolve().parents[1]
name='tundra-pine'
with tempfile.TemporaryDirectory(prefix='variant-decisions-',dir=root/'out') as tmp:
    fixture=Path(tmp)
    (fixture/'specs').mkdir()
    shutil.copy2(root/'specs'/f'{name}.json',fixture/'specs'/f'{name}.json')
    for seed in (1,2):
        entry=f'{name}-{seed:04d}'
        src=root/'out/forge-candidates'/name/entry
        if not src.exists():src=root/'library'/name/entry
        dest=fixture/'out/forge-candidates'/name/entry
        shutil.copytree(src,dest)
        meta=json.loads((dest/'meta.json').read_text())
        meta.update(inventory_candidate=True,visual_approved=False,review_status='inventory_candidate')
        inventory.write_json(dest/'meta.json',meta)
    server.ROOT=fixture;server.LIBRARY=fixture/'library';server.SPECS=fixture/'specs'
    inventory.promote(f'{name}-0001')
    inventory.reject(f'{name}-0002')
    code=r'''
import json,sys
from pathlib import Path
from forge import inventory,server,spec
root=Path(sys.argv[1]);name='tundra-pine'
server.ROOT=inventory.ROOT=root
server.LIBRARY=root/'library';server.SPECS=root/'specs'
inventory.CANDIDATES=root/'out/forge-candidates'
assert [m['id'] for m in server.library_list()]==[name+'-0001']
assert [m['id'] for m in server.library_list(True)]==[name+'-0001']
body,_=spec.load(server.SPECS/(name+'.json'))
result=inventory.build_one((name,body,2,inventory.generator_digest(),str(inventory.CANDIDATES),'persistence-probe'))
assert result['status']=='cached'
assert [m['id'] for m in server.library_list(True)]==[name+'-0001']
try:inventory.promote(name+'-0002')
except ValueError:pass
else:raise AssertionError('Rejected seed endorsed by stale request')
inventory.reject(name+'-0001')
assert not server.library_list(True)
assert json.loads((server.SPECS/(name+'.json')).read_text())['curation']['status']=='draft'
'''
    # generator_digest uses the actual source tree, independently of fixture ROOT.
    code=code.replace("inventory.generator_digest()",repr(inventory.generator_digest()))
    subprocess.run([sys.executable,'-c',code,str(fixture)],cwd=root,check=True)
    assert not server.library_list(True)
    print('PASS: endorsement and rejection persist in fresh session; cached regeneration stays hidden; rejected endorsement refused; unendorsing updates publication seeds')
