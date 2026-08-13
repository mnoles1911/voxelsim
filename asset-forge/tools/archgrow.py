"""Grow an arch by stress-gated erosion, instead of cutting a hole in a lump.

WHY THIS EXISTS
---------------
The owner looked at six seeds of `hero-arch-colossal` and said they were not
natural at all. He is right, and the renders say why: every one of them is a
ribbon of roughly constant thickness bent into a hoop, standing free in the
air, with knobbly legs no thicker than the roof. Nothing in nature is shaped
like that.

A real arch is not a hoop. It is a HOLE IN A WALL. Sandstone fractures along
parallel joints into thin vertical fins; water eats an alcove into each side of
a fin at the weakest bed; the two alcoves meet and the fin is perforated. So
there is always rock ABOVE the opening, the legs widen downward into the fin,
and the fin usually carries on sideways past the arch.

`forge.rock._arch` cannot produce that, because it CUTS a parametric opening
out of a finished stone. The shape of the hole is authored, so the rock has no
say in it, and rock having a say is the entire reason real arches look the way
they do.

WHAT THE RESEARCH SAYS
----------------------
Bruthans et al., "Sandstone landforms shaped by negative feedback between
stress and erosion", Nature Geoscience 7, 597-601 (2014):
https://www.nature.com/articles/ngeo2209

In sandstone, load-bearing rock weathers SLOWER. Under vertical stress the
grains dissolve and re-precipitate at their contacts -- "fabric interlocking"
-- which drops the porosity and locks the fabric. Past a critical stress the
rock stops eroding almost entirely. So erosion removes whatever is NOT
carrying load, which pushes the load onto what is left, which locks it. The
feedback converges on the load path. That is why an arch is shaped like an
arch: it is the thrust line, exposed.

Yang, Jain, Cordonnier, Cani, Wang and Benes, "Arenite: A Physics-based
Sandstone Simulator", ACM TOG 44(4), SIGGRAPH 2025:
https://dl.acm.org/doi/10.1145/3731201

Builds the above into a simulator. Two findings matter here more than the
method does:

  * Their ablation is blunt about it -- "without considering stress, this
    method cannot create realistic arches". Which is our exact failure.
  * The initial condition for their arch is not an arch. It is "a rectangular
    cuboid posed on two disjoint supports". The arch is what is LEFT after
    everything not carrying load has gone. They also note rough initial shapes
    barely matter: "even a cube may erode into a realistic sandstone pillar".

WHAT THIS FILE DOES, AND WHAT IT DELIBERATELY DOES NOT
------------------------------------------------------
Arenite is material-point-method plus SPH plus fluvial routing on a GPU. We are
numpy on a CPU, and the colossal arch is already a 1057 M-cell grid. Running
that here is not on the table.

It does not need to be. What the ablation says is needed is that erosion be
GATED BY A STRESS FIELD THAT ROUTES LOAD AROUND THE HOLE. So that is what this
implements, and nothing else:

  * stress is a discrete load routing -- every solid voxel hands its weight to
    whatever solid sits under it, spreading sideways when nothing does;
  * erodibility is the paper's Eq. 2, a hard switch on a threshold, weak below
    and strong above;
  * the threshold is a QUANTILE of the stress actually present rather than a
    number in pascals, so it means the same thing on a 4 m boulder and a 90 m
    arch;
  * erosion only touches surface voxels, weighted by how exposed they are, with
    a directional wind term and a per-bed erodibility.

And it runs COARSE. Arenite's arch is a 128^3 grid; the shape of an arch is a
large-scale fact and does not need a 5 cm lattice to be decided. Deciding it
coarse and adding surface detail afterwards is also what stops this costing
20 minutes and 24 GB.

    python tools/archgrow.py --n 128 --seeds 4
    python tools/archgrow.py --n 96 --steps 200 --wind 0.4
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)

ROOT = Path(__file__).resolve().parents[1]


# --- stress ------------------------------------------------------------------

def _share(load: np.ndarray, solid: np.ndarray, smear: float, passes: int):
    """Let neighbouring rock at one level share the weight it is carrying.

    Without this the load falls straight down its own column and NOTHING EVER
    CONCENTRATES. Eroding the flank of a fin does not change the height of rock
    above the columns that remain, so their load is identical before and after,
    and the fraction of the rock that is locked stays pinned wherever it
    started -- measured at 0.40 of the solid, from the first step to the last,
    while the fin dissolved from 85,702 voxels to nothing.

    Rock is not a bundle of independent columns. It is stiff, so a wide mass
    above a narrowing waist puts ALL of its weight through the waist, and that
    is precisely why the waist locks before it parts. One conservative
    diffusion pass per level buys that: each cell hands a share of its load to
    its solid neighbours, the total is preserved, and load funnels into
    whatever is still holding the roof up.
    """
    if smear <= 0.0:
        return load
    for _ in range(passes):
        give = load * smear
        count = np.zeros_like(load)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            count += _shift(solid, dx, dy)
        has = count > 0
        per = np.zeros_like(load)
        per[has] = give[has] / count[has]
        got = np.zeros_like(load)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            got += _shift(np.where(has, per, 0.0), -dx, -dy)
        # A cell with no solid neighbour keeps what it was going to give away,
        # so nothing leaks out of the structure.
        load = load - np.where(has, give, 0.0) + np.where(solid, got, 0.0)
    return load


def _shift(a, dx, dy):
    """np.roll that does not wrap. A load path round the edge of the grid is
    not a load path, and the wrapped version quietly propped up whichever side
    of the fin happened to be at x=0."""
    out = np.roll(np.roll(a, dx, 0), dy, 1)
    if dx > 0:
        out[:dx] = 0
    elif dx < 0:
        out[dx:] = 0
    if dy > 0:
        out[:, :dy] = 0
    elif dy < 0:
        out[:, dy:] = 0
    return out


def load_field(occ: np.ndarray, spread: int = 3, smear: float = 0.5,
               passes: int = 2) -> np.ndarray:
    """How much weight each voxel carries, in units of one voxel's own weight.

    This is the whole mechanism, so it is worth being clear about what it is
    and is not. It is NOT a stress tensor. It is the vertical load routed down
    through the solid, which is the part of a stress field that decides an
    arch: a voxel with nothing under it has to hand its weight sideways until
    it finds something, and the places it hands it to are the legs.

    Sweeping one z-level at a time, top down. At each level the load arriving
    from above is added to the level's own weight, then passed to the level
    below. Where the level below is solid it goes straight down. Where it is
    not -- under a span, under an overhang -- it is spread laterally across the
    solid cells that ARE there, a ring at a time, up to `spread` cells away.
    That lateral hop is the only interesting line in the function: without it
    load falls into the hole and vanishes, the roof reads as carrying nothing,
    and the arch erodes from the top down like everything else.

    Anything still holding load with nowhere to put it is hanging, and gets
    zero -- it is not load-bearing, so it should not be protected.
    """
    nx, ny, nz = occ.shape
    stress = np.zeros(occ.shape, np.float32)
    carry = np.zeros((nx, ny), np.float32)

    for z in range(nz - 1, -1, -1):
        here = occ[:, :, z]
        # Arriving load only rests on solid; anything over a gap keeps falling
        # and is dealt with by the spreading below, not by hovering.
        total = np.where(here, carry + 1.0, 0.0).astype(np.float32)
        total = _share(total, here, smear, passes)
        stress[:, :, z] = total
        if z == 0:
            break

        below = occ[:, :, z - 1]
        direct = np.where(below, total, 0.0)
        loose = np.where(below, 0.0, total)

        # Spread what has nothing under it onto nearby support, one ring at a
        # time so the load reaches the closest legs first.
        ring = ((1, 0), (-1, 0), (0, 1), (0, -1),
                (1, 1), (1, -1), (-1, 1), (-1, -1))
        for _ in range(spread):
            if not loose.any():
                break
            count = np.zeros_like(loose)
            for dx, dy in ring:
                count += _shift(below.astype(np.float32), dx, dy)
            has = count > 0
            share = np.zeros_like(loose)
            share[has] = loose[has] / count[has]
            moved = np.zeros_like(loose)
            for dx, dy in ring:
                moved += _shift(np.where(has, share, 0.0), -dx, -dy)
            direct = direct + np.where(below, moved, 0.0)
            loose = np.where(below, 0.0, moved)
        carry = direct
    return stress


# --- erosion -----------------------------------------------------------------

def _exposure(occ: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Air neighbours, counted separately for the sides, the underside and the top.

    They have to be separate, and this is not a refinement -- it is the
    difference between the process working and not working at all.

    Weighting every exposed face the same eats the fin downward from its flat
    top. That removes the overburden, the overburden was the load, and without
    load nothing locks: measured, a 96-wide fin ran from 85,702 solid voxels to
    8 while the locked count fell the whole way. It is a death spiral, and it
    is also just wrong about rock. The top of a sandstone fin sheds water and
    case-hardens; what actually retreats is the FLANKS -- wind scour, spray,
    salt weathering, and the undercut at the weak bed that becomes the alcove.

    So sides are the drive, the underside is a fraction of it (spalling off a
    ceiling is real but slower), and the top is nearly immune.
    """
    def air(ax, sh):
        nb = np.roll(occ, sh, axis=ax)
        sl = [slice(None)] * 3
        sl[ax] = 0 if sh > 0 else -1
        nb[tuple(sl)] = False               # outside the grid is air
        return (~nb).astype(np.float32)

    side = air(0, 1) + air(0, -1) + air(1, 1) + air(1, -1)
    up = air(2, -1)                         # air above -> this is a top face
    down = air(2, 1)                        # air below -> this is a ceiling
    solid = occ.astype(np.float32)
    return side * solid, up * solid, down * solid


