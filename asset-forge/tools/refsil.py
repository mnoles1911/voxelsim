"""Reduce a SIDE SILHOUETTE to proportions, for a reference and for our own
asset, with the SAME code on both sides.

WHY A SILHOUETTE AND NOT A MESH. The obvious way to use five photographs of an
animal is a visual hull, and it is the wrong way for this library: it produces
one static blob with no part tags, no joints, no sexes and no seeds, which
trades a parametric generator for a mesh and throws away every rigging decision
in `forge/quadruped.py`. The reference is used here as a JUDGE instead. Our
generator draws the animal; this file measures the drawing and the reference the
same way; `tools/reffit.py` moves the spec until the two measurements agree.

WHY THE SAME CODE ON BOTH SIDES IS THE WHOLE DESIGN. This measurement does NOT
agree with `quadprobe.m_limb_slender` in absolute terms -- it reads about 16%
low, consistently, because `quadprobe` measures the free limb from its PART TAG
and stops where the leg enters the body, while a silhouette cannot see a part
tag and measures down from where the legs visibly separate. Measured on our own
assets, seed 1, variation pinned off:

    species            quadprobe   silhouette   error
    american-bison       0.250       0.208      -16.7%
    brown-bear           0.364       0.286      -21.4%
    plains-zebra         0.156       0.127      -18.2%
    red-deer-stag        0.163       0.174       +6.8%
    wild-boar            0.173       0.167       -3.7%
    warthog              0.217       0.200       -8.0%
    greater-kudu         0.196       0.176       -9.8%
    grey-wolf            0.192       0.147      -23.5%

The LIMB THICKNESS ITSELF agrees exactly -- 5.0 against 5.0 on the bison, 7.0
against 7.0 on the zebra, 9.0 against 9.0 on the kudu. The entire disagreement
is in the denominator, and it is a definition difference, not an error. So the
bias is REAL and it CANCELS, because a reference silhouette is measured by this
file too. Nothing in `tools/reffit.py` corrects for it and nothing should: a
correction factor would be a fudge standing where a definition belongs.

`quadprobe.py` keeps its own numbers and its own gates. This file does not
replace it and must not be used to argue with it.

WHERE THE BELLY IS, AND THE MEASUREMENT DEFECT THAT COST AN AFTERNOON. The first
version found the belly by COVERAGE -- the lowest row where the silhouette fills
half the columns a trunk fills. That is wrong on exactly the animals this
project cares most about getting right. A brown bear's legs are so thick that at
ankle height they already cover half the trunk's columns, so the coverage rule
put the belly one pixel off the ground and reported a limb FOUR TIMES AS THICK
AS IT WAS LONG. It did not crash and it did not look absurd in a table of
ratios; it simply produced a number.

The belly is a TOPOLOGY change, not a coverage threshold: below it there are two
or more separate runs of silhouette across a row (the legs), above it there is
one (the body). Counting separate runs does not care how thick the legs are, and
it fixed the bear.
"""
from __future__ import annotations

import numpy as np

# A run narrower than this fraction of the animal's length is not a leg. It is
# a tail tip, an ear, a whisker or an antler tine crossing the leg band. Set
# from the PhyloPic corpus: at 1536 px long this is 18 px, and the narrowest
# real limb measured in the set is 47.
MIN_RUN_FRAC = 0.012

# The band of the free limb that gets measured, as fractions of the belly
# height. Deliberately not the whole limb: the bottom is a hoof or a paw, which
# flares, and the top is where the limb meets the body and the silhouette is
# already thickening into the chest.
BAND_LO, BAND_HI = 0.30, 0.75


def from_png(path) -> np.ndarray:
    """A PhyloPic raster as an (x, z) mask with z up.

    PhyloPic ships LA -- luminance and alpha -- with the shape carried entirely
    by the alpha channel, so a naive read of the luminance returns a solid black
    rectangle. Checked on the corpus: every file in `refs/silhouettes` is mode
    LA and every one has a full 0-255 alpha ramp at its edges.
    """
    from PIL import Image
    a = np.array(Image.open(path).convert("LA"))
    return np.ascontiguousarray((a[..., 1] > 127).T[:, ::-1])


