"""Check the completed UE prototype capture log, including sustained rest."""
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
text = (root / 'Saved/tree-felling-game.log').read_text(errors='replace')
# A later persistence probe reloads the bodies and produces a second set of
# rest logs. This validator covers the preceding felling capture only.
text = text.split('DetachedSave wrote', 1)[0]
assert 'TreeFelling TEST COMPLETE bodies=2' in text, 'Capture incomplete or wrong fragment count'
detach = text.index('TreeFelling DETACH')
prior_hits = text[:detach].count('TreeFelling AXE_STRIKE hit=1')
assert prior_hits >= 2, 'Expected successful partial cuts before release'
assert text.count('TreeFelling DETACH') == 1
assert text.count('TreeFelling BREAK') == 1
placement = re.search(r'EnvironmentLOD SPAWN temperate-oak position=\(([-\d.]+),([-\d.]+),([-\d.]+)\)', text)
assert placement, 'Missing oak placement'
tree_x, tree_y, tree_z = map(float, placement.groups())
assert 'initialLeanDeg=1.5 initialAngularSpeed=0' in text
assert 'TreeFelling HINGE_RELEASE groundContact=1' in text
fall = re.findall(r'TreeFelling MOTION VoxelFallingTimber_0 age=([\d.]+) speed=([\d.]+) angular=([\d.]+) position=V\(X=([\d.-]+), Y=([\d.-]+), Z=([\d.-]+)\)', text)
assert len(fall) >= 3, 'Insufficient observation of gravity-driven tipping'
early = [list(map(float, row)) for row in fall[:3]]
assert 0 < early[0][2] < early[1][2] < early[2][2], 'Fall must gather angular speed'
assert max(row[3] for row in early)-min(row[3] for row in early) < 1, 'Test heading drifted sideways'
impact_j = list(map(float, re.findall(r'TreeFelling IMPACT VoxelFallingTimber_0 energyJ=([\d.]+)', text)))
assert impact_j and max(impact_j) > 1500, 'Hard-contact fixture did not exceed its fracture threshold'
rests = re.findall(r'TreeFelling REST (\w+) age=([\d.]+) tilt=([\d.]+)', text)
assert len(rests) == 2, 'Both fragments must settle'
for name, _, tilt in rests:
    assert float(tilt) > 45, 'Resting fragment must have toppled'
    rows = re.findall(rf'TreeFelling MOTION {name} age=([\d.]+) speed=([\d.]+) angular=([\d.]+) position=V\(X=([\d.-]+), Y=([\d.-]+), Z=([\d.-]+)\)', text)
    assert len(rows) >= 10, 'Insufficient observation after impact'
    for row in rows[-5:]:
        age, speed, angular, x, y, z = map(float, row)
        assert speed < 1 and angular < 1, 'Fragment has not stayed at rest'
        assert abs(x - tree_x) < 1600 and abs(y - tree_y) < 1600
        assert tree_z - 1000 < z < tree_z + 1000, 'Fragment fell through the local terrain'
edit_ms = list(map(float, re.findall(r'incremental temperate-oak editMs=([\d.]+)', text)))
transfer_ms = float(re.search(r'TreeFelling DETACH .*transferMs=([\d.]+)', text)[1])
result = dict(partial_hits_before_release=prior_hits, severed_bodies=1,
              impact_fragments=2, sustained_rest=True, chop_ms=edit_ms,
              detach_cpu_ms=transfer_ms, early_angular_deg_s=[row[2] for row in early],
              peak_impact_j=max(impact_j))
(root / 'Saved/tree-felling-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
