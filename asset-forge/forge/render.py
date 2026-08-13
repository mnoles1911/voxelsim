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

from . import materials, palette
from .grid import VoxelGrid

# Face shading. Top brightest, +x face next, +y face darkest -- a single
# top-left-ish key light, which is what reads most clearly for voxel art.
SHADE_TOP = 1.00
SHADE_RIGHT = 0.76
SHADE_LEFT = 0.55

BACKGROUND = (24, 26, 30, 255)

# How far above level the elevation camera sits. Low enough to see under an
# overhang and through the gaps in a stack; not zero, so ledges still read.
ELEVATION_TILT_DEG = 10.0


def _slots() -> int:
    """How many material slots the renderer has to be able to colour.

    The generated palette is the truth for everything the ENGINE has. It is not
    the whole table here, because a material is always proposed in
    `forge/materials.py` before it is appended to `voxelcore/core.h` -- bark,
    the leaf variants and the blossom colour all spent time in that state, and
    the creature skins are there now. A material with no palette row renders
    magenta (`palette.entry`), which is right for a MISTAKE and wrong for a
    proposal: it would make every fish in the app magenta and there would be
    nothing to look at until an engine change lands.
    """
    return max(palette.MATERIAL_COUNT, materials.MAX_ID + 1)


def _proposed(m: int) -> bool:
    """True for a material the engine's palette does not carry yet."""
    return m >= palette.MATERIAL_COUNT


def _material_faces():
    """Base colour per material for the top face and for the side faces.

    Built once from the generated palette, falling back to the stand-in colour
    in `forge/materials.py` for anything the engine has not been given yet. The
    bottom face never faces the camera in this projection, so it is not carried.

    THIS IS NOT A SECOND COLOUR SYSTEM and must not become one. There is exactly
    one place a shipped material's appearance is defined -- the engine header --
    and `tools/gen_palette.py` plus the selftest make sure the copy here is
    generated from it. What this covers is the gap between proposing a material
    and it existing, and the moment the append lands, the generated row wins
    silently and automatically because it is checked first.
    """
    n = _slots()
    top = np.zeros((n, 3), np.float32)
    side = np.zeros((n, 3), np.float32)
    for m in range(n):
        if _proposed(m):
            top[m] = side[m] = materials.color(m)
        else:
            e = palette.entry(m)
            top[m] = e[palette.FACE_TOP]
            side[m] = e[palette.FACE_SIDE]
    return top, side


def _hash01(a, b, c, salt: int):
    """A stable 0..1 value per integer lattice cell."""
    h = (a.astype(np.int64) * 73856093) ^ (b.astype(np.int64) * 19349663) \
        ^ (c.astype(np.int64) * 83492791) ^ np.int64(salt)
    h = (h ^ (h >> 13)) * np.int64(1274126177)
    return ((h ^ (h >> 16)) & 0xFFFF).astype(np.float32) / 65535.0


def _value_noise(x, y, z, scale: float, salt: int):
    """Smooth 0..1 noise, trilinear over a hash lattice.

    Smooth rather than blocky on purpose. Hashing the cell index directly is
    one line and gives hard-edged cubes of colour metres across, which reads as
    a bug rather than as mottling -- and, more to the point, would not match
    what the shader does, which is the whole reason this renderer exists.
    """
    s = max(float(scale), 1.0)
    fx, fy, fz = x / s, y / s, z / s
    ix, iy, iz = np.floor(fx), np.floor(fy), np.floor(fz)
    tx, ty, tz = fx - ix, fy - iy, fz - iz
    # Smoothstep, so the lattice grid does not show as straight seams.
    tx, ty, tz = (t * t * (3.0 - 2.0 * t) for t in (tx, ty, tz))
    out = np.zeros_like(tx, dtype=np.float32)
    for dx in (0, 1):
        wx = tx if dx else 1.0 - tx
        for dy in (0, 1):
            wy = ty if dy else 1.0 - ty
            for dz in (0, 1):
                wz = tz if dz else 1.0 - tz
                out += _hash01(ix + dx, iy + dy, iz + dz, salt) * wx * wy * wz
    return out


