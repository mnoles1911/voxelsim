"""karst_playability.py -- can a WALKING PLAYER actually explore this network?

THE QUESTION PHASE 0 STOPS BEING ABOUT GEOLOGY AND STARTS BEING ABOUT A GAME.

A cave that is geologically impeccable and unwalkable is a failed feature. So
before any of this is voxelised, the skeleton is measured against the engine's
OWN player, whose numbers are not guesses -- they are read out of
`ue-project/Source/VoxelEarth/VoxelMovementTuning.h`:

    BoxHalfExtentXY    30 UU  -> the player is a BOX 0.6 m wide, not a capsule
    StandHalfExtentZ   90 UU  -> 1.8 m tall standing
    CrouchHalfExtentZ  60 UU  -> 1.2 m crouched
    StepUpHeightUU     30 UU  -> 0.3 m absorbed silently while walking
    JumpSpeedUU       495 UU/s -> about 1.25 m of jump height

Worth noticing, because it decides which references transfer: **this is almost
exactly Minecraft's player** -- 0.6 x 1.8 blocks with a 0.5 m step. So
Minecraft's cave design rules apply here nearly unchanged, and the two numbers
that matter in both games are the same two: HEADROOM and FLOOR STEP.

WHAT THIS MEASURES, AND WHY AT THE SKELETON RATHER THAN IN VOXELS.

Voxelising a single 1 km system at 25 cm is a 4,000^3 grid; the question does not
need it. Walkability is a property of the SEGMENT -- its gradient and its radius
-- and connectivity is a property of the graph over walkable segments. So:

  * GRADIENT decides the class. A passage climbing at a gradient the player
    cannot walk up is not a passage, it is a wall with a view.
  * RADIUS decides headroom and width. A circular tube of radius R voxelised at
    10 cm gives a curved floor; the band where that floor rises less than the
    step height over the player's own width is about 0.9R wide, so R sets both
    how much floor there is and how much air is over it.
  * CONNECTIVITY over walkable segments only, from a surface entrance, is the
    number that says whether the network is EXPLORABLE rather than merely
    present. A beautiful system reachable only by falling down a 200 m shaft is
    content the player sees once.

THE THREE CLASSES, and they are the Minecraft vocabulary on purpose:

    WALK      gradient <= 0.5   -- 0.3 m rise per 0.6 m of travel, i.e. exactly
                                   the step-up. Walk up it without jumping.
    SCRAMBLE  gradient <= 2.0   -- needs jumps. Passable, costly, and the thing
                                   that makes a cave feel like a cave.
    SHAFT     gradient  > 2.0   -- effectively vertical. One-way down, and only
                                   survivable if what is at the bottom is water.

Usage:
    python tools/karst_playability.py <network.npz> <fields.npz> [--out DIR]
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib

import numpy as np
from scipy.spatial import cKDTree

GRID_M = 30.0

# --- the player, from VoxelMovementTuning.h. Restated WITH provenance, never
# --- re-derived: a duplicated constant that drifts is this repo's oldest bug.
PLAYER_WIDTH_M = 0.60
PLAYER_STAND_M = 1.80
PLAYER_CROUCH_M = 1.20
STEP_UP_M = 0.30
JUMP_M = 1.25

WALK_GRADIENT = STEP_UP_M / PLAYER_WIDTH_M      # 0.5

#: Radius range. THE FLOOR IS A PLAYABILITY LIMIT, NOT A TASTE ONE. A tube of
#: radius r has a walkable floor about 0.894r wide and 1.79r of headroom over
#: it, so the player's 0.6 m box and 1.2 m crouched height both bottom out at
#: r = 0.67 m. 0.8 m is that limit plus margin: a 1.6 m tube you crouch through.
#: The ceiling is the owner's 5x hall.
RADIUS_MIN_M = 0.8
RADIUS_MAX_M = 9.0
SCRAMBLE_GRADIENT = 2.0

#: The walkable band across a circular tube's floor. For floor z = -sqrt(R^2-x^2)
#: the gradient reaches WALK_GRADIENT at x = R*g/sqrt(1+g^2); doubling that and
#: dividing by R gives the fraction of the diameter that is walkable floor.
_g = WALK_GRADIENT
WALKABLE_FLOOR_FRACTION = 2.0 * _g / np.sqrt(1.0 + _g * _g) / 2.0   # ~0.447 of R


def assign_radii(seg: np.ndarray, trunk_order: np.ndarray) -> np.ndarray:
    """Conduit radius per segment, metres.

    Sized from the owner's directive -- passages 6-15 m wide, i.e. radius 3-7.5 m
    -- and varied by how trunk-like a segment is, because a cave whose passages
    are all one size is the artefact the previous system was rejected for
    ("one characteristic scale per feature class"). Discharge is the physical
    driver (r ~ Q^0.4); node degree stands in for it until the prototype carries
    Q, and that substitution is why this is a prototype.
    """
    lo, hi = RADIUS_MIN_M, RADIUS_MAX_M
    # ABSOLUTE mapping from junction order, NOT a per-tile min/max normalisation.
    # Normalising is unstable when degree is nearly constant: on one tile it
    # spread 0.8-9.0 m with a 1.79 m mean, and on another it collapsed to a 8.92 m
    # mean -- every passage a hall -- because dividing by a near-zero range
    # amplifies whatever variation is left. Degree 1 is a tip, 4+ is a trunk
    # confluence, and those mean the same thing on every tile.
    o = np.clip((trunk_order.astype(float) - 1.0) / 3.0, 0.0, 1.0)
    # Per-segment jitter, so passage size is not a pure function of topology.
    # Deterministic in the segment index: a prototype that reshuffles its own
    # radii between runs cannot be A/B'd against itself.
    rng = np.random.default_rng(20260819)
    o = np.clip(o + rng.normal(0.0, 0.18, size=o.shape), 0.0, 1.0)
    # LOG-UNIFORM, NOT LINEAR. Discharge across a karst network spans orders of
    # magnitude and r ~ Q^0.4, so a linear ramp between two radii is the wrong
    # shape: it produces a middle-heavy distribution where almost everything is
    # a hall. Interpolating in log space gives what real caves have and what
    # Minecraft has -- MOST passage small, a few large, which is the "wide not
    # shifted" distribution the owner chose over uniform 5x.
    return lo * (hi / lo) ** o


def classify(seg: np.ndarray):
    """Gradient and class per segment."""
    a, b = seg[:, 0, :], seg[:, 1, :]
    d = b - a
    horiz = np.hypot(d[:, 0], d[:, 1])
    rise = np.abs(d[:, 2])
    grad = np.where(horiz > 1e-6, rise / np.maximum(horiz, 1e-6), np.inf)
    length = np.linalg.norm(d, axis=1)
    cls = np.where(grad <= WALK_GRADIENT, 0,
                   np.where(grad <= SCRAMBLE_GRADIENT, 1, 2))
    return grad, cls, length


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("network", type=pathlib.Path)
    ap.add_argument("fields", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=None)
    args = ap.parse_args()
    out = args.out or args.network.parent
    stem = args.network.stem.replace("-network", "")

    net = dict(np.load(args.network).items())
    f = dict(np.load(args.fields).items())
    seg = net["segments"]
    elev = f["elev_m"]
    if len(seg) == 0:
        print("no segments"); return 2

    grad, cls, length = classify(seg)
    total_km = length.sum() / 1000.0

    # --- node graph over the segment endpoints, quantised so shared endpoints
    # --- actually share. Float endpoints that differ in the last bit would make
    # --- every segment its own component and the connectivity result a fiction.
    key = lambda p: (round(p[0], 2), round(p[1], 2), round(p[2], 2))
    ids, pts = {}, []
    def nid(p):
        k = key(p)
        if k not in ids:
            ids[k] = len(pts); pts.append(p)
        return ids[k]
    edges = [(nid(seg[i, 0]), nid(seg[i, 1]), i) for i in range(len(seg))]
    pts = np.asarray(pts)
    deg = np.zeros(len(pts), int)
    for u, v, _ in edges:
        deg[u] += 1; deg[v] += 1

    radius = assign_radii(seg, np.array([min(deg[u], deg[v]) for u, v, _ in edges]))
    walk_floor_w = 2.0 * radius * WALKABLE_FLOOR_FRACTION
    headroom = 2.0 * radius            # tube diameter at the axis
    # Computed here, next to `headroom`, because `stats` below reports them --
    # defining them in the verdict block put them after their own use.
    stand_frac = float((headroom >= PLAYER_STAND_M).mean())
    pass_frac = float((headroom >= PLAYER_CROUCH_M).mean())

    # --- depth below surface, per node, for the surface-entrance test
    mx = np.clip((pts[:, 0] / GRID_M).astype(int), 0, elev.shape[1] - 1)
    my = np.clip((pts[:, 1] / GRID_M).astype(int), 0, elev.shape[0] - 1)
    node_depth = elev[my, mx] - pts[:, 2]

    # An ENTRANCE is a node whose conduit roof reaches within one player height
    # of the surface: the conduit is r below the surface at its own axis, so the
    # roof is node_depth - r. This is the same rule the runtime carve will need,
    # stated once here.
    seg_r_at_node = np.zeros(len(pts))
    for (u, v, i) in edges:
        seg_r_at_node[u] = max(seg_r_at_node[u], radius[i])
        seg_r_at_node[v] = max(seg_r_at_node[v], radius[i])
    is_entrance = (node_depth - seg_r_at_node) <= PLAYER_STAND_M

    # --- connectivity over WALKABLE edges only, then over walk+scramble
    def components(max_cls):
        parent = list(range(len(pts)))
        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]; x = parent[x]
            return x
        for u, v, i in edges:
            if cls[i] <= max_cls:
                ru, rv = find(u), find(v)
                if ru != rv:
                    parent[ru] = rv
        return [find(i) for i in range(len(pts))]

    # WALK-IN ENTRANCES, kept separate from every other kind of breach.
    # A doline, a shaft and a swallet all reach daylight, and all three are a
    # fall. Only a hillside mouth is something a player WALKS IN through, so
    # "reachable from an entrance" and "reachable from a walk-in entrance" are
    # different numbers and the second is the one that describes the feature.
    mouth_nodes = net.get("mouth_nodes", np.empty((0, 3), np.float32))
    is_mouth = np.zeros(len(pts), bool)
    if len(mouth_nodes):
        mt = cKDTree(np.asarray(mouth_nodes, float))
        d_m, _ = mt.query(pts)
        is_mouth = d_m <= 1.0          # snapped to the same node

    res = {}
    for name, mc in (("walk", 0), ("walk_scramble", 1)):
        comp = components(mc)
        reachable_roots = {comp[i] for i in np.nonzero(is_entrance)[0]}
        km = 0.0
        for u, v, i in edges:
            if cls[i] <= mc and comp[u] in reachable_roots:
                km += length[i] / 1000.0
        mouth_roots = {comp[i] for i in np.nonzero(is_mouth)[0]}
        km_mouth = 0.0
        for u, v, i in edges:
            if cls[i] <= mc and comp[u] in mouth_roots:
                km_mouth += length[i] / 1000.0
        res[name] = {
            "reachable_km": round(float(km), 2),
            "reachable_pct": round(100.0 * float(km) / float(total_km), 1),
            "from_hillside_mouth_pct": round(100.0 * float(km_mouth) / float(total_km), 1),
            "components": len(set(comp)),
        }

    by_cls = collections.Counter()
    for i in range(len(seg)):
        by_cls[int(cls[i])] += length[i] / 1000.0

    stats = {
        "player": {"width_m": PLAYER_WIDTH_M, "stand_m": PLAYER_STAND_M,
                   "step_up_m": STEP_UP_M, "jump_m": JUMP_M,
                   "walk_gradient": round(WALK_GRADIENT, 3)},
        "passage_km_total": round(float(total_km), 2),
        "by_class_km": {"walk": round(float(by_cls[0]), 2),
                        "scramble": round(float(by_cls[1]), 2),
                        "shaft": round(float(by_cls[2]), 2)},
        "by_class_pct": {k: round(100.0 * float(v) / float(total_km), 1)
                         for k, v in (("walk", by_cls[0]), ("scramble", by_cls[1]),
                                      ("shaft", by_cls[2]))},
        "radius_m": {"min": round(float(radius.min()), 2),
                     "mean": round(float(radius.mean()), 2),
                     "max": round(float(radius.max()), 2)},
        "walkable_floor_width_m": {"min": round(float(walk_floor_w.min()), 2),
                                   "mean": round(float(walk_floor_w.mean()), 2)},
        "stand_upright_frac": round(stand_frac, 3),
        "passable_frac": round(pass_frac, 3),
        "headroom_m": {"min": round(float(headroom.min()), 2),
                       "mean": round(float(headroom.mean()), 2)},
        "entrances": int(is_entrance.sum()),
        "hillside_mouth_nodes": int(is_mouth.sum()),
        "reachable_on_foot": res,
    }
    print(json.dumps(stats, indent=2))

    # --- the verdict, against thresholds stated before the run ---------------
    print("\nPLAYABILITY")
    # STANDING vs PASSABLE, not one flat pass/fail. Once the radius distribution
    # went wide, crouch-only passages started to exist ON PURPOSE, and a single
    # "headroom >= 1.8 m everywhere" check reports that as a regression. The bar
    # that matters is PASSABLE; standing is a texture statistic, and the mix of
    # the two is a large part of what makes a cave read as a cave.
    ok_head = headroom.min() >= PLAYER_CROUCH_M
    ok_floor = walk_floor_w.min() >= PLAYER_WIDTH_M
    print(f"  passable (>= {PLAYER_CROUCH_M} m, crouched) everywhere: "
          f"{'YES' if ok_head else 'NO'} (min {headroom.min():.2f} m)")
    print(f"  stand upright in {100 * stand_frac:.0f}% of passage; crouch-only {100 * (pass_frac - stand_frac):.0f}%")
    print(f"  walkable floor >= {PLAYER_WIDTH_M} m everywhere: "
          f"{'YES' if ok_floor else 'NO'} (min {walk_floor_w.min():.2f} m)")
    r = res["walk_scramble"]["reachable_pct"]
    print(f"  reachable on foot from ANY entrance:      "
          f"{res['walk']['reachable_pct']}% walking, {r}% with jumps")
    print(f"  reachable from a WALK-IN hillside mouth:  "
          f"{res['walk']['from_hillside_mouth_pct']}% walking, "
          f"{res['walk_scramble']['from_hillside_mouth_pct']}% with jumps "
          f"({stats['hillside_mouth_nodes']} mouths on the network)")
    if stats["entrances"] == 0:
        print("  NO SURFACE ENTRANCE -- the whole network is unreachable on foot.")
    elif r < 50:
        print("  Under half the network is reachable on foot. The shafts are")
        print("  isolating it; that is a routing result, not a rendering one.")

    (out / f"{stem}-playability.json").write_text(json.dumps(stats, indent=2))
    print(f"\nwrote {out}/{stem}-playability.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
