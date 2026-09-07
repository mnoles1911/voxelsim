"""Refresh every wildlife spec and regenerate every saved seed plus seed 7.

Run from asset-forge: python tools/refresh_creatures.py
The resumable report records exact pitches, hashes, health and round trips.
Existing files are backed up once; curation is never changed by this batch.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import json
import hashlib
from pathlib import Path
import shutil
import time
import numpy as np
import _path  # noqa: F401
from forge import bird, fish, quadruped, pipeline, spec, render, vox, vxa, parts

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out' / 'creature-refresh'
KINDS = {'bird': bird, 'fish': fish, 'cetacean': fish, 'quadruped': quadruped}
GENERATION_REVISION = hashlib.sha256(b''.join(
    (ROOT / 'forge' / f'{name}.py').read_bytes()
    for name in ('bird', 'fish', 'quadruped', 'creature_detail', 'pipeline', 'parts', 'render')
)).hexdigest()
MAX_CELLS = 16_000_000  # Dense fields use many temporary arrays beyond uint8.

# Small, specific art corrections; all other species retain their own palettes.
RETUNES = {
    'common-raven': {'materials.bird_back': 'skin_dark', 'materials.bird_head': 'skin_dark',
                     'materials.bird_eye': 'skin_dark', 'materials.bird_bill': 'beak_horn',
                     'materials.bird_mark': 'plume_slate', 'bird.wing_mark': 'none'},
    'grey-wolf': {'materials.quad_head': 'plume_grey', 'materials.quad_leg': 'plume_grey',
                  'quad.cape': 0.12, 'quad.foot': 0.78},
    'golden-eagle': {'materials.bird_mark': 'skin_brown',
                     'materials.bird_head_mark': 'plume_buff', 'bird.wing_mark': 'none'},
    'rainbow-trout': {'materials.fish_pattern': 'skin_red'},
}


def tuned(raw):
    for key, value in RETUNES.get(raw['name'], {}).items():
        spec.set_(raw, key, value)
    return raw


def choose_pitch(raw, seeds):
    """Measure realized envelopes for every exported seed before allocation."""
    gen = KINDS[raw['kind']]
    for cm in ('1.25', '2.5', '5', '10'):
        candidate = dict(raw, resolution_cm=cm)
        maximum = 0
        for seed in seeds:
            rng = pipeline.rng_for(candidate, seed)
            live, _ = spec.realize(candidate, rng)
            maximum = max(maximum, int(np.prod(gen._params(live, rng, float(cm)/100)['shape'])))
        if maximum <= MAX_CELLS:
            return cm, maximum
    raise RuntimeError(f"{raw['name']}: even 100 mm exceeds {MAX_CELLS:,} cells")


def backup(path):
    dest = OUT / 'originals' / path.relative_to(ROOT)
    if path.exists() and not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        if path.is_dir():
            shutil.copytree(path, dest)
        else:
            shutil.copy2(path, dest)


def export_one(job):
    raw, seed, cells = job
    started = time.monotonic()
    body, validation = spec.validate(raw)
    a = pipeline.build(body, seed)
    name = a.name
    out = ROOT / 'library' / raw['name'] / name
    backup(out)
    stage = OUT / 'staging' / name
    stage.mkdir(parents=True, exist_ok=True)
    spec.save(body, stage / 'spec.json')
    spec.save(a.realized, stage / 'realized.json')
    models = vox.write(a.grid, stage / 'tree.vox', name=name)
    inspected = vox.inspect(stage / 'tree.vox')
    assert inspected['voxels'] == a.grid.count() and not inspected['oversized'], name
    vxa.write(a.grid, stage / 'tree.vxa', a.parts, parts.joints(a.parts))
    decoded, rig, joints = vxa.read_full(stage / 'tree.vxa')
    assert decoded.voxel_m == a.grid.voxel_m
    assert np.array_equal(decoded.origin, a.grid.origin)
    assert np.array_equal(a.parts != 0, a.grid.data != 0)
    assert np.array_equal(decoded.data, a.grid.data), name
    assert np.array_equal(rig, a.parts), name
    assert set(np.unique(a.parts)) - {0, 1} <= {j['part'] for j in joints}, name
    render.view(a.grid, render.camera_for(body), target_px=640, background=(108, 116, 124, 255)).save(stage / 'thumb.png')
    problems = pipeline.health(a)
    meta = dict(id=name, species=raw['name'], kind=raw['kind'], category='creature',
                seed=seed, spec_hash=spec.spec_hash(body), stats=a.stats,
                problems=problems, vox_models=models)
    (stage / 'meta.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
    out.mkdir(parents=True, exist_ok=True)
    for source in stage.iterdir():
        source.replace(out / source.name)
    stage.rmdir()
    return dict(id=name, species=raw['name'], kind=raw['kind'], seed=seed,
                pitch_mm=a.grid.voxel_m*1000, voxels=a.grid.count(),
                envelope_cells=cells, spec_hash=spec.spec_hash(body), problems=problems,
                seconds=round(time.monotonic()-started, 3), roundtrip=True,
                generation_revision=GENERATION_REVISION,
                path=str(out.relative_to(ROOT)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--workers', type=int, default=2)
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    report_path = OUT / 'report.json'
    rows = json.loads(report_path.read_text()) if report_path.exists() else []
    completed = {r['id']: r for r in rows}
    jobs = []
    for path in sorted((ROOT / 'specs').glob('*.json')):
        raw = json.loads(path.read_text(encoding='utf-8'))
        if raw.get('kind') not in KINDS or raw.get('goblin', {}).get('role', 'none') != 'none':
            continue
        if args.only and raw['name'] not in args.only:
            continue
        backup(path)
        raw = tuned(raw)
        saved = sorted((ROOT / 'library' / raw['name']).glob('*/meta.json'))
        seeds = sorted({7} | {int(json.loads(p.read_text())['seed']) for p in saved})
        # Existing variants retain their saved parameter edits.
        variants = {int(json.loads(p.read_text())['seed']):
                    tuned(json.loads((p.parent / 'spec.json').read_text())) for p in saved}
        body, validation = spec.validate(raw)
        cm, cells = choose_pitch(body, seeds)
        raw['resolution_cm'] = cm
        path.write_text(json.dumps(raw, indent=2, sort_keys=True)+'\n', encoding='utf-8')
        for seed in seeds:
            individual = variants.get(seed, raw)
            # Use the canonical spec for newly generated variants and saved edits
            # for existing ones; each still gets its finest feasible pitch.
            ready, _ = spec.validate(individual)
            individual['resolution_cm'], variant_cells = choose_pitch(ready, [seed])
            ready, _ = spec.validate(individual)
            entry = f"{raw['name']}-{seed:04d}"
            prior = completed.get(entry)
            if (not args.force and prior and prior['spec_hash'] == spec.spec_hash(ready)
                    and prior.get('generation_revision') == GENERATION_REVISION):
                if (ROOT / prior['path'] / 'tree.vxa').exists():
                    continue
            jobs.append((individual, seed, variant_cells))
    failures = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(export_one, job): job for job in jobs}
        for i, future in enumerate(concurrent.futures.as_completed(futures), 1):
            job = futures[future]
            try:
                row = future.result()
                completed[row['id']] = row
                print(f"{i}/{len(jobs)} {row['id']} {row['pitch_mm']:g}mm {row['voxels']:,} voxels {row['problems']}", flush=True)
            except Exception as exc:
                failures.append(dict(species=job[0]['name'], seed=job[1], error=str(exc)))
                print(f"FAILED {job[0]['name']}: {exc}", flush=True)
            report_path.write_text(json.dumps(sorted(completed.values(), key=lambda r:r['id']), indent=2)+'\n')
    (OUT / 'failures.json').write_text(json.dumps(failures, indent=2)+'\n')
    print(f'{len(completed)} exported, {len(failures)} failed', flush=True)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())

