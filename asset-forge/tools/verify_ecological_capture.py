"""Verify a completed GPU crop retained native ecological XY/source selections."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def verify(run):
    manifest=json.loads((run/'run-manifest.json').read_text(encoding='utf-8-sig'))
    layout_path=Path(manifest['ecologicalLayout'])
    layout_bytes=layout_path.read_bytes()
    if hashlib.sha256(layout_bytes).hexdigest()!=manifest['ecologicalLayoutSha256']:
        raise ValueError('Layout identity changed')
    layout=json.loads(layout_bytes)
    models=layout['models']
    log=(run/'game.log').read_text(encoding='utf-8-sig')
    if f'AppearanceForest COMPLETE count={len(models)}' not in log:
        raise ValueError('Capture has not completed')
    if 'automatic scatter explicitly disabled for isolated actor preview' not in log:
        raise ValueError('Background scatter was not disabled')
    placements=re.findall(r'AppearanceForest PLACEMENT i=(\d+) attempt=(\d+) position=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)',log)
    sources=re.findall(r'AppearanceForest SPAWN i=(\d+) species=([a-z0-9-]+) seed=(\d+)',log)
    if len(placements)!=len(models) or len(sources)!=len(models):
        raise ValueError('Missing/duplicate source or position evidence')
    translation=None
    for i,(position,source,model) in enumerate(zip(placements,sources,models)):
        index,attempt,x,y,z=position
        if int(index)!=i or int(attempt)!=0 or source!=(str(i),model['species'],str(model['seed'])):
            raise ValueError(f'Placement/source mismatch at {i}')
        delta=(float(x)-model['x_mm']/10,float(y)-model['y_mm']/10)
        if translation is None:translation=delta
        if max(abs(a-b) for a,b in zip(delta,translation))>0.02:
            raise ValueError(f'Horizontal layout changed at {i}')
    for name in ('forest-before.png','forest-after.png','frames.csv'):
        if not (run/name).is_file():raise ValueError(f'Missing capture artifact: {name}')
    report=dict(verified_tree_count=len(models),xy_preserved=True,source_selections_preserved=True,
                terrain='projected; terrain habitat and understory untested',
                visual_acceptance='requires image inspection',gpu_performance='requires CSV analysis')
    (run/'ecological-capture-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run',type=Path)
    print(json.dumps(verify(parser.parse_args().run),indent=2))
