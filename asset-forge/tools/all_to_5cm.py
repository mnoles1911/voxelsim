"""Put every species on the 5 cm asset lattice.

One size for every asset, so nothing downstream ever has to ask which lattice a
given object lives in. The terrain stays at 10 cm and assets sit on a 5 cm
lattice nested 2:1 inside it.

The cost is 8x the voxels on everything that was at 10 cm -- 866k to 6.7M across
the sixteen large species -- and it is worth stating what that does and does not
break:

- **Build time is fine.** The worst is the 30 m jungle emergent at about five
  seconds; everything else is under three.
- **Previews are unaffected.** Gallery tiles and the 3D viewer already choose
  their own resolution against a budget, so a big tree still previews at 10 cm.
- **.vox exports split more.** The format caps a model at 256 voxels per axis
  and several trees now exceed that on their long axis, the emergent at 572.
  The writer already splits and the selftest checks the round trip, so this is
  more models per file rather than a failure.
"""
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def main():
    moved = kept = 0
    for fp in sorted(SPECS.glob("*.json")):
        s, _ = sm.load(fp)
        if sm.get(s, "resolution_cm") == "5":
            kept += 1
            continue
        s2, rep = sm.patch(s, {"resolution_cm": "5"})
        sm.save(s2, fp)
        moved += 1
        note = ("! " + rep.warnings[0]) if rep.warnings else "ok"
        print(f"  {fp.stem:<24} -> 5 cm  {note}")
    print(f"{moved} moved, {kept} already at 5 cm")


if __name__ == "__main__":
    main()
