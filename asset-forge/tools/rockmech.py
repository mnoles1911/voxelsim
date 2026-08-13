"""Prove each rock mechanism does something, and say what it costs.

Every slider here is meant to change the stone. A slider that changes nothing
is worse than a missing feature, because it looks like a knob and reads as
tuning -- the angularity slider silently did nothing for a long stretch of this
project because its cut planes were landing outside the rock, and the only
reason that was ever caught was a measurement like this one.

So: build a baseline, build again with one mechanism turned on, and report how
much the voxels and the silhouette moved. Both matter. A mechanism that changes
the voxel count but not the outline is doing something internal; one that moves
neither is dead.

Same seed on both sides. `build` draws its seed from the generator it is handed,
so handing each build a fresh generator with the same seed means the two stones
differ by the mechanism and by nothing else.
"""
from __future__ import annotations

import sys
import time

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import rock as rocklib
from forge.spec import default_spec, patch

VOXEL_M = 0.05
SEED = 7

# How big the test stone is. This used to be pinned at 3 m, and pinning it is
# how the weathering pass got away with a retreat measured in absolute voxels
# for as long as it did: at 3 m the absolute number happens to be about a
# seventh of the radius, which is what it should be, so every mechanism here
# passed while the same settings did nothing at all on a 13 m hero. Run it at
# two sizes.
#
#     python tools/rockmech.py            # 3 m, the old behaviour
#     python tools/rockmech.py 12         # a hero-sized stone
#
# Everything authored in METRES scales with the stone, so the mechanism being
# tested stays the same shape relative to the rock. Leaving a 1 m block size on
# a 12 m stone would test a different thing, not the same thing bigger.
SIZE_M = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
K = SIZE_M / 3.0

# Each entry: label, {param: value}. The baseline is a plain 3 m boulder with
# joints available, since several mechanisms need a joint frame to hang off.
BASE = {
    "kind": "rock", "resolution_cm": "5",
    "rock.size_m": SIZE_M, "rock.lumps": 4, "rock.angular": 0.5,
    # Weathering runs high deliberately. Half the mechanisms here work by making
    # some of the stone harder than the rest, and a hardness contrast can only
    # show up in proportion to how much the SOFT rock actually loses -- a vein
    # three times tougher than its surroundings stands proud by nothing at all
    # if the surroundings only retreat by half a voxel. Testing differential
    # erosion at a low erosion setting measures the setting, not the mechanism.
    "rock.facets": 5, "rock.rough": 0.35, "rock.erode": 0.5,
    "rock.joint_sets": 3, "rock.block_size_m": 1.0 * K, "rock.bury": 0.2,
    "rock.rubble": 0.0,
}

# name, what the mechanism needs switched on around it, the mechanism itself.
#
# Each case gets its OWN reference -- the setup with the mechanism at zero --
# rather than being compared against one global baseline. Two reasons, both
# learned the hard way here. Several mechanisms need scaffolding (a joint
# opening, a bigger stone) that changes the rock by itself, so a shared
# baseline measures the scaffolding rather than the mechanism. And `rock.erode`
# must be identical on both sides: `build` measures the finished stone and
# rescales it to hit the authored size, so any change in how much weathering
# takes away comes back as a change in scale, and the voxel counts stop being
# comparable at all. That confound made four mechanisms report near-identical
# numbers on the first run and hid whether any of them worked.
CASES = [
    ("caprock",     {}, {"rock.caprock": 0.9}),
    ("notch",       {}, {"rock.notch": 3.0, "rock.notch_z_m": 0.6 * K,
                         "rock.notch_spread_m": 0.2 * K}),
    ("aspect",      {}, {"rock.aspect": 0.9}),
    ("cross_beds",  {"rock.bed_thickness_m": 0.25 * K},
                    {"rock.cross_beds": 3}),
    ("veins",       {}, {"rock.veins": 3, "rock.vein_width_m": 0.15 * K}),
    ("rind",        {"rock.cavernous": 0.8},
                    {"rock.rind": 0.8, "rock.rind_m": 0.1 * K}),
    ("clasts",      {}, {"rock.clasts": 220, "rock.clast_size_m": 0.3 * K}),
    ("corestone",   {}, {"rock.corestone": 0.5}),
    ("joint_taper", {"rock.block_relief_m": 0.25 * K}, {"rock.joint_taper": 0.9}),
    ("settle",      {"rock.block_relief_m": 0.25 * K}, {"rock.settle_m": 0.2 * K}),
    ("flutes",      {}, {"rock.flutes": 0.8, "rock.flute_width_m": 0.3 * K}),
    ("pans",        {}, {"rock.pans": 0.8, "rock.pan_depth_m": 0.25 * K}),
    ("arch",        {"rock.size_m": 6.0 * K, "rock.elongate": 2.4,
                     "rock.flatten": 1.5}, {"rock.arch": 0.8}),
]


def build(changes):
    spec, rep = patch(default_spec(), dict(BASE, **changes))
    if rep.warnings:
        print("   ! " + "; ".join(rep.warnings))
    t0 = time.perf_counter()
    grid = rocklib.build(spec, np.random.default_rng(SEED), VOXEL_M)
    ms = (time.perf_counter() - t0) * 1e3
    occ = grid.data != 0
    box = rocklib._occupied_box(occ)
    return occ[box], ms


def divergence(a: np.ndarray, b: np.ndarray) -> float:
    """How much of the stone actually moved, as a fraction of the two together.

    Voxel COUNT cannot answer this, and finding that out cost a full round of
    wrong verdicts. `build` measures the finished stone and rescales it until it
    hits the authored size, so a mechanism that makes the rock harder in places
    erodes less, comes out larger, gets scaled back down, and lands at almost
    exactly the voxel count it started with. Five mechanisms reported as doing
    nothing on that basis while they were plainly redistributing where the
    stone was.

    Overlaying the two and counting where they disagree asks the question that
    was meant all along: is this a different rock?
    """
    shape = tuple(max(a.shape[i], b.shape[i]) for i in range(3))
    pa = np.zeros(shape, bool); pa[:a.shape[0], :a.shape[1], :a.shape[2]] = a
    pb = np.zeros(shape, bool); pb[:b.shape[0], :b.shape[1], :b.shape[2]] = b
    union = int((pa | pb).sum())
    return int((pa ^ pb).sum()) / max(union, 1) * 100.0


def main():
    print(f"baseline: {SIZE_M:g} m boulder ({SIZE_M / VOXEL_M:.0f} voxels "
          f"across), {VOXEL_M * 100:g} cm, seed {SEED}")
    print("each mechanism against its own reference, weathering held equal\n")
    print(f"  {'mechanism':<12} {'voxels':>9} {'d vox':>8}  {'changed':>8}  "
          f"{'ms':>6}")

    dead = []
    for name, setup, mech in CASES:
        ref, _ = build(setup)
        got, ms = build(dict(setup, **mech))
        dv = (int(got.sum()) - int(ref.sum())) / max(int(ref.sum()), 1) * 100.0
        moved = divergence(ref, got)
        flag = ""
        if moved < 1.0:
            flag = "   <-- DEAD: changes nothing"
            dead.append(name)
        print(f"  {name:<12} {int(got.sum()):>9,} {dv:>+7.1f}%  "
              f"{moved:>7.1f}%  {ms:>5.0f}{flag}")

    print()
    if dead:
        print("DEAD SLIDERS: " + ", ".join(dead))
    else:
        print("every mechanism moves the stone.")


if __name__ == "__main__":
    main()
