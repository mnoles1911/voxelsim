"""Publish approved saved craft assets with an omission-based runtime gate."""
from __future__ import annotations
import json
from pathlib import Path
import shutil
import tempfile
import time
from . import categories, materials, spec, vxa


def craft_snapshot(specs: Path, library: Path):
    """Validate sources before any published file changes; return index/copies."""
    rows, copies = [], []
    for path in sorted(specs.glob('*.json')):
        body, _ = spec.load(path)
        if categories.of(body) != 'craftable' or spec.curation(body)['status'] != 'approved':
            continue
        seeds = set(spec.curation(body)['seeds'])
        grids = []
        for entry in sorted((library / path.stem).glob('*')):
            if not entry.is_dir():
                continue
            suffix = entry.name.rsplit('-', 1)[-1]
            if not suffix.isdigit() or int(suffix) not in seeds:
                continue
            source = entry / 'tree.vxa'
            if not source.is_file():
                raise ValueError(f'{entry.name}: approved saved craft has no tree.vxa')
            grid, _, _ = vxa.read_full(source)
            if grid.count() == 0 or grid.data.max() >= materials.ENGINE_MATERIAL_COUNT:
                raise ValueError(f'{entry.name}: empty grid or unsupported material')
            if grid.voxel_m * 1000 not in (12.5, 25, 50, 100):
                raise ValueError(f'{entry.name}: unsupported pitch {grid.voxel_m*1000:g} mm')
            relative = Path(path.stem) / entry.name / 'tree.vxa'
            copies.append((source, relative))
            grids.append(dict(seed=int(suffix), vxa=relative.as_posix(),
                              voxel_mm=grid.voxel_m*1000, voxels=grid.count(),
                              bytes=source.stat().st_size))
        if not grids:
            raise ValueError(f'{path.stem}: approved craft has no saved approved-seed model')
        rows.append(dict(name=path.stem, kind=body['kind'], curation='approved', grids=grids))
    return dict(format='asset-forge-categories', version=1,
                note='Published approved craft only. Grid paths are relative to this directory.',
                categories={'craftable': dict(count=len(rows), species=rows)}, refused=[]), copies


def _replace_directory(source, target):
    # Windows scanners can briefly hold a renamed directory open. Retrying
    # that transient sharing failure preserves the all-or-rollback operation.
    for attempt in range(8):
        try:
            source.replace(target)
            return
        except PermissionError:
            if attempt == 7:
                raise
            time.sleep(.05 * (attempt + 1))


def publish_craft(specs: Path, library: Path, destination: Path, *, check_only=False):
    index, copies = craft_snapshot(specs, library)
    expected = json.dumps(index, indent=2, sort_keys=True)+'\n'
    destination = destination.absolute()
    if destination.is_symlink():
        raise ValueError('Published craft destination must not be a symbolic link')
    if check_only:
        actual = destination / 'categories.json'
        if not actual.is_file() or actual.read_text(encoding='utf-8') != expected:
            raise ValueError('Published craft categories are missing or stale')
        wanted = {relative.as_posix() for _, relative in copies}
        present = {p.relative_to(destination).as_posix() for p in destination.rglob('*.vxa')}
        if wanted != present:
            raise ValueError('Published craft has missing or held-back model files')
        for source, relative in copies:
            if source.read_bytes() != (destination / relative).read_bytes():
                raise ValueError(f'{relative}: published model differs from its saved source')
        return len(copies)
    destination.parent.mkdir(parents=True, exist_ok=True)
    # TemporaryDirectory owns only its freshly created path. A failed source
    # validation/copy leaves the live directory intact; failed replacement rolls back.
    with tempfile.TemporaryDirectory(prefix='.craft-publish-', dir=destination.parent) as tmp:
        stage, old = Path(tmp)/'next', Path(tmp)/'previous'
        stage.mkdir()
        for source, relative in copies:
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        (stage/'categories.json').write_text(expected, encoding='utf-8')
        if destination.exists():
            _replace_directory(destination, old)
        try:
            _replace_directory(stage, destination)
        except BaseException:
            if old.exists():
                _replace_directory(old, destination)
            raise
    return len(copies)
