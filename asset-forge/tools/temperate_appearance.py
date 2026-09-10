"""Palette-preserving appearance sidecars; never changes geometry or decisions."""
import argparse,json,struct,time,hashlib
from pathlib import Path
if not __package__:import _path
import numpy as np
from forge import inventory,materials,vxa
ROOT=inventory.ROOT
POLICY=ROOT/'rules/temperate-appearance-policy.json'

def in_scope(body):
    allow=body.get('biome_allow')
    return body.get('biomes',{}).get('temperate_forest',0)>0 and (allow is None or 'temperate_forest' in allow)

def protected(directory):
    return {name:inventory.digest(directory/name) for name in ('tree.vxa','meta.json','spec.json','appearance.npz') if (directory/name).is_file()}

def install(directory,thumbnail=True):
    directory=Path(directory).resolve();body=json.loads((directory/'spec.json').read_text(encoding='utf8'))
    if not in_scope(body):return None
    sidecar=directory/'tree-appearance.json'
    if sidecar.exists():
        old=json.loads(sidecar.read_text())
        if old.get('geometry_sha256')!=inventory.digest(directory/'tree.vxa'):raise ValueError('stale geometry sidecar '+str(directory))
        if old.get('revision')!='temperate-spring-variation-v1':return dict(id=directory.name,status='existing-approved-tree-appearance',kind=body['kind'])
        if old.get('preview_sha256')!=inventory.digest(directory/'tree-appearance.bin'):raise ValueError('corrupt appearance sidecar '+str(directory))
        from tools.export_temperate_runtime_appearance import ensure
        runtime=ensure(directory)
        if thumbnail:
            from tools.tree_appearance_thumbnails import render
            render(directory)
        return dict(id=directory.name,status='verified-existing-generic',kind=body['kind'],voxel_mm=runtime['voxel_mm'],source=str(directory.relative_to(ROOT)))
    before=protected(directory);grid=vxa.read(directory/'tree.vxa');coords=np.argwhere(grid.surface_mask()).astype(np.int16)
    if not len(coords):raise ValueError('empty saved asset '+str(directory))
    mats=grid.data[tuple(coords.T)].astype(np.uint8)
    rgb=np.asarray([materials.color(int(m)) for m in mats],dtype=np.uint8)
    if (directory/'appearance.npz').is_file():
        with np.load(directory/'appearance.npz',allow_pickle=False) as data:
            if not np.array_equal(data['cells'],coords) or data['rgb'].shape!=rgb.shape or data['rgb'].dtype!=np.uint8:raise ValueError('authored appearance mismatch')
            rgb=data['rgb'].copy()
    blob=struct.pack('<IIII',*map(int,grid.shape),len(coords))+coords.tobytes()+mats.tobytes()+b'RGB1'+rgb.tobytes()
    dest=directory/'tree-appearance.bin';tmp=dest.with_suffix('.tmp');tmp.write_bytes(blob);tmp.replace(dest)
    record=dict(revision='temperate-spring-variation-v1',geometry_sha256=before['tree.vxa'],preview_sha256=inventory.digest(dest),
        policy_sha256=inventory.digest(POLICY),needle=False,foliage_mask=False,voxel_mm=grid.voxel_m*1000,
        palette='authored RGB preserved' if (directory/'appearance.npz').is_file() else 'canonical material RGB preserved',
        color_variation=dict(voxel_gain=.085,face_gain=.045,warmth=.025),season='spring')
    inventory.write_json(sidecar,record)
    from tools.export_temperate_runtime_appearance import export
    export(directory)
    if thumbnail:
        from tools.tree_appearance_thumbnails import render
        render(directory)
    if protected(directory)!=before:raise ValueError('protected source changed '+str(directory))
    return dict(id=directory.name,status='installed',kind=body['kind'],voxel_mm=record['voxel_mm'],geometry_sha256=before['tree.vxa'],protected=before,
        source=str(directory.relative_to(ROOT)))

def targets():
    for area in (ROOT/'library',inventory.CANDIDATES):
        for p in sorted(area.glob('**/meta.json')):
            if (p.parent/'tree.vxa').is_file() and (p.parent/'spec.json').is_file():
                body=json.loads((p.parent/'spec.json').read_text(encoding='utf8'))
                if in_scope(body):yield p.parent

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--workers',type=int,default=2);ap.add_argument('--limit',type=int);args=ap.parse_args()
    from concurrent.futures import ThreadPoolExecutor
    started=time.time();items=list(targets());items=items[:args.limit] if args.limit else items
    ledger=ROOT/'out/temperate-appearance-rollout';ledger.mkdir(parents=True,exist_ok=True)
    before={}
    for i,d in enumerate(items):
        before[str(d.relative_to(ROOT))]=protected(d)
        if i%250==0:print('AUDIT',i,'/',len(items),flush=True)
    species={str(p.relative_to(ROOT)):inventory.digest(p) for p in (ROOT/'library').glob('*/species.json')}
    inventory.write_json(ledger/'before.json',dict(artifacts=before,species=species))
    rows=[]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for row in pool.map(install,items):
            rows.append(row)
            if len(rows)%50==0:inventory.write_json(ledger/'progress.json',dict(completed=len(rows),total=len(items)));print('APPEARANCE',len(rows),'/',len(items),flush=True)
    failures=[name for name,hashes in before.items() if protected(ROOT/name)!=hashes]
    failures += [name for name,h in species.items() if inventory.digest(ROOT/name)!=h]
    report=dict(models=rows,count=len(rows),protected_failures=failures,elapsed_seconds=round(time.time()-started,2),geometry_and_decisions_preserved=not failures)
    inventory.write_json(ledger/'report.json',report)
    if failures:raise ValueError(str(failures))
    print(json.dumps({k:v for k,v in report.items() if k!='models'}),flush=True)
if __name__=='__main__':main()
