"""The voxel grid, and the primitives that draw into it.

Everything here works in VOXELS, not metres. The conversion happens once, at
the pipeline boundary, against `VOXEL_M`.

The important rule in this file is the minimum thickness rule. The engine's
voxels are 10 cm (`kVoxelSizeMm = 100`), and a real twig is thinner than that.
If we drew branches by their true radius, every branch under 10 cm across would
either vanish or land on an arbitrary side of a rounding decision, and a crown
could end up not touching its trunk. So `capsule()` always draws the segment's
centreline with a face-connected voxel traversal first, and only then thickens
it. Connectivity is therefore something we guarantee, not something we measure
afterwards and hope for.
"""

from __future__ import annotations

import functools
import math

import numpy as np

VOXEL_M = 0.10  # metres per voxel, matches vxc::kVoxelSizeMm = 100


def m_to_vox(metres: float) -> float:
    return metres / VOXEL_M


class VoxelGrid:
    """Dense uint8 material grid indexed [x, y, z], z up.

    Dense rather than sparse because even the largest tree we expect (a 30 m
    jungle emergent, 300 voxels tall) is a few megabytes, and dense keeps every
    drawing primitive a plain numpy slice.
    """

    __slots__ = ("data", "origin")

    def __init__(self, shape: tuple[int, int, int], origin: tuple[int, int, int] = (0, 0, 0)):
        self.data = np.zeros(shape, dtype=np.uint8)
        self.origin = np.asarray(origin, dtype=np.int64)

    @property
    def shape(self) -> tuple[int, int, int]:
        return self.data.shape

    def count(self) -> int:
        return int(np.count_nonzero(self.data))

    def histogram(self) -> dict[int, int]:
        ids, counts = np.unique(self.data, return_counts=True)
        return {int(i): int(c) for i, c in zip(ids, counts) if i != 0}

    # -- drawing ------------------------------------------------------------

    def _write(self, xs, ys, zs, mat: int, only_air: bool) -> None:
        nx, ny, nz = self.data.shape
        ok = (xs >= 0) & (xs < nx) & (ys >= 0) & (ys < ny) & (zs >= 0) & (zs < nz)
        if not ok.all():
            xs, ys, zs = xs[ok], ys[ok], zs[ok]
        if xs.size == 0:
            return
        if only_air:
            free = self.data[xs, ys, zs] == 0
            xs, ys, zs = xs[free], ys[free], zs[free]
            if xs.size == 0:
                return
        self.data[xs, ys, zs] = mat

    def ball(self, c: np.ndarray, r_vox: float, mat: int, only_air: bool = False) -> None:
        """Solid sphere centred at `c` (float voxel coords).

        `floor`, not `round`: a voxel spans [i, i+1), so the voxel containing a
        point is its floor. The traversal in `_traverse` uses the same
        convention, and it has to -- when a taper thins to a single-voxel ball,
        a half-voxel disagreement between the two puts that ball diagonally off
        the centreline, and it ends up as an orphaned voxel floating beside the
        twig it belongs to.
        """
        dx, dy, dz = _ball_offsets(_quantize(r_vox))
        cx, cy, cz = np.floor(c).astype(np.int64)
        self._write(dx + cx, dy + cy, dz + cz, mat, only_air)

    def line(self, p0: np.ndarray, p1: np.ndarray, mat: int, only_air: bool = False) -> None:
        """Every voxel the segment passes through, face-connected."""
        xs, ys, zs = _traverse(p0, p1)
        self._write(xs, ys, zs, mat, only_air)

    def capsule(
        self,
        p0: np.ndarray,
        p1: np.ndarray,
        r0_vox: float,
        r1_vox: float,
        mat: int,
        core_mat: int | None = None,
        core_inset_vox: float = 1.0,
    ) -> None:
        """Tapered capsule from p0 (radius r0) to p1 (radius r1).

        The centreline goes down first at full connectivity, so a branch too
        thin to survive rounding still exists as an unbroken one-voxel thread.

        `core_mat` fills the interior wherever the segment is thick enough to
        have one, which is what makes a chopped trunk show heartwood instead of
        bark all the way through.
        """
        self.line(p0, p1, mat)

        length = float(np.linalg.norm(p1 - p0))
        rmax = max(r0_vox, r1_vox)
        if rmax <= 0.5:
            # Thinner than a voxel: the centreline already is the branch, and
            # most segments in a tree are twigs, so this early exit is most of
            # the generator's speed.
            return

        # Half-voxel steps: consecutive balls always overlap, so the thickened
        # body cannot be beaded even at the taper's thin end.
        steps = max(1, int(math.ceil(length / 0.5)))
        ts = np.linspace(0.0, 1.0, steps + 1)
        for t in ts:
            c = p0 + (p1 - p0) * t
            r = r0_vox + (r1_vox - r0_vox) * t
            if r > 0.5:
                self.ball(c, r, mat)
            if core_mat is not None and r - core_inset_vox > 0.5:
                self.ball(c, r - core_inset_vox, core_mat)

    def blob(
        self,
        c: np.ndarray,
        r_vox: float,
        mat: int,
        rng: np.random.Generator,
        density: float = 1.0,
        squash: float = 1.0,
        only_air: bool = True,
    ) -> None:
        """A ragged ellipsoid, for foliage clumps.

        `density` below 1 drops voxels at random, which is what stops a canopy
        reading as a solid green lump at 10 cm.
        """
        dx, dy, dz = _ball_offsets(_quantize(r_vox))
        if squash != 1.0:
            keep = (dx * dx + dy * dy + (dz / max(squash, 1e-3)) ** 2) <= r_vox * r_vox + 1e-6
            dx, dy, dz = dx[keep], dy[keep], dz[keep]
        if dx.size == 0:
            return
        if density < 1.0:
            # Bias retention toward the clump centre so it thins at the edges
            # rather than turning into uniform static.
            d = np.sqrt(dx * dx + dy * dy + dz * dz) / max(r_vox, 1e-6)
            p = np.clip(density * (1.35 - 0.55 * d), 0.0, 1.0)
            keep = rng.random(dx.size) < p
            dx, dy, dz = dx[keep], dy[keep], dz[keep]
            if dx.size == 0:
                return
        cx, cy, cz = np.floor(c).astype(np.int64)
        self._write(dx + cx, dy + cy, dz + cz, mat, only_air)

    # -- finishing ----------------------------------------------------------

    def crop(self) -> "VoxelGrid":
        """Shrink to the occupied bounding box, keeping world origin correct."""
        occ = self.data != 0
        if not occ.any():
            return self
        xs = np.flatnonzero(occ.any(axis=(1, 2)))
        ys = np.flatnonzero(occ.any(axis=(0, 2)))
        zs = np.flatnonzero(occ.any(axis=(0, 1)))
        out = VoxelGrid(
            (xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1, zs[-1] - zs[0] + 1),
            tuple(self.origin + np.array([xs[0], ys[0], zs[0]])),
        )
        out.data[:] = self.data[xs[0] : xs[-1] + 1, ys[0] : ys[-1] + 1, zs[0] : zs[-1] + 1]
        return out

    def surface_mask(self) -> np.ndarray:
        """Voxels with at least one of their six neighbours empty.

        The renderer's own cull only drops faces the fixed isometric camera can
        never see. A viewer you can orbit needs every voxel that is exposed in
        any direction, so this is the version to send to the browser.
        """
        occ = self.data != 0
        exposed = np.zeros_like(occ)
        for axis in (0, 1, 2):
            hi = [slice(None)] * 3
            lo = [slice(None)] * 3
            hi[axis] = slice(1, None)
            lo[axis] = slice(None, -1)
            exposed[tuple(lo)] |= occ[tuple(lo)] & ~occ[tuple(hi)]
            exposed[tuple(hi)] |= occ[tuple(hi)] & ~occ[tuple(lo)]
            edge_lo = [slice(None)] * 3
            edge_hi = [slice(None)] * 3
            edge_lo[axis] = 0
            edge_hi[axis] = -1
            exposed[tuple(edge_lo)] |= occ[tuple(edge_lo)]
            exposed[tuple(edge_hi)] |= occ[tuple(edge_hi)]
        return exposed & occ

    def material_mask(self, mats) -> np.ndarray:
        return np.isin(self.data, np.asarray(list(mats), dtype=np.uint8))

    def component_fraction(self, mask: np.ndarray | None = None, connectivity: int = 1) -> float:
        """Share of the masked voxels in the largest connected component.

        `connectivity` is scipy's: 1 is face-only (6 neighbours), 3 is corners
        too (26). Which one is right depends on what is being asked. Wood has
        to be face-connected -- a branch joined to the trunk only at a corner is
        a branch that falls off. Foliage only has to touch, because a leaf
        clump is deliberately speckled and demanding face connectivity there
        would force it back into a solid lump.
        """
        occ = (self.data != 0) if mask is None else mask
        total = int(occ.sum())
        if total == 0:
            return 1.0
        try:
            from scipy import ndimage
        except ImportError:
            return float("nan")
        structure = ndimage.generate_binary_structure(3, connectivity)
        labels, n = ndimage.label(occ, structure=structure)
        if n <= 1:
            return 1.0
        counts = np.bincount(labels.ravel())
        counts[0] = 0
        return float(counts.max() / total)


