# GPU pod bring-up â€” exact command blocks (Track B1)

Copy-paste blocks for the Vast.ai RTX 4090 pod, in order. Each block states
its expected output. Context: the pod already ran the background install
(clone `xandergos/terrain-diffusion`, its requirements, plus this repo's
`terrain-service/requirements.txt`), logging to `/setup.log`. These blocks
assume the voxelsim repo on the pod has pulled the commit containing the
implemented `DiffusionProvider._call_model` (this document's commit).

Full narrative runbook: `terrain-service/docs/diffusion-bringup.md`. This
file is just the compressed command sequence for the session.

## Block 0 â€” verify setup finished

```sh
tail -n 20 /setup.log
python -c "import torch; print(torch.__version__, torch.cuda.is_available())"
python -c "import terrain_diffusion; print('terrain_diffusion importable')"
```

Expected: no errors in the log tail; torch version plus `True`;
`terrain_diffusion importable`. If `terrain_diffusion` fails to import, run
`pip install -e /path/to/terrain-diffusion` (wherever the clone landed) and
retry.

## Block 1 â€” find the canonical checkpoint ids, download a local snapshot

Run from `terrain-service/` in the voxelsim repo (all later Python blocks
too â€” they import `terrain_service`):

```sh
python -c "from terrain_diffusion.common.model_utils import MODEL_PATHS; print(MODEL_PATHS)"
python - <<'EOF'
from huggingface_hub import snapshot_download
p = snapshot_download("xandergos/terrain-diffusion-30m",
                      local_dir="/workspace/ckpt/terrain-diffusion-30m")
print("snapshot at:", p)
EOF
ls /workspace/ckpt/terrain-diffusion-30m
```

Expected: `MODEL_PATHS` dict printed (use it to correct the repo id in the
`snapshot_download` call if `xandergos/terrain-diffusion-30m` is not the real
30 m pipeline id â€” that id is our best guess from the terrain-diffusion
source); download progress; then a directory listing containing
`config.json` plus `coarse_model/`, `base_model/`, `decoder_model/`
subfolders. If the listing does NOT have that shape, paste it back to
Claude â€” `TerrainDiffusionBackend` expects a `WorldPipeline` snapshot tree.

## Block 2 â€” compute the checkpoint sha256 manifest hash

```sh
python - <<'EOF'
from terrain_service.providers.diffusion import _sha256_of_checkpoint_path
print(_sha256_of_checkpoint_path("/workspace/ckpt/terrain-diffusion-30m"))
EOF
```

Expected: one 64-character hex string. **Record it** â€” it is
`checkpoint_sha256` for every block below (written `<HASH>` from here on).

## Block 3 â€” one-tile inference + validate against EXPECTED_CHANNELS

The single most important block of the session (backlog item "Confirm real
terrain-diffusion tile outputs"):

```sh
python - <<'EOF'
from terrain_service.providers.diffusion import (
    DiffusionConfig, DiffusionProvider, validate_model_output, ModelOutputMismatch)

config = DiffusionConfig(
    checkpoint_id="/workspace/ckpt/terrain-diffusion-30m",
    checkpoint_sha256="<HASH>",
)
print("provider_id:", config.provider_id())

provider = DiffusionProvider(config=config)
raster = provider._call_model(seed=20260719, x=0, y=0, scale=1)
for k, v in raster.items():
    print(f"{k}: shape={v.shape} dtype={v.dtype} min={v.min():.3f} max={v.max():.3f}")
try:
    validate_model_output(raster, config.channel_mapping)
    print("MATCHES our assumption - no adaptation needed")
except ModelOutputMismatch as e:
    for issue in e.issues:
        print("-", issue)
EOF
```

Expected: a `provider_id` line (**record it** â€” it is the cache namespace),
five raster lines (elevation ~[-12000, 9000] metres-ish, four climate
channels in [0, 1], all `(512, 512) float32`), then
`MATCHES our assumption - no adaptation needed`.

If it prints issue lines instead: paste them back to Claude â€” per the
runbook that is a `channel_mapping`/`EXPECTED_CHANNELS` config edit, not a
redesign. Also note the wall-clock time of this block: it bounds the pregen
cost in Block 5.

## Block 4 â€” axis-mapping seam check (ASSUMPTION #2)

```sh
python - <<'EOF'
import numpy as np
from terrain_service.providers.diffusion import DiffusionConfig, DiffusionProvider

config = DiffusionConfig(checkpoint_id="/workspace/ckpt/terrain-diffusion-30m",
                         checkpoint_sha256="<HASH>")
p = DiffusionProvider(config=config)
a = p._call_model(seed=20260719, x=0, y=0, scale=1)["elevation"]
b = p._call_model(seed=20260719, x=1, y=0, scale=1)["elevation"]  # east neighbour
c = p._call_model(seed=20260719, x=0, y=1, scale=1)["elevation"]  # south neighbour
print("east seam  max|diff|:", float(np.abs(a[:, -1] - b[:, 0]).max()))
print("south seam max|diff|:", float(np.abs(a[-1, :] - c[0, :]).max()))
EOF
```

Expected: BOTH seam diffs small (roughly < 1 m â€” the pipeline generates one
continuous world). If the east seam is huge while comparing `a`'s bottom row
to `b`'s top row would have matched, the x/y -> (i,j) axis mapping in
`TerrainDiffusionBackend.generate_rasters` is flipped â€” paste the numbers
back to Claude; it's a two-line fix.

## Block 5 â€” pregen the launch grid + informational golden hash

Start with radius 2 (25 tiles) so cost stays bounded; bump `R` to 8 only if
per-tile time from Block 3 makes that reasonable. Uses an explicit pinned
config (the `pregen` CLI's `--provider diffusion` would construct the
UNPINNED default config and be refused by the sha256 gate â€” that CLI wiring
is a known follow-up):

```sh
python - <<'EOF'
import hashlib, time
from terrain_service.providers.diffusion import DiffusionConfig, DiffusionProvider
from terrain_service.cache import TileCache
from terrain_service import tile_codec

config = DiffusionConfig(checkpoint_id="/workspace/ckpt/terrain-diffusion-30m",
                         checkpoint_sha256="<HASH>")
provider = DiffusionProvider(config=config)
cache = TileCache("/workspace/tile-cache")
SEED, R, SCALE = 20260719, 2, 1

t0 = time.time()
for x in range(-R, R + 1):
    for y in range(-R, R + 1):
        if cache.get(provider.provider_id, SEED, x, y, SCALE) is not None:
            print(f"({x},{y}) cached, skip"); continue
        data = tile_codec.encode(provider.generate(SEED, x, y, SCALE))
        cache.put(provider.provider_id, SEED, x, y, SCALE, data)
        print(f"({x},{y}) done  {time.time()-t0:6.0f}s  {len(data)} bytes", flush=True)

golden = cache.get(provider.provider_id, SEED, 0, 0, SCALE)
print("provider_id:", provider.provider_id)
print("tile(0,0) sha256 (informational golden):",
      hashlib.sha256(golden).hexdigest())
EOF
```

Expected: 25 `(x,y) done` lines (~1.5 MB each), the `provider_id`, and one
informational golden hash (record it in a comment near the pinned config
later â€” NOT a cross-machine reproducibility promise, per doctrine).

## Block 6 â€” package the cache and pull it off the pod

```sh
tar czf /workspace/tile-cache-seed20260719.tar.gz -C /workspace tile-cache
ls -l /workspace/tile-cache-seed20260719.tar.gz
```

Expected: a tarball of roughly 25 x 1.5 MB (compression will shave some).
Download it with your usual Vast method (e.g. from the dev machine:
`scp -P <pod ssh port> root@<pod host>:/workspace/tile-cache-seed20260719.tar.gz D:\voxelsim\`),
then **stop the pod** (not just idle).

## Block 7 â€” consume on the dev machine (no pod needed)

Extract, then point the game at the cache leaf directory:

```powershell
tar xzf D:\voxelsim\tile-cache-seed20260719.tar.gz -C D:\voxelsim
# leaf dir shape: D:\voxelsim\tile-cache\<provider_id>\000000000135276f\s1
"D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" "D:\voxelsim\ue-project\VoxelEarth.uproject" `
  -game -windowed -resx=1280 -resy=720 -log -unattended -nosplash `
  -VoxelTileDir="D:\voxelsim\tile-cache\<provider_id>\000000000135276f\s1" `
  -VoxelScreenshotAfter=25
```

Expected: `LogVoxelEarth` reports the tiles loaded from that directory and
the world renders REAL terrain-diffusion output â€” the first Earth-looking
world in the project. (`000000000135276f` is hex for seed 20260719; pass
`-VoxelSeed=` too if you pregen'd a different seed.)