def erode(occ: np.ndarray, *, steps: int, lock_quantile: float, k_weak: float,
          k_strong: float, wind: float, wind_dir, beds: np.ndarray | None,
          rng, spread: int = 3, log=None) -> np.ndarray:
    """Run the feedback: route load, lock what carries it, wear off the rest.

    THE THRESHOLD IS SET ONCE, FROM THE STARTING SHAPE, AND THEN HELD FIXED.
    That is the whole feedback and getting it wrong destroys the mechanism
    completely, so it is worth saying why.

    `lock_quantile` picks the threshold as a quantile of the load present at
    the start, because a stress in pascals means nothing on a voxel grid and
    the load in a 90 m arch is two orders of magnitude larger than in a 4 m
    boulder purely because more voxels are stacked up. But it is evaluated ONCE
    and the resulting number is a constant from then on -- it stands in for a
    material property of the sandstone.

    Re-taking the quantile every step was tried first and it cannot work, which
    is obvious in hindsight and was not obvious in advance: a quantile locks the
    same FRACTION of whatever is left, so no matter how much rock has gone, 38%
    of it is protected and 62% is not. There is no state in which erosion stops.
    Measured: a 96-wide fin went from 85,702 solid voxels to 304 -- it ate the
    arch, then the legs, then itself.

    With the threshold fixed, removing rock raises the load on what remains,
    more of it crosses the threshold, and more of it locks. That is the
    negative feedback from Bruthans et al., and it is the only reason the
    process settles on a shape instead of running to nothing.
    """
    occ = occ.copy()
    b = np.ones(occ.shape, np.float32)      # viability, Arenite Eq. 1
    wind_dir = np.asarray(wind_dir, np.float32)
    wind_dir = wind_dir / max(float(np.linalg.norm(wind_dir)), 1e-6)

    # A fixed roughness field, so the surface wanders instead of retreating in
    # perfect shells. Regenerated per step it averages out to nothing at all --
    # that is the silent no-op this project keeps rediscovering.
    grain = rng.random(occ.shape).astype(np.float32) * 0.6 + 0.7

    quiet = 0
    thresh = None
    for step in range(steps):
        if not occ.any():
            break
        stress = load_field(occ, spread=spread)
        held = stress[occ]
        if held.size == 0:
            break
        if thresh is None:
            thresh = float(np.quantile(held, lock_quantile))
            interlocked = np.zeros(occ.shape, bool)
            if log:
                log(f"  lock threshold {thresh:.1f} (quantile {lock_quantile:.2f} "
                    f"of the starting load), held fixed from here")
        # LATCHING. Once rock has been under load it stays strong, even if the
        # load later moves elsewhere. Fabric interlocking is a change to the
        # rock -- the cement dissolves and re-precipitates at the grain
        # contacts, the porosity drops -- not a switch that flips back the
        # moment the weight shifts.
        #
        # Recomputing it fresh each step was the third and last reason this
        # would not converge. A voxel locks at step 50, stops eroding with its
        # viability already half spent, unlocks at step 300 when the load finds
        # a better path, and dies within a few steps. Measured: the locked
        # count climbed to 40,494 and then fell away to nothing while the fin
        # dissolved. With the latch it only ever grows.
        interlocked |= stress > thresh
        locked = interlocked

        side, up, down = _exposure(occ)
        surface = (side + up + down) > 0
        # Sides carry the process; a ceiling spalls more slowly; a top face is
        # nearly immune. See `_exposure` for what happens without this.
        drive = (side + 0.25 * down + 0.04 * up) / 4.0
        if wind > 0.0:
            gx, gy, gz = np.gradient(occ.astype(np.float32))
            into = -(gx * wind_dir[0] + gy * wind_dir[1] + gz * wind_dir[2])
            drive = drive * (1.0 + wind * np.clip(into, 0.0, None) * 3.0)
        drive = drive * grain
        if beds is not None:
            drive = drive * beds

        rate = np.where(locked, k_strong, k_weak).astype(np.float32)
        b -= np.where(surface, rate * drive, 0.0)
        gone = occ & (b <= 0.0)
        # NOT "stop the first time nothing comes off". Viability starts at 1
        # and one step takes about 0.05 off an exposed face, so the first
        # twenty steps remove nothing at all by construction -- the first
        # version of this bailed on step 0 and reported the input shape as the
        # result. Stop only when the shape has genuinely settled.
        if not gone.any():
            quiet += 1
            if quiet >= 40:
                if log:
                    log(f"  step {step}: settled, nothing removed for 40 steps")
                break
            continue
        quiet = 0
        occ[gone] = False
        if log and (step % 50 == 0 or step == steps - 1):
            # Daylight in the trace, not just at the end. The interesting
            # question is not what the equilibrium shape is, it is WHEN the
            # opening appears and whether it then holds -- a run that opens an
            # arch at step 300 and has dissolved it by step 800 reports the
            # same final number as one that never opened.
            px, _ = daylight_px(occ)
            log(f"  step {step:4d}  solid {int(occ.sum()):>8,}  "
                f"locked {int((locked & occ).sum()):>8,}  "
                f"removed {int(gone.sum()):>6,}  daylight {px:>6,}")
    return occ


