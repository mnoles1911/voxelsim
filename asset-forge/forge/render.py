"""Isometric preview renderer.

The whole point of this tool is that a designer looks at trees and keeps the
good ones, so the picture is the product, not a debug aid. It renders in-process
with numpy and Pillow -- no engine, no editor, no GPU -- which is what lets a
hundred-tree contact sheet be a thing you run rather than a thing you schedule.

Method: painter's algorithm on the voxel lattice. In a 2:1 isometric projection
the drawing order `x + y + z` is exactly back-to-front for a cubic grid, so
depth is resolved by sorting rather than by a per-pixel depth test, and the
whole image is one vectorised scatter.
"""

from __future__ import annotations

import math

import numpy as np
from PIL import Image

from . import materials
from .grid import VoxelGrid

# Face shading. Top brightest, +x face next, +y face darkest -- a single
# top-left-ish key light, which is what reads most clearly for voxel art.
SHADE_TOP = 1.00
SHADE_RIGHT = 0.76
SHADE_LEFT = 0.55

BACKGROUND = (24, 26, 30, 255)


def _sprite(scale: int):
    """One cube, as flat offset arrays: (dx, dy, shade).

    Geometry for half-width a = 2s, top-face half-height b = s, vertical edge
    h = 2s. Sprite box is 4s wide and 4s tall.
    """
    a, b, h = 2 * scale, scale, 2 * scale
    wp, hp = 2 * a, 2 * b + h
    px, py = np.meshgrid(np.arange(wp), np.arange(hp), indexing="ij")
    px = px.ravel()
    py = py.ravel()

    # Top face: rhombus with corners (a,0) (2a,b) (a,2b) (0,b).
    top = (np.abs(px - a) / a + np.abs(py - b) / b) <= 1.0 + 1e-9

    # Side faces hang below the rhombus edges.
    edge_right = b + (2 * a - px) * b / a   # for px >= a
    edge_left = b + px * b / a              # for px <  a
    right = (px >= a) & (py >= edge_right) & (py < edge_right + h)
    left = (px < a) & (py >= edge_left) & (py < edge_left + h)

    keep = top | left | right
    shade = np.where(top, SHADE_TOP, np.where(right, SHADE_RIGHT, SHADE_LEFT))
    return px[keep] - a, py[keep], shade[keep].astype(np.float32)


def _visible(data: np.ndarray) -> np.ndarray:
    """Voxels with at least one camera-facing neighbour empty.

    The camera looks down the (-1,-1,-1) diagonal, so only the +x, +y and +z
    faces can ever be seen. Dropping the rest cuts a solid trunk's interior out
    of the scatter entirely.
    """
    occ = data != 0
    exposed = np.zeros_like(occ)
    exposed[:-1] |= occ[:-1] & ~occ[1:]
    exposed[-1] |= occ[-1]
    exposed[:, :-1] |= occ[:, :-1] & ~occ[:, 1:]
    exposed[:, -1] |= occ[:, -1]
    exposed[:, :, :-1] |= occ[:, :, :-1] & ~occ[:, :, 1:]
    exposed[:, :, -1] |= occ[:, :, -1]
    return exposed & occ


