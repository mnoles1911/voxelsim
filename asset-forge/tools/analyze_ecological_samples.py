"""Audit native placement samples against the isolated fixture's exact bank slots.

Spacing checks use authored trunk exclusion; crown coverage is a conservative
bounding-disc diagnostic, not rendered foliage, navigability or visual acceptance.
"""
import argparse
import csv
import json
from statistics import median
from collections import Counter, defaultdict
from pathlib import Path


def analyze(directory: Path):
    marker = json.loads((directory/'PREVIEW_ONLY.json').read_text())
    if marker.get('preview_only') is not True:
        raise ValueError('Expected isolated preview')
    config = json.loads((directory/'placement.json').read_text())
    profiles = {p['species']: p for p in config['profiles']}
    names = sorted(marker['species'])
    slots = {name: sorted(profiles[name]['variants'], key=lambda v: v['bank_file']) for name in names}
    samples = defaultdict(list)
    for row in csv.DictReader((directory/'placement-samples.csv').open(encoding='utf-8-sig')):
        row = {k: int(v) for k, v in row.items()}
        name = names[row['species']]
        variant = slots[name][row['bank_slot']]
        samples[row['world_seed']].append((row, profiles[name], variant))
    report = {'fixture': str(directory.resolve()), 'controlled_terrain': True,
              'crown_metric': 'bounding-disc proxy, not visible leaf coverage', 'samples': []}
    for seed, rows in sorted(samples.items()):
        # Native queries also return assets whose bounds intersect the region.
        # Count anchors inside it, but retain the halo for neighborhood checks.
        inside = [r for r in rows if -128000 <= r[0]['x_mm'] < 128000 and -128000 <= r[0]['y_mm'] < 128000]
        trees = [r for r in rows if r[1]['kind'] == 'tree']
        violations = []
        minimum_clearance = None
        for i, (a, _, av) in enumerate(trees):
            for b, _, bv in trees[i+1:]:
                distance2 = (a['x_mm']-b['x_mm'])**2 + (a['y_mm']-b['y_mm'])**2
                required = av['trunk_exclusion_mm'] + bv['trunk_exclusion_mm']
                clearance = distance2**0.5-required
                minimum_clearance = clearance if minimum_clearance is None else min(minimum_clearance, clearance)
                if distance2 < required**2:
                    violations.append([a['x_mm'], a['y_mm'], b['x_mm'], b['y_mm']])
        open_cells = 0
        total_cells = 0
        for x in range(-126000, 128000, 4000):
            for y in range(-126000, 128000, 4000):
                total_cells += 1
                if not any((r['x_mm']-x)**2+(r['y_mm']-y)**2 < v['bounds_radius_mm']**2 for r, _, v in trees):
                    open_cells += 1
        by_stand = {}
        if inside and 'stand' in inside[0][0]:
            for stand, label in enumerate(('mixed','young','open','ancient','thicket')):
                selected=[(r,p,v) for r,p,v in inside if r['stand']==stand and p['kind']=='tree']
                heights=[v['height_mm'] for _,_,v in selected]
                by_stand[label]={'tree_anchors':len(selected),
                    'median_height_mm':median(heights) if heights else None,
                    'max_height_mm':max(heights) if heights else None,
                    'growth_forms':dict(Counter(v.get('growth_form') or 'unspecified' for _,_,v in selected)),
                    'community_counts':dict(Counter(str(r['community']) for r,_,_ in selected))}
        report['samples'].append({'seed': seed, 'anchor_counts': dict(sorted(Counter(p['species'] for _, p, _ in inside).items())),
            'tree_stand_statistics':by_stand,
            'stand_statistics_scope':'Selected tree anchors only; not stand land-area or event encounter frequency',
            'spacing_violations': violations, 'minimum_exclusion_clearance_mm': minimum_clearance,
            'outside_crown_bounds_fraction': open_cells/total_cells})
    output = directory/'placement-analysis.json'
    output.write_text(json.dumps(report, indent=2)+'\n')
    if any(s['spacing_violations'] for s in report['samples']):
        raise ValueError(f'Tree spacing violations; inspect {output}')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(json.dumps(analyze(parser.parse_args().directory), indent=2))
