"""Crown envelopes: the volume a tree is allowed to grow into.

This is where biome variety actually comes from. In space colonization the
branch pattern is a consequence of where the growth targets are, so a tundra
pine, a savanna acacia and a jungle emergent are three different volumes rather
than three different pieces of code. Changing `crown.shape` changes the tree's
whole silhouette without touching the growth step at all.
"""

from __future__ import annotations

import copy
import math

import numpy as np

from .spec import _CROWN_SHAPES as _spec_shapes, get

def lame(widest: float, t_up: float, t_low: float):
    """A crown profile in the form the measurement literature actually uses.

    Foresters describe a crown by three heights -- base, widest point, top --
    and a shape exponent for the part above the widest point and another for
    the part below. Each half is a Lamé curve:

        r(v) = [ 1 - ( |v - m| / reach ) ** t ] ** (1 / t)

    where `m` is the height of maximum width. The exponent is the whole shape:
    t = 1 gives a straight-sided cone, t = 2 gives an ellipse, larger values
    approach a cylinder and smaller ones pinch inward. That is not an analogy,
    it is what the curve does, and it is why one number per half covers most of
    the range real crowns occupy.

    The reason this replaced hand-drawn profiles is that the hand-drawn conifer
    was wrong in a way nobody would notice by eye but the measurements are
    unanimous about. It was widest at the very bottom of the crown and down to
    56% of its width by mid-crown. Trees measured with laser scanners are
    widest 15-30% of the way UP the crown and are still 80-94% wide at
    mid-crown. On a 12 m tree that gap is 8 to 16 voxels of radius -- not a
    subtlety -- and two separate studies found a cone to be the worst-fitting
    crown model of every one they tried.

    One thing deliberately NOT modelled: the fitted exponents for Scots pine
    and Norway spruce differ by 0.07, which works out under one voxel of radius
    below about 12 m of tree. Species should differ here by crown length,
    radius, asymmetry and exponents far apart -- not by the second decimal of
    one, which cannot survive being turned into cubes.
    """
    def profile(v):
        v = np.clip(np.asarray(v, np.float64), 0.0, 1.0)
        up = np.clip((v - widest) / max(1.0 - widest, 1e-6), 0.0, 1.0)
        down = np.clip((widest - v) / max(widest, 1e-6), 0.0, 1.0)
        r_up = (1.0 - up ** t_up) ** (1.0 / t_up)
        r_dn = (1.0 - down ** t_low) ** (1.0 / t_low)
        return np.where(v >= widest, r_up, r_dn)
    return profile


# Radius profile of each shape as a function of v, the height through the
# crown from 0 at the bottom to 1 at the top. Returns a radius fraction in
# [0, 1] which is then scaled by crown.radius_m.
_PROFILES = {
    # round broadleaf. The Lamé form with the widest point halfway up and both
    # exponents at 2 is exactly a sphere, so this is the old profile unchanged.
    "sphere": lame(0.50, 2.0, 2.0),
    # conifer. Fitted to the beta crown profile measured by terrestrial laser
    # scanning on 86 interior-northwest conifers: widest 30% of the way up the
    # crown, still 94% wide at mid-crown, 70% at three-quarters height. The old
    # hand-drawn cone was widest at the base and 56% at mid-crown.
    "cone": lame(0.30, 1.8, 3.2),
    # a narrower, higher-shouldered conifer -- subalpine fir in the same study,
    # widest only 15% up and much slimmer throughout.
    "spire": lame(0.15, 1.5, 3.0),
    # broadleaf carrying its width high: widest two-thirds up, full below.
    "ovoid": lame(0.65, 2.2, 2.6),
    # acacia / palm: bare below, flat and wide on top
    "umbrella": lambda v: np.clip(v, 0.0, 1.0) ** 0.30,
    # poplar / cypress: a narrow column
    "column": lambda v: 0.85 + 0.15 * np.sin(math.pi * np.clip(v, 0.0, 1.0)),
    # elm / mature oak: narrow at the fork, spreading above
    "vase": lambda v: 0.30 + 0.70 * np.clip(v, 0.0, 1.0) ** 0.7,
    # wind-sheared: a rounded crown pushed hard to one side (see lean)
    "wedge": lambda v: np.sqrt(np.clip(1.0 - (2.0 * v - 1.0) ** 2, 0.0, 1.0)) ** 0.8,
    # willow: a dome on top with a curtain trailing down around its edge.
    # Widest at the very top and narrowing all the way to the bottom, so with
    # a high `shell` the targets form a hanging skirt rather than a ball --
    # which is what lets strong negative gravity produce trailing branches
    # instead of just a droopy sphere. (After vengi's tree_domehanging.)
    "hanging": lambda v: np.sqrt(np.clip(1.0 - (1.0 - v) ** 2, 0.0, 1.0)),
}

