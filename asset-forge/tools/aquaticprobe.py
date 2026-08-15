"""Prove that each mechanism the aquatic specs claim actually moves the asset.

Written because this project's signature failure is the silent no-op -- a
feature that runs, reports success and changes nothing -- and because the
aquatic pass leans on six rock mechanisms that were never asked to work at
these sizes. `brain-coral` already records one: its `rock.flutes` at 0.35
changed the voxel count by 1.4% and the silhouette by 5.8%, which on a 1.4 m
dome is decoration you would not find in a render, and it was raised to 0.70
only after being measured against itself turned off.

This is that test, generalised over the specs this pass authored. Same seed on
both sides, one field changed, and BOTH numbers reported:

  * VOXELS moved -- the mechanism is doing something to the mass.
  * SILHOUETTE moved -- the mechanism is doing something you can SEE from
    outside. A mechanism that moves voxels and not the outline is working
    internally, which for a subtractive surface feature means it is not
    working.

The silhouette is the union of the three axis projections, compared as a
symmetric difference over their union. It is the same measure `tools/rockmech.py`
uses and it is deliberately crude: it answers "would this look different",
which is the only question that matters for a knob a designer will turn.

    python tools/aquaticprobe.py             # every mechanism, 3 seeds averaged
    python tools/aquaticprobe.py --seeds 6   # more seeds, slower
    python tools/aquaticprobe.py --tuft      # the tuft-kind checks as well

WHAT COUNTS AS DEAD. Under 2% on BOTH numbers is printed as DEAD and is a
defect to fix or to write down in the spec's own notes. 2-6% on the silhouette
is printed as FAINT -- present in the voxels, probably invisible in a render --
which is exactly the band `brain-coral`'s first draft landed in.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


# (spec, field, off-value, what it is meant to produce)
CHECKS = [
    # --- the massive corals and the sponges: rock mechanisms at coral sizes ---
    ("organ-pipe-coral", "rock.columns", 0, "parallel tubes with flat tops"),
    ("bubble-coral", "rock.lumps", 3, "a dome made of fused grape vesicles"),
    ("lettuce-coral", "rock.bedding", 0.0, "stacked out-turned plates"),
    ("boulder-star-coral", "rock.lumps", 2, "low knobs over a hemisphere"),
    ("pillar-coral", "rock.flatten", 0.88, "blunt vertical fingers"),
    ("fire-coral", "rock.elongate", 1.0, "an upright standing blade"),
    ("barrel-sponge", "rock.pans", 0.0, "the central well"),
    ("vase-sponge", "rock.pans", 0.0, "the cup"),
    ("tube-sponge-cluster", "rock.columns", 0, "a bundle of separate tubes"),
    ("elephant-ear-sponge", "rock.elongate", 1.0, "a sheet standing on edge"),
    # --- reef structure and submarine stone ---
    ("reef-spur", "rock.elongate", 1.0, "a long seaward finger"),
    ("patch-reef-head", "rock.notch", 0.0, "the grazed undercut at the foot"),
    ("reef-flat-pavement", "rock.pans", 0.0, "pools left at low tide"),
    ("coral-rubble-bank", "rock.clasts", 0, "broken coral sticks in the heap"),
    ("maerl-bed", "rock.clasts", 0, "loose coralline knuckles"),
    ("oyster-reef-bank", "rock.clasts", 0, "whole overlapping shell valves"),
    ("honeycomb-worm-reef", "rock.cavernous", 0.0, "the pocked tube surface"),
    ("pillow-lava-mound", "rock.rind", 0.0, "the hard glassy chilled crust"),
    ("black-smoker-chimney", "rock.flutes", 0.0, "vertical fluid runnels"),
    ("hydrothermal-mound", "rock.rubble", 0.0, "collapsing sulphide"),
    ("submerged-limestone-pavement", "rock.block_relief_m", 0.0,
     "open water-widened grikes"),
    ("lava-tube-bench", "rock.notch", 0.0, "the collapsed tube lip"),
    ("storm-cast-boulder", "rock.bury", 0.30, "a block that looks dropped"),
    # --- the freshwater rocks ---
    ("plunge-pool-boulder", "rock.notch", 0.0, "the low-water attack band"),
    ("waterfall-lip-ledge", "rock.caprock", 0.0, "the surviving hard bed"),
    ("waterfall-lip-ledge", "rock.notch", 0.0, "the undercut under the lip"),
    ("bedrock-pothole", "rock.pans", 0.0, "the ground-out basin"),
    ("riffle-slab", "rock.aspect", 0.0, "one water-polished face"),
    ("step-pool-boulder", "rock.joint_sets", 0, "one shared fracture frame"),
    ("lake-bed-slab", "rock.bury", 0.22, "a slab mostly under silt"),
    ("undercut-bank-block", "rock.notch", 0.0, "the fish-holding slot"),
    ("travertine-rimstone-dam", "rock.bedding", 0.0, "deposition banding"),
    ("tufa-spring-mound", "rock.cavernous", 0.0, "the porous holey surface"),
    ("marl-bench", "rock.erode", 0.20, "a crumbling soft edge"),
]

# The tuft kinds. These are cheaper and less likely to be dead, but three
# claims in the plant files are load-bearing enough to check: that a head is
# actually drawn, that arc separates a submerged plant from an emergent one,
# and that splay is what makes a carpet anemone wide.
TUFT_CHECKS = [
    ("broadleaf-cattail", "tuft.head_share", 0.0, "the brown cigar"),
    ("purple-loosestrife", "tuft.head_share", 0.0, "the magenta spire"),
    ("white-water-lily", "tuft.head_share", 0.0, "the floating pads"),
    ("giant-water-lily", "tuft.head_m", 0.12, "a two-metre pad"),
    ("yellow-flag-iris", "tuft.head_share", 0.0, "the yellow flag"),
    ("pickerelweed", "tuft.head_share", 0.0, "the blue spike"),
    ("curled-pondweed", "tuft.arc", 0.10, "limp, because water holds it up"),
    ("river-water-crowfoot", "tuft.arc", 0.10, "streamers combed downstream"),
    ("magnificent-sea-anemone", "tuft.splay_deg", 12.0, "a carpet, not a bush"),
    ("water-chestnut", "tuft.splay_deg", 12.0, "a flat floating rosette"),
    ("sea-palm", "tuft.arc", 0.10, "the drooping palm mop"),
    ("water-horsetail", "tuft.taper", 0.35, "a tube, not a blade"),
]


def silhouette(grid) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    solid = np.asarray(grid.data) != 0
    return solid.any(axis=0), solid.any(axis=1), solid.any(axis=2)


def _pad_to(a: np.ndarray, shape) -> np.ndarray:
    out = np.zeros(shape, dtype=bool)
    out[:a.shape[0], :a.shape[1]] = a
    return out


def sil_delta(a, b) -> float:
    """Symmetric difference over union, across the three projections."""
    diff = union = 0
    for pa, pb in zip(silhouette(a), silhouette(b)):
        shape = (max(pa.shape[0], pb.shape[0]), max(pa.shape[1], pb.shape[1]))
        pa, pb = _pad_to(pa, shape), _pad_to(pb, shape)
        diff += int(np.logical_xor(pa, pb).sum())
        union += int(np.logical_or(pa, pb).sum())
    return diff / union if union else 0.0


def run(checks, seeds: int) -> int:
    bad = 0
    print(f"{'spec':<30} {'mechanism':<24} {'voxels':>9} {'silhouette':>11}   verdict")
    print("-" * 92)
    for name, field, off, what in checks:
        path = SPECS / f"{name}.json"
        if not path.exists():
            print(f"{name:<30} {'':<24} {'':>9} {'':>11}   MISSING SPEC")
            bad += 1
            continue
        on_spec, _ = sm.load(path)
        off_spec, _ = sm.patch(on_spec, {field: off})
        dv = ds = 0.0
        for seed in range(1, seeds + 1):
            a = pipeline.build(on_spec, seed)
            b = pipeline.build(off_spec, seed)
            va, vb = a.stats["voxels"], b.stats["voxels"]
            dv += abs(va - vb) / max(va, vb, 1)
            ds += sil_delta(a.grid, b.grid)
        dv, ds = dv / seeds * 100, ds / seeds * 100
        if dv < 2.0 and ds < 2.0:
            verdict, bad = "DEAD", bad + 1
        elif ds < 6.0:
            verdict = "FAINT"
        else:
            verdict = "ok"
        print(f"{name:<30} {field.split('.')[-1]:<24} {dv:>8.1f}% {ds:>10.1f}%   "
              f"{verdict}   ({what})")
    return bad


def check_hosts() -> int:
    """Every spec's biome weights, against that biome's `hosts` tuple.

    `spec.validate` does NOT enforce this. A `flower` spec accepts
    `biomes.ocean` 0.8 with no warning, and a `grass` spec accepts
    `biomes.bare_rock` -- because `spec.py:488` passes `kinds=b.hosts` to the
    parameter, and `kinds` gates which sliders the APP shows rather than what
    validation allows. So a species can be authored into a biome that will
    never place it, and nothing says so.

    It caught two in this pass on its first run -- `rock-samphire` and
    `sea-campion`, both weighted to bare rock, which hosts no plant kind at all
    because `plantable` is False.
    """
    import json
    from forge import biomes as bio

    bad = []
    for path in sorted(SPECS.glob("*.json")):
        s = json.loads(path.read_text(encoding="utf-8"))
        kind = s.get("kind")
        for b in bio.BIOMES:
            w = float(s.get("biomes", {}).get(b.key, 0.0) or 0.0)
            if w > 0 and kind not in b.hosts:
                bad.append((path.stem, kind, b.key, w))
    print("BIOME WEIGHTS AGAINST `hosts`")
    print()
    for name, kind, key, w in bad:
        print(f"  {name:<28} {kind:<10} weighted {w:.2f} to {key}, "
              f"which hosts {bio.BY_KEY[key].hosts}")
    print()
    if bad:
        print(f"aquaticprobe --hosts: {len(bad)} spec"
              f"{'s' if len(bad) != 1 else ''} weighted into a biome that will "
              f"never place them")
        return 1
    print(f"aquaticprobe --hosts: all {len(list(SPECS.glob('*.json')))} specs "
          f"are weighted only to biomes that host their kind")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", type=int, default=3,
                    help="how many individuals to average over (default 3)")
    ap.add_argument("--tuft", action="store_true",
                    help="also check the tuft-kind claims")
    ap.add_argument("--hosts", action="store_true",
                    help="only check every spec's biome weights against hosts")
    args = ap.parse_args()

    if args.hosts:
        return check_hosts()

    print("ROCK MECHANISMS AT AQUATIC SIZES")
    print()
    bad = run(CHECKS, args.seeds)
    if args.tuft:
        print()
        print("TUFT CLAIMS")
        print()
        bad += run(TUFT_CHECKS, args.seeds)
    print()
    if bad:
        print(f"aquaticprobe: {bad} DEAD or missing -- each one is a knob that "
              f"reads as tuning and does nothing")
        return 1
    print("aquaticprobe: every mechanism moves the asset")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
