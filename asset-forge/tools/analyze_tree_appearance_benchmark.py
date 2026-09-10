"""Summarize controlled game captures, excluding startup and screenshot frames."""
import csv
import json
import re
from pathlib import Path
import numpy as np
csv.field_size_limit(16*1024*1024)

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/tree-appearance-pilot/game-v2/benchmark-nonnanite'

def main():
    manifest=json.loads((OUT/'manifest.json').read_text(encoding='utf-8-sig'))
    assert len(manifest['runs'])==8,'Expected two reversed-order repeats of both species/modes'
    results=[];gpus=set()
    for run in manifest['runs']:
        log=(OUT/run['log']).read_text(encoding='utf8',errors='replace')
        assert 'shaderplatform="PCD3D_SM6"' in log
        assert not any(s in log for s in ('LoadErrors:','missing usage flag','Fatal error:'))
        gpus.add(re.search(r'Metadata set : gpu="([^"]+)"',log).group(1))
        with (OUT/run['csv']).open(newline='',encoding='utf-8-sig') as stream:
            rows=list(csv.DictReader(stream))
        # CSV capture starts at game frame zero. The screenshot is scheduled at
        # 900. Use the settled interval after it and leave the final frames out.
        sample=rows[1000:1700];assert len(sample)==700
        stats={}
        for name in ('GPUTime','FrameTime','GameThreadTime','RenderThreadTime',
                     'GPU/NaniteVisBuffer','GPU/NaniteBasePass','GPU/Basepass',
                     'GPU/ShadowDepths','GPU/Postprocessing','RHI/PrimitivesDrawn'):
            if name not in sample[0]:continue
            values=np.asarray([float(r[name]) for r in sample])
            assert np.all(np.isfinite(values))
            stats[name]=dict(median=float(np.median(values)),p95=float(np.percentile(values,95)))
        assert stats['GPUTime']['median']>0,'No GPU measurement'
        assert stats.get('RHI/PrimitivesDrawn',{}).get('median',0)>1000,'No measured scene geometry'
        results.append(dict(**run,frames=[1000,1699],samples=700,stats=stats))
    comparisons=[]
    for species in sorted({r['species'] for r in results}):
        modes={m:[r['stats']['GPUTime']['median'] for r in results if r['species']==species and r['mode']==m] for m in ('opaque','mask')}
        opaque=float(np.median(modes['opaque']));masked=float(np.median(modes['mask']))
        comparisons.append(dict(species=species,opaque_gpu_ms=opaque,masked_gpu_ms=masked,delta_ms=masked-opaque,delta_percent=(masked/opaque-1)*100,repeat_medians=modes))
    assert len(gpus)==1,'Capture hardware changed between runs'
    report=dict(status='timings verified; camera images require visual review',gpu=next(iter(gpus)),renderer=manifest['renderer'],resolution=[1280,720],frame_cap=60,
                scope='Single static diagnostic tree plus fixed studio, not a populated Voxelsim forest',
                excluded='Frames 0–999 including screenshot at 900; frames 1700 onward',runs=results,comparisons=comparisons)
    (OUT/'analysis.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(comparisons,indent=2))

if __name__=='__main__':main()