# --- primitives -------------------------------------------------------------


def _quantize(r: float) -> float:
    """Round radius to quarter voxels so the offset cache actually hits."""
    return max(0.5, round(float(r) * 4.0) / 4.0)


@functools.lru_cache(maxsize=512)
def _ball_offsets(r: float):
    R = int(math.floor(r))
    a = np.arange(-R, R + 1, dtype=np.int64)
    dx, dy, dz = np.meshgrid(a, a, a, indexing="ij")
    m = (dx * dx + dy * dy + dz * dz) <= r * r + 1e-6
    if not m.any():
        z = np.zeros(1, dtype=np.int64)
        return z, z, z
    return dx[m].copy(), dy[m].copy(), dz[m].copy()


def _traverse(p0: np.ndarray, p1: np.ndarray):
    """Amanatides-Woo voxel traversal: every voxel the segment enters.

    Unlike a 3D Bresenham line this never steps diagonally past a corner, so
    the result is 6-connected. That matters because a diagonal-only chain of
    voxels reads as a broken branch and meshes badly.
    """
    p0 = np.asarray(p0, dtype=np.float64)
    p1 = np.asarray(p1, dtype=np.float64)
    cur = np.floor(p0).astype(np.int64)
    end = np.floor(p1).astype(np.int64)
    d = p1 - p0

    step = np.sign(d).astype(np.int64)
    out = [cur.copy()]

    tmax = np.empty(3)
    tdelta = np.empty(3)
    for i in range(3):
        if d[i] == 0:
            tmax[i] = math.inf
            tdelta[i] = math.inf
        else:
            nxt = cur[i] + (1 if step[i] > 0 else 0)
            tmax[i] = (nxt - p0[i]) / d[i]
            tdelta[i] = step[i] / d[i]

    # Bound the walk by the true Manhattan distance; a segment can never enter
    # more voxels than that, so this cannot loop forever on a degenerate input.
    budget = int(np.abs(end - cur).sum()) + 2
    for _ in range(budget):
        if np.array_equal(cur, end):
            break
        i = int(np.argmin(tmax))
        if not math.isfinite(tmax[i]) or tmax[i] > 1.0:
            break
        cur = cur.copy()
        cur[i] += step[i]
        tmax[i] += tdelta[i]
        out.append(cur)

    arr = np.array(out, dtype=np.int64)
    return arr[:, 0], arr[:, 1], arr[:, 2]
