"""karst_voxel.py -- what the conduits look like AS VOXELS, and the blend question.

THE RISK THAT CAN KILL THIS PROJECT, ANSWERED IN THE CHEAPEST PLACE.

Paris et al. get their organic cave geometry from smooth-min blended signed
distance functions. `smin` has no early-out and no bounded primitive count,
which is incompatible with everything voxel-core needs: a fixed per-column
segment cap, a cheap per-voxel early-out, and integer-only arithmetic under a CI
float ban. The obvious degradation is a HARD UNION of capsules -- and a hard
union of capsules reads as intersecting cylinders, which is verbatim the owner's
original complaint about the system being replaced ("caves look very computer
made with procedural shapes"). Replacing a hash lattice of capsules with a
Dijkstra graph of capsules would ship the same artefact with better provenance.

So this renders the SAME junction three ways at 20 cm and lets a human look:

    hard      union of capsules, min(sdf)          -- the cheap, shippable one
    blended   smooth-min, k configurable           -- the paper's look, unshippable as-is
    filleted  hard union PLUS bake-time fillet     -- the proposed mitigation:
              nodes inserted at junctions with       spend nodes at bake time to
              interpolated radii                     buy the blended look at
                                                     hard-union runtime cost

If `filleted` is indistinguishable from `blended`, the mitigation works and the
shipping path is clear. If it is not, the geometry layer needs rethinking, and
finding that out here costs an afternoon instead of a shader port.

IT ALSO ANSWERS THE FLOOR QUESTION, which is a playability question rather than
a look one. A circular tube has a CURVED floor: walkable near the axis, and
rising out of reach at the sides. Real cave passages are walkable because
sediment fills the bottom -- which is both genuine karst geomorphology and
exactly what a game needs. `--sediment` fills below a per-segment floor level
and the walkable-floor map shows what it buys.

Usage:
    python tools/karst_voxel.py <network.npz> [--vox-m 0.2] [--span-m 60]
                                [--sediment 0.35] [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt   # noqa: E402
import numpy as np                 # noqa: E402

# Player, from VoxelMovementTuning.h -- see karst_playability.py's header.
PLAYER_WIDTH_M = 0.60
PLAYER_STAND_M = 1.80
STEP_UP_M = 0.30


def segment_sdf(px, py, pz, a, b, r):
    """Signed distance to one capsule. Negative inside."""
    abx, aby, abz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    apx, apy, apz = px - a[0], py - a[1], pz - a[2]
    ab2 = abx * abx + aby * aby + abz * abz
    t = (apx * abx + apy * aby + apz * abz) / max(ab2, 1e-9)
    np.clip(t, 0.0, 1.0, out=t)
    dx = apx - abx * t
    dy = apy - aby * t
    dz = apz - abz * t
    return np.sqrt(dx * dx + dy * dy + dz * dz) - r


def _h3(ix, iy, iz, seed):
    """Integer hash -> [-1, 1]. Value noise needs a lattice, and a lattice needs
    a hash; this is the prototype's own, deliberately NOT voxel-core's, because
    the shipping generator will use the integer one and two copies of a hash
    that must agree is a bug this repo has shipped before."""
    v = (np.uint64(ix) * np.uint64(0x9E3779B97F4A7C15)
         ^ np.uint64(iy) * np.uint64(0xC2B2AE3D27D4EB4F)
         ^ np.uint64(iz) * np.uint64(0x165667B19E3779F9)
         ^ np.uint64(seed))
    v = (v ^ (v >> np.uint64(29))) * np.uint64(0xBF58476D1CE4E5B9)
    v = (v ^ (v >> np.uint64(32)))
    return (v & np.uint64(0xFFFFFF)).astype(np.float64) / float(1 << 23) - 1.0


def _vnoise(p, cell, seed):
    """Trilinear value noise at world point p (3,)."""
    q = np.asarray(p, float) / cell
    i = np.floor(q).astype(np.int64)
    f = q - i
    w = f * f * (3.0 - 2.0 * f)
    acc = 0.0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                wt = ((w[0] if dx else 1 - w[0]) * (w[1] if dy else 1 - w[1])
                      * (w[2] if dz else 1 - w[2]))
                acc += wt * _h3(i[0] + dx, i[1] + dy, i[2] + dz, seed)
    return acc


def subdivide(seg, piece_m, wander, seed=7):
    """MIDPOINT SUBDIVISION WITH DISPLACEMENT -- the step the reference code
    lists as missing, and the reason a first implementation looks wrong.

    A skeleton edge is a straight line between two graph nodes. Here those nodes
    are ~217 m apart, so drawing one capsule per edge produces a DEAD STRAIGHT
    217 m tunnel of constant radius -- which is exactly the artefact the system
    being replaced was rejected for ("straight constant-radius capsules, one
    hash draw per edge") and exactly what the owner rejected on sight here.

    Real conduits wander because they follow joints, bedding and the local
    hydraulic gradient, none of which are straight. Minecraft's spaghetti caves
    do the same thing with the same trick: perturb the centreline continuously.
    So each edge is cut into `piece_m` pieces and every interior point is pushed
    off the chord by value noise, with the amplitude scaled by the piece length
    so the wander is a shape property rather than a fixed wobble.
    """
    out = []
    for a, b in seg:
        L = float(np.linalg.norm(b - a))
        n = max(1, int(round(L / piece_m)))
        pts = [a + (b - a) * (k / n) for k in range(n + 1)]
        for k in range(1, n):
            t = k / n
            taper = 4.0 * t * (1.0 - t)      # zero at the shared nodes
            amp = wander * piece_m * taper
            d = np.array([_vnoise(pts[k], piece_m * 2.5, seed + 11),
                          _vnoise(pts[k], piece_m * 2.5, seed + 23),
                          _vnoise(pts[k], piece_m * 2.5, seed + 37) * 0.6])
            pts[k] = pts[k] + d * amp
        for k in range(n):
            out.append((pts[k], pts[k + 1]))
    return out


def smin(a, b, k):
    """Polynomial smooth minimum -- the blend the reference method relies on."""
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b * (1.0 - h) + a * h - k * h * (1.0 - h)


def pick_junction(seg):
    """The node with the highest degree: junctions are where a hard union shows
    its crease, so measuring anywhere else would flatter it."""
    key = lambda p: (round(float(p[0]), 2), round(float(p[1]), 2), round(float(p[2]), 2))
    deg = {}
    for i in range(len(seg)):
        for e in (0, 1):
            deg[key(seg[i, e])] = deg.get(key(seg[i, e]), 0) + 1
    best = max(deg.items(), key=lambda kv: kv[1])
    return np.array(best[0], float), best[1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("network", type=pathlib.Path)
    ap.add_argument("--vox-m", type=float, default=0.2)
    ap.add_argument("--span-m", type=float, default=60.0)
    ap.add_argument("--radius-m", type=float, default=3.0)
    ap.add_argument("--blend-k", type=float, default=4.0)
    ap.add_argument("--piece-m", type=float, default=12.0,
                    help="subdivision length; the wander's characteristic scale")
    ap.add_argument("--wander", type=float, default=0.5,
                    help="centreline displacement, in piece lengths (0 = straight)")
    ap.add_argument("--radius-var", type=float, default=0.45,
                    help="fractional radius variation along the conduit")
    ap.add_argument("--wall-rough", type=float, default=0.18,
                    help="wall roughness as a fraction of the tube radius")
    ap.add_argument("--fillet-scale", type=float, default=1.35,
                    help="fillet sphere radius as a multiple of the tube radius")
    ap.add_argument("--fillet-offset", type=float, default=0.6,
                    help="how far along each tube the fillet sits, in tube radii")
    ap.add_argument("--sediment", type=float, default=0.35,
                    help="fraction of the tube radius filled with flat sediment floor")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    args = ap.parse_args()
    out = args.out or args.network.parent
    out.mkdir(parents=True, exist_ok=True)

    seg = np.load(args.network)["segments"].astype(float)
    c, degree = pick_junction(seg)
    print(f"junction at {c.round(1)} with degree {degree}")

    h = args.span_m * 0.5
    # Select on ENDPOINT proximity, not midpoint. Segments here average 217 m,
    # so a segment that touches the junction has its midpoint 100 m away and a
    # midpoint test finds nothing -- which is exactly what the first run did.
    near = []
    for i in range(len(seg)):
        if (np.linalg.norm(seg[i, 0] - c) < h + 40.0
                or np.linalg.norm(seg[i, 1] - c) < h + 40.0):
            near.append(seg[i])
    near = np.asarray(near)
    print(f"{len(near)} segments in a {args.span_m:.0f} m window")
    if len(near) == 0:
        return 2

    v = args.vox_m
    n = int(args.span_m / v)
    ax = (np.arange(n) - n / 2) * v
    X, Y, Z = np.meshgrid(c[0] + ax, c[1] + ax, c[2] + ax, indexing="ij")
    print(f"grid {n}^3 = {n**3/1e6:.1f}M voxels at {v*100:.0f} cm")

    R = args.radius_m
    pieces = subdivide(near, args.piece_m, args.wander) if args.wander > 0 else         [(s[0], s[1]) for s in near]
    print(f"{len(near)} edges -> {len(pieces)} pieces "
          f"(wander {args.wander}, piece {args.piece_m} m)")

    hard = np.full(X.shape, 1e9, np.float32)
    blend = np.full(X.shape, 1e9, np.float32)
    for a, b in pieces:
        # RADIUS VARIES ALONG THE CONDUIT. A constant radius is the other half
        # of "too perfect": real passages pinch and open out, and a tube that
        # never does reads as pipework. Noise on the piece midpoint, so
        # neighbouring pieces agree and the change is gradual.
        mid = 0.5 * (a + b)
        rv = 1.0 + args.radius_var * _vnoise(mid, args.piece_m * 6.0, 101)
        d = segment_sdf(X, Y, Z, a, b, R * max(0.35, rv)).astype(np.float32)
        np.minimum(hard, d, out=hard)
        blend = smin(blend, d, args.blend_k).astype(np.float32)

    # WALL ROUGHNESS. Even a wandering tube of varying radius has a perfectly
    # smooth wall; rock does not. A field perturbation of the distance is the
    # cheapest honest version and is what the existing cavern pass already does
    # (caverns.h's kCavernRoughAmpQ10), so it is a shape the engine can afford.
    if args.wall_rough > 0:
        rough = np.zeros_like(hard)
        for oct_, (cell, amp) in enumerate(((6.0, 1.0), (2.0, 0.45), (0.8, 0.2))):
            gx = np.floor(X / cell).astype(np.int64)
            gy = np.floor(Y / cell).astype(np.int64)
            gz = np.floor(Z / cell).astype(np.int64)
            rough += amp * _h3(gx, gy, gz, 5000 + oct_).astype(np.float32)
        rough *= np.float32(args.wall_rough * R / 1.65)
        hard = hard + rough
        blend = blend + rough

    # FILLETED: the mitigation. Insert a node at the junction carrying a swollen
    # radius, which is what a bake-time fillet IS -- extra primitives bought
    # once, so the runtime can stay a hard union with an early-out.
    fillet = hard.copy()
    hub = []
    for s in near:
        for e in (0, 1):
            if np.linalg.norm(s[e] - c) < R * 2.0:
                other = s[1 - e]
                v_ = other - s[e]
                nv = np.linalg.norm(v_)
                if nv > 1e-6:
                    # ALONG THE BISECTOR, not at the node. A sphere centred on
                    # the node only inflates the node; the crease a hard union
                    # leaves is in the WEDGE between the two tubes, a little way
                    # down each of them, so that is where a fillet has to sit.
                    hub.append(s[e] + v_ / nv * (R * args.fillet_offset))
    for pnt in hub:
        d = segment_sdf(X, Y, Z, pnt, pnt + np.array([0.0, 0.0, 1e-3]),
                        R * args.fillet_scale).astype(np.float32)
        np.minimum(fillet, d, out=fillet)

    fields = {"hard": hard, "blended": blend, "filleted": fillet}

    # --- sediment floor: flat fill below a level, per the header ------------
    if args.sediment > 0:
        floor_z = c[2] - R * (1.0 - 2.0 * args.sediment)
        for k, fld in list(fields.items()):
            solid_below = (Z < floor_z)
            fld = np.where(solid_below, np.maximum(fld, 0.05), fld)
            fields[k + "+sed"] = fld

    stats = {}
    mid = n // 2
    order = ["hard", "blended", "filleted"] + \
            ([k for k in fields if k.endswith("+sed")] if args.sediment > 0 else [])
    fig, axes = plt.subplots(2, len(order), figsize=(4.0 * len(order), 8.4), dpi=130,
                             squeeze=False)
    for j, name in enumerate(order):
        fld = fields[name]
        air = fld <= 0.0
        # vertical slice through the junction, and a plan slice at axis height
        axes[0][j].imshow(air[:, mid, :].T, origin="lower", cmap="bone_r",
                          extent=[ax[0], ax[-1], ax[0], ax[-1]], interpolation="nearest")
        axes[0][j].set_title(f"{name} — vertical", fontsize=10)
        axes[1][j].imshow(air[:, :, mid].T, origin="lower", cmap="bone_r",
                          extent=[ax[0], ax[-1], ax[0], ax[-1]], interpolation="nearest")
        axes[1][j].set_title(f"{name} — plan", fontsize=10)
        for a_ in (axes[0][j], axes[1][j]):
            a_.set_xlabel("m"); a_.set_ylabel("m")

        # --- walkable floor: an air voxel with solid directly under it and
        # --- PLAYER_STAND_M of air above. This is the playability truth at
        # --- voxel resolution rather than the skeleton approximation.
        solid = ~air
        # STANDABLE: the lowest air voxel of each column-run that has solid
        # under it and PLAYER_STAND_M of clear air above.
        need = int(round(PLAYER_STAND_M / v))
        floor = air[:, :, 1:-need] & solid[:, :, :-1 - need]
        head = np.ones_like(floor)
        for dz in range(1, need + 1):
            head &= air[:, :, 1 + dz: air.shape[2] - need + dz]
        stand = floor & head

        # WALKABLE adds the test the first version of this metric omitted, and
        # the omission flattered curved floors badly: a floor voxel is only
        # walkable if a NEIGHBOURING standable floor is within the step-up.
        # Without it, the staircase down the inside of a circular tube counts as
        # walkable floor, which is how a tube bottom scored higher than a flat
        # sediment floor -- the exact opposite of the truth.
        step = int(round(STEP_UP_M / v))
        h = np.where(stand, np.arange(stand.shape[2])[None, None, :], -10**6).max(axis=2)
        ok = h > -10**5
        reach = np.zeros_like(ok)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nb = np.roll(np.where(ok, h, -10**6), (dx, dy), axis=(0, 1))
            reach |= ok & (np.abs(nb - h) <= step) & (nb > -10**5)
        stats[name] = {
            "air_frac": round(float(air.mean()), 5),
            "standable_m2": round(float(stand.sum()) * v * v, 1),
            "walkable_m2": round(float(reach.sum()) * v * v, 1),
        }
    fig.suptitle(f"conduit geometry at {v*100:.0f} cm — junction of degree {degree}, "
                 f"tube radius {R:.1f} m", fontsize=12)
    fig.tight_layout()
    fig.savefig(out / "karst-voxel-blend.png")
    plt.close(fig)

    ref = fields["blended"] <= 0.0
    for name in stats:
        a2 = fields[name] <= 0.0
        inter = float((a2 & ref).sum())
        union = float((a2 | ref).sum())
        stats[name]["iou_vs_blended"] = round(inter / union, 4) if union else None
    print(json.dumps(stats, indent=2))
    (out / "karst-voxel-blend.json").write_text(json.dumps(stats, indent=2))
    print(f"wrote {out}/karst-voxel-blend.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
