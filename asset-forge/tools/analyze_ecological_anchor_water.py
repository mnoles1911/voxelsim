"""Audit exact-anchor water observations from native world captures.

Counts describe placed assets, not area-normalized density or causal water
response. Shoreline concentration also needs available-habitat area/control
comparisons. Never infer individual anchor facts from display-grid samples.
"""
import argparse
from collections import Counter,defaultdict
import csv
import json
from pathlib import Path
from analyze_ecological_plots import captured_bank_names

REQUIRED={'bank_id','facts_known','active','water_mm','distance_water_mm','slope_mm_per_m'}


def distance_band(d):
    return ('unknown_or_saturated' if d==2147483647 else
            'within_8m' if 0<=d<=8000 else '8_to_32m' if 8000<d<=32000 else
            '32_to_80m' if 32000<d<=80000 else 'beyond_80m' if d>80000 else 'invalid_distance')


def sampled_exposure(anchors,terrain,names):
    """Estimate band area from the native 8m grid, never anchor facts.

    Narrow banks can fall between samples. Rates are diagnostic estimates,
    not species-suitable habitat, a causal experiment, or acceptance evidence.
    """
    xs=sorted({r['x_mm'] for r in terrain});ys=sorted({r['y_mm'] for r in terrain})
    if len(xs)!=32 or len(ys)!=32 or len(terrain)!=1024 or len({(r['x_mm'],r['y_mm']) for r in terrain})!=1024:
        raise ValueError('Expected complete native 32 by 32 terrain survey')
    if any(b-a!=8000 for axis in (xs,ys) for a,b in zip(axis,axis[1:])):
        raise ValueError('Unexpected terrain survey spacing')
    samples=Counter(distance_band(r['distance_water_mm']) for r in terrain if r['active']==1)
    counts=defaultdict(Counter)
    for row in anchors:
        if not xs[0]-4000<=row['x_mm']<xs[-1]+4000 or not ys[0]-4000<=row['y_mm']<ys[-1]+4000:
            continue
        if not row['facts_known'] or row['active']!=1:continue
        if not 0<=row['bank_id']<len(names):raise ValueError('Invalid bank identity')
        counts[names[row['bank_id']]][distance_band(row['distance_water_mm'])]+=1
    species=[]
    for name,bands in sorted(counts.items()):
        observations=[]
        for band in sorted(set(samples)|set(bands)):
            area=samples[band]*64
            rate=bands[band]*10000/area if area and band not in ('unknown_or_saturated','invalid_distance') else None
            observations.append(dict(band=band,anchors=bands[band],terrain_samples=samples[band],
                estimated_area_m2=area,estimated_anchors_per_hectare=rate))
        species.append(dict(species=name,bands=observations))
    return dict(scope=sampled_exposure.__doc__,sample_spacing_m=8,
        bounds_mm=[xs[0]-4000,ys[0]-4000,xs[-1]+4000,ys[-1]+4000],
        active_terrain_samples=sum(samples.values()),species=species)


def summarize(rows,names):
    groups=defaultdict(list)
    for row in rows:
        if not REQUIRED<=row.keys():raise ValueError('Capture lacks exact-anchor water facts; a new native capture is required')
        bank=row['bank_id']
        if not 0<=bank<len(names):raise ValueError('Invalid bank identity')
        groups[names[bank]].append(row)
    result=[]
    for name,anchors in sorted(groups.items()):
        counts=Counter()
        for row in anchors:
            if not row['facts_known']:
                counts['unknown_facts']+=1
                continue
            d=row['distance_water_mm']
            band=distance_band(d)
            counts[band]+=1
            if row['water_mm']>300:counts['over_300mm_standing_water']+=1
            if row['active']!=1:counts['outside_ecology_scope']+=1
        result.append(dict(species=name,anchors=len(anchors),observations=dict(counts)))
    return result


def analyze(root):
    run=json.loads((root/'run-manifest.json').read_text(encoding='utf-8-sig'))
    fixture=Path(next(a.split('=',1)[1] for a in run['arguments'] if a.startswith('-VoxelAssetDir=')))
    names=captured_bank_names(fixture,run)
    with (root/'terrain-placement.csv').open(encoding='utf-8-sig',newline='') as stream:
        reader=csv.DictReader(stream)
        if not REQUIRED<=set(reader.fieldnames or []):raise ValueError('Capture lacks exact-anchor water facts; a new native capture is required')
        rows=[{k:int(v) for k,v in row.items()} for row in reader]
    with (root/'terrain-samples.csv').open(encoding='utf-8-sig',newline='') as stream:
        terrain=[{k:int(v) for k,v in row.items()} for row in csv.DictReader(stream)]
    return dict(scope=__doc__,species_manifest_sha256=run['speciesManifestSha256'],
        includes_intersecting_anchors_outside_survey=True,species=summarize(rows,names),
        sampled_area_comparison=sampled_exposure(rows,terrain,names),
        shoreline_concentration_validated=False)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    args=parser.parse_args()
    result=analyze(args.capture)
    (args.capture/'anchor-water-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
