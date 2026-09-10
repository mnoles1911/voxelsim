"""Local, resumable environment production. Shared by the CLI and Forge UI.

Workers build once, validate, export and save a candidate without approving it.
Cache hits verify every persisted artifact. No paid API or network call.
"""
from __future__ import annotations
import concurrent.futures
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import threading
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
from .kinds import READY
PLANTS=frozenset(k.key for k in READY)
CANDIDATES=ROOT/'out/forge-candidates'
_lock=threading.Lock()
_active=None
_live_reports={}
_session=uuid.uuid4().hex


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _understory_dependency_bytes(filename, data, body):
    """Fingerprint only the selected explicit profile/architecture dependency.

    The architecture dispatch is a closed if/elif chain. Keep every shared
    statement and the selected arm; reject an unfamiliar dispatch expression.
    This prevents a fern-only edit from revoking an unchanged flower source.
    """
    import ast
    from .understory_profiles import PROFILES, VERSION, SPRING_FLOWERS
    name=body['plant_recipe']['profile'];profile=PROFILES[name];arch=profile['architecture']
    if filename=='understory_profiles.py':
        return json.dumps(dict(profile=name,parameters=profile,version=VERSION,spring_flowers=name in SPRING_FLOWERS),sort_keys=True).encode()
    if filename!='understory.py':return data
    module=ast.parse(data)
    build=next(n for n in module.body if isinstance(n,ast.FunctionDef) and n.name=='build')
    woody_assignment=next(n for n in build.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='woody' for t in n.targets))
    woody=arch in ast.literal_eval(woody_assignment.value.comparators[0])
    index=next(i for i,n in enumerate(build.body) if isinstance(n,ast.If) and isinstance(n.test,ast.Name) and n.test.id=='woody')
    arm=build.body[index]
    while True:
        test=arm.test
        if isinstance(test,ast.Name) and test.id=='woody':matches=woody
        elif isinstance(test,ast.Compare) and isinstance(test.left,ast.Name) and test.left.id=='arch' and len(test.ops)==1:
            value=ast.literal_eval(test.comparators[0])
            if isinstance(test.ops[0],ast.Eq):matches=arch==value
            elif isinstance(test.ops[0],ast.In):matches=arch in value
            else:raise ValueError('Unrecognized understory architecture comparison')
        else:raise ValueError('Unrecognized understory architecture dispatch')
        if matches:selected=arm.body;break
        if len(arm.orelse)==1 and isinstance(arm.orelse[0],ast.If):arm=arm.orelse[0]
        else:selected=arm.orelse;break
    build.body[index:index+1]=selected
    # Nested drawing helpers are explicit dependencies too. A shrub shoot
    # helper that this herb/fern architecture never references is not relevant.
    helpers={n.name:n for n in build.body if isinstance(n,ast.FunctionDef)}
    outside=ast.Module(body=[n for n in build.body if not isinstance(n,ast.FunctionDef)],type_ignores=[])
    used={n.id for n in ast.walk(outside) if isinstance(n,ast.Name)}.intersection(helpers)
    while True:
        expanded=used|{n.id for name in used for n in ast.walk(helpers[name]) if isinstance(n,ast.Name) and n.id in helpers}
        if expanded==used:break
        used=expanded
    build.body=[n for n in build.body if not isinstance(n,ast.FunctionDef) or n.name in used]
    return ast.dump(module,include_attributes=False).encode()


def generator_digest(body=None):
    """Geometry identity with an isolated opt-in understory recipe dependency.

    Legacy bytes remain identical when the two guarded dispatch additions are
    removed. Every pre-existing geometry file is still hashed. New understory
    dependencies participate only when that recipe is selected; shared geometry
    changes still invalidate both revisions.
    """
    understory = (body or {}).get('plant_recipe',{}).get('generator') == 'temperate-understory-v1'
    h=hashlib.sha256()
    for p in sorted((ROOT/'forge').glob('*.py')):
        if p.name in {'inventory.py','server.py','server_version.py','cli.py','species_registry.py'}:
            continue
        if p.name.startswith('understory') and not understory:
            continue
        data=p.read_bytes()
        if understory:data=_understory_dependency_bytes(p.name,data,body)
        if not understory and p.name in {'pipeline.py','forest.py'}:
            # Only these exact, opt-in route additions are identity-neutral for
            # legacy specs. A changed route must fail closed, never disappear
            # behind a broad comment-marker exclusion.
            routes={
                'pipeline.py': ['    if spec.get("plant_recipe", {}).get("generator") == "temperate-understory-v1":', '        from . import understory', '        return understory.build(spec, seed, voxel_m, connectivity=connectivity)'],
                'forest.py': ['    if isinstance(recipe, dict) and recipe.get("generator") == "temperate-understory-v1":', '        from . import understory', '        return understory.validate_recipe(recipe, kind)'],
            }
            nl=b'\r\n' if b'\r\n' in data else b'\n'
            block=nl.join(x.encode() for x in ['    # UNDERSTORY_DISPATCH_BEGIN',*routes[p.name],'    # UNDERSTORY_DISPATCH_END',''])
            if data.count(block)!=1:raise ValueError('Unverified legacy dispatch change: '+p.name)
            data=data.replace(block,b'')
        h.update(p.name.encode());h.update(data)
    return h.hexdigest()


