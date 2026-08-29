"""Pair two arms' MOVING captures by distance travelled, and check they are of the
same ground before anybody looks at a pixel.

WHY THIS RUNS BEFORE THE EYES DO
================================

A moving A/B is only evidence if the two frames being compared are OF THE SAME
GROUND FROM THE SAME POSE. Everything about the fixture is arranged to make that
true -- a deterministic line flight from a fixed spawn at a fixed speed with the
sun pinned, and a shutter that triggers on DISTANCE TRAVELLED rather than on the
clock so a slower arm is not photographed over different hillsides. But
"arranged to be true" is not "checked", and this project has a well-worn family
of bugs whose shape is exactly that: a join computed instead of verified.

So this is the check. It reads the two legs' own logs, pairs their shots by
nominal distance, and reports which distances are actually comparable and which
are not, with the numbers behind each call.

WHAT IT DELIBERATELY DOES NOT DO
================================

IT DOES NOT LOOK AT THE PIXELS AND IT DOES NOT PRINT A VERDICT. The image
judgement belongs to the owner -- readings of this project's captures have been
wrong in both directions. This tool's entire job is to hand over a list of pairs
that are safe to compare, plus a list of the ones that are not and why.

Usage:
    python tools/voxel-pair-moving-shots.py ARM_A.log ARM_B.log
    python tools/voxel-pair-moving-shots.py a.log b.log --tol-m 2.0 --tol-deg 0.5
    python tools/voxel-pair-moving-shots.py a.log b.log --shots-dir path/to/Screenshots

Exit codes:
    0  at least one comparable pair, and the two legs flew the same fixture
    1  the two legs flew the same fixture but no distance is comparable
    2  the legs are not comparable at all (different origin/speed/sun/resolution)
"""

import argparse
import math
import os
import re
import sys

# One shot, as the engine logged it at the moment the shutter was requested.
# GREP the engine side: "VoxelPerfShot n=" in VoxelPerfRunSubsystem.cpp.
SHOT_RE = re.compile(
    r"VoxelPerfShot n=(?P<n>\d+)/(?P<of>\d+) "
    r"nominalM=(?P<nominal>-?\d+) "
    r"actualM=(?P<actual>-?[\d.]+) "
    r"residualM=(?P<residual>[+-][\d.]+) "
    r"pos=\((?P<x>-?[\d.]+), (?P<y>-?[\d.]+), (?P<z>-?[\d.]+)\) "
    r"yaw=(?P<yaw>-?[\d.]+) pitch=(?P<pitch>-?[\d.]+) "
    r"headingDeg=(?P<heading>-?[\d.]+) speedMPerSec=(?P<speed>-?[\d.]+) "
    r"pathFrame=(?P<pathframe>\d+) engineFrame=(?P<engineframe>\d+) "
    r"flightSec=(?P<flightsec>-?[\d.]+) name=(?P<name>\S+)"
)

# The width of the uncertainty on each shot's pose: FScreenshotRequest only
# raises a flag, so the drawn frame is somewhere between the request pose and
# the pose one tick later. The engine logs that gap per shot rather than
# assuming it is zero; this tool carries it through rather than hiding it.
BRACKET_RE = re.compile(
    r"VoxelPerfShotBracket nominalM=(?P<nominal>-?\d+) advancedSinceRequestM=(?P<advance>-?[\d.]+)"
)

ORIGIN_RE = re.compile(
    r"VoxelPerfRun: path centered at \((?P<x>-?[\d.]+), (?P<y>-?[\d.]+)\), height (?P<z>-?[\d.]+) UU"
)
FIXTURE_RE = re.compile(
    r"VoxelPerfRun: scripted (?P<dur>[\d.]+)s flight requested "
    r"\(flight=(?P<flight>\w+), depth=(?P<depth>[\d.]+)m, speed=(?P<speed>[\d.]+)m/s, "
    r"preflight=(?P<preflight>[\d.]+)s, linger=(?P<linger>[\d.]+)s\)"
)
SKY_RE = re.compile(
    r"VoxelPerfRun sky: tod=(?P<tod>[\d:]+) date=(?P<date>[\d-]+) "
    r"sunAlt=(?P<alt>-?[\d.]+) sunAz=(?P<az>-?[\d.]+) timeScale=(?P<scale>-?[\d.]+)"
)
VIEW_RE = re.compile(r"view=(?P<w>\d+)x(?P<h>\d+) px")
SKIP_RE = re.compile(r"VoxelPerfShot: the path stepped over (?P<n>\d+) boundary")