def _voxel_tint(xs, ys, zs, mats):
    """Per-voxel colour multiplier: near-field jitter plus far-field patches.

    Returns an (N, 3) multiplier. Hue is applied as a warm/cool tilt -- red up
    and blue down, or the reverse -- rather than a true hue rotation, because
    that is what the shader will do and matching it matters more than being
    colorimetrically proper.
    """
    n = _slots()
    jit = np.zeros(n, np.float32)
    hue = np.zeros(n, np.float32)
    pat = np.zeros(n, np.float32)
    scl = np.ones(n, np.float32)
    for m in range(n):
        if _proposed(m):
            # An animal is one smooth creature, not a granular surface. These
            # are the numbers the palette proposal asks the engine for, and they
            # sit at the quiet end of both ranges -- a flank with foliage's
            # jitter (58/255) reads as static on a twelve-voxel body.
            jit[m], hue[m], pat[m] = 14 / 255.0, 6 / 255.0, 10 / 255.0
            scl[m] = 8
            continue
        e = palette.entry(m)
        jit[m], hue[m], pat[m] = e[3] / 255.0, e[4] / 255.0, e[5] / 255.0
        scl[m] = max(e[6], 1)

    # One hash of the voxel position, shared by every face of that voxel.
    light = (_hash01(xs, ys, zs, 0x9E37) - 0.5) * 2.0
    warm = (_hash01(xs, ys, zs, 0x85EB) - 0.5) * 2.0
    # The patch term is sampled at one wavelength for the whole grid rather
    # than per material: an asset is essentially one substance, and sampling
    # per material would put a seam wherever two of them meet.
    patch = (_value_noise(xs, ys, zs, float(np.median(scl[mats])), 0x27D4) - 0.5) * 2.0

    lightness = 1.0 + light * jit[mats] + patch * pat[mats]
    tilt = warm * hue[mats]
    out = np.stack([lightness * (1.0 + 0.6 * tilt),
                    lightness,
                    lightness * (1.0 - 0.6 * tilt)], axis=1)
    return np.clip(out, 0.25, 1.9).astype(np.float32)


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


def _elevation_axes(scale: int, tilt_deg: float) -> tuple[int, int, int]:
    """Sprite half-width, top-face half-height and vertical edge, in pixels.

    The isometric preview's `a = h = 2 * scale` is not right for a low camera.
    One voxel step across the screen is `sqrt(2) * a` pixels here, because the
    camera looks down a diagonal, and one voxel of height is `h`; the preview
    gets away with `h = a` because at 35 degrees the vertical really is
    foreshortened by about that much. At 10 degrees it is not, and using the
    preview's numbers renders every stone 40% too wide -- a standing stone
    comes out looking like a slab, which is the class of wrong reading the
    elevation exists to stop.
    """
    a = 2 * scale
    tilt = max(0.0, min(44.0, float(tilt_deg)))
    h = max(1, int(round(a * math.sqrt(2.0) * math.cos(math.radians(tilt)))))
    b = int(round(h * math.tan(math.radians(tilt)) / math.sqrt(2.0)))
    return a, min(b, a), h


def _elevation_sprite(a: int, b: int, h: int):
    """`_sprite` with the three sizes free, and surviving b = 0.

    At b = 0 the top face degenerates from a rhombus to a line, which the
    preview's version divides by.
    """
    wp, hp = 2 * a, 2 * b + h
    px, py = np.meshgrid(np.arange(wp), np.arange(hp), indexing="ij")
    px = px.ravel()
    py = py.ravel()

    if b > 0:
        top = (np.abs(px - a) / a + np.abs(py - b) / b) <= 1.0 + 1e-9
        edge_right = b + (2 * a - px) * b / a
        edge_left = b + px * b / a
    else:
        top = py == 0
        edge_right = np.zeros_like(px, dtype=float)
        edge_left = np.zeros_like(px, dtype=float)

    right = (px >= a) & (py >= edge_right) & (py < edge_right + h)
    left = (px < a) & (py >= edge_left) & (py < edge_left + h)
    keep = top | left | right
    shade = np.where(top, SHADE_TOP, np.where(right, SHADE_RIGHT, SHADE_LEFT))
    return px[keep] - a, py[keep], shade[keep].astype(np.float32)


