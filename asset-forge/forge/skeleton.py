"""Growing the branch skeleton by space colonization.

Runions et al. 2007, "Modeling Trees with a Space Colonization Algorithm"
(https://algorithmicbotany.org/papers/colonization.egwnp2007.large.pdf).

Scatter growth targets in the crown volume; every branch tip that can see a
target grows toward the average of the targets it is nearest to; targets are
consumed once a branch reaches them. Competition for space produces the
branching pattern, rather than a recursion rule producing it. That is why it
handles irregular natural crowns that a purely recursive model struggles with,
and why branches never grow through each other.

The output is a skeleton -- curves with a radius at every point -- not a mesh.
Nothing here knows what a voxel is. `rasterize.py` makes that decision.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
from scipy.spatial import cKDTree

from . import envelope
from .spec import get

MAX_NODES = 30_000  # runaway guard; a dense 30 m tree lands near 6k
MAX_CHILDREN = 4    # a node that has forked four times stops competing


@dataclass
class Skeleton:
    pos: np.ndarray       # (N, 3) metres, tree base at origin
    parent: np.ndarray    # (N,) int64, -1 at the root
    order: np.ndarray     # (N,) int64, 0 on the trunk, +1 per fork
    radius: np.ndarray    # (N,) metres
    is_tip: np.ndarray    # (N,) bool
    targets_left: int
    iterations: int
    # Position along a node's own branch, 0 at its base and 1 at its tip. Only
    # the frond model populates it; it is what lets a blade be widest in the
    # middle and taper to a point, which spheres-on-twigs cannot express.
    along: np.ndarray | None = None

    @property
    def n(self) -> int:
        return self.pos.shape[0]

    def segments(self):
        """(parent_index, child_index) for every branch segment."""
        child = np.flatnonzero(self.parent >= 0)
        return self.parent[child], child

    def height_m(self) -> float:
        return float(self.pos[:, 2].max()) if self.n else 0.0


def _normalize(v: np.ndarray, axis: int = -1) -> np.ndarray:
    n = np.linalg.norm(v, axis=axis, keepdims=True)
    return np.divide(v, np.where(n < 1e-12, 1.0, n))


def grow(spec: dict, rng: np.random.Generator) -> Skeleton:
    targets = envelope.points(spec, rng)
    step = float(get(spec, "growth.step_m"))
    influence = float(get(spec, "growth.influence_m"))
    kill = float(get(spec, "growth.kill_m"))
    inertia = float(get(spec, "growth.inertia"))
    jitter = float(get(spec, "growth.jitter"))
    z_bias = float(get(spec, "growth.phototropism")) + float(get(spec, "growth.gravity"))

    pos: list[np.ndarray] = []
    parent: list[int] = []
    direction: list[np.ndarray] = []
    n_children: list[int] = []

    def add(p: np.ndarray, par: int, d: np.ndarray) -> int:
        pos.append(p)
        parent.append(par)
        direction.append(d)
        n_children.append(0)
        if par >= 0:
            n_children[par] += 1
        return len(pos) - 1

    # -- trunk ---------------------------------------------------------------
    # Grow straight up until the tip can see a target and we are clear of the
    # branch-free height. Without this the crown grows from the ground and the
    # tree has no trunk at all.
    lean = math.radians(float(get(spec, "trunk.lean_deg")))
    lean_dir = math.radians(float(get(spec, "trunk.lean_dir_deg")))
    d = np.array(
        [math.sin(lean) * math.cos(lean_dir), math.sin(lean) * math.sin(lean_dir), math.cos(lean)]
    )
    add(np.zeros(3), -1, d.copy())

    clear_z = float(get(spec, "height_m")) * float(get(spec, "trunk.clear_frac"))
    crown_bottom, crown_top = envelope.crown_bounds(spec)
    wander = float(get(spec, "trunk.wander"))
    tip = 0
    ceiling = min(crown_top, float(get(spec, "height_m")))

    for _ in range(int(math.ceil(ceiling / max(step, 1e-3))) + 4):
        p = pos[tip]
        if p[2] >= clear_z and targets.size:
            if np.min(np.linalg.norm(targets - p, axis=1)) <= influence:
                break
        if p[2] >= ceiling:
            break
        nd = direction[tip].copy()
        if wander > 0.0:
            nd = nd + rng.normal(scale=0.35 * wander, size=3)
            nd[2] = abs(nd[2]) + 0.25  # never let the trunk turn back downward
        nd = _normalize(nd)
        tip = add(p + step * nd, tip, nd)

    # -- colonization --------------------------------------------------------
    iterations = 0
    max_iter = int(get(spec, "growth.max_iter"))
    for iterations in range(1, max_iter + 1):
        if targets.shape[0] == 0 or len(pos) >= MAX_NODES:
            break

        node_pos = np.asarray(pos)
        tree = cKDTree(node_pos)
        dist, idx = tree.query(targets, distance_upper_bound=influence)
        seen = np.isfinite(dist)
        if not seen.any():
            break

        # A node that has already forked its limit stops attracting, which
        # keeps whorls from forming at a single point.
        owner = idx[seen].astype(np.int64)
        seen_targets = targets[seen]
        counts = np.asarray(n_children)
        room = counts[owner] < MAX_CHILDREN
        if not room.any():
            break
        owner = owner[room]
        seen_targets = seen_targets[room]

        pull = _normalize(seen_targets - node_pos[owner])

        acc = np.zeros_like(node_pos)
        np.add.at(acc, owner, pull)
        active = np.flatnonzero(np.bincount(owner, minlength=node_pos.shape[0]) > 0)

        v = _normalize(acc[active])
        v = v + inertia * np.asarray(direction)[active]
        v[:, 2] += z_bias
        if jitter > 0.0:
            v = v + rng.normal(scale=jitter, size=v.shape)
        v = _normalize(v)

        new_pos = node_pos[active] + step * v
        grew = False
        for k, par in enumerate(active):
            if new_pos[k, 2] <= 0.0:  # never grow into the ground
                continue
            add(new_pos[k], int(par), v[k])
            grew = True
        if not grew:
            break

        # Consume targets that a branch has now reached.
        alive = cKDTree(np.asarray(pos))
        d2, _ = alive.query(targets, distance_upper_bound=kill)
        targets = targets[~np.isfinite(d2)]

    pos_a = np.asarray(pos, dtype=np.float64)
    parent_a = np.asarray(parent, dtype=np.int64)
    children_a = np.asarray(n_children, dtype=np.int64)
    order_a = _orders(parent_a, children_a)
    radius_a = _radii(spec, pos_a, parent_a)

    return Skeleton(
        pos=pos_a,
        parent=parent_a,
        order=order_a,
        radius=radius_a,
        is_tip=children_a == 0,
        targets_left=int(targets.shape[0]),
        iterations=iterations,
    )


def grow_whorl(spec: dict, rng: np.random.Generator) -> Skeleton:
    """Conifer structure: rings of branches up a straight leader.

    Space colonization cannot produce this. Its branch pattern is an emergent
    consequence of where targets happen to fall, so it gives an irregular crown
    that is merely cone-*shaped*; a spruce's tiers are an actual botanical
    structure — a ring of branches laid down each growing season, spaced up a
    single dominant leader. Building that explicitly is the difference between
    a cone of generic foliage and something that reads as a conifer.

    The crown profile is reused from `envelope`, so `crown.shape` still governs
    the silhouette: branch length at each ring is the profile's radius there.
    """
    pos: list[np.ndarray] = []
    parent: list[int] = []
    direction: list[np.ndarray] = []
    n_children: list[int] = []

    def add(p: np.ndarray, par: int, d: np.ndarray) -> int:
        pos.append(p)
        parent.append(par)
        direction.append(d)
        n_children.append(0)
        if par >= 0:
            n_children[par] += 1
        return len(pos) - 1

    height = float(get(spec, "height_m"))
    step = float(get(spec, "growth.step_m"))
    wander = float(get(spec, "trunk.wander"))
    irregular = float(get(spec, "whorl.irregular"))

    # -- leader: one straight trunk all the way to the tip -------------------
    lean = math.radians(float(get(spec, "trunk.lean_deg")))
    lean_dir = math.radians(float(get(spec, "trunk.lean_dir_deg")))
    d = np.array([math.sin(lean) * math.cos(lean_dir),
                  math.sin(lean) * math.sin(lean_dir), math.cos(lean)])
    add(np.zeros(3), -1, d.copy())

    trunk_idx = [0]
    tip = 0
    for _ in range(int(math.ceil(height / max(step, 1e-3))) + 2):
        p = pos[tip]
        if p[2] >= height:
            break
        nd = direction[tip].copy()
        if wander > 0.0:
            nd = nd + rng.normal(scale=0.28 * wander, size=3)
            nd[2] = abs(nd[2]) + 0.4
        nd = _normalize(nd)
        tip = add(p + step * nd, tip, nd)
        trunk_idx.append(tip)

    trunk_z = np.array([pos[i][2] for i in trunk_idx])

    # -- rings ---------------------------------------------------------------
    bottom, top = envelope.crown_bounds(spec)
    bottom = max(bottom, height * float(get(spec, "trunk.clear_frac")))
    top = min(top, height * (1.0 - float(get(spec, "whorl.leader"))))
    if top <= bottom:
        top = min(height, bottom + max(1.0, height * 0.2))

    profile = envelope.profile_for(get(spec, "crown.shape"))
    radius = float(get(spec, "crown.radius_m"))
    count = int(get(spec, "whorl.count"))
    per = int(get(spec, "whorl.branches"))
    stagger = float(get(spec, "whorl.stagger"))
    rise = float(get(spec, "whorl.rise"))
    droop = float(get(spec, "whorl.droop"))
    subs = int(get(spec, "whorl.sub"))
    sub_angle = math.radians(float(get(spec, "whorl.sub_angle")))
    jitter = float(get(spec, "growth.jitter"))

    for ring in range(count):
        # Rings run up to and including the crown top. Centring them in their
        # slice left the last one short of the tip, so a bare leader stuck out
        # above the foliage.
        frac = (ring + 1.0) / count
        z = bottom + (top - bottom) * frac
        z += (rng.random() - 0.5) * irregular * (top - bottom) / max(count, 1)
        z = float(np.clip(z, bottom, top))

        v = (z - bottom) / max(top - bottom, 1e-6)
        length = float(profile(np.array([v]))[0]) * radius
        length *= 1.0 + irregular * (rng.random() - 0.5) * 0.8
        # Floor every ring at something drawable. A cone profile goes to zero at
        # the tip, so without this the top rings round away entirely and the
        # leader stands up bare above the foliage like a flagpole.
        length = max(length, step * 1.6)

        anchor = trunk_idx[int(np.argmin(np.abs(trunk_z - z)))]
        base_az = ring * stagger * 2.0 * math.pi + rng.random() * irregular * 2.0
        for b in range(per):
            az = base_az + b * 2.0 * math.pi / per
            az += (rng.random() - 0.5) * irregular * 0.7
            _branch(add, pos, anchor, az, length, step, rise, droop, jitter, rng,
                    subs, sub_angle, depth=0)

    pos_a = np.asarray(pos, dtype=np.float64)
    parent_a = np.asarray(parent, dtype=np.int64)
    children_a = np.asarray(n_children, dtype=np.int64)
    return Skeleton(
        pos=pos_a,
        parent=parent_a,
        order=_orders(parent_a, children_a),
        radius=_radii(spec, pos_a, parent_a),
        is_tip=children_a == 0,
        targets_left=0,
        iterations=count,
    )


def grow_frond(spec: dict, rng: np.random.Generator) -> Skeleton:
    """Palm structure: an unbranched trunk carrying a crown of arcing fronds.

    A palm has no branches at all. Trying to express one with a branching model
    is why our first attempt read as a lollipop: clumps scattered on twigs make
    a blob, where a palm is a bare column topped by a dozen long leaves, each
    lifting away from the crown and arcing over under its own weight.
    """
    pos: list[np.ndarray] = []
    parent: list[int] = []
    direction: list[np.ndarray] = []
    n_children: list[int] = []
    along: list[float] = []

    def add(p, par, d, s=0.0) -> int:
        pos.append(p)
        parent.append(par)
        direction.append(d)
        n_children.append(0)
        along.append(s)
        if par >= 0:
            n_children[par] += 1
        return len(pos) - 1

    height = float(get(spec, "height_m"))
    step = float(get(spec, "growth.step_m"))
    wander = float(get(spec, "trunk.wander"))

    lean = math.radians(float(get(spec, "trunk.lean_deg")))
    lean_dir = math.radians(float(get(spec, "trunk.lean_dir_deg")))
    d = np.array([math.sin(lean) * math.cos(lean_dir),
                  math.sin(lean) * math.sin(lean_dir), math.cos(lean)])
    add(np.zeros(3), -1, d.copy())

    # -- bare trunk, no branches ---------------------------------------------
    tip = 0
    for _ in range(int(math.ceil(height / max(step, 1e-3))) + 2):
        if pos[tip][2] >= height:
            break
        nd = direction[tip].copy()
        if wander > 0.0:
            nd = nd + rng.normal(scale=0.30 * wander, size=3)
            nd[2] = abs(nd[2]) + 0.5
        nd = _normalize(nd)
        tip = add(pos[tip] + step * nd, tip, nd)
    crown_node = tip

    # -- fronds ---------------------------------------------------------------
    count = int(get(spec, "frond.count"))
    length = float(get(spec, "frond.length_m"))
    rise = float(get(spec, "frond.rise"))
    droop = float(get(spec, "frond.droop"))
    dead = float(get(spec, "frond.dead"))
    irregular = float(get(spec, "frond.irregular"))
    jitter = float(get(spec, "growth.jitter"))

    for f in range(count):
        az = f * 2.0 * math.pi / count + (rng.random() - 0.5) * irregular * 1.4
        L = length * (1.0 + irregular * (rng.random() - 0.5) * 0.7)
        collapsed = rng.random() < dead
        f_rise = -0.9 if collapsed else rise * (1.0 + irregular * (rng.random() - 0.5))
        f_droop = droop * (2.2 if collapsed else 1.0 + irregular * (rng.random() - 0.5) * 0.6)

        n = max(3, int(round(L / max(step, 1e-3))))
        out = np.array([math.cos(az), math.sin(az), 0.0])
        prev = crown_node
        p = pos[crown_node].copy()
        for k in range(1, n + 1):
            s = k / n
            # Lift decays fast, droop grows late: the arc of a palm leaf, not a
            # straight spoke and not a uniform curve.
            dz = f_rise * (1.0 - s) ** 0.55 - f_droop * (s ** 1.9)
            step_dir = _normalize(out + np.array([0.0, 0.0, dz]))
            if jitter > 0.0:
                step_dir = _normalize(step_dir + rng.normal(scale=jitter * 0.4, size=3))
            p = p + step * step_dir
            if p[2] <= 0.05:
                break
            prev = add(p, prev, step_dir, s)

    pos_a = np.asarray(pos, dtype=np.float64)
    parent_a = np.asarray(parent, dtype=np.int64)
    children_a = np.asarray(n_children, dtype=np.int64)
    return Skeleton(
        pos=pos_a,
        parent=parent_a,
        order=_orders(parent_a, children_a),
        radius=_radii(spec, pos_a, parent_a),
        is_tip=children_a == 0,
        targets_left=0,
        iterations=count,
        along=np.asarray(along, dtype=np.float64),
    )


def _branch(add, pos, anchor: int, az: float, length: float, step: float,
            rise: float, droop: float, jitter: float, rng, subs: int,
            sub_angle: float, depth: int) -> None:
    """One branch arcing out from the trunk: lifting at the base, drooping at
    the tip. A straight ray reads as a spoke; the arc is what makes it a bough."""
    n = max(2, int(round(length / max(step, 1e-3))))
    out = np.array([math.cos(az), math.sin(az), 0.0])
    prev = anchor
    start = pos[anchor].copy()
    travelled = 0.0

    for k in range(1, n + 1):
        s = k / n
        # Vertical profile along the branch: rise decays, droop grows with s.
        dz = rise * (1.0 - s) - droop * (s ** 1.6)
        d = _normalize(out + np.array([0.0, 0.0, dz]))
        if jitter > 0.0:
            d = _normalize(d + rng.normal(scale=jitter * 0.6, size=3))
        start = start + step * d
        travelled += step
        prev = add(start, prev, d)

        # Sub-branches fan off the middle of the branch, angled forward and
        # slightly down, which is how conifer side-shoots actually sit.
        if depth == 0 and subs > 0 and 0.25 < s < 0.85:
            if rng.random() < subs / max(n * 0.6, 1.0):
                side = 1.0 if rng.random() < 0.5 else -1.0
                _branch(add, pos, prev, az + side * sub_angle,
                        length * (1.0 - s) * 0.65, step, rise * 0.4, droop * 1.2,
                        jitter, rng, 0, sub_angle, depth + 1)


def add_roots(skel: Skeleton, spec: dict, rng: np.random.Generator) -> Skeleton:
    """Surface roots radiating from the base.

    Distinct from `trunk.buttress`, which only multiplies the trunk radius near
    the ground -- that thickens a cylinder, it does not make a root. These arch
    up out of the base and run back down to the ground as separate ridges, which
    is what a kapok or a mature beech actually stands on.
    """
    count = int(get(spec, "roots.count"))
    if count <= 0 or skel.n == 0:
        return skel

    length = float(get(spec, "roots.length_m"))
    rise = float(get(spec, "roots.rise"))
    thickness = float(get(spec, "roots.thickness"))
    irregular = float(get(spec, "roots.irregular"))
    step = float(get(spec, "growth.step_m"))
    trunk_r = float(get(spec, "trunk.radius_base_m"))
    tip_r = float(get(spec, "growth.tip_radius_m"))

    pos = list(skel.pos)
    parent = list(skel.parent)
    radius = list(skel.radius)
    order = list(skel.order)
    n_children = np.bincount(skel.parent[skel.parent >= 0], minlength=skel.n).tolist()
    n_children += [0] * (len(pos) - len(n_children))

    for r in range(count):
        az = r * 2.0 * math.pi / count + (rng.random() - 0.5) * irregular * 1.5
        L = length * (1.0 + irregular * (rng.random() - 0.5) * 0.9)
        n = max(2, int(round(L / max(step, 1e-3))))
        out = np.array([math.cos(az), math.sin(az), 0.0])
        prev = 0
        p = skel.pos[0].copy()
        for k in range(1, n + 1):
            s = k / n
            # Up over the shoulder, then back down to the ground by the tip.
            z = rise * math.sin(math.pi * min(s * 1.15, 1.0)) * (1.0 - s * 0.65)
            p = np.array([out[0] * L * s, out[1] * L * s, max(z, 0.0)])
            pos.append(p.copy())
            parent.append(prev)
            radius.append(trunk_r * thickness * (1.0 - s) + tip_r * s)
            order.append(1)
            n_children.append(0)
            n_children[prev] += 1
            prev = len(pos) - 1

    n_children_a = np.asarray(n_children[:len(pos)], dtype=np.int64)
    return Skeleton(
        pos=np.asarray(pos, dtype=np.float64),
        parent=np.asarray(parent, dtype=np.int64),
        order=np.asarray(order, dtype=np.int64),
        radius=np.asarray(radius, dtype=np.float64),
        is_tip=n_children_a == 0,
        targets_left=skel.targets_left,
        iterations=skel.iterations,
        along=skel.along,
    )


def add_strands(skel: Skeleton, spec: dict, rng: np.random.Generator) -> Skeleton:
    """Hang long trailing branches from the crown.

    A post-pass rather than part of growth, because the thing that makes a
    weeping willow is exactly what space colonization cannot do: its targets
    are consumed on arrival, so a branch reaching the crown edge stops there.
    A strand instead ignores targets entirely and simply falls, which is what a
    willow withe, a liana and a curtain of hanging moss all are.
    """
    count = int(get(spec, "strand.count"))
    if count <= 0 or skel.n == 0:
        return skel

    length = float(get(spec, "strand.length_m"))
    from_frac = float(get(spec, "strand.from_frac"))
    outer = float(get(spec, "strand.outer"))
    drift = float(get(spec, "strand.drift"))
    spread = float(get(spec, "strand.spread"))
    step = float(get(spec, "growth.step_m"))
    tip_r = float(get(spec, "growth.tip_radius_m"))

    bottom, top = envelope.crown_bounds(spec)
    floor = bottom + (top - bottom) * from_frac

    z = skel.pos[:, 2]
    radial = np.hypot(skel.pos[:, 0], skel.pos[:, 1])
    eligible = np.flatnonzero((z >= floor) & (skel.radius <= tip_r * 4.0))
    if eligible.size == 0:
        return skel

    # Weight anchors toward the crown edge so strands form a curtain rather
    # than a beard hanging down the middle of the tree.
    weight = 1.0 + outer * 6.0 * (radial[eligible] / max(radial[eligible].max(), 1e-6)) ** 2
    weight = weight / weight.sum()
    anchors = rng.choice(eligible, size=min(count, eligible.size * 3), p=weight, replace=True)

    pos = list(skel.pos)
    parent = list(skel.parent)
    radius = list(skel.radius)
    order = list(skel.order)
    n_children = np.bincount(skel.parent[skel.parent >= 0], minlength=skel.n).tolist()
    n_children += [0] * (len(pos) - len(n_children))

    for anchor in anchors:
        anchor = int(anchor)
        p = skel.pos[anchor].copy()
        out = np.array([skel.pos[anchor][0], skel.pos[anchor][1], 0.0])
        out = out / max(np.linalg.norm(out), 1e-6)
        n = max(2, int(round(length * (0.5 + rng.random()) / max(step, 1e-3))))
        prev = anchor
        base_order = int(skel.order[anchor]) + 1
        for _ in range(n):
            d = np.array([0.0, 0.0, -1.0]) + out * spread
            d = d + rng.normal(scale=drift, size=3)
            d[2] = min(d[2], -0.25)  # a strand always falls
            d = _normalize(d)
            p = p + step * d
            if p[2] <= 0.1:
                break
            pos.append(p.copy())
            parent.append(prev)
            radius.append(tip_r)
            order.append(base_order)
            n_children.append(0)
            n_children[prev] += 1
            prev = len(pos) - 1

    n_children_a = np.asarray(n_children[:len(pos)], dtype=np.int64)
    return Skeleton(
        pos=np.asarray(pos, dtype=np.float64),
        parent=np.asarray(parent, dtype=np.int64),
        order=np.asarray(order, dtype=np.int64),
        radius=np.asarray(radius, dtype=np.float64),
        is_tip=n_children_a == 0,
        targets_left=skel.targets_left,
        iterations=skel.iterations,
        along=None,
    )


def _orders(parent: np.ndarray, n_children: np.ndarray) -> np.ndarray:
    """Branch order: 0 along the trunk, +1 every time a node forks.

    Nodes are always appended after their parent, so one forward pass is
    enough. The first child continues its parent's branch; later children start
    a new one.
    """
    order = np.zeros(parent.shape[0], dtype=np.int64)
    seen_child = np.zeros(parent.shape[0], dtype=bool)
    for i in range(1, parent.shape[0]):
        p = parent[i]
        if seen_child[p]:
            order[i] = order[p] + 1
        else:
            order[i] = order[p]
            seen_child[p] = True
    return order


def _radii(spec: dict, pos: np.ndarray, parent: np.ndarray) -> np.ndarray:
    """Branch thickness by Murray's law, then scaled to the authored trunk.

    r_parent^e = sum(r_child^e). The relation is homogeneous, so scaling every
    radius by one factor scales the trunk base by the same factor -- which
    means the designer can set the trunk radius directly and the whole tree
    stays in proportion.
    """
    n = pos.shape[0]
    e = float(get(spec, "growth.radius_exp"))
    tip_r = float(get(spec, "growth.tip_radius_m"))

    acc = np.full(n, tip_r**e, dtype=np.float64)
    for i in range(n - 1, 0, -1):  # children always have a higher index
        acc[parent[i]] += acc[i]
    radius = acc ** (1.0 / e)

    root = radius[0] if n else 1.0
    if root > 1e-9:
        radius *= float(get(spec, "trunk.radius_base_m")) / root

    # Root flare. Thickens the lowest metre and a half of wood, which is what
    # reads as a tree standing in the ground rather than pushed into it.
    buttress = float(get(spec, "trunk.buttress"))
    if buttress > 0.0:
        t = np.clip(1.0 - pos[:, 2] / 1.5, 0.0, 1.0)
        radius = radius * (1.0 + buttress * t**2)
    return radius
