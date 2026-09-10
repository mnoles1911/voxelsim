"""Export an exact XY crop of a native placement sample for a tree-only GPU view.

The renderer projects this controlled-terrain sample onto its test terrain.
This is not validation of habitat gates against that rendered terrain.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path


def export(directory, seed, width):
    marker=json.loads((directory/'PREVIEW_ONLY.json').read_text())
    if marker.get('preview_only') is not True:raise ValueError('Private fixture required')
    config=json.loads((directory/'placement.json').read_text())
    profiles={p['species']:p for p in config['profiles']}
    names=sorted(marker['species'])
    raw=(directory/'placement-samples.csv').read_bytes()
    models=[]
    for row in csv.DictReader(raw.decode('utf-8-sig').splitlines()):
        row={k:int(v) for k,v in row.items()}
        if row['world_seed']!=seed or not(-width//2<=row['x_mm']<width//2 and -width//2<=row['y_mm']<width//2):continue
        profile=profiles[names[row['species']]]
        if profile['kind']!='tree':continue
        variant=sorted(profile['variants'],key=lambda v:v['bank_file'])[row['bank_slot']]
        models.append(dict(species=profile['species'],seed=variant['asset_seed'],x_mm=row['x_mm'],y_mm=row['y_mm'],yaw_quarter=row['yaw_quarter']))
    if not 1<=len(models)<=128:raise ValueError(f'Crop contains {len(models)} trees; supported range1..128')
    data=dict(preview_only=True,tree_only=True,world_seed=seed,width_mm=width,source_csv_sha256=hashlib.sha256(raw).hexdigest(),
              terrain_mode='XY preserved; projected onto separate rendered terrain, not a habitat validation',models=models)
    output=directory/f'view-{seed}-{width}.json'
    output.write_text(json.dumps(data,indent=2)+'\n')
    # The existing editable-tree preview expects per-variant identity records.
    publication=json.loads((directory/'appearance/published.json').read_text())
    for row in publication['models']:
        (directory/'appearance'/(row['id']+'.json')).write_text(json.dumps(row,indent=2)+'\n')
    return output,len(models)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--seed',type=int,default=42)
    parser.add_argument('--width-mm',type=int,default=64000)
    args=parser.parse_args()
    print(export(args.directory,args.seed,args.width_mm))
