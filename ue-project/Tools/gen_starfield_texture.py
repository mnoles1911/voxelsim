"""Generate the procedural 8192x4096 equirectangular star map (T_SkyStarmap).

    python ue-project/Tools/gen_starfield_texture.py            # 8192x4096, ~3 min
    python ue-project/Tools/gen_starfield_texture.py --preview  # 2048x1024 draft

SYSTEM Python, not the editor's. UE 5.8's bundled interpreter has neither numpy
nor Pillow, which is the same split gen_terrain_textures.py lives on: pixels are
made outside the editor, the editor only imports them
(ue-project/Tools/import_sky_textures.py).


WHY THIS EXISTS: THE GRAIN
--------------------------
The shipped star map was `starmap_2020_4k.exr` -- NASA SVS 4851, Gaia DR2
rendered to 4096x2048 equirect half-float, fetched by tools/fetch-sky-assets.ps1
and imported by import_sky_textures.py. It is a beautiful ASTRONOMICAL product
and a poor TEXTURE, for three measured reasons (measured 2026-08-09 by decoding
the EXR directly; all numbers are luminance, Rec.709 weights, linear):

  * Its dark sky is not dark, it is NOISY. In a starless patch the mean is
    0.0053 and the high-frequency standard deviation about a 4x4 box mean is
    0.0173 -- noise 3.5x the local signal. That is the grain. The night sky is
    then displayed at a FIXED deep-night exposure (DeepNightEV 13.6, see the
    VoxelSky settings line in any capture log), so that noise floor is not
    tone-mapped away; it is lifted straight onto the screen.

  * Its stars are single texels. 100609 pixels are 3x3 local maxima above 0.05
    with no point-spread function at all, so every faint star is one hard texel
    that aliases into the same static as the noise floor.

  * 4096 texels of right ascension is 5.3 arcmin per texel. At a 90 degree
    horizontal FOV on a 2560-wide frame one screen pixel is 2.1 arcmin, so each
    texel covers ~2.5 screen pixels -- the noise is MAGNIFIED, not minified,
    and mips never get a chance to average it away.

This generator answers all three: 8192x4096 (2.6 arcmin/texel, close to
one-texel-per-pixel at 2K), every star splatted with a real PSF so it lands as a
smooth round dot rather than a hard texel, and a Milky Way built from smooth
band-limited noise whose finest feature is several texels wide. There is no
random per-texel term anywhere in the output except the 1-LSB dither at the very
end.


WHAT IT IS NOT
--------------
Not an astrometric catalogue. Star POSITIONS are random (with a realistic
galactic-latitude concentration), so no constellation is where it should be.
That is a deliberate trade and it is the same trade the moon model in
VoxelEphemeris.h makes: nothing in this project can resolve the difference, and
nobody navigates by these stars. What IS faithful is everything the eye actually
judges -- the magnitude distribution, the count, the colour spread, the way the
Milky Way thickens toward the galactic centre and is cut by dust lanes, and the
band's inclination to the celestial equator (that one is exact: the galactic
frame is rotated into equatorial with the IAU 1958 pole, so the band crosses the
sky at the correct 62.9 degrees and the poles sit where they belong).


COORDINATES -- the convention is fixed by the material, not chosen here
----------------------------------------------------------------------
Tools/sky_star_graph.py's "EQUIRECT UV" section defines the lookup:

    v = 0.5 - dec/pi     ->  row 0 is dec +90 (north celestial pole) at the TOP
    u = StarRotation + StarUDirection * (-H / 2pi)

Address mode is WRAP in U and CLAMP in V, so the map must be seamless across the
u=0/1 meridian: every noise field here is generated periodic in galactic
longitude, and the galactic->equatorial warp inherits that seamlessness.

The U CONVENTION IS COPIED FROM THE MAP BEING REPLACED, and it had to be
measured because nothing in this repo records it. Measured 2026-08-09 by
correlating the NASA map's own pixels against the 25 brightest stars' J2000
positions over every (handedness, offset) at 0.25 degree resolution: the peak
scores 24.97 out of a possible 25.0 against a median of 1.80 across the search,
which is not a fit that can be argued with. The convention is

    u = (180 - RA) / 360        MIRRORED, with a 180 degree offset
    v = (90 - dec) / 180

and this generator writes the same one (see ra_from_column / column_from_ra).
It matters for exactly one reason: with the same convention this map is a DROP-IN
swap and MPC_VoxelSky's StarUDirection and StarRotation keep their current
values. With the opposite handedness the sky would render mirrored -- which for
randomly-placed stars is undetectable, but it would put the Milky Way's band at
the wrong parity against the horizon, and it would silently invalidate whatever
StarUDirection was set to when it was tuned against the NASA map.


BRIGHTNESS SCALE -- matched to the map being replaced, on purpose
-----------------------------------------------------------------
M_NightSky adds `starmap.rgb * StarBrightness` (default 1.0) to emissive, and
M_SkyAtmosphereDome feeds the same sample into the SkyLight capture through
StarAmbientGain, so the map's absolute values ARE the night sky's brightness and
part of its ambient light. Change the scale and you have silently re-graded
night and moved the starlight contribution the S2 experiment in
docs/sky-and-local-light-plan.md is calibrated against.

So the targets below are the NASA map's own measured statistics, not taste:

    peak                   1.0     (NASA max is exactly 1.0 -- the map is
                                    normalised despite being half-float)
    mean luminance      ~0.013     (NASA: 0.0129)
    99.9th percentile    ~0.42     (NASA: 0.424)

The difference is WHERE the light sits: NASA spends most of its mean on a noisy
floor, this spends it on a smooth Milky Way plus a smooth diffuse floor. Same
integrated light, no grain.


16-BIT IS LOAD-BEARING, NOT A FLOURISH
--------------------------------------
The dark-sky floor is ~0.003 linear and the brightest star is 1.0: a 300:1
range whose interesting half lives entirely in the bottom 1% of the encoding.
In 8-bit the floor is 0.8 of a code value and the entire Milky Way -- 0.003 at
its edge to 0.09 in the bulge -- gets 22 code values to render its gradients in,
which is banding, not a sky. In 16-bit the floor is 197 levels and the band has
5700. Everything this generator does to avoid grain would be undone at the last
step by an 8-bit write. Pillow cannot write
16-bit RGB PNG, so this file contains a small PNG writer (write_png16); UE's
libpng path reads 16-bit truecolour fine and fills alpha with 0xffff
(PngImageWrapper.cpp:436-446).

Output goes to tools/sky-assets/ -- the same directory the NASA sources land in,
gitignored for the same reason (this PNG is 38 MB against a repo whose largest
tracked binary is 385 KB). The reproducible artifact is THIS SCRIPT plus its
fixed seed, exactly as gen_terrain_textures.py is for the terrain PNGs.
"""

