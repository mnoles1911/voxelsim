"""Isolated publish gating, read-only checks, stale-pruning and failure tests."""
import importlib.util
import json
from pathlib import Path
import tempfile
import _path  # noqa: F401
from forge import publishing, vxa
from forge.grid import VoxelGrid

ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT/'out'/'publish-probe'
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        root = Path(tmp)
        specs, library, dest = root/'specs', root/'library', root/'engine'/'craft'
        specs.mkdir(); library.mkdir()
        raw = dict(name='fixture-craft', kind='artifact', resolution_cm='1.25',
                   curation=dict(status='approved', seeds=[1], notes='fixture'))
        path = specs/'fixture-craft.json'
        def verdict(status):
            raw['curation']['status'] = status
            path.write_text(json.dumps(raw))
        verdict('approved')
        entry = library/'fixture-craft'/'fixture-craft-0001'
        entry.mkdir(parents=True)
        grid = VoxelGrid((3, 3, 3), voxel_m=.0125)
        grid.data[:] = 17
        vxa.write(grid, entry/'tree.vxa')
        # Validates same evidence that manifest.kept_seeds consumes.
        (entry/'meta.json').write_text(json.dumps(dict(seed=1)))
        count = publishing.publish_craft(specs, library, dest)
        assert count == 1
        before = {p.relative_to(dest): p.read_bytes() for p in dest.rglob('*') if p.is_file()}
        assert publishing.publish_craft(specs, library, dest, check_only=True) == 1
        publishing.publish_craft(specs, library, dest)
        assert before == {p.relative_to(dest):p.read_bytes() for p in dest.rglob('*') if p.is_file()}
        decoded = vxa.read(dest/'fixture-craft'/'fixture-craft-0001'/'tree.vxa')
        assert decoded.voxel_m == .0125
        for status in ('draft', 'rejected'):
            verdict(status)
            try:
                publishing.publish_craft(specs, library, dest, check_only=True)
                raise AssertionError('held-back state did not invalidate check')
            except ValueError:
                pass
            assert publishing.publish_craft(specs, library, dest) == 0
            assert not list(dest.rglob('*.vxa'))
            verdict('approved')
            assert publishing.publish_craft(specs, library, dest) == 1
        # Extra bank files and edited indexes cannot pass the checker.
        extra = dest/'stale.vxa';extra.write_bytes((entry/'tree.vxa').read_bytes())
        try:
            publishing.publish_craft(specs, library, dest, check_only=True)
            raise AssertionError('stale file passed')
        except ValueError:
            pass
        publishing.publish_craft(specs, library, dest)
        (dest/'categories.json').write_text('{}')
        try:
            publishing.publish_craft(specs, library, dest, check_only=True)
            raise AssertionError('edited index passed')
        except ValueError:
            pass
        publishing.publish_craft(specs, library, dest)
        good = (dest/'categories.json').read_bytes()
        (entry/'tree.vxa').write_bytes(b'corrupt')
        try:
            publishing.publish_craft(specs, library, dest)
            raise AssertionError('corrupt model published')
        except ValueError:
            pass
        assert (dest/'categories.json').read_bytes() == good
        assert vxa.read(dest/'fixture-craft'/'fixture-craft-0001'/'tree.vxa').count() == 27
        # The CLI's resync report used to mutate curation even in --check-only.
        modspec = importlib.util.spec_from_file_location('publish_under_test', ROOT/'tools'/'publish.py')
        module = importlib.util.module_from_spec(modspec);modspec.loader.exec_module(module)
        module.SPECS, module.LIBRARY = specs, library
        verdict('draft')
        old = path.read_bytes()
        lines, conflicts = module.resync_from_library(dry_run=True)
        assert lines and not conflicts and path.read_bytes() == old
    print('publishprobe: PASS (approval transitions, omission, exact pitch, determinism, stale/corrupt red arms, read-only checks)')


if __name__ == '__main__':
    main()
