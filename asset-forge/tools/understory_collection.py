"""Stage 36 understory variants without editing active species or user decisions."""
import argparse,concurrent.futures,json,time,os,hashlib,tempfile,uuid
import numpy as np
from pathlib import Path
import _path
from forge import inventory,pipeline,vxa,spec
from forge.understory_profiles import PROFILES
from understory_review import body_for
from PIL import Image,ImageDraw
ROOT=Path(__file__).resolve().parents[1];STAGE=ROOT/'out/understory-collection'
def refresh_one(task):
 name,body,seed,code,library_root,run_id=task
 entry=f'{name}-{seed:04d}';folder=Path(library_root)/name/entry
 if not folder.exists():return inventory.build_one(task)
 meta=json.loads((folder/'meta.json').read_text())
 if meta.get('generator_digest')==code:return inventory.build_one(task)
 if not all(inventory.digest(folder/f)==sha for f,sha in meta['artifact_hashes'].items()):raise ValueError('Corrupt research artifact: '+entry)
 asset=pipeline.build(body,seed)
 problems=pipeline.health(asset)
 if problems:raise ValueError('; '.join(problems))
 with tempfile.TemporaryDirectory(prefix='revalidate-',dir=STAGE) as tmp:
  test=Path(tmp)/'tree.vxa';vxa.write(asset.grid,test)
  same=inventory.digest(test)==meta['artifact_hashes']['tree.vxa']
 if same and meta['spec_hash']==spec.spec_hash(body):
  meta.setdefault('identity_revalidations',[]).append(dict(previous_digest=meta['generator_digest'],new_digest=code,method='exact rebuilt VXA SHA-256 equality',timestamp=time.time()))
  meta.update(generator_digest=code,generation_key=hashlib.sha256(f'{spec.spec_hash(body)}:{seed}:{code}'.encode()).hexdigest(),stats=asset.stats)
  inventory.write_json(folder/'meta.json',meta)
  return dict(id=entry,species=name,seed=seed,status='revalidated',stats=meta['stats'],seconds=0,bytes=meta['asset_bytes'])
 # Only offline research staging may be versioned here. Never touch the live
 # library/candidates or a user's endorsement/rejection through this path.
 resolved=folder.resolve();root=(STAGE/'variants').resolve()
 if not resolved.is_relative_to(root):raise ValueError('Unsafe research staging path')
 old=STAGE/'superseded-research'/name/(entry+'-'+uuid.uuid4().hex[:8]);old.parent.mkdir(exist_ok=True,parents=True)
 os.rename(folder,old)
 try:return inventory.build_one(task)
 except Exception:
  if not folder.exists():os.rename(old,folder)
  raise


def stage(names,workers):
 for key in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS'):os.environ[key]='1'
 STAGE.mkdir(parents=True,exist_ok=True)
 with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
  for name in names:
   body=body_for(name);code=inventory.generator_digest(body);run_id='understory-'+name;start=time.time()
   futures={pool.submit(refresh_one,(name,body,i,code,str(STAGE/'variants'),run_id)):i for i in range(1,37)};rows=[];failures=[]
   for f in concurrent.futures.as_completed(futures):
    try:rows.append(f.result())
    except Exception as e:failures.append(dict(seed=futures[f],error=str(e)))
   rows.sort(key=lambda r:r['seed']);report=dict(species=name,spec=body,generator_digest=code,rows=rows,failures=failures,seconds=round(time.time()-start,2),status='awaiting photo and batch review' if not failures else 'failed',paid_api_calls=0)
   inventory.write_json(STAGE/f'{name}.json',report)
   if not failures:
    sheet=Image.new('RGB',(6*220,6*240),(24,27,31));draw=ImageDraw.Draw(sheet)
    for i,r in enumerate(rows):
     im=Image.open(STAGE/'variants'/name/r['id']/'thumb.png').convert('RGB');im.thumbnail((220,195));x,y=i%6*220,i//6*240;sheet.paste(im,(x+(220-im.width)//2,y))
     draw.text((x+5,y+198),f"Seed {r['seed']} | {r['stats']['size_class']}",fill='white');draw.text((x+5,y+216),f"{r['stats']['growth_form']} | {r['stats']['height_m']:.2f}m |25mm",fill=(168,195,162))
    sheet.save(STAGE/f'{name}-36.png')
   print(name,len(rows),'variants',report['seconds'],'seconds',failures,flush=True)
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('names',nargs='*');ap.add_argument('--workers',type=int,default=4);args=ap.parse_args();stage(args.names or [n for n,p in PROFILES.items() if p['architecture']!='unresolved' and not (ROOT/'library'/n/'species.json').exists()],args.workers)