def elevation_scale_for(extents, target_px: int,
                        tilt_deg: float = ELEVATION_TILT_DEG) -> int:
    """`scale_for`, for the elevation camera.

    A low camera makes an image wider and shorter than the isometric one of the
    same asset, so the isometric's picker returns a scale that leaves a tall
    stone using a third of its cell.
    """
    nx = max(e[0] for e in extents)
    ny = max(e[1] for e in extents)
    nz = max(e[2] for e in extents)
    for s in (8, 6, 4, 3, 2, 1):
        a, b, h = _elevation_axes(s, tilt_deg)
        w = (nx + ny) * a + 2 * a
        ht = (nx + ny) * b + nz * h + 2 * b + h
        if max(w, ht) <= target_px:
            return s
    return 1


def elevation(
    grid: VoxelGrid,
    *,
    scale: int | None = None,
    target_px: int = 512,
    tilt_deg: float = ELEVATION_TILT_DEG,
    background: tuple[int, int, int, int] | None = BACKGROUND,
    ao: float = 0.45,
) -> Image.Image:
    """The same picture as `render`, from a camera a few degrees above level.

    `render` looks down a fixed 35 degree corner. That is a good angle for a
    tree and the wrong angle for the two things rocks are made of:

      - a STACK, whose subject is the pinch points where one boulder rests on
        the next. From 35 degrees the camera looks down INTO every gap and
        fills it with the boulder behind, so a heap of separate blocks reads as
        one mass. `hero-tor-stack` was called "overlapping plates" from such a
        picture; from level it plainly reads as three tiers.
      - an OVERHANG, whose subject is the space under it. A downward camera
        sees the cap and the ground and nothing of the undercut between.

    The tilt is a few degrees rather than exactly zero because at zero the top
    faces vanish and a ledge and a wall look identical; a few degrees puts a
    sliver of top face on every horizontal surface. Ten degrees can see over a
    lip only if what is behind it stands 0.18 m higher per metre of gap; the
    isometric needs 0.71 m.
    """
    data = grid.data
    if scale is None:
        scale = elevation_scale_for([grid.shape], target_px, tilt_deg)
    a, b, h = _elevation_axes(scale, tilt_deg)

    vis = _visible(data)
    xs, ys, zs = np.nonzero(vis)
    if xs.size == 0:
        return Image.new("RGBA", (16, 16), background or (0, 0, 0, 0))
    mats = data[xs, ys, zs]

    sx = (xs - ys) * a
    sy = (xs + ys) * b - zs * h
    dx, dy, shade = _elevation_sprite(a, b, h)

    x0 = int(sx.min()) - a
    y0 = int(sy.min())
    width = int(sx.max()) + a - x0 + 1
    height = int(sy.max()) + 2 * b + h - y0 + 1

    top_rgb, side_rgb = _material_faces()
    tint = _voxel_tint(xs, ys, zs, mats)
    base_top = top_rgb[mats] * tint
    base_side = side_rgb[mats] * tint
    if ao > 0.0:
        enclosed = (1.0 - ao * _occlusion(data, (xs, ys, zs)))[:, None]
        base_top = base_top * enclosed
        base_side = base_side * enclosed

    s = dx.size
    pix_x = (sx[:, None] - x0) + dx[None, :]
    pix_y = (sy[:, None] - y0) + dy[None, :]
    flat = (pix_y * width + pix_x).ravel()

    # Depth along the view direction, which moved with the camera. The
    # isometric's `x + y + z` is back-to-front only from 35 degrees; from a low
    # camera the near voxel is the one furthest along (1, 1, 2b/h) and z counts
    # for almost nothing. Left as `x + y + z`, a low elevation draws the FAR
    # side of the stone over the near side.
    depth = (xs + ys + ((2.0 * b) / h) * zs).astype(np.float32)
    depth_flat = np.repeat(depth, s)

    order = np.lexsort((-depth_flat, flat))
    flat_sorted = flat[order]
    first = np.ones(flat_sorted.shape[0], dtype=bool)
    first[1:] = flat_sorted[1:] != flat_sorted[:-1]
    chosen = order[first]
    target_pixels = flat_sorted[first]

    voxel_of = chosen // s
    sprite_of = chosen % s
    is_top = shade[sprite_of] == SHADE_TOP
    colour = np.where(is_top[:, None], base_top[voxel_of], base_side[voxel_of])
    colour = colour * shade[sprite_of][:, None]

    canvas = np.zeros((height * width, 4), dtype=np.uint8)
    if background is not None:
        canvas[:] = background
    canvas[target_pixels, :3] = np.clip(colour, 0, 255).astype(np.uint8)
    canvas[target_pixels, 3] = 255
    return Image.fromarray(canvas.reshape(height, width, 4), "RGBA")


