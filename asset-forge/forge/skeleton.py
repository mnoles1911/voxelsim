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
