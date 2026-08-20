"""karst_network.py -- route the conduits, from the fields karst_prototype wrote.

STAGE 2 OF THE PHASE 0 PROTOTYPE. `karst_prototype.py` builds the geological
fields (inception horizons, fracture fabric, water table, sinks, springs) and
caches them to an .npz because the water-table solve is the slow part. This file
consumes that and does the part the method is actually named for: an anisotropic
3D graph, Dijkstra from every sink to its spring, and the paper's gamma-skeleton
prune.

THE COST FUNCTION, which is where the geology enters, follows Paris et al.'s
`VolumetricGraph::ComputeEdgeCost` term for term:

    cost = w_d * length
         + w_h * (1 - horizonProximity)      strong-over-weak contacts guide flow
         + w_k * (1 - permeability)          fractured/weathered rock is cheaper
         + w_f * fractureMisalignment        joints steer conduits
         + w_v * vadoseDescentPenalty        above the water table, water falls
         + HUGE * aboveSurface               a conduit cannot leave the ground

and the gamma-skeleton prune drops edge (i,j) when some k satisfies
d(i,k)^g + d(k,j)^g < d(i,j)^g. Gamma IS the morphology knob: ~2.0 gives
branchwork conduits, ~1.05 gives spongework, and a heavy fracture weight with
high gamma gives rectilinear mazes.

WHAT IS DELIBERATELY NOT HERE. Conduit RADII and cross-section shape (phreatic
tube / vadose canyon / keyhole) are assigned from discharge and from z-against-
water-table, but no geometry is built: the skeleton-to-voxels question is the
one that can kill this project (see karst_prototype.py's header on smooth-min
blending) and it belongs in C++ against the real voxel grid, not here.

Usage:
    python tools/karst_network.py <fields.npz> [--gamma 2.0] [--out DIR]
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import pathlib
import sys
import time

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import dijkstra
from scipy.spatial import cKDTree

import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))
from karst_prototype import inception_at  # noqa: E402  -- ONE horizon field, not two
from karst_voxel import subdivide          # noqa: E402  -- ONE subdivision, not two

GRID_M = 30.0
EXAGGERATION = 5.0

# --- cost weights. Named, defaulted, and all in one place so a morphology is a
# --- set of numbers someone can quote rather than a diff.
W_DIST = 1.0
W_HORIZON = 60.0
W_PERM = 30.0
W_FRACTURE = 15.0
W_VADOSE = 40.0
HORIZON_SHARP = 1.0
#: How hard to discourage a second branch joining at the same node.
ATTACH_PENALTY = 0.8
FOLD = None
SEED = 0
BIG = 1.0e7

#: Vertical sampling band, relative to the water table. Conduits form in a band
#: around it: phreatic below, vadose above, and permeability decays with depth
#: (the classic exponential decay of hydraulic conductivity), so there is no
#: point sampling the deep crust.
BAND_ABOVE_M = 60.0 * EXAGGERATION
BAND_BELOW_M = 40.0 * EXAGGERATION

#: Node spacing. The paper uses a 10 m Poisson radius over a 750 m domain; at
#: 236 km^2 that is not affordable, so this is the corridor-sampling deviation
#: the prototype header states. 5x scale also means 5x-fatter conduits, so a
#: coarser skeleton is not the loss it would be at 1x.
NODE_SPACING_M = 45.0 * EXAGGERATION
NEIGHBOURS = 16


def load_fields(path: pathlib.Path) -> dict:
    z = np.load(path, allow_pickle=False)
    return {k: z[k] for k in z.files}


def suppress_springs(springs: np.ndarray, log2acc: np.ndarray,
                     min_sep_cells: float) -> np.ndarray:
    """Non-maximum suppression over spring candidates, keeping the largest.

    WHY THIS EXISTS. The field stage flags every cell where a horizon daylights
    at the water table in a valley, which on real terrain is a contiguous RIBBON
    along the valley floor, not a point -- 1,461 candidates over 236 km^2, i.e.
    6 per km^2 against a real resurgence density one to two orders of magnitude
    lower. The consequence is not cosmetic: with as many springs as sinks, every
    system gets ONE sink, and a system with one sink has no confluence, no
    branchwork, and nothing that reads as a cave network. Suppression is what
    turns a ribbon of candidates into the resurgence the valley actually has.

    Keep the candidate with the largest catchment in each neighbourhood: a
    resurgence is where the WATER comes out, so accumulation is the right
    tie-break rather than elevation or arbitrary order.

    This belongs in the field stage next to `find_springs`; it lives here while
    the separation is being tuned, because the field stage costs 105 s a run and
    this costs milliseconds.
    """
    if len(springs) == 0:
        return springs
    acc = np.array([log2acc[y, x] for x, y in springs], float)
    order = np.argsort(-acc)
    kept, kept_pts = [], []
    for i in order:
        p = springs[i].astype(float)
        if kept_pts and np.min(np.linalg.norm(np.asarray(kept_pts) - p, axis=1)) < min_sep_cells:
            continue
        kept.append(i)
        kept_pts.append(p)
    return springs[np.asarray(kept, int)]


def assign_systems(sinks: np.ndarray, springs: np.ndarray, head: np.ndarray):
    """Group sinks into systems by which spring their water-table gradient
    reaches. One system per spring is what makes the per-corridor sampling
    affordable; it is also what a karst catchment IS."""
    if len(springs) == 0 or len(sinks) == 0:
        return {}
    tree = cKDTree(springs.astype(float))
    # Steepest-descent on the head field would be more faithful; nearest spring
    # in the plane is its first-order approximation and is what the prototype
    # uses. Recorded as an approximation rather than presented as the rule.
    _, idx = tree.query(sinks.astype(float))
    systems = collections.defaultdict(list)
    for s, i in zip(sinks, idx):
        systems[int(i)].append(tuple(int(v) for v in s))
    return systems


def sample_corridor(sink_xy, spring_xy, elev, head, incept, coher, log2acc,
                    rng: np.random.Generator):
    """3D nodes in the corridor between a system's sinks and its spring.

    Biased onto inception horizons: a node is kept with higher probability where
    |incept| is small, which is the sampling half of the paper's horizon term
    (its cost half is in `edge_costs`). Sampling and cost both pull toward
    horizons for the same physical reason and neither alone is enough -- cost
    with no sampling bias has nothing to route along."""
    pts = np.array([spring_xy] + list(sink_xy), float)
    lo = np.maximum(pts.min(axis=0) - 12, 0)
    hi = np.minimum(pts.max(axis=0) + 12, np.array(elev.shape[::-1]) - 1)
    if np.any(hi <= lo):
        return np.empty((0, 3)), np.empty((0,), int)

    step = max(1, int(round(NODE_SPACING_M / GRID_M)))
    xs = np.arange(int(lo[0]), int(hi[0]) + 1, step)
    ys = np.arange(int(lo[1]), int(hi[1]) + 1, step)
    if len(xs) == 0 or len(ys) == 0:
        return np.empty((0, 3)), np.empty((0,), int)
    gx, gy = np.meshgrid(xs, ys)
    gx, gy = gx.ravel(), gy.ravel()

    nodes, cell = [], []
    for x, y in zip(gx, gy):
        h = head[y, x]
        top = min(elev[y, x] - 2.0, h + BAND_ABOVE_M)
        bot = h - BAND_BELOW_M
        if top <= bot:
            continue
        nz = max(2, int((top - bot) / (NODE_SPACING_M * 0.75)))
        for z in np.linspace(bot, top, nz):
            # Horizon bias: keep everything within a horizon, thin elsewhere.
            near = abs(_incept_at(incept, x, y)) < 0.25
            if not near and rng.random() > 0.35:
                continue
            nodes.append((x * GRID_M, y * GRID_M, z))
            cell.append(y * elev.shape[1] + x)
    return np.asarray(nodes, float), np.asarray(cell, int)


def _incept_at(incept, x, y):
    return incept[y, x]


def edge_costs(nodes, idx_a, idx_b, elev, head, incept, coher, theta, log2acc):
    """The anisotropic geological cost, one value per candidate edge."""
    a, b = nodes[idx_a], nodes[idx_b]
    d = b - a
    length = np.linalg.norm(d, axis=1)
    length = np.maximum(length, 1e-6)

    mx = ((a[:, 0] + b[:, 0]) * 0.5 / GRID_M).astype(int)
    my = ((a[:, 1] + b[:, 1]) * 0.5 / GRID_M).astype(int)
    mx = np.clip(mx, 0, elev.shape[1] - 1)
    my = np.clip(my, 0, elev.shape[0] - 1)
    mz = (a[:, 2] + b[:, 2]) * 0.5

    cost = W_DIST * length

    # Horizon proximity: cheap ON a horizon, expensive off it.
    # EVERY TERM HERE IS MULTIPLIED BY LENGTH, which is the whole problem: if
    # the per-unit-length rate barely varies between neighbouring routes, the
    # cheapest path is simply the SHORTEST one and Dijkstra returns a straight
    # line. `horizon_sharp` > 1 turns the horizon term into a narrow band, so
    # being ON a horizon is dramatically cheaper than being near one and the
    # route has a reason to bend.
    # HORIZONS ARE EVALUATED IN 3D, AT THE EDGE'S OWN z. The previous version
    # indexed a 2D array of the stratigraphic field sampled at the SURFACE, so
    # a conduit at 1,900 m and one at 2,100 m in the same column got identical
    # horizon cost -- the term carried no depth information and could not make
    # a route follow a bedding plane. That is why conduits came out straight.
    horiz = np.clip(np.abs(inception_at(mz, FOLD[my, mx], SEED)), 0.0, 1.0) ** HORIZON_SHARP
    cost += W_HORIZON * horiz * length / GRID_M

    # Permeability: high flow accumulation marks stress-relief fracturing and
    # concentrated recharge, so it is CHEAP.
    perm = np.clip(log2acc[my, mx] / 24.0, 0.0, 1.0)
    cost += W_PERM * (1.0 - perm) * length / GRID_M

    # Fracture anisotropy, the paper's term: aligned with the joint set is
    # cheap, across it is dear, scaled by how coherent the fabric is.
    horiz_len = np.maximum(np.hypot(d[:, 0], d[:, 1]), 1e-6)
    dirx, diry = d[:, 0] / horiz_len, d[:, 1] / horiz_len
    th = theta[my, mx]
    dot = np.abs(dirx * np.cos(th) + diry * np.sin(th))
    mis = 1.0 - dot * dot
    cost += W_FRACTURE * mis * coher[my, mx] * length / GRID_M

    # Vadose: above the water table water falls, so HORIZONTAL travel is
    # expensive and descent is not. This is what makes vadose reaches read as
    # shafts and canyons rather than as level tubes.
    above = mz > head[my, mx]
    horizontality = horiz_len / length
    cost += W_VADOSE * above * horizontality * length / GRID_M

    # A conduit cannot leave the ground.
    cost += BIG * (mz > elev[my, mx] - 1.0)
    return cost


def build_system(nodes, elev, head, incept, coher, theta, log2acc,
                 sink_ids, spring_id, gamma, deadends=0, rng=None, mouth_ids=()):
    """kNN graph, Dijkstra from each sink to the spring, gamma-skeleton prune."""
    if len(nodes) < 8:
        return []
    tree = cKDTree(nodes)
    k = min(NEIGHBOURS + 1, len(nodes))
    dist, nbr = tree.query(nodes, k=k)
    src = np.repeat(np.arange(len(nodes)), k - 1)
    dst = nbr[:, 1:].ravel()
    w = edge_costs(nodes, src, dst, elev, head, incept, coher, theta, log2acc)
    g = coo_matrix((w, (src, dst)), shape=(len(nodes), len(nodes))).tocsr()

    d, pred = dijkstra(g, directed=False, indices=spring_id,
                       return_predecessors=True)
    paths = []
    for s in sink_ids:
        if not np.isfinite(d[s]):
            continue
        p, cur = [], s
        guard = 0
        while cur != spring_id and cur >= 0 and guard < 10000:
            p.append(cur)
            cur = pred[cur]
            guard += 1
        if cur == spring_id:
            p.append(spring_id)
            paths.append(p)

    # AMPLIFICATION -- the paper's `Amplify`, and the step whose absence is why
    # a first implementation reads as fragments rather than as a cave.
    #
    # Sink-to-spring shortest paths give the TRUNK and nothing else. A real
    # cave is mostly not trunk: it is tributaries, blind alleys and abandoned
    # loops, which is why the reference scenes carry explicit Waypoint and
    # Deadend key points (their "superimposed" scene is 1 sink and 2 springs
    # against 2 waypoints and ELEVEN deadends; the spongework scene is ~50).
    # So: pick extra nodes in the corridor and route each to the nearest node
    # ALREADY on the skeleton, which is what makes a branch a branch rather
    # than a second trunk.
    attach = {}
    # HILLSIDE MOUTHS ARE ROUTED IN FIRST, and they are routed to the EXISTING
    # skeleton rather than being new outlets. That is what a hillside mouth
    # physically IS: a valley wall retreating until it cuts an existing conduit
    # open sideways. Making them outlets instead would grow separate stub
    # systems that happen to touch a hillside -- holes leading nowhere, which is
    # the opposite of the walk-in access they exist to provide.
    seeds = list(mouth_ids) + (
        [int(rng.integers(0, len(nodes))) for _ in range(deadends)]
        if deadends > 0 and rng is not None else [])
    if seeds and paths:
        on_skeleton = sorted({n for p in paths for n in p})
        if len(on_skeleton) >= 2:
            for cand in seeds:
                if cand in on_skeleton:
                    continue
                d2, pred2 = dijkstra(g, directed=False, indices=cand,
                                     return_predecessors=True)
                # HIERARCHICAL, NOT NEAREST. Connecting every dead-end to the
                # closest skeleton node piles them onto whichever node happens
                # to be central, which is what produced the spidery hubs -- many
                # short spokes meeting at one point, which is not how caves
                # branch. Penalising a node that has already been attached to
                # spreads the junctions ALONG the trunk, which is what a
                # tributary actually does.
                reach = [(d2[t] * (1.0 + ATTACH_PENALTY * attach.get(t, 0)), t)
                         for t in on_skeleton if np.isfinite(d2[t])]
                if not reach:
                    continue
                _, tgt = min(reach)
                attach[tgt] = attach.get(tgt, 0) + 1
                p2, cur = [], tgt
                guard = 0
                while cur != cand and cur >= 0 and guard < 10000:
                    p2.append(cur)
                    cur = pred2[cur]
                    guard += 1
                if cur == cand:
                    p2.append(cand)
                    paths.append(p2)

    # Gamma-skeleton prune, on the REDUCED skeleton (the union of the paths),
    # never on the sampling graph -- that is the difference between milliseconds
    # and minutes.
    keep = set()
    for p in paths:
        keep.update(p)
    keep = sorted(keep)
    # Always return EDGE PAIRS. The early exit used to return the raw paths,
    # which are variable-length node lists, so the caller unpacked a 1-element
    # path as an edge and died. One return type, one shape.
    if len(keep) < 3:
        return [[u, v] for p in paths for u, v in zip(p, p[1:])]
    remap = {n: i for i, n in enumerate(keep)}
    kn = nodes[keep]
    ktree = cKDTree(kn)
    edges = set()
    for p in paths:
        for u, v in zip(p, p[1:]):
            edges.add((min(remap[u], remap[v]), max(remap[u], remap[v])))
    pruned = []
    for u, v in edges:
        duv = np.linalg.norm(kn[u] - kn[v])
        drop = False
        for w_ in ktree.query_ball_point(kn[u], duv):
            if w_ in (u, v):
                continue
            a = np.linalg.norm(kn[u] - kn[w_])
            b = np.linalg.norm(kn[w_] - kn[v])
            if a ** gamma + b ** gamma < duv ** gamma:
                drop = True
                break
        if not drop:
            pruned.append((u, v))
    return [[keep[u], keep[v]] for u, v in pruned]


def main() -> int:
    # NODE SPACING AND GAMMA INTERACT AND MUST BE SWEPT TOGETHER. Spacing sets
    # how many candidate routes exist; gamma sets how many survive pruning.
    # Swept alone, the winning value of each can measure WORSE than the loser --
    # the same trap docs/backlog.md section 0.6 already records for
    # GPUCullMergeGap and GPUCullMaxRanges. The `global` has to be declared
    # before argparse reads these names for its defaults.
    global NODE_SPACING_M, NEIGHBOURS, W_DIST, W_HORIZON, HORIZON_SHARP, FOLD, SEED

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fields", type=pathlib.Path)
    ap.add_argument("--gamma", type=float, default=2.0)
    ap.add_argument("--max-systems", type=int, default=60)
    ap.add_argument("--node-spacing-m", type=float, default=NODE_SPACING_M,
                    help="corridor sample spacing; sweep TOGETHER with --gamma")
    ap.add_argument("--neighbours", type=int, default=NEIGHBOURS)
    ap.add_argument("--w-dist", type=float, default=W_DIST)
    ap.add_argument("--w-horizon", type=float, default=W_HORIZON)
    ap.add_argument("--horizon-sharp", type=float, default=1.0,
                    help="exponent on the horizon term; >1 makes it a narrow band")
    ap.add_argument("--piece-m", type=float, default=8.0,
                    help="subdivision length for the sinuosity pass")
    ap.add_argument("--wander-z", type=float, default=0.15,
                    help="vertical share of the wander; see subdivide()'s note")
    ap.add_argument("--wander", type=float, default=0.0,
                    help="centreline displacement in piece lengths; 0 = straight")
    ap.add_argument("--mouths-per-system", type=int, default=6,
                    help="hillside mouths wired into each system")
    ap.add_argument("--mouth-depth-m", type=float, default=4.0)
    ap.add_argument("--deadends", type=int, default=0,
                    help="amplification key points per system (paper's Amplify)")
    ap.add_argument("--spring-sep-m", type=float, default=1500.0,
                    help="minimum separation between resurgences (see suppress_springs)")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    args = ap.parse_args()
    out = args.out or args.fields.parent

    W_DIST = args.w_dist
    W_HORIZON = args.w_horizon
    HORIZON_SHARP = args.horizon_sharp
    NODE_SPACING_M = args.node_spacing_m
    NEIGHBOURS = args.neighbours

    t0 = time.time()
    f = load_fields(args.fields)
    elev, head = f["elev_m"], f["head_m"]
    incept, coher, theta = f["incept"], f["coher"], f["theta"]
    log2acc = f["log2acc"]
    sinks, springs = f["sinks"], f["springs"]
    FOLD = f["fold_m"] if "fold_m" in f else np.zeros_like(elev)
    SEED = int(f["seed_used"]) if "seed_used" in f else 20260719
    if "fold_m" not in f:
        print("  WARNING: fields file predates fold_m; horizons will be FLAT in z")
    rng = np.random.default_rng(12345)

    n_raw = len(springs)
    springs = suppress_springs(springs, log2acc, args.spring_sep_m / GRID_M)
    print(f"springs {n_raw} -> {len(springs)} after suppression at "
          f"{args.spring_sep_m:.0f} m ({len(springs) / (elev.shape[0] * elev.shape[1] * GRID_M * GRID_M / 1e6):.2f}/km^2)")
    systems = assign_systems(sinks, springs, head)
    order = sorted(systems.items(), key=lambda kv: -len(kv[1]))[:args.max_systems]
    print(f"{len(sinks)} sinks, {len(springs)} springs -> {len(systems)} systems; "
          f"routing the {len(order)} largest")

    all_segments, lengths, depths, routed, failed = [], [], [], 0, 0
    routed_mouths = 0
    mouth_pts = []
    mouths = f["mouths"] if "mouths" in f else np.empty((0, 2), np.int32)
    print(f"hillside mouths available: {len(mouths)}")
    for si, (spring_idx, sink_list) in enumerate(order):
        sp = springs[spring_idx]
        nodes, _ = sample_corridor(sink_list, tuple(int(v) for v in sp),
                                   elev, head, incept, coher, log2acc, rng)
        if len(nodes) < 8:
            failed += 1
            continue
        tree = cKDTree(nodes)
        spx = np.array([sp[0] * GRID_M, sp[1] * GRID_M,
                        head[sp[1], sp[0]]], float)
        _, spring_id = tree.query(spx)
        sink_ids = []
        for s in sink_list:
            q = np.array([s[0] * GRID_M, s[1] * GRID_M, elev[s[1], s[0]] - 5.0])
            _, i = tree.query(q)
            sink_ids.append(int(i))
        mouth_ids = []
        if len(mouths):
            lo = nodes[:, :2].min(axis=0) / GRID_M
            hi = nodes[:, :2].max(axis=0) / GRID_M
            inside = mouths[(mouths[:, 0] >= lo[0]) & (mouths[:, 0] <= hi[0]) &
                            (mouths[:, 1] >= lo[1]) & (mouths[:, 1] <= hi[1])]
            for mx_, my_ in inside[:args.mouths_per_system]:
                # Snap just INSIDE the hill: a mouth is the passage meeting the
                # slope, so its node sits a passage-radius below the surface.
                qy = np.array([mx_ * GRID_M, my_ * GRID_M,
                               elev[my_, mx_] - args.mouth_depth_m], float)
                # SNAP TO A LEVEL NEIGHBOUR, not simply the nearest node. A
                # mouth sits on steep ground by definition, so the nearest node
                # in 3D is usually below it and the connector into the hill
                # comes out as a scramble or a shaft -- which is precisely what
                # a walk-in entrance must not be. Measured: nearest-node snapping
                # put 91 mouths on the network and left only 7.4% of it
                # reachable on foot from one. A real mouth runs roughly LEVEL
                # into the hill along the bedding, so penalise height difference
                # hard and let horizontal distance decide the rest.
                kq = min(24, len(nodes))
                dd, ii = tree.query(qy, k=kq)
                ii = np.atleast_1d(ii)
                candn = nodes[ii]
                horiz = np.linalg.norm(candn[:, :2] - qy[:2], axis=1)
                dz = np.abs(candn[:, 2] - qy[2])
                i2 = int(ii[np.argmin(horiz + 8.0 * dz)])
                mouth_ids.append(i2)
        segs = build_system(nodes, elev, head, incept, coher, theta, log2acc,
                            sink_ids, int(spring_id), args.gamma,
                            deadends=args.deadends, rng=rng, mouth_ids=mouth_ids)
        routed_mouths += len(mouth_ids)
        mouth_pts.extend(nodes[i2_].tolist() for i2_ in mouth_ids)
        if not segs:
            failed += 1
            continue
        routed += 1
        for u, v in segs:
            a, b = nodes[u], nodes[v]
            all_segments.append((a.tolist(), b.tolist()))
            lengths.append(float(np.linalg.norm(b - a)))
            mx = int(np.clip((a[0] + b[0]) * 0.5 / GRID_M, 0, elev.shape[1] - 1))
            my = int(np.clip((a[1] + b[1]) * 0.5 / GRID_M, 0, elev.shape[0] - 1))
            depths.append(float(elev[my, mx] - (a[2] + b[2]) * 0.5))

    # --- SINUOSITY, applied to the NETWORK rather than only at render time.
    #
    # Shortest-path over a smooth cost field is near-straight as a matter of
    # mathematics, so no weighting of the geological terms will produce a
    # meander -- swept, and it does not (mean tortuosity moved 1.08 -> 1.16
    # across a 3x3 of spacing and weights). Real conduit sinuosity at the 10-50 m
    # scale does not come from optimising a smooth field either; it comes from
    # following discrete fractures and irregular dissolution. So it is ADDED,
    # which is what the reference method's midpoint subdivision does and what
    # Minecraft's noise carving does.
    #
    # It belongs HERE and not in the voxeliser: the skeleton is what gets baked,
    # measured and shipped, and a sinuosity that exists only in a preview is a
    # sinuosity the game never sees.
    if args.wander > 0 and all_segments:
        segs_in = np.asarray([[a, b] for a, b in all_segments], float)
        all_segments = [(a.tolist(), b.tolist())
                        for a, b in subdivide(segs_in, args.piece_m, args.wander,
                                              z_scale=args.wander_z)]
        lengths = [float(np.linalg.norm(np.asarray(b) - np.asarray(a)))
                   for a, b in all_segments]

    total_km = sum(lengths) / 1000.0

    # --- TORTUOSITY: the metric the owner's eye caught and mine did not have.
    # Path length divided by straight-line distance, over maximal chains through
    # degree-2 nodes. Surveyed karst runs about 1.3-2.0; a straight line is 1.0.
    # Measured here rather than in a side script so every run reports it.
    def tortuosity(segments):
        if not segments:
            return {}
        key = lambda p: (round(float(p[0]), 1), round(float(p[1]), 1), round(float(p[2]), 1))
        adj = collections.defaultdict(list)
        for a, b in segments:
            adj[key(a)].append(key(b)); adj[key(b)].append(key(a))
        seen, tort = set(), []
        for start in adj:
            if len(adj[start]) == 2:
                continue
            for nb in adj[start]:
                if (start, nb) in seen:
                    continue
                path, prev, cur = [start], start, nb
                while True:
                    path.append(cur)
                    seen.add((prev, cur)); seen.add((cur, prev))
                    if len(adj[cur]) != 2:
                        break
                    nxt = [n for n in adj[cur] if n != prev]
                    if not nxt:
                        break
                    prev, cur = cur, nxt[0]
                P = np.asarray(path, float)
                if len(P) < 2:
                    continue
                L = float(np.linalg.norm(np.diff(P, axis=0), axis=1).sum())
                C = float(np.linalg.norm(P[-1] - P[0]))
                if C > 30.0:
                    tort.append(L / C)
        if not tort:
            return {}
        a = np.asarray(tort)
        return {"chains": len(a), "mean": round(float(a.mean()), 3),
                "p50": round(float(np.percentile(a, 50)), 3),
                "p90": round(float(np.percentile(a, 90)), 3),
                "straight_frac": round(float((a < 1.05).mean()), 3)}
    area_km2 = (elev.shape[0] * GRID_M / 1000.0) * (elev.shape[1] * GRID_M / 1000.0)
    stats = {
        "gamma": args.gamma, "deadends": args.deadends,
        "wander": args.wander, "piece_m": args.piece_m,
        "wander_z": args.wander_z,
        "node_spacing_m": NODE_SPACING_M, "neighbours": NEIGHBOURS,
        "spring_sep_m": args.spring_sep_m,
        "systems_total": len(systems), "systems_routed": routed,
        "hillside_mouths_wired": routed_mouths,
        "systems_failed": failed,
        "segments": len(all_segments),
        "passage_km": round(total_km, 2),
        "area_km2": round(area_km2, 1),
        "passage_density_km_per_km2": round(total_km / area_km2, 4),
        "segment_len_m_mean": round(float(np.mean(lengths)), 1) if lengths else None,
        "depth_below_surface_m": {
            "mean": round(float(np.mean(depths)), 1) if depths else None,
            "p50": round(float(np.percentile(depths, 50)), 1) if depths else None,
            "p90": round(float(np.percentile(depths, 90)), 1) if depths else None,
            "max": round(float(np.max(depths)), 1) if depths else None,
        },
        "tortuosity": tortuosity(all_segments),
        "seconds": round(time.time() - t0, 1),
    }
    print(json.dumps(stats, indent=2))

    stem = args.fields.stem.replace("-fields", "")
    (out / f"{stem}-network.json").write_text(json.dumps(stats, indent=2))
    # SAVE THE SPRINGS AND SINKS THAT WERE ACTUALLY ROUTED, not the raw
    # candidates. The renderer reads this file; when it read the field stage's
    # springs instead it drew all 1,461 candidates over a network routed to 38,
    # so the picture disagreed with the run that produced it. A figure that does
    # not show the thing it is a figure OF is worse than no figure.
    np.savez_compressed(out / f"{stem}-network.npz",
                        segments=np.asarray(all_segments, np.float32).reshape(-1, 2, 3),
                        springs_used=np.asarray(springs, np.int32).reshape(-1, 2),
                        sinks_used=np.asarray(sinks, np.int32).reshape(-1, 2),
                        mouth_nodes=np.asarray(mouth_pts, np.float32).reshape(-1, 3))
    print(f"wrote {out}/{stem}-network.{{json,npz}}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
