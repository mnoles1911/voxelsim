#!/usr/bin/env python3
"""Assert that a starlit capture's ground/sky luminance ratio equals the ground's
albedo -- i.e. that the sky's ILLUMINATION agrees with the sky's APPEARANCE.

=========================== PROVISIONAL, 2026-07-29 ===========================
DO NOT USE THIS AS A PASS/FAIL GATE YET. It is not reproducible run-to-run.

Two captures at an IDENTICAL pose (-8448000,5376000,81772 rot 0,0,0), identical
date and hour (22:00 on 03-20), identical resolved exposure (+16.600 EV) and
identical resolved StarAmbientGain (1.0) produced:

    run 1   ratio 0.7928   snow mask  8.39% of frame   snow mean 0.11371
    run 2   ratio 0.6446   snow mask 12.62% of frame   snow mean 0.09245

That is a ~20% spread on a quantity being tested against a +/-6% band, so a FAIL
from this script currently does NOT distinguish "the calibration drifted" from
"this run streamed different terrain". The snow mask growing by half between the
two runs is the tell: the auto-detector is picking up surfaces that are not the
same set of surfaces, so it is partly measuring frame CONTENT rather than the
illuminant.

The manual measurement on a hand-picked snow band gave 0.790 against snow's
0.75-0.85 albedo, so the LIGHTING is believed correct; what is unreliable is this
script's automatic region selection.

WHAT IT NEEDS to become a gate: a deterministic reference surface instead of
auto-detected snow -- a fixture-spawned quad of known albedo at a known screen
rectangle, or a fixed pixel window with a hard requirement that streaming has
converged. Until then, read the printed ratio as an observation and diff it only
against another capture from the SAME run.
===============================================================================

THE MISTAKE THIS EXISTS TO PREVENT. The night sky's brightness and the light it
casts are produced by two different rendering contexts reading the same star map:
M_NightSky's additive dome paints the visible stars in the main view, while
M_SkyAtmosphereDome's emissive star branch is what the SkyLight's real-time
capture integrates into ambient irradiance (behind a ReflectionCapturePassSwitch).
The conversion between those two conventions is a MEASURED calibration constant,
kStarAmbientCalibration in ue-project/Source/VoxelEarth/VoxelSkySubsystem.cpp.
Nothing in the renderer checks it. If either dome's material graph changes -- the
gain chain, the horizon fade, T_SkyStarmap's encoding, M_NightSky's blend mode --
the constant is stale and the failure is a plausible image: a sky whose brightness
does not match its illumination. No error, no log line, nothing to grep for. This
script is the re-measurement, and it exits non-zero, so it can gate.

WHY A SCRIPT AND NOT AN EYEBALL, AND NOT A ONE-LINER. Three things all have to be
right at once and each one is silently wrong by default:

  1. sRGB must be decoded with the real EOTF, not a 2.2 power. Raw sRGB ratios are
     not luminance ratios; a gamma-2.2 approximation is off by several percent in
     the dark end, which is the entire tonal range of a starlit frame -- enough to
     move the answer across the albedo band this gate asserts.
  2. CLIPPED PIXELS MUST BE EXCLUDED. A clipped region cannot get brighter, so
     clipping in the sky drags the ratio TOWARD 1 -- always in the direction that
     makes a broken calibration look like snow. This script excludes any pixel with
     a channel >= 254 from both regions and reports how much it threw away.
  3. The bright region must actually be SNOW. Measure dark rock instead and the
     ratio comes back around 0.2 and reads as "the calibration is badly off" when
     the truth is "the camera was not pointed at any snow". That is the failure
     mode most likely to cost someone an afternoon, so a frame without enough
     neutral high-albedo ground is REFUSED (exit 2) rather than measured.

THE PHYSICS, which is why the ratio is the right statistic. For a Lambertian
surface of albedo rho under a uniform sky of luminance L_sky, the illuminance is
E = pi * L_sky and the surface's own luminance is L_ground = rho * E / pi = rho *
L_sky. So:

    L_ground / L_sky == rho

Every geometry and unit term cancels, and so does exposure: any EV bias, any
tonemapping gain applied equally to both regions divides out of the ratio. That is
what makes this measurable from one screenshot with no light meter, and it is why
the capture command below can use +3 EV for visibility WITHOUT the bias having to
be removed. Snow's albedo is conventionally 0.75..0.85, which is the default band.

THE REFERENCE MEASUREMENT the shipped calibration constant was fitted to (22h00
on 03-20, Milky Way core high, +3 EV):

    whole visible sky   0.14342 linear      Milky Way core   0.27833
    snowfield ground    0.11325             ground / sky     0.790   <- snow albedo

CAPTURE COMMAND that produces a valid input frame -- one process, frozen epoch and
pose, settled so the SkyLight's real-time capture has reconverged:

    UnrealEditor-Cmd.exe <uproject> -game -nosplash -unattended -sm6 -dx12 \
      -ResX=1920 -ResY=1080 "-VoxelSpawnAt=-84480,53760" -VoxelSkyLadder=1 \
      -VoxelSkyLadderStartHour=22 -VoxelSkyLadderSettle=10 -VoxelDate=03-20 \
      -ExecCmds="voxel.Sky.ExposureBias 3"

  The +3 EV is for VISIBILITY ONLY and cancels in the ratio, so leave it in -- a
  frame dark enough to be photometrically pure is also dark enough that the snow
  detector finds nothing and the whole run is refused. The capture lands in
  ue-project/Saved/Screenshots/WindowsEditor/VoxelSkyLadder_00_22h00*.png.
  -VoxelSkyLadderSettle must stay generous: an unsettled capture measures the
  PREVIOUS hour's ambient against this hour's sky, which is a wrong ratio that
  looks like a calibration drift.

Usage:
  python tools/sky-albedo-check.py <capture.png> [--albedo-min 0.75] [--albedo-max 0.85]
                                   [--sky-frac 0.35] [--ground-frac 0.50] [--json]

Exit codes: 0 = ratio inside the albedo band; 1 = outside it (the gate failed);
2 = the frame could not be measured at all (unreadable, no sky band, or not enough
snow) -- which is deliberately NOT the same answer as "the calibration is wrong".

System Python, numpy + Pillow. Same split as ue-project/Tools/gen_terrain_textures.py:
UE 5.8's bundled interpreter has neither, so anything doing real array work over a
2560x1440 frame lives out here and not under -run=pythonscript.
"""