def write_json(path,body):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    tmp.write_text(json.dumps(body,indent=2)+'\n',encoding='utf8')
    try:
        for attempt in range(8):
            try:
                os.replace(tmp,path)
                break
            except PermissionError:
                if attempt==7:raise
                time.sleep(0.025 * (attempt+1))
    finally:
        if tmp.exists():tmp.unlink()


def write_report(path,body):
    # Polling has a truthful in-memory result even if persistence fails.
    snapshot=copy.deepcopy(body)
    _live_reports[body['id']]=snapshot
    write_json(path,snapshot)


def build_one(task):
    from . import pipeline,spec,vxa,vox,render,parts,categories
    import numpy as np
    name,body,seed,code,library_root,run_id=task
    started=time.perf_counter()
    entry=f'{name}-{seed:04d}'
    dest=Path(library_root)/name/entry
    endorsed=ROOT/'library'/name/entry
    if Path(library_root).resolve()==CANDIDATES.resolve() and endorsed.exists():
        if dest.exists():raise ValueError(f'{entry} exists in both pending and endorsed collections')
        dest=endorsed
    key=hashlib.sha256(f'{spec.spec_hash(body)}:{seed}:{code}'.encode()).hexdigest()
    if dest.exists():
        try:
            meta=json.loads((dest/'meta.json').read_text())
            if meta.get('generation_key')==key and all(
                    (dest/p).is_file() and digest(dest/p)==sha
                    for p,sha in meta['artifact_hashes'].items()):
                return dict(id=entry,species=name,seed=seed,status='cached',stats=meta['stats'],
                            seconds=round(time.perf_counter()-started,3),bytes=meta['asset_bytes'])
            if not meta.get('generation_key') and not meta.get('imported') and meta.get('spec_hash')==spec.spec_hash(body):
                # A hand-kept seed can predate Production metadata. Prove its
                # geometry matches, then reuse without changing its approval.
                saved=vxa.read(dest/'tree.vxa')
                expected=pipeline.build(body,seed).grid
                if (np.array_equal(saved.data,expected.data) and np.array_equal(saved.origin,expected.origin)
                        and saved.voxel_m==expected.voxel_m and (dest/'tree.vox').is_file()
                        and (dest/'thumb.png').is_file()):
                    return dict(id=entry,species=name,seed=seed,status='existing',stats=meta['stats'],
                        seconds=round(time.perf_counter()-started,3),bytes=sum(p.stat().st_size for p in dest.iterdir() if p.is_file()))
        except (OSError,ValueError,KeyError):pass
        raise ValueError(f'{entry} already exists with a different or incomplete build; choose unused seeds')
    asset=pipeline.build(body,seed)
    problems=pipeline.health(asset)
    if problems:raise ValueError('; '.join(problems))
    staging=ROOT/'out/inventory-stage'/run_id/entry
    staging.mkdir(parents=True,exist_ok=False)
    spec.save(body,staging/'spec.json');spec.save(asset.realized,staging/'realized.json')
    vxa.write(asset.grid,staging/'tree.vxa',asset.parts,parts.joints(asset.parts))
    restored=vxa.read(staging/'tree.vxa')
    if not (np.array_equal(restored.data,asset.grid.data) and
            np.array_equal(restored.origin,asset.grid.origin) and restored.voxel_m==asset.grid.voxel_m):
        raise ValueError('VXA roundtrip failed')
    models=vox.write(asset.grid,staging/'tree.vox',name=entry)
    render.view(asset.grid,render.camera_for(body),target_px=420).save(staging/'thumb.png')
    hashes={p.name:digest(p) for p in staging.iterdir() if p.is_file()}
    size=sum(p.stat().st_size for p in staging.iterdir() if p.is_file())
    elapsed=time.perf_counter()-started
    meta=dict(id=entry,species=name,kind=body['kind'],category=categories.of(body),seed=seed,
        imported=False,inventory_candidate=True,visual_approved=False,review_status='inventory_candidate',
        spec_hash=spec.spec_hash(body),generation_key=key,generator_digest=code,
        artifact_hashes=hashes,asset_bytes=size,stats=asset.stats,problems=[],vox_models=models,
        generation_seconds=round(elapsed,3),batch_id=run_id)
    write_json(staging/'meta.json',meta)
    dest.parent.mkdir(parents=True,exist_ok=True)
    os.rename(staging,dest)
    return dict(id=entry,species=name,seed=seed,status='built',stats=asset.stats,
                seconds=round(elapsed,3),bytes=size)


