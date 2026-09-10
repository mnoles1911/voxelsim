"""Prove a saved endorsed tree reaches the game bank without regeneration."""
import json
import shutil
import sys
import tempfile
from pathlib import Path
import _path
import export_banks
from forge import inventory, pipeline, spec, manifest
import library_layers

root=Path(__file__).resolve().parents[1]
seed=7
for name in ('american-beech','hero-sequoia'):
 with tempfile.TemporaryDirectory(prefix='library-export-probe-',dir=root/'out') as d:
    fixture=Path(d).resolve()
    assert fixture.parent==(root/'out').resolve()
    (fixture/'specs').mkdir()
    body,_=spec.load(root/'specs'/f'{name}.json')
    # Exercise oversized geometry independently of the landmark's intentionally
    # sparse world placement, which otherwise rounds to zero scatter weight.
    if name=='hero-sequoia':body['placement']['spacing_m']=32
    body['curation']={'status':'approved','seeds':[seed],'notes':'Temporary verification fixture'}
    spec.save(body,fixture/'specs'/f'{name}.json')
    src=root/'out/forge-candidates'/name/f'{name}-{seed:04d}'
    if not src.exists():src=root/'library'/name/f'{name}-{seed:04d}'
    dest=fixture/'library'/name/src.name
    shutil.copytree(src,dest)
    meta=json.loads((dest/'meta.json').read_text());meta['inventory_candidate']=False
    inventory.write_json(dest/'meta.json',meta)
    shutil.copy2(root/'library'/name/'species.json',dest.parent/'species.json')
    export_banks.ROOT=fixture;export_banks.SPECS=fixture/'specs'
    def forbidden(*args,**kwargs):raise AssertionError('Endorsed geometry must not be regenerated')
    pipeline.build=forbidden
    sys.argv=['export_banks','--only',name,'--out',str(fixture/'banks')]
    result=export_banks.main()
    output=fixture/'banks'/name/f'{name}-{seed:04d}.vxa'
    assert result==0 and output.read_bytes()==(src/'tree.vxa').read_bytes()
    layers=library_layers.configure(fixture)
    assert library_layers.configure(fixture)==layers
    assert manifest.assign_layer('tree',100,manifest.ExportReport(),'unrelated-legacy',50)==-1
    print('PASS:',name,'exact endorsed bytes, measured bounds, stable legacy filing and repeat configuration')
