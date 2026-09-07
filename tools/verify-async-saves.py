"""Check async completion/failure/exit behavior and the resulting save files."""
from pathlib import Path
from checkpoint_store import resolve_checkpoint
import hashlib
import json
import re
import struct
import zlib
import runpy
import argparse

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--normal-log', default='Saved/async-save-game.log')
parser.add_argument('--output', default='Saved/async-save-validation.json')
parser.add_argument('--expect-resource-expiry', action='store_true', help='Require the near-expired resource to become a persisted tombstone in the exit save')
args = parser.parse_args()
normal = (root / args.normal_log).read_text(errors='replace')
exiting = (root / 'Saved/async-save-exit-game.log').read_text(errors='replace')
baseline = (root / 'Saved/async-save-before-packing.log').read_text(errors='replace')
assert 'SaveAsync PROBE PASS exit=0 busyRejected=1' in normal
assert 'SaveAsync FAILURE_PROBE PASS' in normal
assert 'SaveAsync COMPLETE success=0' in normal
assert 'SaveAsync PROBE PASS exit=1 busyRejected=1' in exiting
assert 'SaveAsync DRAIN waiting for pending save' in exiting
assert ('DetachedSave restored actors=7 format=3' in exiting or
        'ObjectSave installed records=7; nearby residency staged' in exiting), 'Fresh snapshot was not installed'
assert ('DetachedSave teardown snapshot actors=7 failed=0' in exiting or
        'ObjectSave teardown snapshot records=7' in exiting), 'Shutdown lost registry records'
assert 'LogExit: Exiting.' in normal and 'LogExit: Exiting.' in exiting

def metrics(log):
    match = re.search(r'SaveAsync COMPLETE success=1 captureMs=([\d.]+) workerMs=([\d.]+) framesAdvanced=(\d+)', log)
    assert match
    return dict(capture_ms=float(match[1]), worker_ms=float(match[2]), frames_advanced=int(match[3]))

read_snapshot = runpy.run_path(str(root/'tools/verify-detached-persistence.py'))['snapshot']

source = read_snapshot(root/'ue-project/Saved/Tests/detached-roundtrip.vxlog')
results = {}
for slug in ('async_save_verification', 'async_exit_verification'):
    directory = root/'ue-project/Saved/SaveGames'/slug
    meta_path = resolve_checkpoint(directory/'world.vxlog').parent/'meta.json'
    if not meta_path.exists():  # Metadata moved out of the player's save list after verification.
        meta_path = root/'ue-project/Saved/Tests'/(slug+'.meta.json')
    metadata = json.loads(meta_path.read_text(encoding='utf-8-sig'))
    assert metadata['display_name'].lower().replace(' ', '_') == slug
    assert metadata['meta_version'] >= 1
    expect_expiry = args.expect_resource_expiry and slug == 'async_exit_verification'
    saved = read_snapshot(directory/'world.vxlog', (2,2,3,3,3,3) if expect_expiry else (1,2,2,3,3,3,3))
    assert saved['version'] == (4 if 'ObjectSave teardown snapshot records=' in normal else 3)
    assert saved['plants'] == source['plants']
    assert saved['plant_grids'] == source['plant_grids'], 'Authoritative grid changed'
    if expect_expiry:
        assert saved['resource_remaining_seconds'] is None and saved['tombstone_count'] == 1
    else:
        assert 0 < saved['resource_remaining_seconds'] < source['resource_remaining_seconds']
    results[slug] = saved

first, second = results['async_save_verification'], results['async_exit_verification']
if first['version'] == second['version'] == 4:
    first_ids = {o['id']: o for o in first['objects']}
    second_ids = {o['id']: o for o in second['objects']}
    assert first_ids.keys() == second_ids.keys(), 'Stable IDs changed across restart'
    for identity, old in first_ids.items():
        new = second_ids[identity]
        assert new['revision'] >= old['revision'], 'Object revision regressed'
        if args.expect_resource_expiry and old['kind'] == 1:
            assert 0 < old['remaining_seconds'] < 1, 'Expiry test requires a near-expired input resource'
            assert new['state'] == 4 and new['remaining_seconds'] == 0
            assert new['revision'] > old['revision']
            assert new['geometry_bytes'] == new['dynamic_bytes'] == 0
            assert new['geometry_revision'] == old['geometry_revision']
        else:
            assert new['state'] != 4, 'Unexpected object deletion'
            assert new['geometry_sha256'] == old['geometry_sha256'], 'Geometry changed across restart'
        assert new['kind'] == old['kind'] and new['owner'] == old['owner']
        assert new['retained'] == old['retained'] and new['lifetime_kind'] == old['lifetime_kind']
    if not args.expect_resource_expiry:
        assert 0 < second['resource_remaining_seconds'] < first['resource_remaining_seconds'], 'Remaining lifetime reset across restart'

before, after = metrics(baseline), metrics(normal)
assert after['capture_ms'] < before['capture_ms'] and after['frames_advanced'] > 2
result = dict(background_save_passed=True, busy_rejection_passed=True, io_failure_passed=True,
              shutdown_drain_passed=True, old_and_new_formats_loaded=True,
              expected_resource_expiry_verified=args.expect_resource_expiry,
              baseline=before, current=after, saves=results)
(root/args.output).write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
