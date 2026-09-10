"""Offline understory pilot staging. Does not edit active sources or decisions."""
import argparse,json,time,concurrent.futures
from pathlib import Path
import _path
from forge import inventory,pipeline,spec,vxa,render
from forge.understory_profiles import PROFILES,VERSION
from PIL import Image,ImageDraw
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'out/understory-review'
def body_for(name):
 body,_=spec.load(ROOT/'specs'/f'{name}.json');body['resolution_cm']='2.5';body['plant_recipe']=dict(generator=VERSION,profile=name,size='mixed',form='mixed')
 body['notes']=f"Spring understory source for {PROFILES[name]['taxon']}; architecture: {PROFILES[name]['architecture']}. Metric geometry at 25 mm cubic voxels. Species baseline and generator define the source; numbered seeds generate variants. User endorsement remains separate from source review."
 if 'baseline_height_m' in PROFILES[name]:body['height_m']=PROFILES[name]['baseline_height_m']
 if 'head_material' in PROFILES[name]:body['materials']['head']=PROFILES[name]['head_material']
 body,rep=spec.validate(body)
 if rep.warnings:raise ValueError(rep.warnings)
 return body
def pilot(name):
 body=body_for(name);folder=OUT/name;folder.mkdir(exist_ok=True,parents=True);rows=[]
 for seed in (1,4,7):
  a=pipeline.build(body,seed);problems=pipeline.health(a);vxa.write(a.grid,folder/f'pilot-{seed}.vxa');render.view(a.grid,render.camera_for(body),target_px=400).save(folder/f'pilot-{seed}.png')
  rows.append(dict(seed=seed,stats=a.stats,problems=problems,artifact_hash=inventory.digest(folder/f'pilot-{seed}.vxa')))
 inventory.write_json(folder/'pilots.json',dict(spec=body,generator_digest=inventory.generator_digest(body),rows=rows,status='awaiting visual review'))
 return name,rows
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('names',nargs='*');ap.add_argument('--workers',type=int,default=4);args=ap.parse_args();names=args.names or [n for n,p in PROFILES.items() if p['architecture']!='unresolved']
 with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
  jobs={pool.submit(pilot,n):n for n in names}
  for f in concurrent.futures.as_completed(jobs):
   try:
    name,rows=f.result();print(name,[(r['seed'],r['stats']['voxels'],r['problems']) for r in rows],flush=True)
   except Exception as e:print('FAILED',jobs[f],repr(e),flush=True)