def _occlusion(data: np.ndarray, coords: tuple[np.ndarray, ...]) -> np.ndarray:
    """Cheap ambient occlusion: how enclosed each voxel is, 0..1.

    Without it a dense canopy renders as one flat green silhouette and the
    designer cannot see its shape at all.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return np.zeros(coords[0].shape[0], dtype=np.float32)
    occ = (data != 0).astype(np.float32)
    dens = ndimage.uniform_filter(occ, size=3, mode="constant", cval=0.0)
    return dens[coords].astype(np.float32)


def predicted_extent(spec: dict, voxel_m: float = 0.10) -> tuple[int, int, int]:
    """Roughly how many voxels an asset from this spec will occupy.

    Used to choose one rendering scale for a whole batch without having to build
    everything first and hold it all in memory, and to pick a preview
    resolution. It only has to be close: the contact sheet applies a single
    shrink factor afterwards, so a slightly wrong guess changes the page size,
    not the relative sizes on it.

    It does have to be close FOR THE RIGHT KIND, though. Reading a tree's height
    and crown radius off a rock spec returns the untouched defaults -- a 12 m
    crown for a half-metre pebble -- and the resolution picker took that at face
    value and previewed every small stone at the coarsest tier it had.
    """
    from .spec import get  # local import keeps this module free of spec at load

    kind = get(spec, "kind")
    if kind in ("grass", "reed", "flower"):
        h = float(get(spec, "height_m"))
        head = float(get(spec, "tuft.head_m")) if get(spec, "tuft.head") != "none" else 0.0
        # An arcing stem reaches out roughly as far as it would have gone up.
        reach = float(get(spec, "tuft.spread_m")) + h * float(get(spec, "tuft.arc")) * 0.8
        span = 2.0 * (reach + head) + 0.1
        return (int(span / voxel_m), int(span / voxel_m),
                int((h * 1.1 + head) / voxel_m))

    if kind == "rock":
        size = float(get(spec, "rock.size_m"))
        span = size * max(1.0, float(get(spec, "rock.elongate"))) * (
            1.0 + float(get(spec, "rock.rubble")) * 1.6) + 0.4
        tall = size * max(1.0, float(get(spec, "rock.flatten"))) + 0.4
        return (int(span / voxel_m), int(span / voxel_m), int(tall / voxel_m))

    height = float(get(spec, "height_m")) * 1.2
    crown = float(get(spec, "crown.radius_m"))
    clump = float(get(spec, "foliage.clump_radius_m")) if get(spec, "foliage.enabled") else 0.0
    crown_h = float(get(spec, "height_m")) * float(get(spec, "crown.height_frac"))
    shear = math.tan(math.radians(float(get(spec, "crown.lean_deg")))) * crown_h
    span = 2.0 * (crown + clump) + shear + 1.0
    return (int(span / voxel_m), int(span / voxel_m), int(height / voxel_m))


def grid_extent(grid: VoxelGrid) -> tuple[int, int, int]:
    return grid.shape


def pick_scale(grid: VoxelGrid, target_px: int) -> int:
    return scale_for([grid_extent(grid)], target_px)


def scale_for(extents, target_px: int) -> int:
    """One scale that fits every extent in the set.

    A sheet where each tree is scaled to fill its own cell makes a 4.5 m
    sapling and a 28 m emergent the same size on the page, which hides the one
    property the designer most needs to compare. Everything on a sheet renders
    at a single scale so that size is readable.
    """
    nx = max(e[0] for e in extents)
    ny = max(e[1] for e in extents)
    nz = max(e[2] for e in extents)
    for s in (8, 6, 4, 3, 2, 1):
        w = (nx + ny) * 2 * s + 4 * s
        h = (nx + ny) * s + nz * 2 * s + 4 * s
        if max(w, h) <= target_px:
            return s
    return 1


def render(
    grid: VoxelGrid,
    *,
    scale: int | None = None,
    target_px: int = 512,
    background: tuple[int, int, int, int] | None = BACKGROUND,
    ao: float = 0.45,
) -> Image.Image:
    data = grid.data
    if scale is None:
        scale = pick_scale(grid, target_px)

    vis = _visible(data)
    xs, ys, zs = np.nonzero(vis)
    if xs.size == 0:
        return Image.new("RGBA", (16, 16), background or (0, 0, 0, 0))

    mats = data[xs, ys, zs]
    a, b, h = 2 * scale, scale, 2 * scale

    sx = (xs - ys) * a
    sy = (xs + ys) * b - zs * h

    dx, dy, shade = _sprite(scale)

    # Canvas: sprite offsets run [-a, a) horizontally and [0, 2b+h) vertically.
    x0 = int(sx.min()) - a
    y0 = int(sy.min())
    width = int(sx.max()) + a - x0 + 1
    height = int(sy.max()) + 2 * b + h - y0 + 1

    # Base colour per voxel, with occlusion and a little per-voxel variation so
    # large flat faces do not band.
    rgb = np.array([materials.color(int(m)) for m in range(materials.MAX_ID + 1)], dtype=np.float32)
    base = rgb[mats]
    if ao > 0.0:
        enclosed = _occlusion(data, (xs, ys, zs))
        base = base * (1.0 - ao * enclosed)[:, None]
    jitter = ((xs * 73856093) ^ (ys * 19349663) ^ (zs * 83492791)) % 17
    base = base * (0.94 + 0.0075 * jitter.astype(np.float32))[:, None]

    n, s = xs.size, dx.size
    pix_x = (sx[:, None] - x0) + dx[None, :]
    pix_y = (sy[:, None] - y0) + dy[None, :]
    flat = (pix_y * width + pix_x).ravel()

    depth = (xs + ys + zs).astype(np.int64)
    depth_flat = np.repeat(depth, s)

    # Nearest voxel wins each pixel: sort by pixel, then by descending depth,
    # and keep the first row of each pixel's run.
    order = np.lexsort((-depth_flat, flat))
    flat_sorted = flat[order]
    first = np.ones(flat_sorted.shape[0], dtype=bool)
    first[1:] = flat_sorted[1:] != flat_sorted[:-1]
    chosen = order[first]
    target_pixels = flat_sorted[first]

    voxel_of = chosen // s
    sprite_of = chosen % s
    colour = base[voxel_of] * shade[sprite_of][:, None]

    canvas = np.zeros((height * width, 4), dtype=np.uint8)
    if background is not None:
        canvas[:] = background
    canvas[target_pixels, :3] = np.clip(colour, 0, 255).astype(np.uint8)
    canvas[target_pixels, 3] = 255

    return Image.fromarray(canvas.reshape(height, width, 4), "RGBA")
