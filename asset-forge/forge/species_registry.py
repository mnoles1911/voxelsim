"""Library-owned species baseline and reference variant records."""
import json
from pathlib import Path
from . import spec

ROOT=Path(__file__).resolve().parents[1]

def reference_review(name):
    if Path(name).name != name:return None
    path=ROOT/'library'/name/'reference-review.json'
    return json.loads(path.read_text(encoding='utf8')) if path.exists() else None

def approve(name):
    from . import inventory
    record=read(name)
    if not record or not record['baseline_current'] or not record['reference_available']:
        raise ValueError('Select a saved reference using the current species baseline first')
    record.update(generator_approved=True,approved_generator_digest=inventory.generator_digest(record['baseline_spec']))
    for key in ['reference_available','baseline_current']:record.pop(key,None)
    inventory.write_json(ROOT/'library'/name/'species.json',record)
    return read(name)

def read(name):
    if Path(name).name != name:return None
    path=ROOT/'library'/name/'species.json'
    if not path.exists():return None
    record=json.loads(path.read_text(encoding='utf8'))
    entry=ROOT/'library'/name/record['reference_variant_id']
    pending=ROOT/'out/forge-candidates'/name/record['reference_variant_id']
    record['reference_available']=(entry/'tree.vxa').is_file() or (pending/'tree.vxa').is_file()
    current,_=spec.load(ROOT/'specs'/f'{name}.json')
    record['baseline_current']=record['baseline_spec_hash']==spec.spec_hash(current)
    if record.get('generator_approved'):
        from . import inventory
        record['generator_approved']=record['baseline_current'] and record.get('approved_generator_digest')==inventory.generator_digest(current)
    return record

def designate(entry_id):
    from . import inventory
    matches=[*(ROOT/'library').glob(f'*/{Path(entry_id).name}/meta.json'),*(ROOT/'out/forge-candidates').glob(f'*/{Path(entry_id).name}/meta.json')]
    if len(matches)!=1:raise ValueError('Saved variant not found')
    path=matches[0];meta=json.loads(path.read_text())
    if meta.get('imported'):raise ValueError('A procedural species requires a generated reference variant')
    saved,_=spec.load(path.parent/'spec.json')
    current,_=spec.load(ROOT/'specs'/f"{meta['species']}.json")
    if spec.spec_hash(saved)!=spec.spec_hash(current):raise ValueError('Reference must use the current species baseline')
    for filename,expected in meta.get('artifact_hashes',{}).items():
        if inventory.digest(path.parent/filename)!=expected:raise ValueError('Reference artifact verification failed')
    if not (path.parent/'tree.vxa').is_file():raise ValueError('Reference geometry missing')
    existing=read(meta['species']) or {}
    if existing.get('reference_variant_id')!=meta['id'] or not existing.get('baseline_current'):
        existing['generator_approved']=False
    record=dict(existing,schema_version=1,species=meta['species'],baseline_spec=saved,
        baseline_spec_hash=spec.spec_hash(saved),reference_variant_id=meta['id'],reference_seed=meta['seed'],
        generator=meta['stats'].get('generator'),generator_digest=meta.get('generator_digest'),
        semantics='species baseline + generator revision + numbered seed = variant',
        reference_selection='authoritative design reference; separate from world approval')
    for key in ['reference_available','baseline_current']:record.pop(key,None)
    inventory.write_json(ROOT/'library'/meta['species']/'species.json',record)
    return read(meta['species'])