# --- initial conditions -------------------------------------------------------

def fin_on_supports(nx: int, ny: int, nz: int, rng) -> np.ndarray:
    """A wall standing on two feet, with a gap between them. Arenite Fig. 7 row 1.

    This is the initial condition and it is most of the fix. The old carve
    started from a finished free-standing lump and cut a hole in it, which is
    why the results read as hoops. Here the rock starts as a slab -- a fin,
    which is what parallel jointing actually leaves -- resting on two separated
    supports, and the opening is not authored anywhere. It is what is left when
    the unloaded rock between the supports has gone.
    """
    occ = np.zeros((nx, ny, nz), bool)
    # The fin: thin in y, long in x, tall in z, with a wandering thickness so
    # it is not a machined plate.
    half = ny * (0.16 + 0.06 * rng.random())
    mid = ny / 2.0 + (rng.random() - 0.5) * ny * 0.05
    yy = np.arange(ny, dtype=np.float32)[None, :, None]
    xx = np.arange(nx, dtype=np.float32)[:, None, None]
    zz = np.arange(nz, dtype=np.float32)[None, None, :]
    wobble = (np.sin(xx / max(nx * 0.11, 1.0) + rng.random() * 6.28) * 0.16
              + np.sin(zz / max(nz * 0.17, 1.0) + rng.random() * 6.28) * 0.12)
    occ |= np.abs(yy - mid) <= half * (1.0 + wobble)
    # Top of the fin, below the grid ceiling so there is headroom to erode into.
    occ &= zz < nz * (0.80 + 0.12 * rng.random())

    # Two feet with a gap between them: the alcoves that will meet. Everything
    # in the gap starts unloaded, which is exactly what makes it go.
    gap = nx * (0.22 + 0.14 * rng.random())
    cx = nx / 2.0 + (rng.random() - 0.5) * nx * 0.06
    foot = nz * (0.30 + 0.12 * rng.random())
    notch = (np.abs(xx - cx) < gap * 0.5) & (zz < foot)
    # A rounded bite rather than a square one, so the first load routing has
    # somewhere sensible to send the weight.
    r = ((xx - cx) / (gap * 0.5)) ** 2 + ((zz - foot) / foot) ** 2
    occ &= ~(notch & (r < 1.4))
    return occ


