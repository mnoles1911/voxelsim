"""Does the apron scheme actually make baked tiles seamless?

docs/terrain-amplification-plan.md rests the whole Phase 2 seam story on one
claim: bake each tile on its domain PLUS an apron, write only the interior, and
because every bounded pass has an influence radius well under the apron, each
interior equals the infinite-domain answer -- so neighbours agree exactly, with
no stitching. That claim has never been tested.

This tests it directly:

  truth  = bake one domain covering BOTH sub-tiles plus an apron, crop the
           interior. This is the "infinite domain" answer for both.
  A, B   = bake each sub-tile separately on its own domain + apron, crop.
  error  = |A - truth| and |B - truth| as a function of distance from the shared
           edge, plus the STEP across the A|B join.

If the apron is adequate the error decays to ~0 well before the interior, and
the join step is indistinguishable from the terrain's own gradient.

Scaled down deliberately: sub-tiles are 256 coarse px rather than the real 512,
so the truth domain fits in memory. What matters is the ratio of APRON to
INFLUENCE RADIUS, and the apron is kept at the plan's real 256 fine px (960 m).

Run with the terrain-diffusion venv python (needs numba/scipy):
  bake_seam_test.py <tileA.vxtl> <tileB.vxtl> [--apron 32] [--png out.png]
"""
import argparse
import numpy as np

import bake_prototype as bp

