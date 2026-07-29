"""Generate real 30 m/px diffusion tiles as `.vxtl` v1, for bake calibration.

The bake tools all take a `.vxtl`, and calibration decisions -- stream_K above all
-- have to be made on real model output. Synthetic fixtures have the right
statistics by construction and so cannot falsify anything about landform realism.

Writes into the scratchpad by default, not the repo: a 512^2 tile is 512 KB and
nothing downstream wants it version-controlled.

  gen_reference_tile.py --out-dir DIR [--origin 0 0] [--tiles 2] [--seed N]

`--tiles 2` emits a 2x2 block of adjacent tiles from ONE pipeline fetch, which is
both cheaper than four fetches and the only way to get tiles that genuinely abut
(the model's own windowing makes neighbours agree; assembling separate fetches
would not).
"""
import argparse, os, struct, sys, time
import numpy as np

TILE = 512
HDR = "<4sHQiiBH"


def write_vxtl(path, elev_i16, seed, tx, ty, scale=1):
    size = elev_i16.shape[0]
    with open(path, "wb") as f:
        f.write(struct.pack(HDR, b"VXTL", 1, seed & 0xFFFFFFFFFFFFFFFF, tx, ty, scale, size))
        f.write(np.ascontiguousarray(elev_i16, dtype="<i2").tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--origin", nargs=2, type=int, default=[0, 0])
    ap.add_argument("--tiles", type=int, default=2, help="edge, in tiles, of the block")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--allow-cpu", action="store_true",
                    help="run inference on CPU: hours per tile, but it is the only "
                         "path on a box without CUDA")
    a = ap.parse_args()

    import torch
    from terrain_diffusion.inference.world_pipeline import WorldPipeline

    os.makedirs(a.out_dir, exist_ok=True)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    if dev == "cpu" and not a.allow_cpu:
        print("CUDA unavailable -- the bringup doc warns the CPU path is hours per tile. "
              "Pass --allow-cpu if that is the deal you want.")
        return 1

    t0 = time.time()
    w = WorldPipeline.from_pretrained("xandergos/terrain-diffusion-30m", seed=a.seed,
                                      log_mode="quiet")
    w.to(dev)
    w.bind(hdf5_file=None)
    print(f"seed {w.seed}   device {dev}   load {time.time()-t0:.1f} s")

    i0, j0 = a.origin
    n = a.tiles * TILE
    t0 = time.time()
    out = w.get(i0, j0, i0 + n, j0 + n, with_climate=False)
    elev = out["elev"].detach().float().cpu().numpy()
    print(f"generated {elev.shape} in {time.time()-t0:.1f} s "
          f"({(time.time()-t0)/(a.tiles**2):.1f} s/tile)")

    # `_elev_to_int16` in api.py is the wire contract: whole metres, clamped.
    q = np.clip(np.rint(elev), -32768, 32767).astype(np.int16)
    for ty in range(a.tiles):
        for tx in range(a.tiles):
            sub = q[ty * TILE:(ty + 1) * TILE, tx * TILE:(tx + 1) * TILE]
            p = os.path.join(a.out_dir, f"tile_{i0//TILE+tx}_{j0//TILE+ty}.vxtl")
            write_vxtl(p, sub, w.seed, i0 // TILE + tx, j0 // TILE + ty)
            print(f"  {os.path.basename(p)}  relief {sub.min()}..{sub.max()} m  "
                  f"mean {sub.mean():.0f} m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