def beds_field(nz: int, shape, rng, contrast: float = 6.0) -> np.ndarray:
    """Erodibility stacked in horizontal beds, per Arenite section 3.3.

    THE CONTRAST BETWEEN BEDS HAS TO BE LARGE, and the first version of this
    got that badly wrong. It varied erodibility by plus or minus 55%, which
    sounds like a lot and does nothing useful: the whole fin then thins at
    roughly the same rate everywhere, the load concentrates into what is left,
    and the rock locks solid while the opening is still the square notch it
    started as. That is exactly what came out -- a block with a hole in it.

    Real fins do not thin evenly. One bed is far weaker than its neighbours,
    water works into that bed alone, and what it cuts is an ALCOVE -- a
    recess at one height, driving inward while the beds above and below barely
    move. Two alcoves on opposite flanks meet, and that is the opening. So the
    weak bed needs to be several times weaker than the rock around it, not
    marginally weaker, and there needs to be a hard bed above it to become the
    lintel.

    `contrast` is that ratio. One bed low in the stack is always the weak one,
    because an alcove that opens near the top of a fin makes a notch rather
    than an arch.
    """
    z = np.arange(nz, dtype=np.float32)
    n = max(4, int(nz / (7.0 + 6.0 * rng.random())))
    edges = np.linspace(0, nz, n + 1)
    edges[1:-1] += (rng.random(n - 1) - 0.5) * (nz / n) * 0.5
    k = np.exp((rng.random(n) - 0.5) * 2.0 * np.log(contrast) * 0.5)

    # One bed, low down, is the weak one the alcove eats into, and the bed
    # directly above it is the hard lintel that will carry the span.
    soft = int(n * (0.18 + 0.20 * rng.random()))
    k[soft] = contrast
    if soft + 1 < n:
        k[soft + 1] = 1.0 / contrast

    band = np.ones(nz, np.float32)
    for i in range(n):
        band[(z >= edges[i]) & (z < edges[i + 1])] = k[i]
    return band[None, None, :] * np.ones(shape, np.float32)