SUB = 256          # coarse px per sub-tile in this test


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tile_a")
    ap.add_argument("tile_b", help="the tile immediately EAST of tile_a")
    ap.add_argument("--apron", type=int, default=32,
                    help="apron in COARSE px (32 = 256 fine px = 960 m, the plan's value)")
    ap.add_argument("--iters", type=int, default=48)
    ap.add_argument("--png")
    a = ap.parse_args()

    A_COARSE = bp.decode_vxtl(a.tile_a)
    B_COARSE = bp.decode_vxtl(a.tile_b)
    strip = np.concatenate([A_COARSE, B_COARSE], axis=1)      # (512, 1024)

    # Window centred on the tile join, tall enough for both sub-tiles.
    ap_c = a.apron
    cx = A_COARSE.shape[1]                                     # join column
    y0 = (strip.shape[0] - SUB) // 2
    truth_x0 = cx - SUB - ap_c
    truth_x1 = cx + SUB + ap_c
    truth_y0 = y0 - ap_c
    truth_y1 = y0 + SUB + ap_c
    truth_coarse = strip[truth_y0:truth_y1, truth_x0:truth_x1]
    print(f"truth domain {truth_coarse.shape} coarse -> "
          f"{truth_coarse.shape[0]*bp.SCALE}^2 fine, apron {ap_c} coarse px "
          f"({ap_c*bp.SCALE} fine px = {ap_c*30} m)")

    # ONE world-anchored noise field, sliced by every bake -- the stand-in for
    # production B1 hashing world coordinates.
    rng = np.random.default_rng(20260719)
    parent = np.zeros((truth_coarse.shape[0] * bp.SCALE,
                       truth_coarse.shape[1] * bp.SCALE), np.float32)
    from scipy.ndimage import zoom
    amp, size = 1.0, 16
    while size <= parent.shape[0]:
        g = rng.standard_normal((size + 4, size + 4)).astype(np.float32)
        up = zoom(g, max(parent.shape) / size, order=3, mode="nearest")
        parent += amp * up[:parent.shape[0], :parent.shape[1]]
        amp *= 0.55
        size *= 2
    parent /= parent.std()

    ap_f = ap_c * bp.SCALE
    sub_f = SUB * bp.SCALE

    print("\n[truth] both sub-tiles + apron")
    tr = bp.run_bake(truth_coarse, iters=a.iters, noise=parent)
    truth_i = tr["z"][ap_f:ap_f + sub_f, ap_f:ap_f + 2 * sub_f]   # both interiors
    truth_acc = tr["acc"][ap_f:ap_f + sub_f, ap_f:ap_f + 2 * sub_f]

    results, accs = {}, {}
    for name, x0c in (("A", truth_x0), ("B", truth_x0 + SUB)):
        dom = strip[truth_y0:truth_y1, x0c:x0c + SUB + 2 * ap_c]
        # slice the SAME parent noise the truth bake used
        nx0 = (x0c - truth_x0) * bp.SCALE
        noise = parent[:dom.shape[0] * bp.SCALE, nx0:nx0 + dom.shape[1] * bp.SCALE]
        print(f"\n[{name}] one sub-tile + apron")
        r = bp.run_bake(dom, iters=a.iters, noise=noise, verbose=False)
        results[name] = r["z"][ap_f:ap_f + sub_f, ap_f:ap_f + sub_f]
        accs[name] = r["acc"][ap_f:ap_f + sub_f, ap_f:ap_f + sub_f]

    joined = np.concatenate([results["A"], results["B"]], axis=1)
    err = np.abs(joined - truth_i)

    # THE UNBOUNDED DEPENDENCY, probed directly. An apron cannot know about flow
    # originating further upstream than itself, so a zero height error is only
    # meaningful if cross-join flow exists here at all. Compare the ACCUMULATION
    # field, which is where that ignorance would show up first.
    jacc = np.concatenate([accs["A"], accs["B"]], axis=1)
    big = truth_acc > 1e5                                   # >= 0.1 km2 catchment
    ratio = np.where(big, jacc / np.maximum(truth_acc, 1.0), 1.0)
    print(f"\n=== the unbounded dependency: flow accumulation vs truth ===")
    print(f"  truth max catchment in the interior: {truth_acc.max()/1e6:.2f} km2")
    print(f"  cells with catchment >= 0.1 km2: {big.sum()}")
    if big.sum():
        print(f"  per-tile / truth accumulation on those cells: "
              f"median {np.median(ratio[big]):.3f}  p01 {np.percentile(ratio[big],1):.3f}  "
              f"min {ratio[big].min():.3f}")
        print(f"  (1.000 = the apron captured everything; << 1 = flow the tile could not see)")

    print(f"\n=== apron adequacy: |per-tile bake - truth| ===")
    print(f"  overall   mean {err.mean()*100:7.2f} cm   p99 {np.percentile(err,99)*100:7.2f} cm"
          f"   max {err.max()*100:7.2f} cm")
    print(f"  {'dist from tile edge':>22}  {'mean err':>10}  {'p99':>10}  {'max':>10}")
    for lo, hi in ((0, 8), (8, 32), (32, 128), (128, 512), (512, sub_f)):
        # distance from whichever interior edge is nearest, in fine px
        colA = err[:, sub_f - hi:sub_f - lo]        # A's right edge (the join)
        colB = err[:, sub_f + lo:sub_f + hi]        # B's left edge
        band = np.concatenate([colA, colB], axis=1)
        print(f"  {f'{lo}-{hi} px ({lo*bp.PIXEL_M:.0f}-{hi*bp.PIXEL_M:.0f} m)':>22}  "
              f"{band.mean()*100:9.2f}c  {np.percentile(band,99)*100:9.2f}c  "
              f"{band.max()*100:9.2f}c")

    step = np.abs(joined[:, sub_f] - joined[:, sub_f - 1])
    nat = np.abs(truth_i[:, sub_f] - truth_i[:, sub_f - 1])
    print(f"\n=== the join itself (one 3.75 m step across the tile boundary) ===")
    print(f"  per-tile bakes: mean {step.mean()*100:6.2f} cm   p99 {np.percentile(step,99)*100:7.2f} cm")
    print(f"  truth          : mean {nat.mean()*100:6.2f} cm   p99 {np.percentile(nat,99)*100:7.2f} cm")
    print(f"  -> excess over the terrain's own gradient: "
          f"{(step.mean()-nat.mean())*100:+.2f} cm mean")

    if a.png:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        c = 384
        sl = (slice(sub_f // 2 - c, sub_f // 2 + c), slice(sub_f - c, sub_f + c))
        fig, ax = plt.subplots(1, 3, figsize=(19, 6.6))
        ax[0].imshow(bp._hillshade(truth_i[sl], bp.PIXEL_M), cmap="gray", vmin=0, vmax=1)
        ax[0].set_title("truth (single domain over both tiles)")
        ax[1].imshow(bp._hillshade(joined[sl], bp.PIXEL_M), cmap="gray", vmin=0, vmax=1)
        ax[1].set_title("per-tile bakes, joined at the dashed line")
        for k in (0, 1):
            ax[k].axvline(c, color="r", ls="--", lw=0.8)
        im = ax[2].imshow(err[sl] * 100, cmap="magma", vmin=0,
                          vmax=max(1.0, np.percentile(err, 99.9) * 100))
        ax[2].set_title("|per-tile - truth| (cm)")
        plt.colorbar(im, ax=ax[2], fraction=0.046)
        for k in range(3):
            ax[k].set_xticks([]); ax[k].set_yticks([])
        fig.suptitle(f"Apron seam test - apron {ap_c*30} m, "
                     f"{2*c*bp.PIXEL_M/1000:.1f} km across the join")
        fig.tight_layout()
        fig.savefig(a.png, dpi=110)
        print(f"\n  wrote {a.png}")


if __name__ == "__main__":
    main()