# --- broadside: the camera a fish needs -------------------------------------

# How far above level the broadside camera sits. Shallower than the rock
# elevation's 10 degrees, because a fish's subject is entirely on its side and
# the tilt is here only to stop the top of the back and the top of the dorsal
# fin from vanishing into a single line.
BROADSIDE_TILT_DEG = 8.0

# The same camera, lifted, for an animal whose tail is HORIZONTAL.
#
# A whale's fluke lies flat, so a broadside camera sees it edge-on and the
# single feature that most says "cetacean" disappears. Measured on a 3 m
# bottlenose: the fluke is 106 of the animal's 3,593 voxels, and from dead
# broadside only EIGHT of those land on the silhouette -- 92% of it hidden.
#
# Share of the rendered picture the fluke occupies, measured by painting it a
# unique material and counting pixels:
#
#     broadside  8 deg (the fish camera)    4.7%
#     broadside 20 deg                      7.4%
#     broadside 30 deg                     10.0%
#     broadside 40 deg                     12.2%
#     side elevation 10 deg                 4.6%
#     isometric 35 deg                     12.4%
#
# The isometric shows it best and is still not chosen: it looks down a
# DIAGONAL, so it foreshortens length, and these animals are up to 25 m of
# mostly length. 30 degrees on the broadside gets within two points of the
# isometric while keeping screen-x exactly equal to body length -- which is
# also what keeps a whale and a minnow comparable on one sheet.
BROADSIDE_HIGH_TILT_DEG = 30.0


def _broadside_axes(scale: int, tilt_deg: float) -> tuple[int, int, int]:
    """Voxel width, depth-into-screen rise, and voxel height, in pixels."""
    w = 2 * scale
    tilt = max(0.0, min(44.0, float(tilt_deg)))
    h = max(1, int(round(w * math.cos(math.radians(tilt)))))
    b = max(0, int(round(w * math.sin(math.radians(tilt)))))
    return w, b, h


def broadside_scale_for(extents, target_px: int,
                        tilt_deg: float = BROADSIDE_TILT_DEG) -> int:
    nx = max(e[0] for e in extents)
    ny = max(e[1] for e in extents)
    nz = max(e[2] for e in extents)
    for s in (16, 12, 10, 8, 6, 4, 3, 2, 1):
        w, b, h = _broadside_axes(s, tilt_deg)
        if max(nx * w + w, nz * h + ny * b + b + h) <= target_px:
            return s
    return 1


def _visible_broadside(data: np.ndarray) -> np.ndarray:
    """Voxels with an empty neighbour on the -y or +z side.

    The isometric's `_visible` keeps +x, +y and +z, which is the wrong set for a
    camera looking along +y from a few degrees up: it discards precisely the
    faces this projection draws. Left as it was, a solid fish rendered as its
    far flank showing through, and the interior of a thick body was submitted
    for drawing while its near surface was not.
    """
    occ = data != 0
    exposed = np.zeros_like(occ)
    exposed[:, 1:] |= occ[:, 1:] & ~occ[:, :-1]
    exposed[:, 0] |= occ[:, 0]
    exposed[:, :, :-1] |= occ[:, :, :-1] & ~occ[:, :, 1:]
    exposed[:, :, -1] |= occ[:, :, -1]
    return exposed & occ


