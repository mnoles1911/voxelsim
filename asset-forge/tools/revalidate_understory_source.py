"""Advance a reviewed source identity only after every saved VXA rebuilds exactly.

This preserves endorsement/rejection flags and original bytes. A geometry change
refuses the entire source before metadata mutation; it requires another review.
"""
import argparse,json,tempfile,time,hashlib
from pathlib import Path
import _path
from forge import inventory,pipeline,spec,vxa
ROOT=Path(__file__).resolve().parents[1]
def revalidate(name):
 record_path=ROOT/'library'/name/'species.json';record=json.loads(record_path.read_text());body,_=spec.load(ROOT/'specs'/f'{name}.json');code=inventory.generator_digest(body)
 if not record.get('generator_approved') or record['baseline_spec_hash']!=spec.spec_hash(body):raise ValueError('Source baseline not already accepted')
 if record['approved_generator_digest']==code:return
 paths=[*(ROOT/'library'/name).glob('*/meta.json'),*(inventory.CANDIDATES/name).glob('*/meta.json')];proof=[]
 for path in paths:
  meta=json.loads(path.read_text())
  if meta.get('imported') or meta['spec_hash']!=spec.spec_hash(body):raise ValueError('Mixed historical source needs explicit review')
  if not all(inventory.digest(path.parent/f)==sha for f,sha in meta['artifact_hashes'].items()):raise ValueError('Artifact changed')
  asset=pipeline.build(body,meta['seed'])
  if pipeline.health(asset):raise ValueError('Rebuild is unhealthy')
  with tempfile.TemporaryDirectory(prefix='source-revalidation-',dir=ROOT/'out') as tmp:
   file=Path(tmp)/'tree.vxa';vxa.write(asset.grid,file)
   if inventory.digest(file)!=meta['artifact_hashes']['tree.vxa']:raise ValueError('Geometry changed; new manual review required: '+meta['id'])
  proof.append(dict(id=meta['id'],seed=meta['seed'],hash=meta['artifact_hashes']['tree.vxa']))
 if len(proof)<36:raise ValueError('Complete saved family required')
 event=dict(previous_digest=record['approved_generator_digest'],new_digest=code,timestamp=time.time(),method='Every saved VXA rebuilt to identical SHA-256; original files and all user decisions preserved',variants=proof)
 inventory.write_json(ROOT/'out/understory-collection'/f'{name}-identity-revalidation.json',event)
 for path in paths:
  meta=json.loads(path.read_text());meta['generator_digest']=code;meta['generation_key']=hashlib.sha256(f"{meta['spec_hash']}:{meta['seed']}:{code}".encode()).hexdigest();inventory.write_json(path,meta)
 record.setdefault('identity_revalidations',[]).append(event);record.update(generator_digest=code,approved_generator_digest=code);inventory.write_json(record_path,record)
 review_path=record_path.with_name('reference-review.json');review=json.loads(review_path.read_text());review.setdefault('identity_revalidations',[]).append(event);review['generator_digest']=code;inventory.write_json(review_path,review)
 report_path=ROOT/'out/understory-collection'/f'{name}.json';report=json.loads(report_path.read_text());report['generator_digest']=code;inventory.write_json(report_path,report)
 findings_path=ROOT/'out/understory-review/manual-findings.json';findings=json.loads(findings_path.read_text());finding=findings.get(name)
 if finding and finding.get('status')=='accepted':
  finding.setdefault('identity_revalidations',[]).append({k:v for k,v in event.items() if k!='variants'});finding['generator_digest']=code;inventory.write_json(findings_path,findings)
 print('REVALIDATED',name,len(proof),'exact saved variants')
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('names',nargs='+');args=ap.parse_args()
 for name in args.names:revalidate(name)