import argparse
import json
import sys

import numpy as np
from PIL import Image

# Any channel at or above this is treated as clipped. 254 rather than 255 because
# the screenshot path is not bit-exact: TSR plus the PNG encode can land a truly
# saturated channel one code value short, and a pixel at 254 is no more trustworthy
# than one at 255 -- it just fails to look like it.
CLIP_U8 = 254

# Rec.709 / sRGB luminance weights. These are the coefficients that go with the
# sRGB PRIMARIES, so they are only correct AFTER the EOTF -- applying them to
# encoded values computes nothing physical.
LUMA_R, LUMA_G, LUMA_B = 0.2126, 0.7152, 0.0722


def srgb_eotf(u8):
    """Decode 8-bit sRGB to linear. The piecewise curve, not a 2.2 power.

    IEC 61966-2-1: linear = s/12.92 for s <= 0.04045, else ((s+0.055)/1.055)**2.4.
    The two differ most in the bottom couple of stops, which on a starlit frame is
    nearly every pixel that matters -- a 2.2 approximation reads the snowfield and
    the sky at different effective gammas and moves the ratio by percent, against a
    0.10-wide band. This is the whole reason the measurement needs code.
    """
    s = u8.astype(np.float64) / 255.0
    return np.where(s <= 0.04045, s / 12.92, ((s + 0.055) / 1.055) ** 2.4)


def find_horizon_row(luma_rows):
    """Estimate the horizon as the sharpest top-to-bottom drop in row-mean luminance.

    WHY ESTIMATE IT AT ALL: the sky band is specified as a fraction of frame height,
    which is a guess about framing. If the camera pitches down, a fraction that used
    to be sky starts including a mountain silhouette, and the sky mean falls -- which
    inflates ground/sky and reads as a HIGHER albedo. Clamping both bands to the
    detected horizon makes that self-correcting instead of silent.

    On a starlit frame the horizon is where airglow peaks and then terrain takes
    over, so the steepest negative gradient sits a few rows above the true horizon.
    That errs toward discarding real sky rather than admitting terrain, which is the
    safe direction: a slightly smaller sky region is still a sky region.

    Returns None when there is no confident horizon -- an all-sky or all-ground
    frame has no drop to find, and inventing one there would be worse than leaving
    the caller's fractions alone.
    """
    n = len(luma_rows)
    if n < 64:
        return None
    kernel = np.ones(9) / 9.0
    smooth = np.convolve(luma_rows, kernel, mode="same")
    # Ignore the outer 5%: mode="same" convolution biases the first and last few
    # rows toward zero, which is itself a fake cliff.
    lo, hi = int(n * 0.05), int(n * 0.95)
    grad = np.diff(smooth[lo:hi])
    idx = int(np.argmin(grad))
    drop = float(grad[idx])
    span = float(smooth[lo:hi].max() - smooth[lo:hi].min())
    # A real horizon is a cliff. Require the single steepest row-to-row drop to be
    # at least 2% of the whole profile's range, or call it "no horizon" -- on a
    # uniform frame argmin still returns something, and it would be noise.
    if span <= 0.0 or abs(drop) < 0.02 * span:
        return None
    return lo + idx