def broadside(
    grid: VoxelGrid,
    *,
    scale: int | None = None,
    target_px: int = 512,
    tilt_deg: float = BROADSIDE_TILT_DEG,
    background: tuple[int, int, int, int] | None = BACKGROUND,
    ao: float = 0.45,
) -> Image.Image:
    """A true side view: camera on -y, looking along +y, a few degrees up.

    The third camera here, and it exists for the same reason the second one did.
    The isometric looks down a 35 degree corner and the rock elevation looks
    along a horizontal DIAGONAL; both are three-quarter views. A fish is the
    first asset in this library that is essentially flat and has a front, and a
    three-quarter view of one lays its own length across its own width -- the
    tail fin projects over the flank, the dorsal fin leans across the back, and
    the lateral stripe, which is most of what tells one species from another
    here, crosses the silhouette at an angle instead of running along it.

    Nothing is skewed in this projection: screen x is grid x, screen y is grid
    z, and the tilt only adds a small rise for grid y. So a fish comes out as
    the picture the reference screenshot is -- which is what makes the two
    comparable, and comparing them is the point.

    The cost is that a cube has no visible left/right split, so shape comes from
    ambient occlusion and per-voxel tint rather than from face shading. On a
    body this shallow that is the right trade; on a rock it would not be, which
    is why this does not replace `elevation`.
    """
    data = grid.data
    if scale is None:
        scale = broadside_scale_for([grid.shape], target_px, tilt_deg)
    w, b, h = _broadside_axes(scale, tilt_deg)

    vis = _visible_broadside(data)
    xs, ys, zs = np.nonzero(vis)
    if xs.size == 0:
        return Image.new("RGBA", (16, 16), background or (0, 0, 0, 0))
    mats = data[xs, ys, zs]

    sx = xs * w
    sy = -zs * h - ys * b

    px, py = np.meshgrid(np.arange(w), np.arange(b + h), indexing="ij")
    dx = px.ravel()
    dy = py.ravel()
    shade = np.where(dy < b, SHADE_TOP, SHADE_RIGHT).astype(np.float32)

    x0 = int(sx.min())
    y0 = int(sy.min())
    width = int(sx.max()) + w - x0
    height = int(sy.max()) + b + h - y0

    top_rgb, side_rgb = _material_faces()
    tint = _voxel_tint(xs, ys, zs, mats)
    base_top = top_rgb[mats] * tint
    base_side = side_rgb[mats] * tint
    if ao > 0.0:
        enclosed = (1.0 - ao * _occlusion(data, (xs, ys, zs)))[:, None]
        base_top = base_top * enclosed
        base_side = base_side * enclosed

    s = dx.size
    pix_x = (sx[:, None] - x0) + dx[None, :]
    pix_y = (sy[:, None] - y0) + dy[None, :]
    flat = (pix_y * width + pix_x).ravel()

    # Nearness along the view direction (0, cos t, -sin t): a voxel is in front
    # of another if it has a smaller y, and a voxel one step UP is in front of
    # the one behind and below it, which is the pair that actually overlaps in
    # this projection -- the top face of the lower voxel lands exactly where the
    # bottom of the nearer, higher one does.
    depth = (-ys.astype(np.float32) * h + zs.astype(np.float32) * b)
    depth_flat = np.repeat(depth, s)

    order = np.lexsort((-depth_flat, flat))
    flat_sorted = flat[order]
    first = np.ones(flat_sorted.shape[0], dtype=bool)
    first[1:] = flat_sorted[1:] != flat_sorted[:-1]
    chosen = order[first]
    target_pixels = flat_sorted[first]

    voxel_of = chosen // s
    sprite_of = chosen % s
    is_top = shade[sprite_of] == SHADE_TOP
    colour = np.where(is_top[:, None], base_top[voxel_of], base_side[voxel_of])
    colour = colour * shade[sprite_of][:, None]

    canvas = np.zeros((height * width, 4), dtype=np.uint8)
    if background is not None:
        canvas[:] = background
    canvas[target_pixels, :3] = np.clip(colour, 0, 255).astype(np.uint8)
    canvas[target_pixels, 3] = 255
    return Image.fromarray(canvas.reshape(height, width, 4), "RGBA")


