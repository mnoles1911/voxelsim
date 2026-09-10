"""Refresh final collection metadata and assert active inventory coverage."""
import json,time
from pathlib import Path
import _path
from forge import inventory,species_registry
from forge.understory_profiles import PROFILES
ROOT=Path(__file__).resolve().parents[1]
def read(p):return json.loads(p.read_text())
def main():
 catalog_path=ROOT/'out/understory-review/references.json'
 catalog=read(catalog_path)
 for r in catalog:
  r['architecture']=PROFILES[r['species']]['architecture']
  if r['species']=='ivy-ground-layer':
   r['photos']=[p for p in r['photos'] if 'Donald_Hobern' in p['url']]
   assert len(r['photos'])==1
   p=ROOT/'library/ivy-ground-layer/reference-review.json';review=read(p)
   review['reference_catalog_photos']=r['photos'];inventory.write_json(p,review)
 inventory.write_json(catalog_path,catalog)
 rows=[]
 for n in PROFILES:
  record=species_registry.read(n);assert record['generator_approved'],n
  expected={f'{n}-{seed:04d}' for seed in range(1,37)};actual=[];orphans=[]
  for base in (ROOT/'library',inventory.CANDIDATES):
   for folder in (base/n).glob('*'):
    if not folder.is_dir():continue
    if (folder/'meta.json').exists():actual.append(folder.name)
    elif (folder/'tree.vxa').exists():orphans.append(str(folder))
  assert set(actual)==expected and len(actual)==36 and not orphans,(n,actual,orphans)
  assert record['reference_variant_id']==f'{n}-0007',n
  rows.append(dict(species=n,active_variants=len(actual),orphan_assets=orphans))
 report=dict(timestamp=time.time(),profiles=len(rows),active_variants=sum(r['active_variants'] for r in rows),old_active_assets=0,orphan_assets=0,rows=rows)
 inventory.write_json(ROOT/'out/understory-collection/active-inventory-audit.json',report)
 checkpoint=dict(updated_at=time.time(),accepted_count=len(rows),pending_variants=len(rows)*36,accepted=sorted(PROFILES),remaining=[],status='Complete: all sources accepted; variants pending user endorsement',server_note='Final reload required; record live verification separately')
 inventory.write_json(ROOT/'out/understory-review/checkpoint.json',checkpoint)
 print({k:v for k,v in report.items() if k!='rows'})
if __name__=='__main__':main()