def region_stats(luma, mask):
    px = luma[mask]
    if px.size == 0:
        return None
    return {
        "pixels": int(px.size),
        "meanLuminance": float(px.mean()),
        "medianLuminance": float(np.median(px)),
        # The brightest tenth, reported because on a starlit sky it IS the Milky Way
        # core (0.27833 in the reference frame) and on the ground it flags a specular
        # or an emissive that should not be in a Lambertian measurement.
        "topDecileLuminance": float(np.sort(px)[int(px.size * 0.9):].mean()),
    }


def measure(path, args):
    """Returns (result_dict, exit_code)."""
    try:
        rgb = np.asarray(Image.open(path).convert("RGB"))
    except (OSError, ValueError) as exc:
        # "capture" is set on EVERY return path including this one: the harness keys
        # results by it, and a result object that omits it on the failure path is a
        # crash inside the reporter instead of the diagnosis it was asked for.
        return {"status": "UNMEASURABLE", "capture": path,
                "error": f"could not read {path}: {exc}"}, 2

    h, w, _ = rgb.shape
    linear = srgb_eotf(rgb)
    luma = LUMA_R * linear[..., 0] + LUMA_G * linear[..., 1] + LUMA_B * linear[..., 2]

    clipped = (rgb >= CLIP_U8).any(axis=2)

    # Detection runs on the ENCODED values, measurement on linear. Deliberate: the
    # saturation/value thresholds below are perceptual classifiers tuned on sRGB
    # code values, and the reference thresholds in this file's defaults were fitted
    # that way. Re-deriving them in linear would silently change what counts as snow.
    chan_max = rgb.max(axis=2).astype(np.float64)
    chan_min = rgb.min(axis=2).astype(np.float64)
    saturation = np.where(chan_max > 0, (chan_max - chan_min) / np.maximum(chan_max, 1e-9), 0.0)
    value = chan_max / 255.0

    horizon = find_horizon_row(luma.mean(axis=1))

    sky_end = int(h * args.sky_frac)
    ground_start = int(h * args.ground_frac)
    notes = []
    if horizon is not None:
        if sky_end > horizon:
            notes.append(
                f"sky band clamped from row {sky_end} to the detected horizon at {horizon}: "
                f"--sky-frac {args.sky_frac} reaches below the skyline in this frame, which "
                f"would have averaged terrain into the sky and inflated the ratio")
            sky_end = horizon
        if ground_start < horizon:
            notes.append(
                f"ground band clamped from row {ground_start} to the detected horizon at {horizon}: "
                f"--ground-frac {args.ground_frac} starts above the skyline, which would have "
                f"averaged sky into the ground and inflated the ratio")
            ground_start = horizon
    else:
        notes.append(
            "no confident horizon found (no single row-to-row drop above 2% of the profile "
            "range) -- the bands are exactly the requested fractions and were NOT verified "
            "against the skyline. Check the framing before trusting the ratio")

    sky_band = np.zeros((h, w), dtype=bool)
    sky_band[:max(sky_end, 0)] = True
    sky_mask = sky_band & ~clipped

    ground_band = np.zeros((h, w), dtype=bool)
    ground_band[min(ground_start, h):] = True
    snow_mask = ground_band & ~clipped & (saturation < args.sat_max) & (value > args.val_min)

    total = float(h * w)
    result = {
        "status": None,
        "capture": path,
        "width": int(w),
        "height": int(h),
        "horizonRow": None if horizon is None else int(horizon),
        "skyRows": [0, int(max(sky_end, 0))],
        "groundRows": [int(min(ground_start, h)), int(h)],
        "clippedFractionFrame": float(clipped.sum() / total),
        "clippedFractionSkyBand": float((sky_band & clipped).sum() / max(sky_band.sum(), 1)),
        "clippedFractionGroundBand": float((ground_band & clipped).sum() / max(ground_band.sum(), 1)),
        "snowFractionFrame": float(snow_mask.sum() / total),
        "expectedAlbedoMin": args.albedo_min,
        "expectedAlbedoMax": args.albedo_max,
        "notes": notes,
    }

    sky = region_stats(luma, sky_mask)
    if sky is None or sky["pixels"] < args.min_sky_px:
        result["status"] = "UNMEASURABLE"
        result["error"] = (
            f"only {0 if sky is None else sky['pixels']} unclipped sky pixels in rows "
            f"0..{max(sky_end, 0)} (need {args.min_sky_px}). Either the frame is not a sky "
            f"capture, or the sky is clipped -- check clippedFractionSkyBand="
            f"{result['clippedFractionSkyBand']:.4f} and drop the exposure bias if it is high.")
        return result, 2
    result["sky"] = sky

    # THE LOUD REFUSAL. Below this count the "snow" is a handful of bright edge
    # pixels -- a moonlit rock rim, a star reflected in ice, the horizon glow
    # bleeding one row past the clamp -- and their mean says nothing about albedo.
    # Measuring it anyway is how a wrong ratio gets written into a measurements file
    # and believed. The reference frame is 8.4% snow; a starlit frame with the
    # ambient term switched off is 0.2%, and that is the case this catches.
    min_snow_px = int(total * args.min_snow_frac)
    snow = region_stats(luma, snow_mask)
    if snow is None or snow["pixels"] < min_snow_px:
        result["status"] = "UNMEASURABLE"
        result["error"] = (
            f"NO SNOW IN THIS FRAME. Found {0 if snow is None else snow['pixels']} neutral "
            f"high-albedo ground pixels (saturation < {args.sat_max}, value > {args.val_min}) "
            f"in rows {min(ground_start, h)}..{h}, need {min_snow_px} "
            f"(--min-snow-frac {args.min_snow_frac} of the frame). This gate compares ground "
            f"luminance against SNOW's albedo, so without snow there is nothing to compare: "
            f"measuring the dark rock that is there would report a ratio near 0.2 and read as "
            f"'the star calibration is broken' when the truth is 'the camera was not pointed at "
            f"snow'. Fix the framing (-VoxelSpawnAt=-84480,53760 is the snowfield the reference "
            f"was measured at), or -- if the ground really is black -- check whether "
            f"voxel.Sky.StarAmbientGain was overridden to 0 on this run; the run log states the "
            f"resolved gain and whether it was DERIVED or OVERRIDDEN.")
        return result, 2
    result["snow"] = snow

    ratio = snow["meanLuminance"] / sky["meanLuminance"]
    result["groundOverSky"] = float(ratio)
    result["impliedAlbedo"] = float(ratio)
    passed = args.albedo_min <= ratio <= args.albedo_max
    result["status"] = "PASS" if passed else "FAIL"
    if not passed:
        result["error"] = (
            f"ground/sky = {ratio:.4f}, outside the expected albedo band "
            f"[{args.albedo_min}, {args.albedo_max}]. "
            + ("Below the band means the ground is receiving LESS light than a sky that bright "
               "should cast -- the capture branch's gain is too low relative to the visible "
               "dome's."
               if ratio < args.albedo_min else
               "Above the band means the ground is receiving MORE light than a sky that bright "
               "should cast -- the capture branch's gain is too high, or a non-Lambertian "
               "surface (specular ice, an emissive) is inside the ground region; check "
               "snow.topDecileLuminance against snow.medianLuminance.")
            + " Either way the calibration constant kStarAmbientCalibration in "
              "ue-project/Source/VoxelEarth/VoxelSkySubsystem.cpp no longer matches the "
              "materials and must be re-fitted, NOT worked around with "
              "voxel.Sky.StarAmbientGain.")
    return result, (0 if passed else 1)