def from_grid(data: np.ndarray) -> np.ndarray:
    """A TRUE orthographic side view of one of our own assets.

    Not a render. `forge/render.broadside` tilts a few degrees up so a body
    reads as solid, which is right for a picture and wrong for a measurement --
    the tilt lifts the far side of the animal into the silhouette and thickens
    every limb. Flattening the grid along y is the projection a field-guide
    plate is drawn in, exactly, with no camera in it at all.
    """
    return (data != 0).any(axis=1)


def _crop(m: np.ndarray) -> np.ndarray:
    xs = np.nonzero(m.any(axis=1))[0]
    zs = np.nonzero(m.any(axis=0))[0]
    if xs.size == 0 or zs.size == 0:
        return m[:0, :0]
    return m[xs.min():xs.max() + 1, zs.min():zs.max() + 1]


def _runs(row: np.ndarray) -> list[tuple[int, int]]:
    """(start, length) of every True run, via the edges of the difference."""
    d = np.diff(np.concatenate(([0], row.view(np.int8), [0])))
    s = np.nonzero(d == 1)[0]
    e = np.nonzero(d == -1)[0]
    return list(zip(s.tolist(), (e - s).tolist()))


def _track(rows: list[list[tuple[int, int]]]) -> list[list[tuple[int, float, int]]]:
    """Follow each leg up through the measured band.

    Rows are bottom-to-top. A run continues the leg whose last centre is nearest,
    provided it is within that run's own width -- which is the loosest rule that
    still refuses to jump between a foreleg and a hind leg. Returns, per leg, a
    list of (row index, centre, width).
    """
    legs: list[list[tuple[int, float, int]]] = []
    for i, row in enumerate(rows):
        for start, width in row:
            centre = start + width / 2.0
            best, best_d = None, None
            for leg in legs:
                if leg[-1][0] == i:          # already has a run in this row
                    continue
                d = abs(leg[-1][1] - centre)
                if d <= max(width, leg[-1][2]) and (best_d is None or d < best_d):
                    best, best_d = leg, d
            if best is None:
                legs.append([(i, centre, width)])
            else:
                best.append((i, centre, width))
    # A leg seen in one row only is a tail tip or an ear clipping the band.
    return [l for l in legs if len(l) >= 3]