import argparse
import math
import os
import struct
import sys
import time
import zlib

import numpy as np

# ---------------------------------------------------------------------------
# Where things go
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT_DIR = os.path.join(REPO_ROOT, "tools", "sky-assets")
OUT_NAME = "starfield_procedural_8k.png"
PREVIEW_DIR = os.path.join(REPO_ROOT, "bake-out", "starfield")

# One seed, fixed. Two runs of this script must produce the same sky, or the
# "reproducible artifact is the script" claim above is a lie.
SEED = 20260809

# Rec.709 luminance weights: the same weights used to measure the NASA map, so
# the brightness targets in the docstring are comparable.
LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


# ---------------------------------------------------------------------------
# Galactic <-> equatorial
# ---------------------------------------------------------------------------
#
# IAU 1958 galactic pole, J2000: north galactic pole at RA 192.85948, Dec
# +27.12825; galactic centre at RA 266.404996, Dec -28.936175. Built as a
# rotation MATRIX rather than as the usual pair of spherical formulae because
# both directions are needed here -- stars are drawn in galactic coordinates and
# splatted in equatorial ones, the nebulosity is painted in galactic coordinates
# and warped into equatorial ones -- and one matrix with its transpose cannot
# drift apart the way two independently-typed formulae can.

def galactic_matrix():
    def unit(ra_deg, dec_deg):
        ra, dec = math.radians(ra_deg), math.radians(dec_deg)
        return np.array([math.cos(dec) * math.cos(ra),
                         math.cos(dec) * math.sin(ra),
                         math.sin(dec)], dtype=np.float64)

    z = unit(192.85948, 27.12825)      # galactic +z, toward the north pole
    x = unit(266.404996, -28.936175)   # galactic +x, toward the centre
    x = x - z * np.dot(x, z)           # orthogonalise against float error
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    m = np.stack([x, y, z])            # rows: g = M @ e
    assert abs(np.linalg.det(m) - 1.0) < 1e-9
    return m


GAL_M = galactic_matrix()


# The measured NASA convention, u = (180 - RA)/360, as the ONE pair of functions
# everything here goes through. See "COORDINATES" in the module docstring.

def ra_from_column(x_plus_half, w):
    """Column centre -> right ascension, radians."""
    return math.pi - x_plus_half * (2.0 * math.pi / w)


def column_from_ra(ra_rad, w):
    """Right ascension, radians -> fractional column."""
    return ((math.pi - ra_rad) % (2.0 * math.pi)) * (w / (2.0 * math.pi))


# ---------------------------------------------------------------------------
# Band-limited value noise, periodic in x
# ---------------------------------------------------------------------------
#
# Periodic in x because galactic longitude wraps and the map is sampled TA_WRAP
# in U. Smoothstep-interpolated value noise rather than anything fancier: the
# only property that matters here is that the result is band-limited -- its
# finest feature is one cell of the finest octave, which is kept several texels
# wide. That is what "no grain" means mechanically. A Perlin/simplex swap would
# change the look slightly and the guarantee not at all.

