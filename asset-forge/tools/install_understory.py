"""Install explicitly reviewed understory research, preserving user decisions.

A manual, current photo/pilot/batch decision is required. No automatic approval.
"""
import argparse,json,os,shutil,time
from pathlib import Path
import _path
from forge import inventory,spec
from forge.understory_profiles import PROFILES,VERSION
ROOT=Path(__file__).resolve().parents[1];STAGE=ROOT/'out/understory-collection'
def contained(p,root):
 p,root=Path(p).resolve(),Path(root).resolve()
 if p==root or not p.is_relative_to(root):raise ValueError('Unsafe retirement path')
 return p
def install(name):
 item=json.loads((STAGE/f'{name}.json').read_text());body=item['spec'];code=inventory.generator_digest(body)
 review=json.loads((ROOT/'out/understory-review/manual-findings.json').read_text()).get(name,{})
 if review.get('status')!='accepted' or review.get('generator_digest')!=code:raise ValueError('Current manual source review required')
 if review.get('reviewed_seeds')!=[1,4,7] or review.get('batch_seeds')!=list(range(1,37)):raise ValueError('Three-size and full batch visual review required')
 if review.get('sheet_hash')!=inventory.digest(STAGE/f'{name}-36.png'):raise ValueError('Reviewed sheet differs')
 if not review.get('viewed_photos'):raise ValueError('Record actually inspected photographs')
 if item['generator_digest']!=code or item['failures'] or len(item['rows'])!=36:raise ValueError('Staging incomplete or outdated')
 incoming=STAGE/'variants'/name;metas=[]
 for row in item['rows']:
  folder=incoming/row['id'];meta=json.loads((folder/'meta.json').read_text());metas.append(meta)
  if meta['generator_digest']!=code or meta['stats']['voxel_cm']!=2.5:raise ValueError('Mixed generator/pitch')
  if not all(inventory.digest(folder/f)==sha for f,sha in meta['artifact_hashes'].items()):raise ValueError('Staged artifact checksum mismatch')
  if meta['seed'] in (1,4,7) and review['pilot_hashes'][str(meta['seed'])]!=meta['artifact_hashes']['tree.vxa']:raise ValueError('Pilot geometry changed')
 if len({m['artifact_hashes']['tree.vxa'] for m in metas})!=36:raise ValueError('Need 36 distinct geometries')
 active=ROOT/'library'/name;dest=inventory.CANDIDATES/name
 oldpaths=[*active.glob('*/meta.json'),*dest.glob('*/meta.json')];old=[(p,json.loads(p.read_text())) for p in oldpaths]
 protected=[m for p,m in old if m.get('review_status') in ('endorsed','rejected') or m.get('visual_approved')]
 newids={m['id'] for m in metas}
 if any(m['id'] in newids for m in protected):raise ValueError('Existing user decision collides; choose fresh seed range before installing')
 # Refuse overwrites of stale candidates. An explicit reconciliation is needed
 # if a user generated this species while the offline review was running.
 if any((dest/m['id']).exists() for m in metas):raise ValueError('Pending variant ids already exist; reconcile before installation')
 ledger=dict(species=name,timestamp=time.time(),old_spec=json.loads((ROOT/'specs'/f'{name}.json').read_text()),retired=[dict(id=m['id'],seed=m['seed'],artifact_hashes=m.get('artifact_hashes',{})) for p,m in old if m not in protected],preserved_user_decisions=[m['id'] for m in protected],replacement_ids=sorted(newids))
 inventory.write_json(STAGE/f'{name}-retirement.json',ledger)
 dest.mkdir(exist_ok=True,parents=True)
 for meta in metas:os.rename(incoming/meta['id'],dest/meta['id'])
 body['curation']=dict(status='draft',seeds=[1],notes='Reference-reviewed spring source; endorse variants individually in Forge.')
 spec.save(body,ROOT/'specs'/f'{name}.json')
 source=next(r for r in json.loads((ROOT/'out/understory-review/references.json').read_text()) if r['species']==name)
 active.mkdir(exist_ok=True,parents=True)
 inventory.write_json(active/'reference-review.json',dict(review,status='passed pilot review',profile=name,review_date=time.strftime('%Y-%m-%d'),source=source['source'],additional_sources=source.get('additional_sources',[]),reference_catalog_photos=source['photos'],taxon=PROFILES[name]['taxon'],batch_review=dict(status='accepted',seeds=list(range(1,37)),sheet_hash=review['sheet_hash'])))
 inventory.write_json(active/'species.json',dict(schema_version=1,species=name,display_name=PROFILES[name].get('display_name'),reference_role=PROFILES[name].get('reference_role'),baseline_spec=body,baseline_spec_hash=spec.spec_hash(body),reference_variant_id=f'{name}-0007',reference_seed=7,generator=VERSION,generator_digest=code,generator_approved=True,approved_generator_digest=code,review_status='Astra reference-reviewed source',review_date=time.strftime('%Y-%m-%d'),sources=list(dict.fromkeys([source['source'],*source.get('additional_sources',[])])),reference_notes=review['findings'],art_baseline=dict(season='spring',voxel_mm=25),semantics='species baseline + generator revision + numbered seed = variant',reference_selection='Authoritative source design; user endorsement is a separate choice',variant_inventory='out/forge-candidates; exact bytes move to species library upon endorsement',variants=[dict(id=m['id'],seed=m['seed'],artifact_hash=m['artifact_hashes']['tree.vxa']) for m in metas]))
 for path,meta in old:
  if meta in protected:continue
  # Check again immediately before retiring to preserve any intervening decision.
  latest=json.loads(path.read_text())
  if latest.get('review_status') in ('endorsed','rejected') or latest.get('visual_approved'):continue
  shutil.rmtree(contained(path.parent,ROOT/'library' if path.is_relative_to(ROOT/'library') else inventory.CANDIDATES))
 # Existing endorsed bank bytes are retained when user-endorsed variants exist.
 bank=ROOT/'out/engine/banks'/name
 if bank.exists() and not protected:shutil.rmtree(contained(bank,ROOT/'out/engine/banks'))
 item.update(status='installed; variants pending user endorsement',installed_at=time.time())
 inventory.write_json(STAGE/f'{name}.json',item)
 print('INSTALLED',name,'36 pending variants; retired',len(ledger['retired']),'; preserved',len(protected))
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('names',nargs='+');args=ap.parse_args()
 for n in args.names:
  if n not in PROFILES:raise ValueError('Unknown profile')
  install(n)
