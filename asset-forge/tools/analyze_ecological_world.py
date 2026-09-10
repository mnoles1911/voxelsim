"""Validate a settled actual-world ecology capture and report whole-frame cost.

This is measurement evidence, not visual acceptance or isolated foliage cost.
"""
import argparse
import csv
import hashlib
import json
import re
from datetime import datetime
from pathlib import Path
import numpy as np


def capture_event_times(log):
    markers=('MEASURE_BEGIN','OVERVIEW','MEASURE_END')
    explicit=[re.findall(r'EcologyWorld '+marker+r' mono=([\d.]+)',log) for marker in markers]
    if any(explicit):
        if any(len(values)!=1 for values in explicit):raise ValueError('Incomplete/duplicate monotonic capture events')
        times=[float(values[0]) for values in explicit]
        source='monotonic'
    else:
        times=[]
        for marker in markers:
            matches=re.findall(r'\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\].*EcologyWorld '+marker,log)
            if len(matches)!=1:raise ValueError('Missing/duplicate capture event: '+marker)
            times.append(datetime.strptime(matches[0],'%Y.%m.%d-%H.%M.%S:%f').timestamp())
        source='legacy_log_clock'
    if not all(np.isfinite(times)) or not times[0]<times[1]<times[2]:raise ValueError('Invalid capture event order')
    return times[1]-times[0],times[2]-times[0],source


def analyze(root):
    log=(root/'game.log').read_text(errors='replace')
    run=json.loads((root/'run-manifest.json').read_text(encoding='utf-8-sig'))
    assert 'EcologyWorld COMPLETE' in log, 'Incomplete capture'
    assert not any(s in log for s in ('EcologyWorld STREAMING_BUSY','Fatal error:',
        'Assertion failed:','FINE TIER GATE LEAK','Failed to compile Material',
        'Terrain appearance upload refused:')), 'Unstable/failed capture'
    args=run['arguments']
    assert '-VoxelEcologyWorldCapture' in args and '-VoxelAssetScatterOff' not in args
    config=Path(next(a.split('=',1)[1] for a in args if a.startswith('-VoxelEcologyConfig=')))
    assert hashlib.sha256(config.read_bytes()).hexdigest().upper()==run['configurationSha256'].upper()
    manifest_pinned='speciesManifestSha256' in run
    if manifest_pinned:
        assert hashlib.sha256((config.parent/'species.vxm').read_bytes()).hexdigest().upper()==run['speciesManifestSha256'].upper(), 'Species lattice changed'
    sample=re.search(r'EcologyWorld SAMPLE biome=(\d+) trees=(\d+) detail=(\d+) liveHism=(\d+) queryMs=([\d.]+)',log)
    assert sample, 'No settled ecological sample'
    biome,trees,detail,live=map(int,sample.groups()[:4])
    assert biome==3, 'Temperate forest validation requires TEMPERATE_FOREST terrain'
    assert trees>0 and detail>0 and live>0, 'Missing populated trees or understory'
    switch,duration,event_clock=capture_event_times(log)
    assert 18<=switch<=22
    csv.field_size_limit(32*1024*1024)
    rows=[];elapsed=0.
    with (root/'frames.csv').open(encoding='utf-8-sig',newline='') as stream:
        for row in csv.DictReader(stream):
            try:delta=float(row['FrameTime'])
            except (ValueError,KeyError,TypeError):continue
            if not np.isfinite(delta) or delta<=0:continue
            elapsed+=delta*.001;row['_time']=elapsed;rows.append(row)
    assert abs(elapsed-duration)<1, f'CSV/event clock mismatch: CSV={elapsed:.6f}s events={duration:.6f}s ({event_clock})'
    windows={}
    for name,lo,hi in [('player',2,8),('overview',switch+2,switch+8)]:
        selected=[r for r in rows if lo<=r['_time']<hi]
        assert len(selected)>=12
        metrics={}
        for field in ('FrameTime','GPUTime','GameThreadTime','RenderThreadTime','GPU/Basepass','GPU/ShadowDepths'):
            if field not in selected[0]:continue
            values=np.asarray([float(r[field]) for r in selected]);assert np.all(np.isfinite(values))
            metrics[field]={'median':float(np.median(values)),'p95':float(np.percentile(values,95))}
        assert metrics['GPUTime']['median']>0
        windows[name]={'samples':len(selected),'metrics_ms':metrics}
    internal=sorted({(int(x),int(y)) for x,y in re.findall(r'px of a (\d+)x(\d+) view',log)})
    assert len(internal)==1,'Missing/unsteady internal resolution'
    for image in ('world-player.png','world-overview.png'):assert (root/image).stat().st_size>0
    # A single forest pixel is insufficient: legacy fallback can dominate the
    # surrounding scene. Keep performance evidence but fail placement scope.
    terrain_path=root/'terrain-samples.csv'
    active_fraction=None
    if terrain_path.exists():
        with terrain_path.open(encoding='utf-8-sig',newline='') as stream:terrain=list(csv.DictReader(stream))
        if len(terrain)==1024:active_fraction=sum(int(r['active'])==1 for r in terrain)/len(terrain)
    result={'scope':'Actual terrain, generated trees and instanced understory; whole-game frame cost',
        'visual_review':'required separately','biome_id':biome,'trees_intersecting_32m_query':trees,
        'detail_intersecting_32m_query':detail,'live_hism_instances':live,'placement_query_ms':float(sample[5]),
        'internal_resolution':internal[0],'windows':windows,'run':run,'event_clock':event_clock,
        'ecology_active_fraction':active_fraction,
        'species_manifest_pinned':manifest_pinned,
        'performance_measurement_excluded':bool(run.get('visualOnly',False)),
        'placement_scope_valid':active_fraction is not None and active_fraction>=.8,
        'limitation':('Visual-only run: frame timings excluded from performance acceptance. ' if run.get('visualOnly',False) else '')+
            'Single stationary capture; not traversal, full-resolution or isolated vegetation cost.'}
    (root/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    result=analyze(parser.parse_args().directory)
    print(json.dumps(result,indent=2))
    if not result['placement_scope_valid']:raise SystemExit('Insufficient verified ecological coverage for placement acceptance')