def report(result, code):
    print(f"sky-albedo-check: {result['capture']}")
    if "width" in result:
        print(f"  frame            {result['width']}x{result['height']}"
              f"   horizon row {result['horizonRow']}")
        print(f"  sky rows         {result['skyRows'][0]}..{result['skyRows'][1]}"
              f"   clipped in band {100.0 * result['clippedFractionSkyBand']:.3f}%")
        print(f"  ground rows      {result['groundRows'][0]}..{result['groundRows'][1]}"
              f"   clipped in band {100.0 * result['clippedFractionGroundBand']:.3f}%")
    if "sky" in result:
        s = result["sky"]
        print(f"  SKY      mean {s['meanLuminance']:.5f} linear   median {s['medianLuminance']:.5f}"
              f"   brightest decile {s['topDecileLuminance']:.5f} (the Milky Way core)"
              f"   n={s['pixels']}")
    if "snow" in result:
        g = result["snow"]
        print(f"  SNOW     mean {g['meanLuminance']:.5f} linear   median {g['medianLuminance']:.5f}"
              f"   brightest decile {g['topDecileLuminance']:.5f}"
              f"   n={g['pixels']} ({100.0 * result['snowFractionFrame']:.2f}% of frame)")
    for note in result.get("notes", []):
        print(f"  NOTE: {note}")

    if "groundOverSky" in result:
        print()
        print("  L_ground / L_sky == albedo, because for a Lambertian surface under a sky of")
        print("  luminance L_sky the illuminance is E = pi * L_sky and the surface reads")
        print("  rho * E / pi. Geometry, units and exposure all cancel in the ratio -- which is")
        print("  why a +3 EV capture bias does not need removing.")
        print()
        print(f"  {result['status']}: ground/sky = {result['groundOverSky']:.4f}"
              f"   expected albedo band [{result['expectedAlbedoMin']}, {result['expectedAlbedoMax']}]"
              f"   (snow is conventionally 0.75-0.85; the reference frame measured 0.790)")

    if result.get("error"):
        print()
        # Flushed before the stderr write so the two streams interleave in the order
        # they were written when both go to a terminal or a log file. Without it the
        # failure message lands ABOVE the measurements that explain it.
        sys.stdout.flush()
        print(f"{result['status']}: {result['error']}", file=sys.stderr)
    return code


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", help="Starlit capture PNG (see the header for the exact command).")
    parser.add_argument("--albedo-min", type=float, default=0.75,
                        help="Low edge of the expected ground albedo. Default 0.75 (snow).")
    parser.add_argument("--albedo-max", type=float, default=0.85,
                        help="High edge of the expected ground albedo. Default 0.85 (snow).")
    parser.add_argument("--sky-frac", type=float, default=0.35,
                        help="Sky region is the top this-much of the frame, then clamped to the "
                             "detected horizon. Default 0.35, which is what the reference "
                             "measurement (0.14342 linear) used.")
    parser.add_argument("--ground-frac", type=float, default=0.50,
                        help="Ground search region starts this far down the frame, then clamped to "
                             "the detected horizon. Default 0.50.")
    parser.add_argument("--sat-max", type=float, default=0.20,
                        help="Snow detector: maximum sRGB saturation (max-min)/max. Default 0.20 -- "
                             "snow under starlight is near-neutral; rock and vegetation are not.")
    parser.add_argument("--val-min", type=float, default=0.10,
                        help="Snow detector: minimum sRGB value max(R,G,B)/255. Default 0.10, which "
                             "on a +3 EV starlit frame separates lit snow from shadowed ground.")
    parser.add_argument("--min-snow-frac", type=float, default=0.02,
                        help="Refuse the frame (exit 2) if less than this fraction of it classifies "
                             "as snow. Default 0.02; the reference frame is 0.084 and a "
                             "starlight-off frame is 0.002, so this separates them by 10x.")
    parser.add_argument("--min-sky-px", type=int, default=10000,
                        help="Refuse the frame (exit 2) below this many unclipped sky pixels.")
    parser.add_argument("--json", action="store_true",
                        help="Emit only the result object, for harness consumption "
                             "(same shape/exit-code contract as tools/check-perf-run.py).")
    args = parser.parse_args()

    if not args.albedo_min <= args.albedo_max:
        print(f"FAIL: --albedo-min {args.albedo_min} is above --albedo-max {args.albedo_max}, "
              f"which is an empty band that nothing can pass.", file=sys.stderr)
        return 2

    result, code = measure(args.capture, args)
    result["exitCode"] = code
    if args.json:
        print(json.dumps(result, indent=2))
        return code
    return report(result, code)


if __name__ == "__main__":
    sys.exit(main())
