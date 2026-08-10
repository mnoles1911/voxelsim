"""Crown envelopes: the volume a tree is allowed to grow into.

This is where biome variety actually comes from. In space colonization the
branch pattern is a consequence of where the growth targets are, so a tundra
pine, a savanna acacia and a jungle emergent are three different volumes rather
than three different pieces of code. Changing `crown.shape` changes the tree's
whole silhouette without touching the growth step at all.
"""

from __future__ import annotations

import math

import numpy as np

from .spec import get

# Radius profile of each shape as a function of v, the height through the
# crown from 0 at the bottom to 1 at the top. Returns a radius fraction in
# [0, 1] which is then scaled by crown.radius_m.
_PROFILES = {
    # round broadleaf
    "sphere": lambda v: np.sqrt(np.clip(1.0 - (2.0 * v - 1.0) ** 2, 0.0, 1.0)),
    # conifer: widest at the bottom, tapering to a spire
    "cone": lambda v: np.clip(1.0 - v, 0.0, 1.0) ** 0.85,
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

    # Radial placement. `shell` pushes targets toward the crown surface, which
    # is what stops a dense crown reading as one solid blob and gives conifers
    # their layered outline.
    shell = float(get(spec, "crown.shell"))
    exponent = 2.0 + 8.0 * shell
    q = rng.random(v.size) ** (1.0 / exponent)
    theta = rng.random(v.size) * (2.0 * math.pi)
    r = profile(v) * q * radius

    x = r * np.cos(theta)
    y = r * np.sin(theta)
    z = bottom + v * crown_h

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