# --- which camera an asset wants ---------------------------------------------

CAMERAS = ("iso", "side", "broad", "broadhigh")


def camera_for(spec: dict) -> str:
    """The review camera for an asset kind. ONE definition, read by everybody.

    The gallery, the detail view, the library thumbnail and the three contact
    sheet tools all need this answer and they must give the same one. It used to
    live as `kind in pipeline.BOULDER_KINDS` in the server and `is_rock` in the
    tools, which is two copies of one rule -- this project's documented failure
    mode -- and they had already drifted: the gallery showed a rock in side
    elevation and the detail overlay showed the same rock from the isometric.

      iso    trees, bushes and ground cover. A crown is a volume seen from
             above and slightly to the side.
      side   rocks. A stack's pinch points and an overhang's undercut are
             invisible from 35 degrees; see `elevation`.
      broad  fish, and a PERCHED bird. Flat, and with a front; see
             `broadside`.
      broadhigh  whales and dolphins. The same camera lifted from 8 degrees to
             30, because a fluke lies flat and a low camera sees it edge-on --
             measured, 92% of it hidden. See `BROADSIDE_HIGH_TILT_DEG`.
      iso    ... and a FLYING bird, which is the one case where the kind alone
             does not decide it. A perched bird is a side-on animal like a
             fish. A flying one has its wings out along the camera's own axis,
             so the broadside view shows a body with two edges sticking out of
             it and hides the entire planform -- which is the thing the wing
             parameters exist to control. The isometric looks down at it and
             shows the wings.
    """
    from .spec import get

    kind = get(spec, "kind")
    if kind == "cetacean":
        return "broadhigh"
    if kind == "fish":
        # A CEPHALOFOIL LIES FLAT, EXACTLY AS A FLUKE DOES, so the same
        # argument applies: the broadside camera sits at 8 degrees and sees a
        # horizontal plate edge-on, and a hammerhead's head is the widest thing
        # on the animal and the whole reason the species is recognisable. At 8
        # degrees a 25-voxel span projects onto 3 voxels of silhouette; at the
        # high camera's 30 it projects onto 12. So a fish that authors a head
        # span is reviewed from where the head can be seen -- the same
        # exception `bird.pose` needs and for the same geometric reason.
        return "broadhigh" if float(get(spec, "fish.head_width")) > 0.0 else "broad"
    if kind == "bird":
        return "broad" if get(spec, "bird.pose") == "perched" else "iso"
    if kind == "rock":
        return "side"
    return "iso"


def view(grid: VoxelGrid, camera: str, *, scale: int | None = None,
         target_px: int = 512, tilt_deg: float | None = None,
         background=BACKGROUND, ao: float = 0.45) -> Image.Image:
    """Render through whichever camera was chosen."""
    if camera == "side":
        return elevation(grid, scale=scale, target_px=target_px,
                         tilt_deg=ELEVATION_TILT_DEG if tilt_deg is None else tilt_deg,
                         background=background, ao=ao)
    if camera in ("broad", "broadhigh"):
        default = (BROADSIDE_HIGH_TILT_DEG if camera == "broadhigh"
                   else BROADSIDE_TILT_DEG)
        return broadside(grid, scale=scale, target_px=target_px,
                         tilt_deg=default if tilt_deg is None else tilt_deg,
                         background=background, ao=ao)
    return render(grid, scale=scale, target_px=target_px, background=background, ao=ao)


def scale_for_camera(extents, camera: str, target_px: int,
                     tilt_deg: float | None = None) -> int:
    if camera == "side":
        return elevation_scale_for(
            extents, target_px,
            ELEVATION_TILT_DEG if tilt_deg is None else tilt_deg)
    if camera in ("broad", "broadhigh"):
        default = (BROADSIDE_HIGH_TILT_DEG if camera == "broadhigh"
                   else BROADSIDE_TILT_DEG)
        return broadside_scale_for(
            extents, target_px, default if tilt_deg is None else tilt_deg)
    return scale_for(extents, target_px)


