"""Endorsement-gated mixed VAC1/VAC2 publication; no implicit world activation."""
import json,hashlib
from pathlib import Path
if not __package__:import _path
from forge import inventory
from tools.temperate_appearance import in_scope,install
from tools.export_temperate_runtime_appearance import ensure

def publish_endorsed(source,output):
    source=Path(source).resolve();source.relative_to((inventory.ROOT/'library').resolve());output=Path(output)
    meta=json.loads((source/'meta.json').read_text());body=json.loads((source/'spec.json').read_text())
    if meta.get('inventory_candidate') or meta.get('review_status')!='endorsed' or not meta.get('visual_approved'):raise ValueError('Explicitly endorsed visual asset required')
    if body.get('kind')=='tree':
        from tools.export_tree_runtime_appearance import publish_endorsed as tree_publish
        return tree_publish(source,output)
    if not in_scope(body):return None
    install(source);row=ensure(source);row=dict(row,id=meta['id'],species=meta['species'],seed=meta['seed'])
    if inventory.digest(source/'tree.vxa')!=row['geometry_sha256']:raise ValueError('Geometry changed before publication')
    output.mkdir(parents=True,exist_ok=True);dest=output/(row['geometry_md5']+'.vac');blob=(source/'tree-appearance-runtime.vac').read_bytes()
    if not dest.exists() or inventory.digest(dest)!=row['sha256']:
        temp=dest.with_suffix('.tmp');temp.write_bytes(blob);temp.replace(dest)
    row['file']=dest.name;inventory.write_json(output/(meta['id']+'.json'),row);return row

def refresh_published_inventory(output,banks):
    output=Path(output);banks=Path(banks);rows=[];unavailable=[]
    for p in sorted((inventory.ROOT/'library').glob('*/*/meta.json')):
        meta=json.loads(p.read_text())
        if meta.get('inventory_candidate') or meta.get('review_status')!='endorsed' or not meta.get('visual_approved'):continue
        body=json.loads((p.parent/'spec.json').read_text())
        if not in_scope(body):continue
        bank=banks/meta['species']/(meta['id']+'.vxa')
        if not bank.is_file() or inventory.digest(bank)!=inventory.digest(p.parent/'tree.vxa'):unavailable.append(meta['id']);continue
        row=publish_endorsed(p.parent,output)
        if row:rows.append(row)
    report=dict(models=rows,count=len(rows),unavailable_banks=unavailable)
    inventory.write_json(output/'published.json',report);return report