class Leg(object):
    """One arm's log, reduced to the facts that decide comparability."""

    def __init__(self, path):
        self.path = path
        self.label = os.path.splitext(os.path.basename(path))[0]
        self.shots = {}          # nominalM -> dict
        self.brackets = {}       # nominalM -> advance in metres
        self.origin = None       # (x, y, z) UU
        self.fixture = None      # dict from the "scripted ..." line
        self.sky = None          # dict from the "sky:" line
        self.view = None         # (w, h)
        self.skips = 0
        self.complete = False    # "VoxelPerfRun complete" -- the leg's own witness
        self.duplicate = False   # the same nominal distance logged twice = two runs in one file
        self._read()

    def _read(self):
        # errors='replace': a 40 MB UE log can carry the odd non-UTF-8 byte from
        # a third-party subsystem, and losing this whole comparison to one of
        # them would be absurd.
        with open(self.path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = SHOT_RE.search(line)
                if m:
                    d = m.groupdict()
                    nominal = int(d["nominal"])
                    # LAST ONE WINS, and that is not arbitrary: a nominal
                    # distance can only be logged twice if the run was
                    # restarted into the same file, in which case the later
                    # record is the live one. It is also reported, below,
                    # because it should not happen.
                    if nominal in self.shots:
                        self.duplicate = True
                    self.shots[nominal] = {
                        "nominal": nominal,
                        "actual": float(d["actual"]),
                        "residual": float(d["residual"]),
                        "pos": (float(d["x"]), float(d["y"]), float(d["z"])),
                        "yaw": float(d["yaw"]),
                        "pitch": float(d["pitch"]),
                        "heading": float(d["heading"]),
                        "speed": float(d["speed"]),
                        "flightsec": float(d["flightsec"]),
                        "name": d["name"],
                    }
                    continue
                m = BRACKET_RE.search(line)
                if m:
                    self.brackets[int(m.group("nominal"))] = float(m.group("advance"))
                    continue
                if self.origin is None:
                    m = ORIGIN_RE.search(line)
                    if m:
                        self.origin = (float(m.group("x")), float(m.group("y")), float(m.group("z")))
                        continue
                if self.fixture is None:
                    m = FIXTURE_RE.search(line)
                    if m:
                        self.fixture = m.groupdict()
                        continue
                m = SKY_RE.search(line)
                if m:
                    self.sky = m.groupdict()
                    continue
                if self.view is None:
                    m = VIEW_RE.search(line)
                    if m:
                        self.view = (int(m.group("w")), int(m.group("h")))
                        continue
                if SKIP_RE.search(line):
                    self.skips += 1
                    continue
                if "VoxelPerfRun complete" in line:
                    self.complete = True



def horizontal_gap_m(a, b):
    """Horizontal separation of two poses, in metres. Horizontal because the
    ground is indexed by XY -- the line flight's Z is a ground-follower and its
    vertical wander over a hillside is not a difference in WHERE the camera is
    along the path."""
    dx = a["pos"][0] - b["pos"][0]
    dy = a["pos"][1] - b["pos"][1]
    return math.hypot(dx, dy) / 100.0


def angle_gap_deg(u, v):
    """Smallest signed-magnitude difference between two angles, in degrees.
    Written out rather than a plain subtraction because yaw wraps and a
    0.0-vs-359.9 pair is a 0.1 degree disagreement, not a 359.8 one."""
    return abs((u - v + 180.0) % 360.0 - 180.0)


def find_images(shot_dirs, name):
    """Every PNG whose basename starts with the engine's shot name.

    A glob rather than an exact path because RequestScreenshot appends its own
    %05i uniqueness suffix. MORE THAN ONE MATCH IS A PROBLEM, not a detail: it
    means two runs' images for the same arm and the same distance are sitting in
    one directory, and picking either one silently compares a world from one leg
    against a world from another."""
    hits = []
    for d in shot_dirs:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.startswith(name) and fn.lower().endswith(".png"):
                hits.append(os.path.join(d, fn))
    return hits


def default_shot_dirs(log_path):
    """Saved/<leg>.log lives at the repo root; the screenshots live under
    ue-project/Saved/Screenshots/<Platform>. Both platform folders are searched
    because a -game run and an editor run do not agree on the folder name and
    this should not be one more thing to get right by hand."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(log_path)))
    base = os.path.join(root, "ue-project", "Saved", "Screenshots")
    return [os.path.join(base, "WindowsEditor"), os.path.join(base, "Windows")]


def compare_fixtures(a, b, out):
    """Everything that must match BEFORE any distance can be paired.

    THIS IS THE LOUD PART. If two legs did not fly the same path from the same
    place under the same sun at the same size, then no amount of per-shot
    tolerance rescues them and pairing the images would produce a difference
    that is entirely real and entirely about the fixture. It refuses rather than
    warns."""
    fatal = []

    if a.origin is None or b.origin is None:
        fatal.append("one or both legs never logged 'path centered at' -- the flight never initialised, "
                     "so there is no origin to measure distance from")
    elif a.origin[:2] != b.origin[:2]:
        fatal.append("DIFFERENT FLIGHT ORIGINS: %s vs %s (UU). The two arms flew from different places, "
                     "so equal distances are different ground. Nothing here is comparable."
                     % (a.origin[:2], b.origin[:2]))

    for key, why in (("flight", "different flight modes"),
                     ("speed", "different speeds"),
                     ("preflight", "different preflight windows")):
        if a.fixture and b.fixture and a.fixture[key] != b.fixture[key]:
            msg = "%s: %s=%s vs %s" % (why, key, a.fixture[key], b.fixture[key])
            # A speed difference does NOT break distance pairing -- that is the
            # whole reason the trigger is distance and not time -- so it is
            # reported and not fatal. A different flight MODE is fatal; the
            # engine refuses to arm shots on anything but 'line', so this can
            # only happen across a version boundary.
            (fatal if key == "flight" else out).append(msg)

    if a.sky and b.sky:
        for key in ("tod", "date", "scale"):
            if a.sky[key] != b.sky[key]:
                fatal.append("DIFFERENT SUN: %s=%s vs %s. Two arms lit differently differ in every pixel."
                             % (key, a.sky[key], b.sky[key]))
        if float(a.sky["scale"]) != 0.0:
            fatal.append("timeScale=%s -- THE SUN MOVED DURING THESE LEGS. Pin -VoxelTimeScale=0."
                         % a.sky["scale"])
    else:
        out.append("one or both legs logged no sun line; the sun is UNVERIFIED for this comparison")

    if a.view and b.view and a.view != b.view:
        fatal.append("DIFFERENT RENDER SIZE: %dx%d vs %dx%d. Not comparable as images."
                     % (a.view[0], a.view[1], b.view[0], b.view[1]))
    elif not a.view or not b.view:
        out.append("one or both legs logged no view= line; the render size is UNVERIFIED "
                   "(do not judge sharpness or grain from these)")

    for leg in (a, b):
        if not leg.complete:
            out.append("%s has no 'VoxelPerfRun complete' line -- that leg did not finish its flight, so its "
                       "later shots may be missing" % leg.label)
        if leg.duplicate:
            out.append("%s logged the same nominal distance twice -- the log contains more than one run" % leg.label)
        if leg.skips:
            out.append("%s stepped over %d boundary group(s); some distances are absent from its shot list"
                       % (leg.label, leg.skips))

    return fatal


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_a")
    ap.add_argument("log_b")
    ap.add_argument("--tol-m", type=float, default=1.0,
                    help="how far apart two arms' actual positions may be and still count as the same "
                         "ground, in metres (default 1.0 -- about one frame of travel at 20 m/s on a slow "
                         "arm, which is the floor this instrument can reach)")
    ap.add_argument("--tol-deg", type=float, default=0.25,
                    help="how far apart two arms' yaw/pitch may be, in degrees (default 0.25). The line "
                         "flight pins both to constants, so anything above noise here means one arm was "
                         "not flying the fixture.")
    ap.add_argument("--shots-dir", action="append", default=None,
                    help="where the PNGs are (repeatable). Default: ue-project/Saved/Screenshots/"
                         "{WindowsEditor,Windows} relative to the first log.")
    args = ap.parse_args(argv)

    a = Leg(args.log_a)
    b = Leg(args.log_b)
    shot_dirs = args.shots_dir or default_shot_dirs(args.log_a)

    notes = []
    fatal = compare_fixtures(a, b, notes)

    print("")
    print("ARM A: %s  (%d shot(s))" % (a.label, len(a.shots)))
    print("ARM B: %s  (%d shot(s))" % (b.label, len(b.shots)))
    if a.origin:
        print("origin: (%.0f, %.0f) UU   fixture: %s" % (a.origin[0], a.origin[1],
              a.fixture["flight"] if a.fixture else "?"))
    if a.sky:
        print("sun:    tod=%s date=%s timeScale=%s" % (a.sky["tod"], a.sky["date"], a.sky["scale"]))
    if a.view:
        print("view:   %dx%d px" % a.view)
    print("")

    if fatal:
        print("NOT COMPARABLE -- the two legs did not fly the same fixture:")
        for f in fatal:
            print("  * %s" % f)
        print("")
        print("Fix the fixture and re-fly. Do not compare these images.")
        return 2

    for n in notes:
        print("NOTE: %s" % n)
    if notes:
        print("")

    distances = sorted(set(a.shots) | set(b.shots))
    comparable = []
    rejected = []

    for d in distances:
        sa = a.shots.get(d)
        sb = b.shots.get(d)
        if sa is None or sb is None:
            rejected.append((d, "only %s has it" % (b.label if sa is None else a.label)))
            continue

        gap_m = horizontal_gap_m(sa, sb)
        dyaw = angle_gap_deg(sa["yaw"], sb["yaw"])
        dpitch = angle_gap_deg(sa["pitch"], sb["pitch"])
        why = []
        if gap_m > args.tol_m:
            why.append("positions %.2f m apart (> %.2f)" % (gap_m, args.tol_m))
        if dyaw > args.tol_deg:
            why.append("yaw differs by %.3f deg" % dyaw)
        if dpitch > args.tol_deg:
            why.append("pitch differs by %.3f deg" % dpitch)

        imgs_a = find_images(shot_dirs, sa["name"])
        imgs_b = find_images(shot_dirs, sb["name"])
        if len(imgs_a) != 1 or len(imgs_b) != 1:
            why.append("images on disk: %d for A, %d for B (need exactly one each)"
                       % (len(imgs_a), len(imgs_b)))

        if why:
            rejected.append((d, "; ".join(why)))
        else:
            comparable.append((d, sa, sb, imgs_a[0], imgs_b[0], gap_m))

    print("COMPARABLE (same ground, same pose, one image each):")
    if not comparable:
        print("  none")
    for d, sa, sb, ia, ib, gap_m in comparable:
        bracket = max(a.brackets.get(d, 0.0), b.brackets.get(d, 0.0))
        print("  d=%6d m   posGap=%.3f m   yaw=%.2f pitch=%.2f   shutterBracket<=%.3f m" %
              (d, gap_m, sa["yaw"], sa["pitch"], bracket))
        print("      A: %s" % ia)
        print("      B: %s" % ib)
    print("")

    if rejected:
        print("NOT COMPARABLE, per distance:")
        for d, why in rejected:
            print("  d=%6d m   %s" % (d, why))
        print("")

    # THE SHUTTER BRACKET, STATED ONCE, AS A FLOOR ON WHAT THIS CAN RESOLVE.
    #
    # FScreenshotRequest raises a flag and the viewport services it at the end of
    # a subsequent draw, so each frame was drawn somewhere between its logged
    # pose and the pose one tick later. The engine measures that gap per shot;
    # the widest one is the honest resolution limit of the whole comparison, and
    # it is worth knowing before someone reads a sub-metre parallax difference as
    # a rendering change.
    all_brackets = list(a.brackets.values()) + list(b.brackets.values())
    if all_brackets:
        print("Shutter bracket across both arms: max %.3f m, mean %.3f m. Each frame was drawn somewhere "
              "within that distance AHEAD of its logged pose, so differences smaller than this are the "
              "instrument, not the renderer." % (max(all_brackets), sum(all_brackets) / len(all_brackets)))
        print("")

    print("This tool does not open the images and does not judge them. The pixel verdict is the owner's.")
    print("")

    if not comparable:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
