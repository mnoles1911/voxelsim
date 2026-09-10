"""Evaluate completed actual-pawn ecological movement captures by phase."""
import argparse
import csv
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import numpy as np
from analyze_ecological_route_frames import frame_rows


def collision_counts(rows):
    totals={}
    for name in ('CollisionSweepCalls','CollisionPreparations'):
        field='VoxelStream/'+name
        if not rows or field not in rows[0]:continue
        values=[float(row[field]) for row in rows]
        if any(not np.isfinite(v) or v<0 or not v.is_integer() for v in values):
            raise ValueError('Invalid collision count')
        totals[name]=int(sum(values))
    calls=totals.get('CollisionSweepCalls');preparations=totals.get('CollisionPreparations')
    return dict(totals=totals,sweeps_per_preparation=(calls/preparations
        if calls is not None and preparations else None))


def read_frames(path):
    rows=[];elapsed=0
    csv.field_size_limit(32*1024*1024)
    csv_diagnostics={}
    for row in frame_rows(path,csv_diagnostics):
        try:delta=float(row['FrameTime'])
        except (KeyError,TypeError,ValueError) as exc:raise ValueError('Malformed frame duration') from exc
        if not np.isfinite(delta) or delta<=0:raise ValueError('Invalid frame duration')
        elapsed+=delta/1000;rows.append((elapsed,row))
    return rows,elapsed,csv_diagnostics


def analyze(root):
    log=(root/'game.log').read_text(errors='replace')
    run=json.loads((root/'run-manifest.json').read_text(encoding='utf-8-sig'))
    complete=re.search(r'VoxelWalkTest COMPLETE: (\d+) passed, (\d+) FAILED\.',log)
    if not complete:raise ValueError('Incomplete movement test')
    for failure in ('Fatal error:','Assertion failed:','FINE TIER GATE LEAK','Terrain appearance upload refused:'):
        if failure in log:raise ValueError(f'Invalid movement capture: {failure}')
    args=run['arguments']
    if '-VoxelWalkWaitForEcology' not in args or '-VoxelAssetScatterOff' in args:
        raise ValueError('Not the settled ecological movement fixture')
    config=Path(next(a.split('=',1)[1] for a in args if a.startswith('-VoxelEcologyConfig=')))
    if hashlib.sha256(config.read_bytes()).hexdigest().upper()!=run['configurationSha256'].upper():
        raise ValueError('Placement configuration changed')
    manifest_pinned='speciesManifestSha256' in run
    if manifest_pinned and hashlib.sha256((config.parent/'species.vxm').read_bytes()).hexdigest().upper()!=run['speciesManifestSha256'].upper():
        raise ValueError('Species lattice changed')
    stamp=r'\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\].*'
    def time_of(marker):
        found=re.search(stamp+re.escape(marker),log)
        if not found:raise ValueError(f'Missing event: {marker}')
        return datetime.strptime(found[1],'%Y.%m.%d-%H.%M.%S:%f')
    start=time_of('VoxelWalkTest ECOLOGY_MEASURE_BEGIN')
    end=time_of('VoxelWalkTest ECOLOGY_MEASURE_END')
    context=re.search(r'ECOLOGY_MEASURE_BEGIN biome=(\d+) liveInstances=(\d+)',log)
    if not context or int(context[1])!=3 or int(context[2])<=0:raise ValueError('Missing populated temperate forest')
    phase_names=['Settle','Fall','WalkForward','SprintGate','JumpApex','JumpTapVsHold','CrouchGeometry']
    starts=[(time_of('VoxelWalkTest: phase '+name)-start).total_seconds() for name in phase_names]
    ends=starts[1:]+[(end-start).total_seconds()]
    rows,elapsed,csv_diagnostics=read_frames(root/'frames.csv')
    if abs(elapsed-(end-start).total_seconds())>1:raise ValueError('CSV/event clock mismatch')
    phases={}
    for name,lo,hi in zip(phase_names,starts,ends):
        selected=[row for t,row in rows if lo+.1<=t<hi]
        if not selected:raise ValueError(f'No measured frames for {name}')
        metrics={}
        fields=['FrameTime','GameThreadTime','GPUTime','RenderThreadTime']
        fields += [f for f in selected[0] if isinstance(f,str) and f.startswith(('VoxelStream/', 'VoxelWorklist/')) and
            (f.endswith('Ms') or f.endswith('CollisionSweep'))]
        for field in fields:
            values=np.array([float(r[field]) for r in selected])
            if not np.all(np.isfinite(values)) or np.any(values<0):raise ValueError('Invalid frame metric')
            metrics[field]={'median':float(np.median(values)),'p95':float(np.percentile(values,95)),'max':float(values.max())}
        worst=sorted(selected,key=lambda r:float(r['FrameTime']),reverse=True)[:3]
        phases[name]={'samples':len(selected),'metrics_ms':metrics,
            'collision_counts':collision_counts(selected),
            'worst_frames_ms':[{f:float(r[f]) for f in fields} for r in worst]}
    result={'scope':'Actual movement controller, scripted short route; not general navigability or feel acceptance',
        'csv_column_diagnostics':csv_diagnostics,
        'passed_checks':int(complete[1]),'failed_checks':int(complete[2]),'phases':phases,
        'species_manifest_pinned':manifest_pinned,
        'internal_resolutions':sorted({(int(x),int(y)) for x,y in re.findall(r'px of a (\d+)x(\d+) view',log)}),
        'check_reports':re.findall(r'VoxelWalkTest \[(?:PASS|FAIL)\].*',log)}
    (root/'walk-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path)
    result=analyze(p.parse_args().directory);print(json.dumps(result,indent=2))
    if result['failed_checks']:raise SystemExit(1)
