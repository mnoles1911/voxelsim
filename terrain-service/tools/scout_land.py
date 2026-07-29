"""Find land before paying for diffusion.

A 512^2 native tile costs ~3 min of CPU inference on this box, and the first one
generated came back as -2316..-288 m: open ocean, useless for calibrating a bake
whose whole subject is drainage. Most of the world is ocean, so blind sampling is
a bad deal.

The pipeline's geographic conditioning is a Perlin stack quantile-matched to
ETOPO/WorldClim, evaluated with NO diffusion at all -- so it is essentially free
and it is precisely the field that decides whether a region comes out as land.
Sampling it over a wide window ranks candidate origins before any of them cost
three minutes.

The conditioning-index -> native-pixel factor is not stated in one place in
`world_pipeline.py`, so this CALIBRATES it rather than assuming: it evaluates
several candidate factors against tiles whose true elevation is already known
(pass them with --known) and reports which factor is consistent.

  scout_land.py --span 4096 [--known 0,0,-1468] [--top 8]

Run from D:\\terrain-diffusion (the conditioning data paths are relative).
"""
import argparse
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20260729)
    ap.add_argument("--span", type=int, default=4096,
                    help="half-width of the conditioning window, in conditioning px")
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--known", action="append", default=[],
                    help="tx,ty,mean_m of an already-generated tile, for factor calibration")
    a = ap.parse_args()

    from terrain_diffusion.inference.world_pipeline import WorldPipeline

    w = WorldPipeline.from_pretrained("xandergos/terrain-diffusion-30m", seed=a.seed,
                                      log_mode="quiet")
    w.bind(hdf5_file=None)

    s = a.span
    cond = w._conditioning_model_input(-s, s, -s, s).numpy()
    elev = cond[0]
    # Channel 0 carries sign(x)*sqrt(|x|) of an elevation-like quantity, so undo
    # the sqrt to get something monotone in metres before ranking.
    elev = np.sign(elev) * elev ** 2
    print(f"conditioning window {elev.shape} over ci,cj in [{-s},{s})")
    print(f"  channel 0 after unsqrt: min {elev.min():.1f} max {elev.max():.1f} "
          f"mean {elev.mean():.1f}   land fraction {(elev > 0).mean():.3f}")

    # Calibrate the conditioning -> native factor against known tiles.
    if a.known:
        print("\n  factor calibration (tile mean vs conditioning at that location)")
        for f in (32, 64, 128, 256):
            rows = []
            for k in a.known:
                tx, ty, mean_m = (float(v) for v in k.split(","))
                # tile (tx,ty) covers native px [tx*512, tx*512+512)
                ci = int((ty * 512 + 256) / f)
                cj = int((tx * 512 + 256) / f)
                if abs(ci) >= s or abs(cj) >= s:
                    rows.append("out-of-window")
                    continue
                rows.append(f"cond {elev[ci + s, cj + s]:+8.1f} vs true {mean_m:+8.0f} m")
            print(f"    factor {f:4d} native px/cond px: " + "; ".join(rows))
        print("  (pick the factor whose sign and rough magnitude agree with truth)")

    # Rank candidate tile origins by mean conditioning elevation and by relief,
    # under each plausible factor. Relief matters more than height: a high flat
    # plateau tells us nothing about drainage.
    print(f"\n  top {a.top} candidate origins (native tile coords), by relief on land")
    for f in (64, 256):
        px_per_tile = max(1, 512 // f)
        h, wd = elev.shape
        nt = min(h, wd) // px_per_tile
        best = []
        for ty in range(nt):
            for tx in range(nt):
                blk = elev[ty * px_per_tile:(ty + 1) * px_per_tile,
                           tx * px_per_tile:(tx + 1) * px_per_tile]
                if blk.size == 0 or blk.mean() <= 100:
                    continue  # ocean or coastal flat
                best.append((float(blk.max() - blk.min()) + float(blk.mean()) * 0.1,
                             float(blk.mean()), float(blk.max() - blk.min()),
                             (tx * px_per_tile - s) * f // 512,
                             (ty * px_per_tile - s) * f // 512))
        best.sort(reverse=True)
        print(f"    assuming {f} native px per conditioning px:")
        for _, mean_v, relief, tx, ty in best[:a.top]:
            print(f"      tile ({tx:6d},{ty:6d})  cond mean {mean_v:8.1f}  "
                  f"relief {relief:8.1f}")


if __name__ == "__main__":
    main()
