"""Compare repeated visible stands with matched hidden-tree controls."""
import argparse
import json
from pathlib import Path
from statistics import median


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directories', nargs='+', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    reports = [json.loads((p / 'analysis.json').read_text()) for p in args.directories]
    assert len(reports) >= 4, 'Need at least two visible and two control captures'
    base = reports[0]
    for report in reports:
        assert not report['run'].get('cpuTrace', False), 'CPU trace runs are diagnostic, not clean performance repeats'
        assert report['run'].get('rayQueryMode', 0) != 2, 'Ray audit runs are diagnostic, not clean performance repeats'
        for field in ('count', 'spawnAt', 'outputResolution', 'publicationSha256',
                      'paletteSha256', 'gameModuleSha256', 'forestInputsSha256'):
            assert report['run'][field] == base['run'][field], f'Unmatched {field}'
        for field in ('gpu', 'internal_resolution'):
            assert report[field] == base[field], f'Unmatched {field}'
        assert report['placements'] == base['placements'], 'Different ground-fit positions'
        def semantic_args(r):
            return [a for a in r['run']['arguments'] if a != '-VoxelAppearanceForestHidden'
                    and not a.startswith(('-UserDir=', '-abslog=', '-VoxelAppearanceForestOutput='))]
        assert semantic_args(report) == semantic_args(base), 'Different game arguments'
        identity = lambda r: [(s['index'], s['species'], s['seed']) for s in r['spawns']]
        assert identity(report) == identity(base), 'Different stand variants/order'
    groups = {label: [r for r in reports if bool(r['run']['hiddenControl']) == hidden]
              for label, hidden in (('visible', False), ('hidden_control', True))}
    assert all(len(group) >= 2 for group in groups.values()), 'Insufficient repeats'
    windows = {}
    for window in ('before_edit', 'after_edit'):
        comparison = {}
        for field in ('GPUTime', 'FrameTime', 'GameThreadTime', 'RenderThreadTime'):
            if not all(field in r['windows'][window]['metrics'] for r in reports):
                continue
            values = {name: [r['windows'][window]['metrics'][field]['median'] for r in group]
                      for name, group in groups.items()}
            comparison[field] = {
                name: {'run_medians_ms': vals, 'median_ms': median(vals),
                       'spread_ms': max(vals) - min(vals)} for name, vals in values.items()}
            comparison[field]['visible_minus_control_ms'] = (
                median(values['visible']) - median(values['hidden_control']))
        windows[window] = comparison
    output = {'scope': 'Repeated full-game editable stand versus the same actors hidden',
              'count': base['count'], 'gpu': base['gpu'],
              'output_resolution': base['run']['outputResolution'],
              'internal_resolution': base['internal_resolution'],
              'captures': [str(p.resolve()) for p in args.directories],
              'windows': windows,
              'limitations': ['Includes whole-tree rendering, shadows, depth and scene interactions; not isolated cutout material cost.',
                              'Hidden actors retain geometry and edit work; this is not a memory or generation baseline.',
                              'Reported spread is repeat variation, not a confidence interval; inspect screenshots separately.']}
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