def elevation_silhouette(data: np.ndarray, turns: int) -> np.ndarray:
    """What the elevation camera sees as a flat mask, after `turns` quarter
    turns of the grid. Shape is (across, z), which is what `rock.daylight`
    wants."""
    if turns:
        data = np.rot90(data, turns, axes=(0, 1))
    xs, ys, zs = np.nonzero(data != 0)
    if xs.size == 0:
        return np.zeros((1, 1), bool)
    across = (xs - ys) - int((xs - ys).min())
    sil = np.zeros((int(across.max()) + 1, int(zs.max()) + 1), bool)
    sil[across, zs] = True
    return sil


def view_score(data: np.ndarray, turns: int) -> tuple[float, float, float]:
    """How much this quarter turn SHOWS: (score, daylight share, overhang).

    A fixed camera hides the defining feature of an asset often enough that it
    has now produced three wrong readings in a row -- a stack that read as
    plates, a balanced rock that read as a lump, and an arch that the owner
    reported as having no hole at all. The arch is the clearest case: measured
    on `hero-natural-arch`, one direction is 65% open sky and the direction
    ninety degrees round is 0.1%. Both are correct pictures of the stone and
    only one of them is a picture of the ASSET.

    So the camera is chosen per asset instead of per library:

      - DAYLIGHT, the openings you can see through, from `rock.daylight`. An
        arch, a window, a natural bridge; nothing else scores here.
      - OVERHANG, the largest step up in width between a level and a level
        above it, which is the balanced rock, the hoodoo and the sea stack --
        the same quantity `tools/waistprobe.py` prints, measured on the
        silhouette instead of on the volume.

    Daylight is weighted well above overhang because a hole is a stronger
    claim than a waist and every stone has some waist.
    """
    from .rock import daylight

    sil = elevation_silhouette(data, turns)
    total = int(sil.sum())
    if total == 0:
        return 0.0, 0.0, 0.0
    day = float(daylight(sil).sum()) / total

    w = sil.sum(axis=0).astype(np.float64)
    nz = np.flatnonzero(w)
    over = 0.0
    if nz.size > 4:
        w = w[nz[0]:nz[-1] + 1]
        n = len(w)
        lo, hi = int(0.15 * n), max(int(0.15 * n) + 1, int(0.97 * n))
        for i in range(lo, hi):
            c = float(w[i:hi].max())
            over = max(over, (c - w[i]) / max(w[i], 1e-9))
    return 3.0 * day + min(over, 3.0), day, over


def best_turn(data: np.ndarray) -> int:
    """Which of the four quarter turns to show, for a one-still preview."""
    return max(range(4), key=lambda t: view_score(data, t)[0])