SHAPES = tuple(_PROFILES)

# Height of maximum width for each shape, as a fraction of crown length. This
# is one of the three heights a forester records for a crown -- base, widest,
# top -- and the shell thicknesses below are defined against it, because the
# upper and lower halves of a crown are filled differently and the widest point
# is the line between them. Measured rather than declared, so a profile and its
# widest point cannot disagree.
_WIDEST = {name: float(np.linspace(0.0, 1.0, 401)[
    int(np.argmax(p(np.linspace(0.0, 1.0, 401))))])
    for name, p in _PROFILES.items()}


def widest_of(shape: str) -> float:
    """Where this crown is widest, as a fraction of its length above the base."""
    return _WIDEST.get(shape, _WIDEST["sphere"])

# The list of shapes the editor offers lives in `spec`, because `spec` cannot
# import this module without a cycle. Checking them against each other here
# costs nothing and catches the failure that already happened once: two
# profiles were added, the choice list in `spec` still held the old seven, and
# every spec asking for a new one was quietly validated back to "sphere".
# Silently substituting a different crown is about the worst way for this to
# fail, because the tree still builds and still looks like a tree.
_MISSING = set(_PROFILES) ^ set(_spec_shapes)
assert not _MISSING, (
    f"crown shapes out of step between envelope.py and spec.py: {sorted(_MISSING)}")


def profile_for(shape: str):
    """Radius-vs-height profile for a crown shape.

    Shared with the whorl growth model so `crown.shape` governs the silhouette
    under either growth model rather than only under space colonization.
    """
    return _PROFILES.get(shape, _PROFILES["sphere"])


def apply_allometry(spec: dict) -> dict:
    """Derive crown size from trunk thickness, instead of setting it by hand.

    Foresters do not measure a crown radius and a crown length independently of
    the stem; they predict both FROM it, because the relationship is tight
    enough to be worth a regression. Wiring that up here means a species is
    described by how big it is and the crown follows, rather than three sliders
    that have to be kept consistent with each other by hand and quietly stop
    being consistent the moment the height changes.

    The two groups are fitted relations, in metres, against diameter at breast
    height in centimetres:

        broadleaf   radius = 0.075 d + 0.9     length = 0.150 d + 4.2
        conifer     radius = 0.045 d + 1.2     length = 0.040 d + 2.7

    The broadleaf pair sits in the middle of the per-species fits published for
    oak, beech, hornbeam, lime, sycamore and maple; the conifer pair follows
    the Scots pine and Norway spruce regressions, converted out of decimetres.
    Both are for FOREST-grown trees. An open-grown crown of the same stem runs
    10-30% wider, which is what `crown.offset` is being told separately, so if
    you author a specimen tree expect to widen it.

    Deliberately left off by default: it overrides sliders a designer may have
    set on purpose, and silently moving a control someone else set is worse
    than making them ask for it.
    """
    mode = get(spec, "crown.allometry")
    if mode in (None, "off"):
        return spec

    height = float(get(spec, "height_m"))
    # Breast height is above the flare, so the stem is thinner there than at
    # the base. The 0.85 is a taper allowance, not a measurement.
    dbh_cm = 2.0 * float(get(spec, "trunk.radius_base_m")) * 0.85 * 100.0
    if mode == "conifer":
        radius, length = 0.045 * dbh_cm + 1.2, 0.040 * dbh_cm + 2.7
    else:
        radius, length = 0.075 * dbh_cm + 0.9, 0.150 * dbh_cm + 4.2

    # A crown cannot be longer than the tree, and something has to be left for
    # a trunk.
    length = min(length, height * 0.95)
    out = copy.deepcopy(spec)
    frac = max(0.10, min(1.0, length / max(height, 1e-3)))
    _put(out, "crown.radius_m", radius)
    _put(out, "crown.height_frac", frac)
    # Sit the crown so its top reaches the top of the tree.
    squash = float(get(spec, "crown.squash"))
    _put(out, "crown.center_frac",
         max(0.20, min(0.98, 1.0 - frac * squash * 0.5)))
    return out


def _put(spec: dict, path: str, value) -> None:
    node = spec
    parts = path.split(".")
    for key in parts[:-1]:
        node = node.setdefault(key, {})
    node[parts[-1]] = value


def crown_bounds(spec: dict) -> tuple[float, float]:
    """(bottom, top) of the crown in metres above ground."""
    height = get(spec, "height_m")
    crown_h = height * get(spec, "crown.height_frac") * get(spec, "crown.squash")
    centre = height * get(spec, "crown.center_frac")
    bottom = centre - crown_h * 0.5
    return bottom, bottom + crown_h


