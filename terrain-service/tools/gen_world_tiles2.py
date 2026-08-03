"""Generate world tiles THROUGH THE SHIPPING PROVIDER, not around it.

Supersedes gen_world_tiles.py, which hand-rolled the model call and the encode
and got the tile WRONG in a way that only surfaced three steps later: it wrote
header + int16 elevation and no climate planes at all, so every tile it produced
was 512 KB short and `bake_tile` died on `np.frombuffer: buffer is smaller than
requested size`. The 52 tiles it already wrote are elevation-only and must be
regenerated before anything downstream trusts them.

That is the second time in this effort that reimplementing a conversion rather
than calling the shipping one has cost real time, and the codebase had already
recorded the first: `adapt_raster_to_tile`'s own comment describes a previous
`raw * 255.0` climate quantisation that saturated all four planes to 255,
"producing four identical constant planes that would have looked like climate
exists while carrying no information at all". Hand-rolling that normalisation
again was exactly the available mistake.

So this drives `DiffusionProvider.generate()` and `TileCache.put()`, which means
the model call, the validation, the physical-units climate normalisation, the
wire encoding, the provider_id and the on-disk layout are all the ones the game
uses, by construction rather than by care.

CPU only on this box (torch is +cpu): ~120 s per tile cold, much less warm since
the pipeline caches its coarse hierarchy across calls in one process.

  gen_world_tiles2.py --verify -5 3          # regenerate an existing tile, compare bytes
  gen_world_tiles2.py --tiles "-55,20 -5,15"
"""
import argparse, os, sys, time
import numpy as np


def _find_hf_snapshot():
    """The local HuggingFace snapshot of xandergos/terrain-diffusion-30m.

    Resolved rather than configured: it is a per-machine path, and the identity
    is carried by the sha256 of its contents, not by where it happens to sit.
    """
    import glob
    hub = os.path.expanduser("~/.cache/huggingface/hub")
    hits = sorted(glob.glob(os.path.join(
        hub, "models--xandergos--terrain-diffusion-30m", "snapshots", "*")))
    if not hits:
        raise SystemExit(
            "no local terrain-diffusion-30m snapshot found under "
            f"{hub}. Pass --checkpoint, or let the pipeline download it once.")
    return hits[-1]


def describe(elev):
    g = np.hypot(*np.gradient(elev.astype(np.float64), 30.0)) * 100.0
    land = elev > 0
    if not land.any():
        return f"relief {int(elev.min())}..{int(elev.max())} m  land   0.0%  (all ocean)"
    return (f"relief {int(elev.min())}..{int(elev.max())} m  land {land.mean()*100:5.1f}%  "
            f"p50 grade {np.percentile(g[land], 50):5.1f}%  p95 {np.percentile(g[land], 95):6.1f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiles", default="", help='space-separated "x,y"')
    ap.add_argument("--verify", nargs=2, type=int, metavar=("X", "Y"))
    ap.add_argument("--seed", type=int, default=0x135276F)
    ap.add_argument("--cache-root", default=r"D:\voxelsim\tile-cache")
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--checkpoint", default=None, help="local checkpoint dir; defaults to the HF snapshot")
    ap.add_argument("--conditioning-root", default=r"D:\terrain-diffusion\data\global")
    a = ap.parse_args()

    os.environ.setdefault("OMP_NUM_THREADS", str(a.threads))
    os.environ.setdefault("MKL_NUM_THREADS", str(a.threads))

    from terrain_service.cache import TileCache
    from terrain_service.providers.diffusion import DiffusionConfig, DiffusionProvider
    from terrain_service.tile_codec import decode, encode

    # PIN THE CHECKPOINT AND THE CONDITIONING, because the shipping code refuses
    # to run without it -- and it is right to. `checkpoint_sha256` defaults to
    # "UNPINNED" and `verify_checkpoint_sha256` raises on that: "a silent
    # checkpoint swap under an unchanged provider_id would poison the cache".
    # That guard is why the previous provider_id carried UNPINNED-UNVERIFIEDDATA
    # and why bring-up step 3 was never finished; `checkpoint_id` is still the
    # placeholder "./checkpoint".
    #
    # Pinned HERE rather than in DiffusionConfig's defaults because checkpoint_id
    # is a LOCAL PATH ("WHERE to load from, not in the id" -- bringup doc Â§3) and
    # committing this machine's HuggingFace snapshot path into the shared config
    # would be wrong on every other machine. The sha256 and the conditioning
    # digest ARE machine-independent and are the parts that enter the identity;
    # a real bring-up should move those two into the committed defaults.
    from terrain_service.providers.diffusion import (
        _sha256_of_checkpoint_path, compute_conditioning_digest)

    ckpt = a.checkpoint or _find_hf_snapshot()
    cfg = DiffusionConfig(
        checkpoint_id=ckpt,
        checkpoint_sha256=_sha256_of_checkpoint_path(ckpt),
        conditioning_digest=compute_conditioning_digest(root=a.conditioning_root),
    )
    provider = DiffusionProvider(config=cfg, dry_run=False)
    pid = cfg.provider_id()
    cache = TileCache(a.cache_root)
    print(f"provider_id: {pid}")
    print(f"cache      : {cache.path(pid, a.seed, 0, 0, cfg.scale).parent}")

    # This tool writes tiles straight into the cache, so it owes the world the
    # same identity record pregen writes -- a world extended by whichever tool
    # was handy must not be half-documented. Note --conditioning-root: this
    # tool hashes the root it is pointed at, which is the root the model reads.
    from terrain_service.world_manifest import record_world_identity

    ok, msg = record_world_identity(cache, provider, a.seed, pid)
    if msg:
        print(msg)
    if not ok:
        return 2

    if a.verify:
        x, y = a.verify
        raw = cache.get(pid, a.seed, x, y, cfg.scale)
        if raw is None:
            print(f"VERIFY IMPOSSIBLE: ({x},{y}) not in cache")
            return 2
        t = time.time()
        mine = encode(provider.generate(a.seed, x, y, cfg.scale))
        same = mine == raw
        print(f"verify ({x},{y}) in {time.time()-t:.0f}s: "
              f"{'BYTE-IDENTICAL' if same else 'DIFFERS'} "
              f"({len(mine)} vs {len(raw)} bytes)")
        if not same and len(mine) == len(raw):
            d = np.abs(decode(mine).elevation.astype(int) - decode(raw).elevation.astype(int))
            print(f"  elevation max|d|={d.max()} mean|d|={d.mean():.3f} "
                  f"differing={100*(d>0).mean():.2f}%")
        return 0 if same else 1

    coords = [tuple(int(v) for v in tok.split(",")) for tok in a.tiles.split()]
    if not coords:
        print("nothing to do")
        return 2

    for x, y in coords:
        t = time.time()
        tile = provider.generate(a.seed, x, y, cfg.scale)
        cache.put(pid, a.seed, x, y, cfg.scale, encode(tile))
        print(f"({x:4d},{y:4d}) {time.time()-t:5.0f}s  {describe(tile.elevation)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
