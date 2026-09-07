"""Independent storage protocol checks; also run with python -O."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from checkpoint_store import resolve_checkpoint


class CheckpointStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.logical = Path(self.temp.name) / 'world.vxlog'
        self.logical.write_bytes(b'legacy')
        self.root = Path(str(self.logical) + '.checkpoints')
        self.root.mkdir()

    def generation(self, sequence, version=2, omit=None):
        name = f'{sequence:020d}-' + f'{sequence:032x}'
        directory = self.root / name
        directory.mkdir()
        data = {'world.vxlog': b'terrain', 'meta.json': b'{}'}
        if version == 2:
            data.update({'water.vxwater': bytes([sequence]),
                         'hydro.vxhydro': b'hydro', 'simulation.bin': b'clock'})
        entries = []
        for filename, payload in data.items():
            if filename == omit:
                continue
            (directory / filename).write_bytes(payload)
            entries.append({'name': filename, 'bytes': len(payload),
                            'hash': hashlib.md5(payload).hexdigest()})
        manifest = json.dumps({'version': version, 'generation': name,
                               'files': entries, 'detached': ''}).encode()
        (directory / 'manifest.json').write_bytes(manifest)
        (self.root / (name + '.commit')).write_text(hashlib.md5(manifest).hexdigest())
        return directory / 'world.vxlog'

    def test_complete_simulation(self):
        newest = self.generation(1)
        self.assertEqual(resolve_checkpoint(self.logical), newest)

    def test_missing_each_domain_recovers_whole_generation(self):
        old = self.generation(1)
        for index, name in enumerate(('water.vxwater', 'hydro.vxhydro', 'simulation.bin'), 2):
            newest = self.generation(index)
            (newest.parent / name).unlink()
            self.assertEqual(resolve_checkpoint(self.logical), old)

    def test_manifest_cannot_omit_required_domain(self):
        self.generation(1, omit='hydro.vxhydro')
        with self.assertRaises(RuntimeError):
            resolve_checkpoint(self.logical)

    def test_damaged_commit_never_uses_legacy(self):
        newest = self.generation(1)
        newest.write_bytes(b'changed')
        with self.assertRaises(RuntimeError):
            resolve_checkpoint(self.logical)

    def test_unknown_version_refuses_older_generation(self):
        self.generation(1, version=1)
        self.generation(2, version=99)
        with self.assertRaisesRegex(RuntimeError, 'Unsupported'):
            resolve_checkpoint(self.logical)

    def test_staging_is_not_published(self):
        newest = self.generation(1)
        marker = next(self.root.glob('*.commit'))
        marker.rename(marker.with_suffix('.commit.tmp'))
        self.assertEqual(resolve_checkpoint(self.logical), self.logical)


if __name__ == '__main__':
    unittest.main()