def measure(m: np.ndarray) -> dict | None:
    """Proportions of one side silhouette. All ratios, no units, no scale.

    Returns None when the shape has no belly line at all, which is what a
    swimming hippopotamus, a rearing bear and a head-on view all look like from
    here. `usable()` says why in words.
    """
    m = _crop(m)
    if m.size == 0:
        return None
    length, height = m.shape
    minrun = max(2, int(MIN_RUN_FRAC * length))

    nruns = np.empty(height, int)
    widest = np.empty(height, int)
    for z in range(height):
        r = _runs(m[:, z])
        nruns[z] = sum(1 for x in r if x[1] >= minrun)
        widest[z] = max((x[1] for x in r), default=0)

    # THE BELLY: the lowest row that is ONE run spanning at least half the
    # animal's length. Below it the legs are separate; at it they have joined.
    joined = np.nonzero((nruns <= 1) & (widest >= 0.5 * length))[0]
    if joined.size == 0:
        return None
    belly = int(joined.min())
    if belly < 2:
        return None

    out = {
        "length": length,
        "height": height,
        "belly": belly,
        "height_over_length": height / length,
        "belly_over_length": belly / length,
    }

    lo = int(BAND_LO * belly)
    hi = max(lo + 1, int(BAND_HI * belly))
    rows: list[list[tuple[int, int]]] = []
    for z in range(lo, hi):
        rows.append([x for x in _runs(m[:, z]) if x[1] >= minrun])
    counts = [len(r) for r in rows if r]
    if not counts:
        return None

    # A LEG RUNS THE WHOLE BAND. Anything that appears for part of it is a tail
    # tip, an ear, a dewclaw or an antler tine crossing on the diagonal, and
    # letting those count as limbs is not a small error: tracked without this
    # filter, the thickest-over-thinnest "leg" within a single silhouette
    # reached 149 on a hippopotamus and 110 on a pine marten, because a
    # twenty-pixel sliver was being averaged against a real limb. Our own assets
    # measure 1.0 to 1.4 on that same ratio, which is what two real limbs look
    # like.
    n_rows = len(rows)
    legs = [l for l in _track(rows) if len(l) >= max(3, int(0.60 * n_rows))]
    if not legs:
        return None
    # A HORIZONTAL CUT ACROSS A SLANTED LEG IS NOT ITS THICKNESS, and half the
    # reference corpus is drawn mid-stride. The chord a row cuts through a limb
    # leaning at theta from vertical is diameter/cos(theta), so a walking
    # wolverine measured 0.457 -- nearly half as thick as its leg is long --
    # against 0.235 for the same animal standing. That is not anatomy, it is
    # trigonometry, and it was about to be written into a spec file.
    #
    # So each leg is TRACKED up through the band, the slope of its centre
    # against height is fitted, and its width is projected back onto the
    # perpendicular. Our own assets stand square, so their slope is ~0 and this
    # correction does nothing to them -- which is the test that it is a fix to
    # the reference side and not a thumb on the scale for both.
    thicks, slopes = [], []
    for leg in legs:
        zs = np.array([p[0] for p in leg], float)
        cs = np.array([p[1] for p in leg], float)
        ws = np.array([p[2] for p in leg], float)
        slope = 0.0
        if zs.size >= 3 and float(zs.max() - zs.min()) > 0:
            slope = float(np.polyfit(zs, cs, 1)[0])
        slopes.append(abs(slope))
        thicks.append(float(np.median(ws)) / float(np.hypot(1.0, slope)))
    out["legs_seen"] = float(np.median(counts))
    out["legs_tracked"] = len(legs)
    out["limb_lean"] = float(np.median(slopes))
    out["limb_thick"] = float(np.median(thicks))
    out["slender"] = out["limb_thick"] / belly
    # HOW MUCH THE TWO LIMBS DISAGREE WITH EACH OTHER, which is the only signal
    # found that separates a standing animal from a SITTING one. `fisher`'s two
    # PhyloPic silhouettes are both of a sitting animal, tail along the floor;
    # they measure a limb half as thick as it is long, they AGREE WITH EACH
    # OTHER to within 21%, and no amount of sample size or cross-silhouette
    # spread would ever have caught them -- both artists drew the same pose.
    # What gives it away is inside one picture: the foreleg is a limb and the
    # hind quarter is a haunch on the ground, and they are nothing like each
    # other.
    out["leg_disagree"] = (float(max(thicks) / max(min(thicks), 1e-6))
                           if len(thicks) >= 2 else 1.0)

    cov = m.sum(axis=0) / length
    top = float(np.percentile(cov, 90))
    tr = np.nonzero(cov[belly:] >= 0.45 * top)[0]
    out["trunk_depth"] = float(tr.max() - tr.min() + 1) if tr.size else float("nan")
    out["trunk_over_length"] = out["trunk_depth"] / length
    out["back_over_length"] = (belly + out["trunk_depth"]) / length
    # HOW MUCH OF THE STANDING ANIMAL IS LEG -- the belly divided by the height
    # of the back, both measured up from the same ground line. THE ONLY STANCE
    # RATIO HERE WITH NO LENGTH IN IT, and that matters more than it sounds.
    #
    # Every ratio measured against `length` reads high by about the same amount
    # -- belly 1.26x the reference, trunk depth 1.17x, back height 1.22x -- and
    # one offset shared by three independent numerators is one defect in the
    # denominator they share, not three defects. `length` is the whole x-extent
    # of the silhouette, which is exactly what a pose moves: our generator
    # carries a tail at -35 degrees and a neck at +30, so one of our assets
    # measures 0.98 of its OWN authored head-body length across and 0.83 of
    # head-body-plus-tail, while a PhyloPic artist may draw the tail streaming
    # out behind. `leg_share` cannot be moved by either end of the animal.
    #
    # A REJECTED SECOND ATTEMPT, RECORDED SO IT IS NOT RETRIED. The obvious
    # length-free ruler is fore-foot to hind-foot centre distance, and it is
    # useless here: our assets stand square, so the left and right of a pair
    # project onto each other and 2 legs are tracked, while the reference
    # corpus is drawn mid-stride and tracks 3 to 5. Measured, our span came out
    # at 0.375 of body length against the reference's 0.742 on `wood-bison` --
    # 2.2x library-wide -- which is the stride, not the animal. Same class of
    # error as the horizontal cut across a slanted leg above.
    back = belly + out["trunk_depth"]
    out["leg_share"] = belly / back if back > 0 else float("nan")
    return out


