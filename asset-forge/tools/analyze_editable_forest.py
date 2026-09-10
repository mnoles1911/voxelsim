"""Report actual full-game forest captures without treating startup as tree cost."""
import argparse
import csv
import json
import re
from datetime import datetime
from pathlib import Path
import numpy as np

def main():
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);args=parser.parse_args()
    root=args.directory;log=(root/'game.log').read_text(errors='replace')
    manifest=json.loads((root/'run-manifest.json').read_text(encoding='utf-8-sig'))
    layout=bool(manifest.get('ecologicalLayout'))
    midpoint='AppearanceForest ECOLOGY_PLAYER_VIEW eyeHeightCm=170' if layout else 'AppearanceForest EDIT success=1'
    assert 'AppearanceForest COMPLETE' in log and midpoint in log
    assert 'AppearanceForest STREAM_SETTLED pending=0 inFlight=0' in log, 'No verified terrain settle before measurement'
    assert 'AppearanceForest STREAMING_BUSY' not in log, 'Terrain resumed streaming during measurement'
    assert not any(s in log for s in ('Fatal error:','Assertion failed:','Failed to compile Material','FINE TIER GATE LEAK'))
    csv.field_size_limit(32*1024*1024)
    rows=[];elapsed=0.
    with (root/'frames.csv').open(newline='',encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            try:delta=float(row['FrameTime']);gpu=float(row['GPUTime'])
            except (ValueError,KeyError,TypeError):continue
            if not np.isfinite(delta) or delta<=0:continue
            elapsed+=delta*.001;row['_seconds']=elapsed;rows.append(row)
    def event_time(marker):
        match=re.search(r'\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\].*'+re.escape(marker),log)
        assert match,f'Missing timed event {marker}'
        return datetime.strptime(match.group(1),'%Y.%m.%d-%H.%M.%S:%f')
    begin=event_time('AppearanceForest MEASURE_BEGIN')
    edit_seconds=(event_time(midpoint)-begin).total_seconds()
    measured_seconds=(event_time('AppearanceForest MEASURE_END')-begin).total_seconds()
    assert abs(elapsed-measured_seconds)<1.,'CSV clock disagrees with actual capture events'
    assert 18<=edit_seconds<=22,'Unexpected stall moved edit into a sampling window'
    # Exclude screenshots, edit and capture startup/tail; anchor the post-edit
    # window to the actual logged edit instead of assuming perfect timers.
    windows={}
    for label,low,high in ((('overview' if layout else 'before_edit'),2,8),(('player_view' if layout else 'after_edit'),edit_seconds+2,edit_seconds+8)):
        selected=[r for r in rows if low<=r['_seconds']<high]
        assert len(selected)>=12,f'Insufficient measured frames for {label}'
        metrics={}
        for field in ('GPUTime','FrameTime','GameThreadTime','RenderThreadTime','RHI/PrimitivesDrawn','GPU/Basepass','GPU/ShadowDepths'):
            if field not in selected[0]:continue
            values=np.asarray([float(r[field]) for r in selected]);assert np.all(np.isfinite(values))
            metrics[field]=dict(median=float(np.median(values)),p95=float(np.percentile(values,95)))
        assert metrics['GPUTime']['median']>0
        windows[label]=dict(samples=len(selected),seconds=[low,high],metrics=metrics)
    spawns=[dict(index=int(i),species=s,seed=int(seed),milliseconds=float(ms)) for i,s,seed,ms in re.findall(r'AppearanceForest SPAWN i=(\d+) species=(\S+) seed=(\d+) ms=([\d.]+)',log)]
    manifest=json.loads((root/'run-manifest.json').read_text(encoding='utf-8-sig'))
    assert manifest['schema']==1
    expected=manifest['count']
    assert len(spawns)==expected and sorted(s['index'] for s in spawns)==list(range(expected)), 'Missing or duplicate forest spawn'
    assert re.search(r'AppearanceForest COMPLETE count='+str(expected)+r'\b',log), 'Completion count mismatch'
    placements=[dict(index=int(i),attempt=int(a),position=p.strip()) for i,a,p in
                re.findall(r'AppearanceForest PLACEMENT i=(\d+) attempt=(\d+) position=([^\r\n]+)',log)]
    assert sorted(p['index'] for p in placements)==list(range(expected)), 'Missing or duplicate placement'
    if 'sequencePass' in manifest:
        assert '-VoxelAppearanceForestSequence' in manifest['arguments']
        assert re.search(r'AppearanceForest PASS_BEGIN name='+re.escape(manifest['sequencePass'])+
                         r' hidden='+str(int(bool(manifest['hiddenControl'])))+r'\b',log)
    else:
        assert bool(manifest['hiddenControl']) == ('-VoxelAppearanceForestHidden' in manifest['arguments'])
    internal=sorted({(int(x),int(y)) for x,y in re.findall(r'px of a (\d+)x(\d+) view',log)})
    assert len(internal)==1,'Expected one evidenced internal view resolution'
    edit=None if layout else float(re.search(r'AppearanceForest EDIT success=1 ms=([\d.]+)',log).group(1))
    gpu=re.search(r'Metadata set : gpu="([^"]+)"',log)
    report=dict(scope=('Ecological XY tree crop projected onto game terrain; excludes understory and terrain habitat validation' if layout else 'Full game terrain and editable published-tree stand; not isolated material cost'),
                visual_review='required separately',gpu=gpu.group(1) if gpu else None,count=len(spawns),spawns=spawns,edit_ms=edit,
                frames=len(rows),capture_seconds=elapsed,windows=windows,run=manifest,
                internal_resolution=list(internal[0]),placements=placements,
                limitation='Single run; compare repeated matched controls before attributing cost to foliage or scaling.')
    (root/'analysis.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
if __name__=='__main__':main()
