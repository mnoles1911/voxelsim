"""Read-only checkpoint resolution for offline verification tools."""
from pathlib import Path
import hashlib
import json
import re


def _require(condition):
    if not condition:
        raise ValueError("Invalid checkpoint")


def resolve_checkpoint(logical):
    logical = Path(logical)
    root = Path(str(logical) + '.checkpoints')
    commits = sorted(root.glob('*.commit'), reverse=True)
    for commit in commits:
        try:
            generation = commit.stem
            _require(re.fullmatch(r'\d{20}-[0-9a-fA-F]{32}', generation))
            directory = root / generation
            manifest_path = directory / 'manifest.json'
            _require(manifest_path.stat().st_size <= 65536)
            manifest_bytes = manifest_path.read_bytes()
            _require(commit.stat().st_size == 32)
            _require(hashlib.md5(manifest_bytes).hexdigest() == commit.read_text().lower())
            manifest = json.loads(manifest_bytes)
            if manifest['version'] not in (1, 2):
                raise RuntimeError('Unsupported checkpoint version')
            _require(manifest['generation'] == generation)
            files = manifest['files']
            simulation = manifest['version'] == 2
            domains = {'water.vxwater', 'hydro.vxhydro', 'simulation.bin'} if simulation else set()
            _require(1 <= len(files) <= (6 if simulation else 3))
            names = set()
            for entry in files:
                name = entry['name']
                if not (name in domains or name in ('world.vxlog', 'meta.json') or re.fullmatch(
                    r'world\.vxlog\.detached-[0-9a-fA-F]{32}\.bin', name)):
                    raise ValueError('Invalid checkpoint filename')
                _require(name not in names)
                names.add(name)
                path = directory / name
                limit = 1024**2 if name == 'meta.json' else (65536 if name == 'simulation.bin' else 512 * 1024**2)
                _require(path.stat().st_size == entry['bytes'] <= limit)
                _require(name not in domains or entry['bytes'] > 0)
                _require(hashlib.md5(path.read_bytes()).hexdigest() == entry['hash'].lower())
            terrain = directory / 'world.vxlog'
            _require('world.vxlog' in names)
            detached = manifest['detached']
            if detached:
                _require(detached == 'world.vxlog.detached-' + hashlib.md5(terrain.read_bytes()).hexdigest() + '.bin')
                _require(detached in names)
            _require(domains <= names)
            _require(len(names) == 1 + ('meta.json' in names) + bool(detached) + len(domains))
            return terrain
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue
    if commits:
        raise RuntimeError(f'No complete checkpoint for {logical}')
    return logical