def value_noise(h, w, cells_y, cells_x, rng):
    grid = rng.random((cells_y + 1, cells_x), dtype=np.float32)

    gx = (np.arange(w, dtype=np.float32) + 0.5) * (cells_x / w)
    ix = np.floor(gx).astype(np.int32)
    fx = (gx - ix).astype(np.float32)
    fx = fx * fx * (3.0 - 2.0 * fx)
    ix0 = ix % cells_x
    ix1 = (ix + 1) % cells_x

    gy = (np.arange(h, dtype=np.float32) + 0.5) * (cells_y / h)
    iy = np.floor(gy).astype(np.int32)
    fy = (gy - iy).astype(np.float32)
    fy = fy * fy * (3.0 - 2.0 * fy)
    iy0 = np.clip(iy, 0, cells_y)
    iy1 = np.clip(iy + 1, 0, cells_y)

    top = grid[iy0][:, ix0] * (1.0 - fx) + grid[iy0][:, ix1] * fx
    bot = grid[iy1][:, ix0] * (1.0 - fx) + grid[iy1][:, ix1] * fx
    return top * (1.0 - fy[:, None]) + bot * fy[:, None]


def fbm(h, w, cells_y, cells_x, octaves, rng, gain=0.5):
    """Sum of value-noise octaves, normalised to [0,1].

    cells_y/cells_x are the BASE octave. Passing cells_x < 2*cells_y makes every
    feature wider in longitude than in latitude, which is how the Milky Way's
    structure is stretched along the plane without a separate warp.
    """
    total = np.zeros((h, w), dtype=np.float32)
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        total += amp * value_noise(h, w, cells_y << o, cells_x << o, rng)
        norm += amp
        amp *= gain
    return total / norm


def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ---------------------------------------------------------------------------
# Blackbody colour
# ---------------------------------------------------------------------------
#
# Planckian locus -> CIE xy (Kim et al. cubic approximation, valid 1667-25000 K)
# -> XYZ at unit luminance -> linear sRGB. Returned NORMALISED to luminance 1 so
# that a star's colour and its brightness are independent knobs: brightness
# comes from the magnitude, tint only redistributes it.

def blackbody_rgb(temp_k):
    t = np.clip(np.asarray(temp_k, dtype=np.float64), 1667.0, 25000.0)
    inv = 1000.0 / t
    x = np.where(
        t < 4000.0,
        -0.2661239 * inv ** 3 - 0.2343589 * inv ** 2 + 0.8776956 * inv + 0.179910,
        -3.0258469 * inv ** 3 + 2.1070379 * inv ** 2 + 0.2226347 * inv + 0.240390)
    y = np.where(
        t < 2222.0,
        -1.1063814 * x ** 3 - 1.34811020 * x ** 2 + 2.18555832 * x - 0.20219683,
        np.where(t < 4000.0,
                 -0.9549476 * x ** 3 - 1.37418593 * x ** 2 + 2.09137015 * x - 0.16748867,
                 3.0817580 * x ** 3 - 5.87338670 * x ** 2 + 3.75112997 * x - 0.37001483))

    yy = np.maximum(y, 1e-6)
    xyz = np.stack([x / yy, np.ones_like(x), (1.0 - x - y) / yy], axis=-1)
    m = np.array([[3.2404542, -1.5371385, -0.4985314],
                  [-0.9692660, 1.8760108, 0.0415560],
                  [0.0556434, -0.2040259, 1.0572252]])
    rgb = np.clip(xyz @ m.T, 0.0, None)
    lum = np.maximum(rgb @ LUMA.astype(np.float64), 1e-6)
    return (rgb / lum[..., None]).astype(np.float32)


def tint(rgb):
    """Normalise a hand-written tint to luminance 1, so amplitudes stay honest."""
    a = np.asarray(rgb, dtype=np.float32)
    return a / float(a @ LUMA)


# ---------------------------------------------------------------------------
# The Milky Way, painted in galactic coordinates
# ---------------------------------------------------------------------------
#
# Painted in GALACTIC coordinates and warped afterwards, rather than painted
# directly in the equatorial output. Two reasons, both structural: in galactic
# coordinates the band is a horizontal stripe, so "thickness", "dust lanes
# elongated along the plane" and "brighter toward the centre" are one-line
# functions of (l, b) instead of a warped mess; and the warp is a pure direction
# lookup, so the result is automatically continuous at both celestial poles and
# across the RA seam, which a painted-in-place band would not be.

# Single gain on the whole diffuse sky, tuned so the finished map's MEAN
# luminance lands on the NASA map's 0.0129. The mean is the number that must
# match: stars contribute ~5e-6 of it (they are bright but they cover almost no
# solid angle), so the diffuse sky alone is what the SkyLight capture integrates
# into ambient starlight through StarAmbientGain. Match the mean and night's
# ambient is unchanged; the redistribution -- out of a noisy floor and into a
# smooth Milky Way -- is the entire visible difference.
NEB_SCALE = 1.39


