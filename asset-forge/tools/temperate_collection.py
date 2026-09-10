"""Stage validated temperate variants, then install reviewed species atomically.

stage never replaces active species; install requires a complete verified family.
Old active geometry is retired only after the replacement is in place.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import time
import uuid
import _path
from forge import inventory,forest,spec,vxa
from forge.forest_profiles import PROFILES,source,ART_BASELINE
from forest_review import elevation
from PIL import Image,ImageDraw

ROOT=Path(__file__).resolve().parents[1]
STAGE=ROOT/'out/temperate-collection'

def body_for(name):
    body,_=spec.load(ROOT/'specs'/f'{name}.json')
    if name=='temperate-oak':
        body['name']=name
        body['plant_recipe']=dict(generator='oak-shoots-v1',size='mixed',form='mixed')
    else:body['plant_recipe']=dict(generator=forest.VERSION,profile=name,size='mixed',form='mixed')
    body['curation']=dict(status='draft',seeds=[1],notes='Reference-informed generator; variants require endorsement in Forge.')
    body,rep=spec.validate(body)
    if rep.warnings:raise ValueError(f'{name}: {rep.warnings}')
    return body

def stage(names,workers):
    for key in ['OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS']:os.environ[key]='1'
    code=inventory.generator_digest()
    STAGE.mkdir(parents=True,exist_ok=True)
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
        for name in names:
            body=body_for(name);run_id='temperate-'+name
            report=dict(id=run_id,species=[name],status='running',started_at=time.time(),total=36,completed=0,
                seed_start=1,count=36,rows=[],failures=[],elapsed_seconds=0,generator_digest=code,
                message='Staging replacement variants for visual review; active collection remains available.')
            report_path=ROOT/'out/inventory-runs'/run_id/'report.json'
            inventory.write_json(report_path,report)
            pending={pool.submit(inventory.build_one,(name,body,i,code,str(STAGE/'variants'),run_id)):i for i in range(1,37)}
            rows=[]
            for f in concurrent.futures.as_completed(pending):
                try:rows.append(f.result())
                except Exception as exc:report['failures'].append(dict(species=name,seed=pending[f],error=str(exc)))
                report['completed']+=1;report['elapsed_seconds']=round(time.time()-report['started_at'],2)
                inventory.write_json(report_path,report)
            rows.sort(key=lambda r:r['seed'])
            report.update(status='ready_for_review' if not report['failures'] else 'completed_with_errors',staged_rows=rows)
            inventory.write_json(report_path,report)
            inventory.write_json(STAGE/f'{name}.json',dict(spec=body,report=report))
            if report['failures']:print('FAILED',name,report['failures'],flush=True);continue
            sheet=Image.new('RGB',(6*280,6*300),(24,26,30));d=ImageDraw.Draw(sheet)
            for i,row in enumerate(rows):
                folder=STAGE/'variants'/name/row['id'];im=Image.open(folder/'thumb.png').convert('RGB');im.thumbnail((280,250))
                x,y=(i%6)*280,(i//6)*300;sheet.paste(im,(x+(280-im.width)//2,y))
                d.text((x+8,y+253),f"Seed {row['seed']} | {row['stats']['size_class']} / {row['stats']['growth_form']}",fill=(228,219,197))
                d.text((x+8,y+273),f"{row['stats']['height_m']:.1f} m | 100 mm",fill=(174,193,172))
            sheet.save(STAGE/f'{name}-36.png')
            g=vxa.read(STAGE/'variants'/name/f'{name}-0007/tree.vxa')
            elevation(g,f'{name} | reference seed 7',width=660,height=660).save(STAGE/f'{name}-reference.png')
            print('READY',name,report['elapsed_seconds'],'seconds',flush=True)

def contained(path,root):
    path,root=Path(path).resolve(),Path(root).resolve()
    if not path.is_relative_to(root) or path==root:raise ValueError(f'Unsafe collection path: {path}')
    return path

def install(names):
    from forge import species_registry
    for name in names:
        review=species_registry.reference_review(name) or {}
        if review.get('status')!='passed pilot review' or review.get('generator_digest')!=inventory.generator_digest():
            raise ValueError('Current generator must pass a documented photo comparison before replacing '+name)
        if not {1,4,7}.issubset(review.get('reviewed_seeds',[])):
            raise ValueError('Review small, medium and large pilots before replacing '+name)
        batch_review=review.get('batch_review',{})
        if batch_review.get('status')!='accepted' or batch_review.get('seeds')!=list(range(1,37)) or batch_review.get('sheet_hash')!=inventory.digest(STAGE/f'{name}-36.png'):
            raise ValueError('Review the current 36-variant contact sheet before replacing '+name)
        item=json.loads((STAGE/f'{name}.json').read_text());body=item['spec'];report=item['report']
        if report['status']!='ready_for_review' or len(report['staged_rows'])!=36:raise ValueError('Incomplete replacement '+name)
        if report['generator_digest']!=inventory.generator_digest():raise ValueError('Generator changed; restage '+name)
        incoming=contained(STAGE/'variants'/name,STAGE)
        for row in report['staged_rows']:
            folder=incoming/row['id'];meta=json.loads((folder/'meta.json').read_text())
            if not all(inventory.digest(folder/f)==h for f,h in meta['artifact_hashes'].items()):raise ValueError('Artifact mismatch '+row['id'])
            if row['seed'] in (1,4,7):
                reviewed=ROOT/'out/temperate-review'/name/f"pilot-{row['seed']}.vxa"
                if inventory.digest(reviewed)!=meta['artifact_hashes']['tree.vxa']:
                    raise ValueError('Staged geometry differs from reviewed pilot '+row['id'])
        if len({inventory.digest(incoming/r['id']/'tree.vxa') for r in report['staged_rows']})!=36:
            raise ValueError('Replacement must contain 36 distinct geometries')
        if len({r['stats']['spec_hash'] for r in report['staged_rows']})!=1:raise ValueError('Mixed baseline')
        active=ROOT/'library'/name
        # Retire old entries after preserving a compact provenance ledger.
        old=[json.loads(p.read_text()) for p in active.glob('*/meta.json')]
        ledger=dict(species=name,replaced_ids=[m['id'] for m in old],replacement_ids=[r['id'] for r in report['staged_rows']],old_spec=json.loads((ROOT/'specs'/f'{name}.json').read_text()),timestamp=time.time())
        inventory.write_json(STAGE/f'{name}-retirement.json',ledger)
        candidates=ROOT/'out/forge-candidates'/name
        if candidates.exists():raise ValueError('Candidate target already exists '+name)
        candidates.parent.mkdir(parents=True,exist_ok=True)
        os.rename(incoming,candidates)
        spec.save(body,ROOT/'specs'/f'{name}.json')
        active.mkdir(parents=True,exist_ok=True)
        p=PROFILES[name]
        inventory.write_json(active/'species.json',dict(schema_version=1,species=name,baseline_spec=body,
            baseline_spec_hash=spec.spec_hash(body),reference_variant_id=f'{name}-0007',reference_seed=7,
            generator=body['plant_recipe']['generator'],generator_digest=report['generator_digest'],
            review_status='Astra reference-reviewed source',habitat=p.habitat,reference_notes=p.cue,
            generator_approved=True,approved_generator_digest=report['generator_digest'],
            sources=[source(p)],review_date='2026-09-07',art_baseline=ART_BASELINE,
            semantics='species baseline + generator revision + numbered seed = variant',
            variant_inventory='out/forge-candidates; endorsement moves exact saved bytes into this species collection',
            variants=[dict(id=r['id'],seed=r['seed'],artifact_hash=inventory.digest(candidates/r['id']/'tree.vxa')) for r in report['staged_rows']],
            reference_selection='Astra design reference; world endorsement is a separate explicit choice'))
        for meta in old:
            target=contained(active/meta['id'],ROOT/'library');shutil.rmtree(target)
        bank=ROOT/'out/engine/banks'/name
        if bank.exists():shutil.rmtree(contained(bank,ROOT/'out/engine/banks'))
        report.update(status='complete',rows=report.pop('staged_rows'),message='Replacement family available in Forge; endorse variants to add to Asset Library.')
        inventory.write_json(ROOT/'out/inventory-runs'/report['id']/'report.json',report)
        print('INSTALLED',name,'36 variants;',len(old),'old assets removed',flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('action',choices=['stage','install']);ap.add_argument('names',nargs='*');ap.add_argument('--workers',type=int,default=3)
    args=ap.parse_args();names=args.names or list(PROFILES)
    if any(n not in PROFILES for n in names):raise ValueError('Unknown temperate profile')
    if args.action=='stage':stage(names,args.workers)
    else:install(names)