def turned(grid: VoxelGrid, turns: int) -> VoxelGrid:
    """The grid rotated a quarter turn at a time about the vertical.

    Rotating the grid rather than the camera keeps the projection untouched,
    and quarter turns are exact so nothing is resampled. Lives here rather than
    in the caller because three tools and the gallery all need it, and two
    copies of one rule drifting apart is this project's recurring failure.
    """
    if not turns:
        return grid
    out = VoxelGrid((1, 1, 1), voxel_m=grid.voxel_m)
    out.data = np.ascontiguousarray(np.rot90(grid.data, turns, axes=(0, 1)))
    return out


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

    if kind in ("fish", "cetacean"):
        # Nose to the end of the tail fin, and deep enough to hold the dorsal
        # and the caudal span, whichever reaches further. Getting this wrong for
        # a fish matters more than it does for a tree: it is what
        # `server.preview_resolution` reads, and a fish that looks like a 12 m
        # crown to the estimator gets previewed at the coarsest tier there is,
        # which for a 25 cm animal means nothing at all.
        length = float(get(spec, "fish.length_m"))
        depth = length * float(get(spec, "fish.depth_ratio"))
        span = length * (1.0 + float(get(spec, "fish.caudal_len"))) + 0.04
        tall = depth * max(1.0, float(get(spec, "fish.caudal_span")),
                           1.0 + float(get(spec, "fish.dorsal_height"))
                           + float(get(spec, "fish.anal_height"))) + 0.04
        wide = depth * (float(get(spec, "fish.width_ratio"))
                        + 2.0 * float(get(spec, "fish.pectoral"))) + 0.04
        # A hammerhead is wider across the HEAD than anywhere else, and by a
        # long way: 39% of body length against a body 9% wide. Estimating from
        # the body alone under-reads the grid by four times and sends the whole
        # species to a preview resolution chosen for an animal a quarter its
        # width.
        wide = max(wide, length * float(get(spec, "fish.head_width")) + 0.04)
        return (max(1, int(span / voxel_m)), max(1, int(wide / voxel_m)),
                max(1, int(tall / voxel_m)))

    if kind == "bird":
        # Bill tip to tail tip, and tall enough to hold a raised neck over a
        # dropped tail. Same warning as the fish above: this is what
        # `server.preview_resolution` reads, and a 24 cm robin that looks like
        # a 12 m crown to the estimator gets previewed at the coarsest tier
        # there is, which for a 24-voxel animal means nothing at all.
        #
        # THE WIDTH IS THE ONE THAT MATTERS HERE and it is the one a fish did
        # not have. Spreading a bird's wings multiplies its width by four and a
        # half while its body length does not move -- measured on the avian-mesh
        # rest poses, span 7.17 -> 32.31 against a body length of 20.06 -> 20.86.
        # Estimating a flying bird's extent from its length alone under-reads
        # the grid by that factor.
        length = float(get(spec, "bird.length_m"))
        body = length * float(get(spec, "bird.body_frac"))
        depth = body * float(get(spec, "bird.body_depth"))
        span = length * 1.15 + 0.04
        tall = (length * float(get(spec, "bird.neck_frac"))
                + length * float(get(spec, "bird.tail_frac")) * 0.7
                + length * float(get(spec, "bird.leg_len"))
                + depth * 2.0 + 0.06)
        wide = (length * float(get(spec, "bird.wing_span"))
                if get(spec, "bird.pose") == "flying"
                else depth * float(get(spec, "bird.body_width")) * 1.6) + 0.04
        return (max(1, int(span / voxel_m)), max(1, int(wide / voxel_m)),
                max(1, int(tall / voxel_m)))

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

    # Colour, the way the game will do it.
    #
    # This is not a preview palette any more. It reads the same table the ray
    # marcher reads, generated out of the engine header, so what a designer
    # approves here is what gets rendered in the world. See ADR-0008. Two of
    # its invariants are load-bearing and easy to break by accident:
    #
    #   - the tint is hashed from the VOXEL POSITION, never the face, so all
    #     six faces of a cube share it. Per-face hashing turns one cube into
    #     six unrelated squares.
    #   - the variation has two frequencies. The per-voxel jitter is the
    #     near-field dither; the patch term is the half that is still visible
    #     once voxels fall below a pixel and the jitter has averaged itself
    #     back to grey.
    top_rgb, side_rgb = _material_faces()
    tint = _voxel_tint(xs, ys, zs, mats)
    base_top = top_rgb[mats] * tint
    base_side = side_rgb[mats] * tint
    if ao > 0.0:
        enclosed = (1.0 - ao * _occlusion(data, (xs, ys, zs)))[:, None]
        base_top = base_top * enclosed
        base_side = base_side * enclosed

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
    # Which face of the cube this pixel is on decides which BASE COLOUR it
    # takes -- grass is green on top and soil on the sides, a cut trunk is
    # heartwood on its ends and bark on its flanks. That is a material
    # difference. `shade` is separate and is lighting, applied on top of it;
    # the palette deliberately carries no top-is-brighter bias of its own.
    is_top = shade[sprite_of] == SHADE_TOP
    colour = np.where(is_top[:, None], base_top[voxel_of], base_side[voxel_of])
    colour = colour * shade[sprite_of][:, None]

    canvas = np.zeros((height * width, 4), dtype=np.uint8)
    if background is not None:
        canvas[:] = background
    canvas[target_pixels, :3] = np.clip(colour, 0, 255).astype(np.uint8)
    canvas[target_pixels, 3] = 255

    return Image.fromarray(canvas.reshape(height, width, 4), "RGBA")