def build_nebula_galactic(gh, gw, rng):
    b_deg = (90.0 - (np.arange(gh, dtype=np.float32) + 0.5) * (180.0 / gh))[:, None]
    l_deg = ((np.arange(gw, dtype=np.float32) + 0.5) * (360.0 / gw))[None, :]
    # Signed longitude in [-180,180]: features like Cygnus (l=+80) and Carina
    # (l=-72) are NOT symmetric about the centre, so |l| will not do.
    ls = (l_deg + 180.0) % 360.0 - 180.0
    dl = np.abs(ls)

    def gauss(centre, sigma):
        return np.exp(-((ls - centre) ** 2) / (2.0 * sigma * sigma))

    # Scale height: the bulge is visibly fat (~10 deg), the anticentre is a thin
    # line (~3 deg).
    width = 3.2 + 7.2 * np.exp(-(dl ** 2) / (2.0 * 45.0 ** 2))

    # Surface brightness along the plane: bulge + a floor along the whole band +
    # two arm tangents where the naked-eye Milky Way is conspicuously brighter.
    amp = (0.0295 * np.exp(-(dl ** 2) / (2.0 * 58.0 ** 2))
           + 0.0115
           + 0.0085 * gauss(80.0, 16.0)     # Cygnus
           + 0.0070 * gauss(-72.0, 14.0))   # Carina

    # sech profile, not gaussian: real integrated starlight has exponential
    # wings, and a gaussian band ends too abruptly against the dark sky.
    def sech(x):
        return 1.0 / np.cosh(np.clip(x, -40.0, 40.0))

    core_shape = sech(b_deg / width)
    halo_shape = sech(b_deg / (3.6 * width))

    # POLE FADE weight, applied once to the finished field at the bottom of this
    # function. See the comment there.
    pole = 1.0 - smoothstep(58.0, 89.5, np.abs(b_deg))

    # Clumping. Base octave 24x24 cells over (180 deg, 360 deg) = features about
    # 7.5 deg tall and 15 deg wide, six octaves down to 0.23 deg -- which is 10
    # texels at 8192x4096, and 5 texels in the half-resolution galactic map this
    # is painted in. That is the floor: the finest octave must stay several
    # texels wide in the map it is BUILT in, or the bilinear upsample turns it
    # into diamond-shaped mush, and anything finer than that is grain by
    # definition. Six is where that runs out; do not add a seventh.
    clump = 0.40 + 1.25 * fbm(gh, gw, 24, 24, 6, rng)
    halo_clump = 0.55 + 0.75 * fbm(gh, gw, 8, 10, 3, rng)

    core = amp * core_shape * clump
    halo = 0.30 * amp * halo_shape * halo_clump

    # Dust lanes: absorption, so MULTIPLICATIVE, and confined to the plane --
    # dust sits in the disc, and lanes floating at b=40 would read as smudges.
    lane_field = fbm(gh, gw, 14, 12, 5, rng)
    confine = np.exp(-((b_deg / (1.5 * width)) ** 2))
    lanes = 1.0 - 0.78 * confine * smoothstep(0.42, 0.72, lane_field)

    # The Great Rift, explicitly: the naked-eye Milky Way's single most
    # recognisable feature is a dark channel running Cygnus -> Sagittarius,
    # slightly below the plane. Noise alone never produces one long coherent
    # lane, so it is drawn.
    rift_centre = -0.8 - 0.045 * np.clip(ls, -10.0, 85.0)
    rift_along = smoothstep(-14.0, 6.0, ls) * (1.0 - smoothstep(72.0, 96.0, ls))
    rift = np.exp(-((b_deg - rift_centre) ** 2) / (2.0 * 3.1 ** 2)) * rift_along
    lanes *= 1.0 - 0.62 * rift

    core *= lanes

    # Diffuse floor: what keeps a starless patch of sky from being pure black.
    # Very low frequency (8x8 cells = 22 deg features) so it can never read as
    # structure, let alone as noise.
    floor = 0.0021 + 0.0011 * fbm(gh, gw, 4, 6, 2, rng)

    # Colour. Warm cream in the bulge (reddened old population seen through
    # dust), cooler and bluer out along the arms, plus a slow per-pixel drift so
    # the band is not one flat hue.
    warm = tint((1.00, 0.905, 0.760))
    cool = tint((0.815, 0.885, 1.000))
    mixw = np.exp(-(dl ** 2) / (2.0 * 52.0 ** 2))
    mixw = np.clip(mixw + 0.22 * (fbm(gh, gw, 10, 10, 2, rng) - 0.5), 0.0, 1.0)

    out = np.empty((gh, gw, 3), dtype=np.float32)
    halo_tint = tint((0.86, 0.92, 1.00))
    floor_tint = tint((0.78, 0.87, 1.00))
    for c in range(3):
        col = warm[c] * mixw + cool[c] * (1.0 - mixw)
        out[:, :, c] = NEB_SCALE * (core * col + halo * halo_tint[c]
                                    + floor * floor_tint[c])

    # POLE FADE, and it is not cosmetic -- it is the difference between a sky
    # and a sky with two visible pinwheels in it. Everything above is a function
    # of (l, b), but at b = +/-90 every longitude IS the same direction, so any
    # residual l-dependence tears open exactly at the galactic poles. Those are
    # not obscure corners of the map: the north galactic pole warps to RA 192.9
    # / Dec +27.1, which at 52 N is overhead on spring evenings.
    #
    # The fix is to blend each row toward ITS OWN longitudinal mean as |b|
    # approaches 90. That mean is precisely the value all longitudes must agree
    # on at the pole, so the field arrives there continuous no matter what
    # produced it -- band amplitude, clumping, dust, tint, floor, or anything
    # added later. Blending only above |b| = 58 leaves the entire Milky Way
    # (|b| < 30) bit-identical.
    row_mean = out.mean(axis=1, keepdims=True)
    w = pole[:, :, None] if pole.ndim == 2 else pole[:, None, None]
    return (out * w + row_mean * (1.0 - w)).astype(np.float32)