def validate_request(species,seed_start,count,workers):
    if not isinstance(species,list) or not 1<=len(species)<=20:
        raise ValueError('Choose 1–20 species')
    if any(not isinstance(n,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}',n) for n in species):
        raise ValueError('Invalid species name')
    for value,label,low,high in [(seed_start,'First seed',1,9999),(count,'Count',1,128),(workers,'Workers',1,4)]:
        if type(value) is not int or not low<=value<=high:raise ValueError(f'{label} must be {low}–{high}')
    if seed_start+count-1>9999 or len(species)*count>512:raise ValueError('Batch exceeds 512 assets or seed 9999')


def run(species,seed_start=1,count=36,workers=2,*,run_id=None,library_root=None,server_session=None):
    from . import spec
    validate_request(species,seed_start,count,workers)
    bodies=[]
    for name in dict.fromkeys(species):
        body,rep=spec.load(ROOT/'specs'/f'{name}.json')
        if body['kind'] not in PLANTS:raise ValueError('This asset kind has no ready generator')
        if rep.warnings:raise ValueError(f'{name}: '+ '; '.join(rep.warnings))
        # Saved imports have geometry that is not reconstructed by their spec.
        imported=list((ROOT/'library'/name).glob('*/meta.json'))
        if any(json.loads(p.read_text()).get('imported') for p in imported):
            raise ValueError(f'{name} is an imported model; choose a procedural species')
        bodies.append((name,body))
    run_id=run_id or uuid.uuid4().hex[:12]
    out=ROOT/'out/inventory-runs'/run_id
    out.mkdir(parents=True,exist_ok=True)
    code=generator_digest()
    jobs=[(name,body,i,generator_digest(body),str(library_root or CANDIDATES),run_id)
          for name,body in bodies for i in range(seed_start,seed_start+count)]
    report=dict(id=run_id,status='running',species=species,total=len(jobs),completed=0,
                workers=workers,seed_start=seed_start,count=count,generator_digest=code,
                rows=[],failures=[],elapsed_seconds=0,paid_api_calls=0,started_at=time.time(),server_session=server_session)
    start=time.perf_counter()
    write_report(out/'report.json',report)
    # Prevent numerical libraries from spawning a second pool per worker.
    for name in ['OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS']:
        os.environ[name]='1'
    os.environ['ASSET_FORGE_MAX_GRID_MB']=str(min(512,int(os.environ.get('ASSET_FORGE_MAX_GRID_MB','512'))))
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
        pending={pool.submit(build_one,t):t for t in jobs}
        for future in concurrent.futures.as_completed(pending):
            task=pending[future]
            try:report['rows'].append(future.result())
            except Exception as exc:
                report['failures'].append(dict(species=task[0],seed=task[2],error=str(exc)))
            report['completed']+=1
            report['elapsed_seconds']=round(time.perf_counter()-start,2)
            write_report(out/'report.json',report)
    report['rows'].sort(key=lambda row:(row['species'],row['seed']))
    report.update(status='complete' if not report['failures'] else 'completed_with_errors',
        built=sum(r['status']=='built' for r in report['rows']),cached=sum(r['status']=='cached' for r in report['rows']),
        existing=sum(r['status']=='existing' for r in report['rows']),
        asset_bytes=sum(r['bytes'] for r in report['rows']),
        worker_seconds=round(sum(r['seconds'] for r in report['rows']),2),
        elapsed_seconds=round(time.perf_counter()-start,2))
    write_report(out/'report.json',report)
    return report


def start(request):
    global _active
    species=request.get('species',[])
    seed=request.get('seed_start',1);count=request.get('count',36);workers=request.get('workers',2)
    validate_request(species,seed,count,workers)
    with _lock:
        if _active:raise ValueError('A production batch is already running')
        ident=uuid.uuid4().hex[:12];_active=ident
        try:
            write_report(ROOT/'out/inventory-runs'/ident/'report.json',dict(id=ident,status='starting',
                species=species,seed_start=seed,count=count,workers=workers,server_session=_session,started_at=time.time(),
                total=len(species)*count,completed=0,rows=[],failures=[],elapsed_seconds=0))
        except Exception:
            _live_reports.pop(ident,None)
            _active=None
            raise
    def work():
        global _active
        try:run(species,seed,count,workers,run_id=ident,server_session=_session)
        except Exception as exc:
            report=copy.deepcopy(_live_reports.get(ident,{}))
            report.update(id=ident,status='failed',error=str(exc))
            try:write_report(ROOT/'out/inventory-runs'/ident/'report.json',report)
            except OSError as storage_error:
                _live_reports[ident]['error']+=f'; progress could not be saved: {storage_error}'
        finally:
            with _lock:_active=None
    threading.Thread(target=work,daemon=True).start()
    return dict(id=ident)


def reports():
    paths=sorted((ROOT/'out/inventory-runs').glob('*/report.json'),key=lambda p:p.stat().st_mtime,reverse=True)
    results={}
    for p in paths[:12]:
        try:
            body=json.loads(p.read_text())
            results[body['id']]=body
        except (OSError,ValueError):continue
    results.update(copy.deepcopy(_live_reports))
    for body in results.values():
        if body.get('status') in {'starting','running'}:
            if body.get('server_session') and body['server_session']!=_session:
                body.update(status='interrupted',error='Server restarted before this batch finished. Retry the seed range to reuse completed assets.')
            elif body.get('started_at'):
                body['elapsed_seconds']=round(time.time()-body['started_at'],2)
    return sorted(results.values(),key=lambda b:b.get('started_at',0),reverse=True)[:12]


def promote(entry_id):
    """Explicit UI Keep gesture promotes exact generated bytes, with no rebuild."""
    from . import server,spec
    directory=server.library_dir(entry_id)
    if directory is None:raise ValueError('Unknown inventory entry')
    meta=json.loads((directory/'meta.json').read_text())
    if meta.get('review_status')=='rejected':raise ValueError('Variant was rejected')
    if not meta.get('inventory_candidate'):raise ValueError('Entry is not a candidate')
    body,_=spec.load(server.SPECS/(meta['species']+'.json'))
    if spec.spec_hash(body)!=meta['spec_hash'] or generator_digest(body)!=meta['generator_digest']:
        raise ValueError('Source changed since generation; generate fresh candidates')
    if not all(digest(directory/p)==h for p,h in meta['artifact_hashes'].items()):
        raise ValueError('Candidate files changed since validation')
    destination=server.LIBRARY/meta['species']/meta['id']
    if directory.resolve()!=destination.resolve():
        if destination.exists():raise ValueError('A saved endorsed variant already uses this id')
        destination.parent.mkdir(parents=True,exist_ok=True)
        os.rename(directory,destination)
    meta.update(inventory_candidate=False,visual_approved=True,review_status='endorsed')
    write_json(destination/'meta.json',meta)
    server.sync_curation_from_library(meta['species'],gesture='keep')
    return meta


def reject(entry_id):
    """Hide a rejected seed persistently; retain bytes so cache cannot resurrect it."""
    from . import server
    directory=server.library_dir(entry_id)
    if directory is None:raise ValueError('Unknown inventory entry')
    meta=json.loads((directory/'meta.json').read_text())
    destination=server.ROOT/'out/forge-candidates'/meta['species']/meta['id']
    if directory.resolve()!=destination.resolve():
        if destination.exists():raise ValueError('Duplicate variant id')
        destination.parent.mkdir(parents=True,exist_ok=True)
        os.rename(directory,destination)
    meta.update(inventory_candidate=True,visual_approved=False,review_status='rejected')
    write_json(destination/'meta.json',meta)
    record_path=server.LIBRARY/meta['species']/'species.json'
    if record_path.exists():
        record=json.loads(record_path.read_text())
        for row in record.get('variants',[]):
            if row['id']==meta['id']:row['review_status']='rejected'
        write_json(record_path,record)
    server.sync_curation_from_library(meta['species'],gesture='unkeep')
    return {'rejected':meta['id']}