def usable(o: dict | None) -> str | None:
    """Why this silhouette cannot be measured, or None if it can.

    EVERY BOUND HERE WAS SET BY LOOKING AT WHAT FAILED, not chosen in advance,
    and the failures are recorded in `docs/reference-fitting-research.md`. Of 54
    CC0 PhyloPic rasters fetched for fourteen species, 13 are rejected here:
    swimming hippopotamuses, rearing bears, head-on views and one silhouette of
    a wolf's head alone.

    THE H/L CEILING IS 1.70 AND THAT NUMBER HAS A SCAR ON IT. It was 1.45, and
    at 1.45 it threw away our own `red-deer-stag` -- which renders at 1.49
    BECAUSE IT HAS ANTLERS ON. A gate that silently discards the correct
    measurement of an antlered deer, on a library with eleven antlered species,
    is worse than no gate. Total height is the wrong quantity to bound and it is
    used anyway, because the thing it is really catching is a rearing or
    swimming pose; the ceiling is set high enough to clear a rack and the
    rejections are printed rather than counted.
    """
    if o is None:
        return "no belly line -- legs never separate, so not a standing lateral view"
    if not 0.35 <= o["height_over_length"] <= 1.70:
        return f"height/length {o['height_over_length']:.2f} -- not standing side-on"
    if not 0.08 <= o["belly_over_length"] <= 0.60:
        return f"belly at {o['belly_over_length']:.2f} of length -- leg gap implausible"
    if o["legs_seen"] < 2:
        return f"{o['legs_seen']:.0f} leg run -- legs merged, or not a lateral view"
    if o["slender"] > 0.75:
        return f"limb thickness/length {o['slender']:.2f} -- implausible"
    return None


def overlay(m: np.ndarray, o: dict, scale: int = 1):
    """The measurement drawn ON the silhouette, so it can be looked at.

    THIS EXISTS BECAUSE A RATIO CANNOT BE EYEBALLED. Every wrong number this
    file has produced looked perfectly reasonable in a table and obviously wrong
    the moment the belly line was drawn on the picture. `docs/` records that
    this project judges shape from renders and not from metrics; this is the
    render for this metric.
    """
    from PIL import Image
    m = _crop(m)
    h, w = m.shape[1], m.shape[0]
    img = np.zeros((h, w, 3), np.uint8)
    img[...] = 255
    body = ~m.T[::-1]
    img[body] = 235
    img[~body] = 60
    # THICK ENOUGH TO SURVIVE BEING LOOKED AT. A one-pixel rule on a 1536-pixel
    # silhouette disappears the moment the picture is scaled to fit a screen,
    # which is the only way anyone ever views it -- so the first version of this
    # overlay showed no lines at all and was useless for the one job it has.
    rule = max(1, h // 200)

    def band(zc: int, col: tuple[int, int, int]) -> None:
        a, b = max(0, zc - rule // 2), min(h, zc + rule // 2 + 1)
        img[a:b, :] = col

    band(h - 1 - o["belly"], (220, 40, 40))                 # where legs join
    for f in (BAND_LO, BAND_HI):                            # the measured band
        band(h - 1 - int(f * o["belly"]), (40, 110, 220))
    im = Image.fromarray(img)
    if scale != 1:
        im = im.resize((w * scale, h * scale), Image.NEAREST)
    return im