# ---------------------------------------------------------------------------
# Galactic -> equatorial warp
# ---------------------------------------------------------------------------

def warp_to_equatorial(neb, h, w, chunk_rows=256):
    gh, gw = neb.shape[:2]
    out = np.empty((h, w, 3), dtype=np.float32)

    ra = ra_from_column(np.arange(w, dtype=np.float64) + 0.5, w)
    cos_ra, sin_ra = np.cos(ra), np.sin(ra)

    for y0 in range(0, h, chunk_rows):
        y1 = min(y0 + chunk_rows, h)
        # v = 0.5 - dec/pi  (sky_star_graph.py) inverted: row 0 is dec +90.
        dec = (math.pi * 0.5) - (np.arange(y0, y1, dtype=np.float64) + 0.5) * (math.pi / h)
        cos_dec, sin_dec = np.cos(dec)[:, None], np.sin(dec)[:, None]

        ex = cos_dec * cos_ra[None, :]
        ey = cos_dec * sin_ra[None, :]
        ez = np.broadcast_to(sin_dec, ex.shape)

        gx = GAL_M[0, 0] * ex + GAL_M[0, 1] * ey + GAL_M[0, 2] * ez
        gy = GAL_M[1, 0] * ex + GAL_M[1, 1] * ey + GAL_M[1, 2] * ez
        gz = GAL_M[2, 0] * ex + GAL_M[2, 1] * ey + GAL_M[2, 2] * ez

        u = (np.arctan2(gy, gx) / (2.0 * math.pi)) % 1.0 * gw - 0.5
        v = (0.5 - np.arcsin(np.clip(gz, -1.0, 1.0)) / math.pi) * gh - 0.5

        u0 = np.floor(u).astype(np.int32)
        fu = (u - u0).astype(np.float32)
        u0m = u0 % gw
        u1m = (u0 + 1) % gw

        v0 = np.floor(v).astype(np.int32)
        fv = (v - v0).astype(np.float32)
        v0c = np.clip(v0, 0, gh - 1)
        v1c = np.clip(v0 + 1, 0, gh - 1)

        for c in range(3):
            plane = neb[:, :, c]
            a = plane[v0c, u0m] * (1.0 - fu) + plane[v0c, u1m] * fu
            b = plane[v1c, u0m] * (1.0 - fu) + plane[v1c, u1m] * fu
            out[y0:y1, :, c] = a * (1.0 - fv) + b * fv
    return out


# ---------------------------------------------------------------------------
# Stars
# ---------------------------------------------------------------------------
#
# COUNTS. N(<m) ~ 10^(alpha*m) with alpha = 0.50, not the Euclidean 0.60: the
# real sky's counts flatten because the disc is not an infinite uniform slab,
# and 0.50 is what reproduces both anchors at once -- ~9100 stars brighter than
# magnitude 6.5 and ~15 brighter than magnitude 1. With alpha = 0.60 normalised
# to the same total you get 3 first-magnitude stars and the sky loses every
# landmark it has.
#
# BRIGHTNESS, and this is the one number that decides whether the sky reads as a
# sky. It is anchored at the FAINT end, not the bright one: the limiting
# magnitude 7.0 star peaks at FAINT_PEAK = 0.040, which is ~14x the diffuse
# floor, and everything brighter scales by 10^(-0.4*dm) through a soft
# saturation 1-exp(-flux) that asymptotes to 1.0.
#
# Anchoring at the bright end instead (brightest star = 1.0, everything else
# falling off from there) is the obvious way to do it and it is wrong: it puts a
# magnitude 6 star at 6.6e-4, which is BELOW the 0.0029 sky floor, so the entire
# faint two-thirds of the sky disappears into the background and the sky looks
# empty. The map being replaced makes the same choice this one does -- its
# maximum is exactly 1.0 (clipped) and it still has 33087 local maxima above
# 0.1, i.e. 20x its own floor.
#
# The cost of saturation is that everything brighter than about magnitude 1.5
# peaks at the same 1.0 and can no longer be told apart by peak value. That is
# what the PSF width and the halo below are for, and it is also how a real
# photograph encodes it.
#
# PSF. Two gaussians: a tight core that widens with brightness (a bright star
# must READ bigger, which is how the eye and every camera see it) and a wide,
# very faint halo on the bright ones only. No diffraction spikes -- those are a
# telescope artifact, the map being replaced has none, and a sky full of them
# reads as a lens flare texture rather than as a sky.
#
# EQUIRECT DISTORTION. A round dot on the sphere is an ELLIPSE in equirect,
# stretched in u by 1/cos(dec). Splatting circles instead would give visibly
# squashed stars over most of the sky and a smear of dots at the poles.

