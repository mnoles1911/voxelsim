"""Strict matched256m size-culling OFF/ON comparison; draw cost, not residency."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from analyze_ecological_walk import analyze
from validate_walk_receipt import validate_receipt

FLAG='-VoxelDetailSizeCull'
PIN_FIELDS=('configurationSha256','speciesManifestSha256','detailCacheManifestSha256')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def argument(run,prefix):
    values=[a[len(prefix):].strip('"') for a in run['arguments'] if a.startswith(prefix)]
    if len(values)!=1:raise ValueError('Missing/duplicate argument '+prefix)
    return values[0]
def validate_inputs(old,new):
    if FLAG in old['arguments'] or new['arguments'].count(FLAG)!=1:
        raise ValueError('Expected size-culling OFF then ON')
    def normalized(r):return sorted(a for a in r['arguments'] if a!=FLAG and not a.startswith(('-UserDir=','-abslog=')))
    if normalized(old)!=normalized(new):raise ValueError('Different scene arguments beyond size flag/output paths')
    for run in (old,new):
        if float(argument(run,'-VoxelDetailRingMeters='))!=256:raise ValueError('Requires actual256m ring in both captures')
        for field in PIN_FIELDS:
            if not re.fullmatch('[0-9a-fA-F]{64}',run.get(field,'')):raise ValueError('Missing valid pin '+field)
        modules=run.get('runtimeModuleHashes')
        if not modules or any(not re.fullmatch('[0-9a-fA-F]{64}',v) for v in modules.values()):raise ValueError('Missing runtime binary pins')
    for field in PIN_FIELDS:
        if old[field].lower()!=new[field].lower():raise ValueError('Different pin '+field)
    if {k:v.lower() for k,v in old['runtimeModuleHashes'].items()}!={k:v.lower() for k,v in new['runtimeModuleHashes'].items()}:
        raise ValueError('Different runtime binaries')

def exercise(log,on):
    matches=re.findall(r'DetailSizeCull key=(\w+) .*?startM=([\d.]+) endM=([\d.]+) ringM=([\d.]+) fallback=(\d+)',log)
    if not on and matches:raise ValueError('OFF capture unexpectedly exercised size policy')
    if on and not matches:raise ValueError('ON policy not exercised')
    records=[]
    for key,start,end,ring,fallback in matches:
        start,end,ring=float(start),float(end),float(ring)
        if ring!=256 or not 0<start<=end<=ring or fallback not in ('0','1'):raise ValueError('Invalid size-cull exercise distances')
        records.append(dict(key=key,start_m=start,end_m=end,ring_m=ring,fallback=int(fallback)))
    if on and not any(r['end_m']<256 and not r['fallback'] for r in records):raise ValueError('ON policy never shortened a draw distance')
    return records

def compare(baseline,current):
    roots=(baseline,current);runs=[json.loads((p/'run-manifest.json').read_text(encoding='utf-8-sig')) for p in roots]
    for root in roots:validate_receipt(root)
    validate_inputs(*runs)
    for r in runs:
        config=Path(argument(r,'-VoxelEcologyConfig='));species=Path(argument(r,'-VoxelAssetDir='))/'species.vxm';cache=Path(argument(r,'-VoxelDetailMeshCache='))
        for path,field in ((config,PIN_FIELDS[0]),(species,PIN_FIELDS[1]),(cache,PIN_FIELDS[2])):
            if sha(path)!=r[field].lower():raise ValueError('Changed captured file '+str(path))
    reports=[analyze(p) for p in roots]
    if any(r['passed_checks']!=8 or r['failed_checks'] for r in reports):raise ValueError('Movement checks failed')
    if not reports[0]['internal_resolutions'] or reports[0]['internal_resolutions']!=reports[1]['internal_resolutions']:raise ValueError('Missing/different internal resolutions')
    logs=[(p/'game.log').read_text(errors='replace') for p in roots]
    contexts=[re.search(r'ECOLOGY_MEASURE_BEGIN biome=(\d+) liveInstances=(\d+)',s) for s in logs]
    if any(c is None for c in contexts) or contexts[0].groups()!=contexts[1].groups():raise ValueError('Different initial biome/instance count')
    shaders=[re.search(r'VoxelMarchDispatchIdentity scheduled=1 type=FVoxelMarchCS permutation=(\d+) outputHash=(\w+) sourceHash=(\w+)',s) for s in logs]
    if any(s is None for s in shaders) or shaders[0].groups()!=shaders[1].groups():raise ValueError('Missing/different shader identity')
    exercise(logs[0],False);applied=exercise(logs[1],True)
    fields=('FrameTime','GameThreadTime','GPUTime','VoxelStream/TickMs','VoxelStream/GameThread/DetailTickMs')
    phases={}
    for phase in ('WalkForward','SprintGate'):
        phases[phase]={}
        for field in fields:
            if any(field not in r['phases'][phase]['metrics_ms'] for r in reports):raise ValueError('Missing/ambiguous primary comparison metric '+field)
            a,b=[r['phases'][phase]['metrics_ms'][field] for r in reports]
            phases[phase][field]={'off_ms':a,'on_ms':b,'change_percent':{k:100*(b[k]/a[k]-1) if a[k] else None for k in a}}
    return {'schema':1,'scope':'One matched actual256m editor pair, draw-distance policy only. Not residency savings, shipping or broad gameplay acceptance; no48m inference.',
            'baseline':str(baseline.resolve()),'current':str(current.resolve()),'matching_pins':{f:runs[0][f] for f in PIN_FIELDS},
            'matching_runtime_module_hashes':runs[0]['runtimeModuleHashes'],'binary_scope':'Both captures have passing persisted receipts binding manifest, artifacts, and matching start/end module and input hashes; no competing UE/compiler process observed.',
            'initial_instances':int(contexts[0][2]),'internal_resolutions':reports[0]['internal_resolutions'],'shader_identity':shaders[0].groups(),
            'size_policy_exercise':applied,'csv_column_diagnostics':[r['csv_column_diagnostics'] for r in reports],'phases':phases}
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('baseline',type=Path);p.add_argument('current',type=Path);a=p.parse_args()
    result=compare(a.baseline,a.current);(a.current/'size-culling-comparison.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
