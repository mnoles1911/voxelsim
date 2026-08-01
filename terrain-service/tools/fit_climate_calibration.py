"""Fit global calibration curves for ALL FOUR climate channels.

Same mechanism as the temperature fit: the coarse model preserves each
channel's spatial pattern and squashes its range, so a fixed monotone
per-pixel remap restores the range without touching spatial structure and
without seaming.

Emits a table ready to paste into providers/diffusion.py.
"""
import numpy as np
import torch

from terrain_diffusion.inference.world_pipeline import WorldPipeline

SEED = 20260719
N = 48
OFFSETS = [(0, 0), (400, 0), (-400, 300), (900, -600), (-1100, -900),
           (1500, 1200), (-700, 1400), (2100, 400), (-1800, -200), (600, 2000)]
Q = [1, 5, 25, 50, 75, 95, 99]

# our name -> (delivered channel index, sketch channel index, clip lo, clip hi)
# Clips are EXPECTED_CHANNELS' physical ranges; the calibrated value must stay
# inside them or adapt_raster_to_tile's quantizer saturates.
CH = {
    "temperature":         (2, 1, -40.0, 40.0),
    "seasonality":         (3, 2, 0.0, 25000.0),
    "precipitation":       (4, 3, 0.0, 12000.0),
    "precip_variability":  (5, 4, 0.0, 200.0),
}

world = WorldPipeline.from_pretrained(
    "xandergos/terrain-diffusion-30m", seed=SEED, log_mode="info",
    frequency_mult=[1.5, 3, 3, 3, 3], drop_water_pct=0.5,
    orographic={}, elev_gain=1.6, elev_gain_power=2.0,
)
world.to("cpu")
world.bind(hdf5_file=None)

A = {k: [] for k in CH}
D = {k: [] for k in CH}
for (oi, oj) in OFFSETS:
    i0, j0 = oi - N // 2, oj - N // 2
    sk = world.synthetic_map_factory(j0, i0, j0 + N, i0 + N).numpy()
    sl = (np.sign(sk[0]) * np.square(sk[0])) > 0
    with torch.no_grad():
        c = world.coarse[:, i0:i0 + N, j0:j0 + N]
        d = (c[:-1] / (c[-1:] + 1e-8)).detach().cpu().numpy()
    dl = (np.sign(d[0]) * np.square(d[0])) > 0
    if sl.sum() < 50 or dl.sum() < 50:
        continue
    for name, (di, si, _, _) in CH.items():
        A[name].append(sk[si][sl])
        D[name].append(d[di][dl])

print()
out = {}
for name, (di, si, lo, hi) in CH.items():
    a = np.concatenate(A[name])
    dv = np.concatenate(D[name])
    aq = np.percentile(a, Q)
    dq = np.percentile(dv, Q)
    xs = [round(float(v), 3) for v in dq]
    ys = [round(float(np.clip(v, lo, hi)), 3) for v in aq]
    mono = all(b > a_ for a_, b in zip(xs, xs[1:])) and all(b > a_ for a_, b in zip(ys, ys[1:]))
    after = np.interp(dv, xs, ys)
    print(f"=== {name} ===   strictly monotone: {mono}")
    print(f"  {'q':>4} {'DELIVERED':>12} {'ASKED':>12}")
    for q, x, y in zip(Q, xs, ys):
        print(f"  {q:3d}% {x:12.3f} {y:12.3f}")
    for label, arr in (("before", dv), ("after", after), ("target", a)):
        p = np.percentile(arr, [5, 50, 95])
        print(f"    {label:<7} p5 {p[0]:10.2f}  p50 {p[1]:10.2f}  p95 {p[2]:10.2f}")
    if not mono:
        print("    !! NOT MONOTONE -- cannot ship; np.interp would reorder values")
    out[name] = (xs, ys)
    print()

print("--- paste into diffusion.py ---")
print("CLIMATE_CALIBRATION: dict[str, tuple[tuple[float, ...], tuple[float, ...]]] = {")
for name, (xs, ys) in out.items():
    print(f"    {name!r}: (")
    print(f"        {tuple(xs)},")
    print(f"        {tuple(ys)},")
    print("    ),")
print("}")
