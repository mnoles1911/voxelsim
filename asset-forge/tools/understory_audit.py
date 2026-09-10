"""Audit research staging, distinct geometry and preserved tree authority."""
import json
from pathlib import Path
import _path
from forge import inventory,species_registry,pipeline,vxa
from forge.forest_profiles import PROFILES as TREES
from forge.understory_profiles import PROFILES
import numpy as np
ROOT=Path(__file__).resolve().parents[1]

def variant_path(name,seed):
 bases=(ROOT/'library',inventory.CANDIDATES,ROOT/'out/understory-collection/variants') if (ROOT/'library'/name/'species.json').exists() else (ROOT/'out/understory-collection/variants',ROOT/'library',inventory.CANDIDATES)
 for base in bases:
  path=base/name/f'{name}-{seed:04d}'/'meta.json'
  if path.exists():return path
 raise FileNotFoundError(f'{name} seed {seed}')
def main():
 root=ROOT/'out/understory-collection';rows=[];code=None
 for p in sorted(root.glob('*.json')):
  item=json.loads(p.read_text())
  if 'species' not in item or 'spec' not in item:continue
  code=inventory.generator_digest(item['spec'])
  name=item['species'];metas=[];errors=[]
  for seed in range(1,37):
   path=variant_path(name,seed)
   m=json.loads(path.read_text());metas.append(m)
   for f,sha in m['artifact_hashes'].items():
    if inventory.digest(path.parent/f)!=sha:errors.append(m['id']+'/'+f)
  rows.append(dict(species=name,count=len(metas),distinct_geometry=len({m['artifact_hashes']['tree.vxa'] for m in metas}),current=item['generator_digest']==code,seconds=item['seconds'],failures=item['failures'],artifact_errors=errors,all_25mm=all(m['stats']['voxel_cm']==2.5 for m in metas),max_grid_mb=max(m['stats']['grid_mb'] for m in metas)))
 deterministic=[]
 for name in ('hazel-coppice','male-fern','primrose','meadow-grass','broadleaf-cattail'):
  item=json.loads((root/f'{name}.json').read_text());saved=vxa.read(variant_path(name,7).parent/'tree.vxa');built=pipeline.build(item['spec'],7)
  assert np.array_equal(saved.data,built.grid.data) and np.array_equal(saved.origin,built.grid.origin) and saved.voxel_m==built.grid.voxel_m,name
  deterministic.append(name)
 tree_digest=inventory.generator_digest();old=json.loads((ROOT/'out/understory-review/legacy-generator.json').read_text())
 assert tree_digest==old['digest'];assert all(species_registry.read(n)['generator_approved'] for n in TREES)
 accepted=[n for n in PROFILES if (ROOT/'library'/n/'species.json').exists() and species_registry.read(n).get('generator_approved')]
 decisions=[json.loads(p.read_text()).get('review_status') for n in PROFILES for base in (ROOT/'library',inventory.CANDIDATES) for p in (base/n).glob('*/meta.json')]
 retired=sum(len(json.loads(p.read_text()).get('retired',[])) for p in root.glob('*-retirement.json'))
 report=dict(profiles=len(rows),variants=sum(r['count'] for r in rows),seconds=round(sum(r['seconds'] for r in rows),2),paid_generation_api_calls=0,accepted_sources=len(accepted),accepted_species=accepted,endorsements=decisions.count('endorsed'),rejections=decisions.count('rejected'),old_assets_removed=retired,preserved_accepted_tree_sources=len(TREES),deterministic_rebuilds=deterministic,rows=rows,status='All scoped sources reviewed and installed; variant endorsement remains user-controlled' if len(accepted)==len(PROFILES) else 'Manual reference review in progress; only listed accepted sources are installed',timing_scope='Sum of latest per-profile batch timings; excludes research, review and earlier iterations')
 inventory.write_json(root/'audit-report.json',report)
 print({k:v for k,v in report.items() if k!='rows'});print('Review flags',[r['species'] for r in rows if r['distinct_geometry']!=36 or r['failures'] or r['artifact_errors'] or not r['current']])
if __name__=='__main__':main()