MAG_MIN = -1.5
MAG_MAX = 8.0
MAG_ALPHA = 0.50
# Brightness anchor, held at magnitude 7 so that extending MAG_MAX to pick up
# the sub-visual sprinkle does not silently re-scale every star in the sky.
ANCHOR_MAG = 7.0
ANCHOR_PEAK = 0.040


def draw_stars(img, n_stars, rng, h, w):
    # --- positions ---------------------------------------------------------
    # 55% belong to the disc population: galactic latitude Laplace-distributed
    # with a 13 degree scale, longitude weighted toward the centre. 45% are the
    # local/halo population, uniform on the sphere. Together that gives the
    # naked-eye sky's real behaviour -- the star density visibly follows the
    # Milky Way instead of the stars and the band being two unrelated layers.
    n_disc = int(n_stars * 0.55)
    n_iso = n_stars - n_disc

    b_disc = rng.laplace(0.0, 13.0, n_disc).astype(np.float64)
    b_disc = np.clip(b_disc, -89.9, 89.9)
    l_disc = np.where(rng.random(n_disc) < 0.45,
                      rng.normal(0.0, 62.0, n_disc),
                      rng.uniform(-180.0, 180.0, n_disc))
    l_disc = (l_disc + 180.0) % 360.0 - 180.0

    b_iso = np.degrees(np.arcsin(rng.uniform(-1.0, 1.0, n_iso)))
    l_iso = rng.uniform(-180.0, 180.0, n_iso)

    l = np.radians(np.concatenate([l_disc, l_iso]))
    b = np.radians(np.concatenate([b_disc, b_iso]))

    g = np.stack([np.cos(b) * np.cos(l), np.cos(b) * np.sin(l), np.sin(b)])
    e = GAL_M.T @ g                       # galactic -> equatorial
    dec = np.arcsin(np.clip(e[2], -1.0, 1.0))
    ra = np.arctan2(e[1], e[0]) % (2.0 * math.pi)

    # --- magnitudes --------------------------------------------------------
    span = 10.0 ** (MAG_ALPHA * (MAG_MAX - MAG_MIN)) - 1.0
    mag = MAG_MIN + np.log10(1.0 + rng.random(n_stars) * span) / MAG_ALPHA

    flux = ANCHOR_PEAK * 10.0 ** (-0.4 * (mag - ANCHOR_MAG))
    peak = 1.0 - np.exp(-flux)
    bright = np.clip((6.5 - mag) / 8.0, 0.0, 1.0)
    sigma = 0.62 + 1.15 * bright ** 2

    # --- colour ------------------------------------------------------------
    # Temperature mixture: a real naked-eye sky is dominated by G/K stars with a
    # blue B/A tail (which is over-represented at bright magnitudes, because hot
    # stars are luminous) and an orange/red giant tail.
    u = rng.random(n_stars)
    temp = np.where(u < 0.22, rng.uniform(3100.0, 4400.0, n_stars),
           np.where(u < 0.66, rng.uniform(4400.0, 6600.0, n_stars),
           np.where(u < 0.90, rng.uniform(6600.0, 10500.0, n_stars),
                              rng.uniform(10500.0, 24000.0, n_stars))))
    temp = np.where((mag < 1.5) & (rng.random(n_stars) < 0.45),
                    rng.uniform(8000.0, 22000.0, n_stars), temp)
    rgb = blackbody_rgb(temp)

    # Desaturate toward white, more for faint stars: below the eye's colour
    # threshold everything is white, and a sky of saturated confetti is the
    # single most common way a procedural starfield announces itself as fake.
    sat = (0.32 + 0.42 * bright)[:, None]
    rgb = 1.0 + (rgb - 1.0) * sat
    rgb = np.clip(rgb, 0.0, None)
    rgb /= np.maximum(rgb @ LUMA, 1e-6)[:, None]

    # --- splat -------------------------------------------------------------
    cx = column_from_ra(ra, w)
    cy = (0.5 - dec / math.pi) * h
    cos_dec = np.maximum(np.cos(dec), 0.012)          # pole guard
    halo_amp = 0.055 * bright ** 3
    halo_sigma = 4.0 + 7.0 * bright

    for i in range(n_stars):
        s_v = float(sigma[i])
        s_u = s_v / float(cos_dec[i])
        r_v = max(1, int(math.ceil(3.4 * s_v)))
        r_u = max(1, int(math.ceil(3.4 * s_u)))
        use_halo = halo_amp[i] > 1.5e-4
        if use_halo:
            hs_v = float(halo_sigma[i])
            hs_u = hs_v / float(cos_dec[i])
            r_v = max(r_v, int(math.ceil(2.6 * hs_v)))
            r_u = max(r_u, int(math.ceil(2.6 * hs_u)))
        # Cap the u-radius at half the map. Beyond that the wrapped column
        # indices would REPEAT, and a repeated index in a fancy-indexed += is
        # silently dropped rather than accumulated -- a near-pole star would
        # lose most of its light with nothing to show for it.
        r_u = min(r_u, (w - 1) // 2)

        y0 = max(0, int(cy[i]) - r_v)
        y1 = min(h, int(cy[i]) + r_v + 1)
        if y1 <= y0:
            continue
        xr = np.arange(int(cx[i]) - r_u, int(cx[i]) + r_u + 1)
        xs = xr % w

        dy = (np.arange(y0, y1) + 0.5 - cy[i])[:, None]
        dx = (xr + 0.5 - cx[i])[None, :]

        prof = np.exp(-0.5 * ((dx / s_u) ** 2 + (dy / s_v) ** 2)) * peak[i]
        if use_halo:
            prof += (np.exp(-0.5 * ((dx / hs_u) ** 2 + (dy / hs_v) ** 2))
                     * (peak[i] * halo_amp[i]))

        # img[y0:y1, xs] -- basic slice on rows, advanced index on columns, in
        # ONE subscript. Splitting it (img[y0:y1][:, xs] += ...) would add into
        # a temporary copy and write nothing.
        img[y0:y1, xs] += prof.astype(np.float32)[:, :, None] * rgb[i][None, None, :]
    return mag


# ---------------------------------------------------------------------------
# 16-bit PNG writer (Pillow cannot write 16-bit RGB)
# ---------------------------------------------------------------------------

def _chunk(f, tag, data):
    f.write(struct.pack(">I", len(data)))
    f.write(tag)
    f.write(data)
    f.write(struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png16(path, rows_iter, w, h):
    """Streaming 16-bit RGB PNG. rows_iter yields (n,w,3) uint16 blocks.

    Filter type 1 (Sub) on every row: at 16 bits the high byte of a smooth sky
    is almost always identical to its neighbour's, so Sub turns most of the file
    into runs of zeros and takes the ~200 MB of raw samples down to ~a third.
    """
    comp = zlib.compressobj(6)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        _chunk(f, b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0))
        written = 0
        for block in rows_iter:
            raw = block.astype(">u2").reshape(block.shape[0], w * 3).view(np.uint8)
            filt = raw.astype(np.int16)
            filt[:, 6:] -= raw[:, :-6]
            filt = filt.astype(np.uint8)
            lines = np.concatenate(
                [np.full((block.shape[0], 1), 1, dtype=np.uint8), filt], axis=1)
            data = comp.compress(lines.tobytes())
            if data:
                _chunk(f, b"IDAT", data)
            written += block.shape[0]
        tail = comp.flush()
        if tail:
            _chunk(f, b"IDAT", tail)
        _chunk(f, b"IEND", b"")
    assert written == h, "wrote %d of %d rows" % (written, h)


def quantise_dithered(block, rng):
    """Float -> uint16 with triangular-PDF dither at +/-1 LSB.

    Invisible at 16 bits (1.5e-5) and kept anyway: it costs nothing and it is
    the difference between a gradient that is mathematically banded and one that
    is not, which matters because the consumer is a BC6H encoder, not a display.
    """
    scaled = np.clip(block, 0.0, 1.0) * 65535.0
    d = rng.random(scaled.shape, dtype=np.float32) - rng.random(scaled.shape, dtype=np.float32)
    return np.clip(np.rint(scaled + d), 0, 65535).astype(np.uint16)


# ---------------------------------------------------------------------------
# Previews
# ---------------------------------------------------------------------------
#
# The linear values in this file are 0.002-1.0, so an honest 8-bit sRGB dump of
# it is a black rectangle. Previews therefore go through a DISPLAY TRANSFORM
# standing in for the fixed deep-night exposure the game renders it at, and the
# transform is written into CONDITIONS.txt next to them -- a preview whose
# exposure is not stated is not evidence of anything.

PREVIEW_GAIN = 6.0
PREVIEW_GAMMA = 2.2


def to_display(a):
    return np.clip((np.clip(a, 0.0, None) * PREVIEW_GAIN) ** (1.0 / PREVIEW_GAMMA), 0.0, 1.0)


def save_preview(path, arr):
    from PIL import Image
    Image.fromarray((to_display(arr) * 255.0 + 0.5).astype(np.uint8), "RGB").save(path)


def box_downsample(img, factor):
    h, w = img.shape[:2]
    return img[:h // factor * factor, :w // factor * factor].reshape(
        h // factor, factor, w // factor, factor, 3).mean(axis=(1, 3))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=8192)
    ap.add_argument("--height", type=int, default=0, help="default width/2")
    # 50,000 down to magnitude 8.0, of which ~18,000 are brighter than
    # magnitude 7 -- the naked-eye set the eye actually counts. The 32,000
    # fainter ones are the SPRINKLE: at 0.016 and below they are 2-5x the sky
    # floor, which is what fills the Milky Way in between the resolved stars.
    # They are not grain -- every one of them is a PSF-splatted dot several
    # texels wide, and the count is set by the same power law as the rest.
    ap.add_argument("--stars", type=int, default=50000)
    ap.add_argument("--preview", action="store_true",
                    help="2048x1024 draft; same maths, minutes faster")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    w = 2048 if args.preview else args.width
    h = args.height or w // 2
    n_stars = args.stars
    assert w == 2 * h, "equirect must be 2:1 (%dx%d)" % (w, h)

    out_path = args.out or os.path.join(OUT_DIR, OUT_NAME)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    os.makedirs(PREVIEW_DIR, exist_ok=True)

    rng = np.random.default_rng(SEED)
    t0 = time.time()

    # The nebulosity is built at half the output resolution and bilinearly
    # warped up. That is not a shortcut taken for speed -- it is the guarantee
    # that the diffuse sky has NO energy at the texel scale, which is exactly
    # the property the map being replaced lacks.
    gh = max(512, h // 2)
    gw = 2 * gh
    print("nebulosity %dx%d (galactic) ..." % (gw, gh), flush=True)
    neb = build_nebula_galactic(gh, gw, rng)

    print("warp -> equatorial %dx%d ..." % (w, h), flush=True)
    img = warp_to_equatorial(neb, h, w)
    del neb

    print("stars: %d ..." % n_stars, flush=True)
    mag = draw_stars(img, n_stars, rng, h, w)

    lum = img @ LUMA
    print("  peak %.4f  mean luminance %.5f  p99.9 %.4f  (NASA 4k: 1.0000 / "
          "0.01290 / 0.4237)" % (float(lum.max()), float(lum.mean()),
                                 float(np.percentile(lum, 99.9))))
    print("  stars brighter than mag 1.0: %d, mag 6.5: %d"
          % (int((mag < 1.0).sum()), int((mag < 6.5).sum())))
    print("  elapsed %.1f s" % (time.time() - t0), flush=True)

    print("writing %s ..." % out_path, flush=True)
    qrng = np.random.default_rng(SEED + 1)

    def blocks():
        for y0 in range(0, h, 256):
            yield quantise_dithered(img[y0:y0 + 256], qrng)

    write_png16(out_path, blocks(), w, h)
    print("  %.1f MB" % (os.path.getsize(out_path) / 1e6))

    # Previews: a whole-sky plate plus two 1:1 crops -- one on the Milky Way,
    # one on empty sky (that second one is the grain check, and it is the only
    # one that can fail).
    print("previews -> %s" % PREVIEW_DIR, flush=True)
    small = box_downsample(img, max(1, w // 2048))
    save_preview(os.path.join(PREVIEW_DIR, "starfield-full-2048.png"), small)

    # Galactic centre (l=0,b=0) and a deliberately empty patch at the north
    # galactic pole, both converted to pixel coordinates through the same matrix
    # the map was built with.
    for name, (gl, gb) in (("milkyway", (0.0, 0.0)), ("darksky", (0.0, 90.0))):
        v = np.array([math.cos(math.radians(gb)) * math.cos(math.radians(gl)),
                      math.cos(math.radians(gb)) * math.sin(math.radians(gl)),
                      math.sin(math.radians(gb))])
        ev = GAL_M.T @ v
        dec = math.asin(max(-1.0, min(1.0, ev[2])))
        ra = math.atan2(ev[1], ev[0]) % (2.0 * math.pi)
        cx, cy = int(column_from_ra(ra, w)), int((0.5 - dec / math.pi) * h)
        half = min(512, h // 4)
        y0 = max(0, min(h - 2 * half, cy - half))
        xs = (np.arange(cx - half, cx + half)) % w
        save_preview(os.path.join(PREVIEW_DIR, "starfield-crop-%s-1to1.png" % name),
                     img[y0:y0 + 2 * half][:, xs])

    with open(os.path.join(PREVIEW_DIR, "CONDITIONS.txt"), "w") as f:
        f.write(
            "Previews of %s (%dx%d, linear, 16-bit).\n\n"
            "DISPLAY TRANSFORM, applied to the previews ONLY -- the texture "
            "itself is linear and untouched:\n"
            "    display = clamp((linear * %.1f) ^ (1/%.1f))\n"
            "standing in for the fixed deep-night exposure the game renders "
            "this at (DeepNightEV 13.6). Without it these files are black "
            "rectangles: the dark-sky floor is 0.003 linear.\n\n"
            "  starfield-full-2048.png          whole sky, box-downsampled from "
            "%dx%d\n"
            "  starfield-crop-milkyway-1to1.png 1:1 texels, galactic centre "
            "(l=0, b=0)\n"
            "  starfield-crop-darksky-1to1.png  1:1 texels, north galactic pole "
            "-- the grain check\n\n"
            "Generated by ue-project/Tools/gen_starfield_texture.py, seed %d.\n"
            % (os.path.basename(out_path), w, h, PREVIEW_GAIN, PREVIEW_GAMMA,
               w, h, SEED))

    print("done in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())
