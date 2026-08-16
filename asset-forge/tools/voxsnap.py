"""Voxel counts for every land animal at three seeds, variation pinned OFF.

A CHANGE THAT CLAIMS TO CHANGE NO GEOMETRY HAS TO PROVE IT. `reffit dead --write`
rewrites 68 specs to the ratio the one-voxel radius floor is already drawing, and
the whole point of that repair is that the drawn animal is identical. In a
project whose signature failure is the silent no-op, "it should be a no-op" is
not a claim anybody should accept on reasoning.

    python tools/voxsnap.py > before.json
    ...make the change...
    python tools/voxsnap.py > after.json      # diff must be empty

VARIATION PINNED OFF IS NOT OPTIONAL AND IS NOT THE WHOLE STORY. Any edit to any
spec changes that spec's seed salt, so with variation ON every individual of that
species differs afterwards -- 204 of 393 builds moved on a change that moved 0 of
393 pinned ones. That is normal for a spec edit here and it is why this snapshot
pins. It also means this file cannot check the VARIED draw; `reffit._settle_dead`
and `tools/quadprobe.py --stance --parts --caps` are what do that.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _path  # noqa: F401,E402
from forge import pipeline, spec as sm  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
out = {}
for p in sorted((ROOT / "specs").glob("*.json")):
    if json.loads(p.read_text(encoding="utf-8")).get("kind") != "quadruped":
        continue
    spec, _ = sm.load(p)
    flat, _ = sm.patch(spec, {"variation.amount": 0.0})
    for seed in (1, 2, 3):
        out[f"{p.stem}|{seed}"] = pipeline.build(flat, seed).stats["voxels"]
print(json.dumps(out, indent=1, sort_keys=True))