def points(spec: dict, rng: np.random.Generator) -> np.ndarray:
    """Growth targets, (N, 3) in metres, tree base at the origin."""
    n = int(get(spec, "crown.points"))
    shape = get(spec, "crown.shape")
    profile = _PROFILES.get(shape, _PROFILES["sphere"])
    radius = get(spec, "crown.radius_m")
    bottom, top = crown_bounds(spec)
    crown_h = max(top - bottom, 1e-3)

    # Sample height weighted by cross-sectional area, so a cone gets most of
    # its targets down where it is wide instead of bunching them at the tip.
    v_cand = rng.random(n * 6)
    weight = profile(v_cand) ** 2
    wmax = float(weight.max()) if weight.size else 1.0
    keep = rng.random(v_cand.size) < (weight / max(wmax, 1e-9))
    v = v_cand[keep][:n]
    if v.size < n:  # degenerate profile; fall back to uniform
        v = np.concatenate([v, rng.random(n - v.size)])

    # Radial placement: how deep the leafy layer goes in from the crown
    # surface, given separately for the parts above and below the widest point.
    #
    # This used to be one number driving an exponent, which is a perfectly good
    # way to push targets outward but does not correspond to anything anyone
    # measures. The forestry models state it as a SHELL THICKNESS, above and
    # below the widest point, precisely because the two halves are not alike --
    # a broadleaf typically carries a solid upper crown over a lower one that
    # is nearly bare branches. An exponent cannot say that: it is symmetric in
    # height by construction.
    #
    # A value of 1 fills the crown solidly, 0.25 leaves a skin a quarter of the
    # local radius deep, and 0 empties that half of the crown entirely --
    # which is the setting a bare lower crown wants.
    m = widest_of(shape)
    up = float(get(spec, "crown.shell_upper"))
    low = float(get(spec, "crown.shell_lower"))
    thick = np.where(v >= m, up, low)

    alive = thick > 0.0
    if not alive.any():
        return np.zeros((0, 3))
    v, thick = v[alive], thick[alive]

    # Uniform by AREA within the annulus that is left, so a thick shell does
    # not quietly pile targets up against its inner wall.
    inner = np.clip(1.0 - thick, 0.0, 1.0)
    u = rng.random(v.size)
    q = np.sqrt(inner ** 2 + u * (1.0 - inner ** 2))
    theta = rng.random(v.size) * (2.0 * math.pi)
    r = profile(v) * q * radius

    # Lopsidedness. Measured crowns are not circular in plan: foresters record
    # four to eight radii around the trunk precisely because one number will not
    # do, and the asymmetry grows with age. Two or three smooth lobes stand in
    # for those radii here -- the same thing the ellipse-sector interpolation
    # between measured radii produces, without needing to carry the radii
    # around, and continuous by construction so no lobe has a seam.
    asym = float(get(spec, "crown.asymmetry"))
    if asym > 0.0:
        scale = np.ones_like(theta)
        for k in (2, 3):
            phase = rng.random() * 2.0 * math.pi
            scale += asym * (0.30 / k) * np.cos(k * theta + phase)
        r = r * scale

    x = r * np.cos(theta)
    y = r * np.sin(theta)
    z = bottom + v * crown_h

    # The crown does not sit centred over the trunk. Measured on old-growth
    # beech, the centre of a forest-grown crown is displaced from its stem by
    # about 0.37 of the crown's own mean radius, as it leans away from its
    # neighbours and into whatever gap it can reach; an isolated open-grown tree
    # manages about half that. On a crown of 2.5 m radius that is nearly a metre
    # -- about nineteen voxels, and the difference between a tree that grew
    # among others and one that was placed by a generator.
    offset = float(get(spec, "crown.offset"))
    if offset > 0.0:
        away = rng.random() * 2.0 * math.pi
        # Full displacement at the top of the crown, none at the base, because
        # the crown pivots about where it joins the trunk.
        grade = offset * radius * np.clip(v, 0.0, 1.0)
        x = x + grade * math.cos(away)
        y = y + grade * math.sin(away)

    # Wind shear: displace the crown sideways, increasingly with height.
    lean = math.radians(float(get(spec, "crown.lean_deg")))
    if lean > 0.0:
        direction = math.radians(float(get(spec, "crown.lean_dir_deg")))
        offset = np.tan(lean) * (z - bottom)
        x = x + offset * math.cos(direction)
        y = y + offset * math.sin(direction)

    pts = np.stack([x, y, z], axis=1)

    # Nothing may sit below ground; a target underground pulls branches into
    # the floor and produces a tree that looks half-buried.
    return pts[pts[:, 2] > 0.15]
