"""Read-only checkpoint resolution for offline verification tools."""
from pathlib import Path
import hashlib
import json
import re


def _require(condition):
    if not condition:
        raise ValueError("Invalid checkpoint")


def _read_bounded(path, limit):
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    _require(len(data) <= limit)
    return data


def _digest(path, size, limit):
    _require(type(size) is int and 0 <= size <= limit)
    digest = hashlib.md5()
    total = 0
    with path.open('rb') as stream:
        while block := stream.read(min(1024**2, size - total + 1)):
            total += len(block)
            _require(total <= size)
            digest.update(block)
    _require(total == size)
    return digest.hexdigest()


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
            manifest_bytes = _read_bounded(manifest_path, 65536)
            marker = _read_bounded(commit, 32)
            _require(len(marker) == 32)
            _require(hashlib.md5(manifest_bytes).hexdigest().encode() == marker.lower())
            manifest = json.loads(manifest_bytes)
            version = manifest['version']
            _require(type(version) in (int, float))
            if version not in (1, 2, 3):
                raise RuntimeError('Unsupported checkpoint version')
            _require(manifest['generation'] == generation)
            files = manifest['files']
            simulation = version >= 2
            domains = {'water.vxwater', 'hydro.vxhydro', 'simulation.bin'} if simulation else set()
            if version == 3:
                domains.add('gameplay.json')
            _require(isinstance(files, list) and 1 <= len(files) <= (7 if version == 3 else 6 if simulation else 3))
            names = set()
            digests = {}
            for entry in files:
                name = entry['name']
                if not (name in domains or name in ('world.vxlog', 'meta.json') or re.fullmatch(
                    r'world\.vxlog\.detached-[0-9a-fA-F]{32}\.bin', name)):
                    raise ValueError('Invalid checkpoint filename')
                _require(name not in names)
                names.add(name)
                path = directory / name
                limit = {'meta.json': 1024**2, 'simulation.bin': 65536,
                         'gameplay.json': 64 * 1024**2}.get(name, 512 * 1024**2)
                digest = _digest(path, entry['bytes'], limit)
                _require(name not in domains or entry['bytes'] > 0)
                _require(digest == entry['hash'].lower())
                digests[name] = digest
            terrain = directory / 'world.vxlog'
            _require('world.vxlog' in names)
            detached = manifest['detached']
            if detached:
                _require(detached == 'world.vxlog.detached-' + digests['world.vxlog'] + '.bin')
                _require(detached in names)
            _require(domains <= names)
            _require(len(names) == 1 + ('meta.json' in names) + bool(detached) + len(domains))
            return terrain
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue
    if commits:
        raise RuntimeError(f'No complete checkpoint for {logical}')
    return logical