# --- reporting ----------------------------------------------------------------

def daylight_px(occ: np.ndarray) -> tuple[int, int]:
    """Sky you can see through the hole, looking along y.

    Calls `forge.rock.daylight` rather than reimplementing it. The first
    version here did reimplement it, reported 0 px on shapes that plainly had
    an opening, and the reason is written in that function's own docstring: the
    bottom of an arch is closed by the GROUND, not by stone, so without a floor
    laid under the rock the opening drains out of the bottom of the picture and
    stops counting as enclosed.
    """
    from scipy import ndimage
    from forge import rock
    holes = rock.daylight(occ.any(axis=1))
    n = ndimage.label(holes)[1] if holes.any() else 0
    return int(holes.sum()), int(n)


def silhouette_img(occ: np.ndarray, cell: int) -> Image.Image:
    """Straight side-on projection. Deliberately not the pretty renderer: the
    question here is the outline, and a shaded render makes a bad outline look
    better than it is."""
    sil = occ.any(axis=1)
    img = np.zeros(sil.shape + (3,), np.uint8)
    img[sil] = (196, 184, 160)
    # Shade by how deep the rock is through the page, so the fin's thickness
    # reads and a paper-thin span cannot pass for a solid one.
    depth = occ.sum(axis=1).astype(np.float32)
    if depth.max() > 0:
        d = (depth / depth.max())[..., None]
        img = (img * (0.45 + 0.55 * d)).astype(np.uint8)
    im = Image.fromarray(np.transpose(img, (1, 0, 2))[::-1])
    im.thumbnail((cell, cell), Image.LANCZOS)
    return im


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=128, help="grid size on the long axis")
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--steps", type=int, default=260)
    ap.add_argument("--lock", type=float, default=0.62,
                    help="share of load-bearing rock that locks (Arenite Eq. 2 "
                         "threshold, as a quantile)")
    ap.add_argument("--weak", type=float, default=0.055)
    ap.add_argument("--strong", type=float, default=0.0035)
    ap.add_argument("--wind", type=float, default=0.35)
    ap.add_argument("--no-beds", action="store_true")
    ap.add_argument("--no-stress", action="store_true",
                    help="ablation: erode without the stress gate, which is what "
                         "the current generator effectively does")
    ap.add_argument("--out", default="archgrow.png")
    args = ap.parse_args()

    nx = args.n
    ny = max(16, int(nx * 0.42))
    nz = max(24, int(nx * 0.78))
    cell = 300
    tiles = []
    for seed in range(1, args.seeds + 1):
        rng = np.random.default_rng(seed)
        occ = fin_on_supports(nx, ny, nz, rng)
        start = int(occ.sum())
        beds = None if args.no_beds else beds_field(nz, (nx, ny, nz), rng)
        t = time.perf_counter()
        print(f"seed {seed}: {nx}x{ny}x{nz}, {start:,} solid to start", flush=True)
        occ = erode(occ, steps=args.steps,
                    lock_quantile=0.0 if args.no_stress else args.lock,
                    k_weak=args.weak,
                    k_strong=args.weak if args.no_stress else args.strong,
                    wind=args.wind, wind_dir=(0.85, 0.2, -0.35), beds=beds,
                    rng=rng, log=lambda s: print(s, flush=True))
        px, n = daylight_px(occ)
        dt = time.perf_counter() - t
        print(f"  -> {int(occ.sum()):,} solid ({100.0 * occ.sum() / max(start, 1):.0f}% "
              f"kept), daylight {px:,} px in {n} opening(s), {dt:.1f} s", flush=True)
        tiles.append((f"seed {seed}   daylight {px:,} px / {n}", silhouette_img(occ, cell)))

    cols = min(4, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * (cell + 22)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(tiles):
        cx, cy = (i % cols) * cell, (i // cols) * (cell + 22)
        sheet.paste(im, (cx + (cell - im.width) // 2, cy + (cell - im.height) // 2))
        d.text((cx + 8, cy + cell + 4), label, (200, 202, 208))
    out = ROOT / "out" / args.out
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
