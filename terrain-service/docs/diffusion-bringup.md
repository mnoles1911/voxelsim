# terrain-diffusion cloud bring-up — background and rationale

> **Doing a bring-up right now? You want `docs/pod-bringup-commands.md`.**
> It is two pasted commands and a failure table. This document is the *why*
> behind them — read it when something breaks in a way the table doesn't
> cover, or when changing the pipeline.
>
> **Status: the bring-up described here HAPPENED, on 2026-07-19.** The open
> questions below are answered, and the answers are marked inline. What was
> once a checklist is now automated by `tools/bootstrap_pod.sh` and
> `tools/generate_world.sh`.

Everything that does not need a GPU is implemented and tested
(`terrain_service/providers/diffusion.py`, `tests/test_diffusion.py`).
Related: `docs/status.md` backlog ("Confirm real terrain-diffusion tile
outputs" / "terrain-diffusion worker bring-up"),
`docs/voxel-earth-implementation-plan.md` §3.1/§3.4, `docs/m4-plan.md`.

## 0. Before you start (no GPU needed — do this locally first)

Confirm the dry-run plumbing is green so you know any failure during
bring-up is about the REAL model, not our code:

```sh
cd terrain-service
python -m pytest -q                                          # all green
python -m terrain_service.pregen --seed 1 --radius 1 \
    --provider diffusion --dry-run --cache-dir /tmp/dryrun    # generated=9
```

## 1. Rent a serverless GPU (scale-to-zero)

Plan §3.4's cost model: production targets **one 4090-class GPU, ~$0.35–0.7/hr
rented, serverless/scale-to-zero** — generation cost scales with *newly
explored area*, not player count, and decays toward zero once the cache
fills (most requests become cache hits). Total production infra for co-op
scale: **<$100–600/mo**. For the ONE bring-up session (not production
hosting), a Runpod/Modal/Vast-class on-demand pod billed by the minute is
the right shape — no need to wire up serverless autoscaling just to run
`validate_model_output` once and pregen a launch radius:

- **Runpod**: On-Demand Pod, RTX 4090 or similar, PyTorch template, pay by
  the minute, stop (not just idle) the pod when done.
- **Modal** (if going serverless from day one): `@app.function(gpu="A10G")`
  class wrapper around the provider; scale-to-zero is the default.
- Any CUDA 12.x + PyTorch 2.x host works — `terrain-diffusion` doesn't need
  anything more exotic. Estimated session cost at ~$0.5/hr: well under $5
  for the checklist below (a couple hours including install/debug).

## 2. Install terrain-diffusion + deps — CORRECTED

The obvious version of this step is wrong in three separate ways, all of
which cost real time on 2026-07-19. `tools/bootstrap_pod.sh` now does it
correctly; this is what it is avoiding.

```sh
git clone https://github.com/xandergos/terrain-diffusion /workspace/terrain-diffusion
export PYTHONPATH=/workspace/terrain-diffusion:$PYTHONPATH
```

- **`pip install -e ./terrain-diffusion` fails outright.** The repo ships no
  `setup.py` and no `pyproject.toml`. `PYTHONPATH` is the supported route.
- **Do NOT `pip install -r requirements.txt`.** That file is the *training*
  stack — wandb, optuna, cartopy, earthengine, lpips. It crawled at 29 kB/s
  and killed a session outright. The inference path needs roughly twenty
  packages; the authoritative list is the `DEPS` array in
  `tools/bootstrap_pod.sh`, assembled one traceback at a time.
- **The image's torch is probably wrong for the driver.** The Vast PyTorch
  image shipped `torch 2.13.0+cu130` against a **12.8** host driver. This
  does not raise — `torch.cuda.is_available()` returns `False` and inference
  silently runs on **CPU**, hours per tile, at 4090 prices. So the wheel must
  be selected from the driver (`tools/cuda_index.py` parses `nvidia-smi`),
  and **torchvision must be installed from the same `--index-url` in the same
  command** or it pulls a PyPI torch back in and reverts the fix.
  `tools/probe_tile.py` now refuses to run at all when CUDA is unavailable,
  so this can no longer be discovered from the bill.

There is also a data dependency that is easy to miss: terrain-diffusion's
`synthetic_map._compute_map_stats` opens **relative** paths under
`data/global/`, so everything must run from `terrain-service/`. WorldClim
auto-downloads on first run **and prompts for consent on stdin** (which hangs
unattended scripts). ETOPO must be *built*, not downloaded: the code reads
`etopo_10m.tif`, a 10 arc-**minute** downsample, while the README points at
NOAA's 30 arc-**second** product — see `tools/fetch_etopo.py`.

## 3. Pin a checkpoint AND the conditioning data into `DiffusionConfig`

**Which checkpoint — ANSWERED.** The bundle is **`xandergos/terrain-diffusion-30m`**.

`terrain_diffusion.common.model_utils.MODEL_PATHS` is actively misleading
here: it lists **three separate repos**, which reads as "download all three".
It is not. `WorldPipeline.from_pretrained` takes **one** path — a local
directory or an HF id — and pulls `coarse_model`, `base_model` and
`decoder_model` out of it via `subfolder=`. The three `MODEL_PATHS` entries
are the separate *training* repos the bundle was assembled from. A
`terrain-diffusion-90m` bundle also exists; it is a different resolution, not
a newer version, and needs its own `DiffusionConfig`/`provider_id`.

`tools/fetch_checkpoint.py` downloads it and verifies the subfolder shape;
`tools/checkpoint_sha256.py` prints the digest to pin.

> **Never type that hash by hand.** On 2026-07-22 the placeholder went in with
> its angle brackets attached (`"<ed06...>"`). A wrong hash does not fail
> loudly — it silently changes `provider_id`, which is *both* the tile-cache
> namespace and the edit-log provider stamp. The scripts read it from
> `/workspace/bringup.env`; the tools reject bracketed values.

`provider_id` does two load-bearing jobs: it is the tile-cache namespace, and
it is the value stamped into the edit log that `EditLog::checkProvider()`
compares to decide whether replaying a saved world against a tile set is safe
(`kMismatch` = refuse the replay outright). So it has to be **content-
addressed**: same generation inputs => same id, different inputs => different
id, and *nothing else*.

Two things must be pinned, because the model generates from two kinds of
input:

1. **The checkpoint** — `checkpoint_sha256`. Note `checkpoint_id` is the
   **load path only** and is *deliberately excluded* from `provider_id`;
   `checkpoint_label` is the human-readable name that goes into the id.
2. **The conditioning data** — `conditioning_digest`. `WorldPipeline` does
   not generate from weights alone: at bring-up it downloads the WorldClim
   2.1 10-arc-minute bio rasters and reads `data/global/etopo_10m.tif`, and
   `synthetic_map._compute_map_stats` derives statistics from them that
   condition generation. Two boxes with different copies produce different
   terrain. `tools/fetch_etopo.py` *builds* `etopo_10m.tif` from whichever
   NOAA product was reachable (its candidate list includes both a `_bed` and
   a `_surface` variant), so this divergence is likely, not theoretical.

Get the conditioning digest for this box (run from `terrain-service/`, the
directory containing `data/global` — the path is relative and resolved from
CWD, same as upstream; `TERRAIN_CONDITIONING_ROOT` overrides it):

```sh
python -m terrain_service.pregen --seed 0 --radius 0 --print-conditioning-digest
```

If the rasters are absent this **refuses** and exits 1 rather than printing an
invented digest — a provider that cannot see its conditioning data has no
business claiming an identity.

Then, in a Python shell on the GPU box:

```python
from terrain_service.providers.diffusion import (
    DiffusionConfig, SamplerConfig, _sha256_of_checkpoint_path, compute_conditioning_digest,
)

config = DiffusionConfig(
    checkpoint_id="/workspace/ckpt/terrain-diffusion-30m",   # WHERE to load from (not in the id)
    checkpoint_label="terrain-diffusion-30m",                # human-readable name (in the id; must NOT be a path)
    checkpoint_sha256=_sha256_of_checkpoint_path("/workspace/ckpt/terrain-diffusion-30m"),
    conditioning_digest=compute_conditioning_digest(),
    terrain_diffusion_version="<git rev / package version>",
    sampler=SamplerConfig(steps=30, guidance_scale=3.0, scheduler="ddim"),  # tune to taste
    scale=1,  # 30m/px. NB scale is a SUPERSAMPLE knob: 8 => 3.75m/px, which
              # covers LESS ground, not more. Bring it up as a SEPARATE
              # config/provider_id once scale=1 is trusted.
)
print(config.provider_id())  # the cache namespace for everything below
```

Record `config.provider_id()`. Sanity-check it before generating anything:

- It must **not** contain `UNPINNED` or `UNVERIFIEDDATA`. Those markers are
  deliberate — an identity that hasn't pinned its checkpoint or its
  conditioning data is printed as such so nobody caches against it by
  accident, and real inference is refused outright by
  `verify_checkpoint_sha256` / `verify_conditioning_digest`.
- It must **not** contain any filesystem path. If it does, you put the mount
  point in `checkpoint_label` (the config now rejects that outright).

Every tile generated under this checkpoint + conditioning data + sampler +
scale + channel_mapping + tile wire format is cached under this key (doctrine
§2.3: diffusion output isn't bit-deterministic across GPUs, so tiles are
canonical data generated once and distributed, never regenerated-and-compared
client-side).

The same env vars exist for the server and pregen CLI —
`TERRAIN_DIFFUSION_CHECKPOINT_ID` / `_LABEL` / `_SHA256`,
`TERRAIN_DIFFUSION_CONDITIONING_DIGEST`, `TERRAIN_DIFFUSION_VERSION`,
`TERRAIN_CONDITIONING_ROOT` (see `app.py`'s docstring), and
`--checkpoint-label` / `--conditioning-digest` / etc. on
`python -m terrain_service.pregen`, which echoes `provider_id:` as its first
line so a run in the wrong namespace is obvious immediately.

### Migration: tiles generated under the pre-2026-07-22 (v1) id

The v1 formula was `terrain-diffusion-{checkpoint_id}-{digest}` with the local
load path inside it and no conditioning-data coverage — both bugs. Anything
generated before this change carries that old id. The new scheme is
**identity schema v2**; ids from the two schemes never collide.

- **Cached tile files keep working as-is.** The cache layout is
  `tile-cache/<provider_id>/<seed>/s<scale>/…` and UE's `-VoxelTileDir` points
  at the **leaf** directory, so the old id survives purely as a directory
  name nobody reads. Nothing to do.
- **To serve or resume that old namespace by name**, set
  `provider_id_override` (env `TERRAIN_DIFFUSION_PROVIDER_ID_OVERRIDE`, flag
  `--provider-id-override`) to the old id verbatim. This is a compatibility
  escape hatch: it bypasses every guarantee above, so use it to *read* an
  existing tile set, not as a way to keep generating under a stale identity.
- **Worlds saved with the old id stamped in their edit log** will report
  `kMismatch` against a v2 id. That is a true positive for the checkpoint
  path bug (same tiles, id changed for a reason that was noise) — replay
  those worlds with the override set, or re-stamp them once. There is no way
  to have both: the whole point of the fix is that the id changed.

## 4. Wire the real model call — DONE, this step is now verification only

`DiffusionProvider._call_model` is implemented (no more TODO): it lazily
constructs `TerrainDiffusionBackend` once and reuses it, which loads
`WorldPipeline` (checkpoint sha256 verified first via
`verify_checkpoint_sha256`), reseeds it per request seed, and queries
`WorldPipeline.get(i1, j1, i2, j2, with_climate=True)` positioned per
`(x, y)` at `[x*512, (x+1)*512)` — same convention as `tile_codec.py`. It
returns raw, unquantized float32 rasters through the SAME
`validate_model_output` -> `adapt_raster_to_tile` path the dry-run already
exercises. This was implemented against the real terrain-diffusion source
(README.md, API_README.md, `world_pipeline.py`, `inference/api.py`,
`common/model_utils.py`), not guessed — see `TerrainDiffusionBackend`'s
class docstring for exactly what was confirmed from source.

What this GPU session still needs to do, in order:

1. **Pin `checkpoint_id`/`checkpoint_sha256` for real** (done — see §3;
   `tools/bootstrap_pod.sh` now derives both automatically and writes them
   to `/workspace/bringup.env`). `TerrainDiffusionBackend` assumes
   `checkpoint_id` is a **local filesystem path** to a pre-downloaded
   `WorldPipeline` snapshot (top-level `config.json` + `coarse_model/`,
   `base_model/`, `decoder_model/` subfolders, each `config.json` +
   `*.safetensors` — confirmed from `world_pipeline.py`'s `from_pretrained`
   and `save_pretrained`), NOT a bare HuggingFace repo id — doctrine
   requires the sha256 check to run *before* any load, and a bare repo id
   can't be hashed before terrain-diffusion downloads it itself. Download a
   snapshot locally (e.g. `huggingface_hub.snapshot_download("xandergos/
   terrain-diffusion-30m", local_dir=...)`), compute its sha256 (`_sha256_
   of_checkpoint_path` in `diffusion.py` hashes a directory as a manifest of
   every file's path+sha256 — you can call it directly, or just run
   `verify_checkpoint_sha256` once with a throwaway expected value to see
   the "actual" hash it reports), and pin both fields into `DiffusionConfig`.
   If a local snapshot proves inconvenient, pinning HF's commit sha instead
   of a content sha256 is the fallback to evaluate — flagged as an
   ASSUMPTION on `TerrainDiffusionBackend._load_pipeline`.
2. Run step 5 below (`validate_model_output` against one real tile).
3. Confirm/adjust the ASSUMPTIONs marked inline on `TerrainDiffusionBackend`
   (grep the class for `# ASSUMPTION:`) — in priority order:
   - **Axis mapping — CONFIRMED CORRECT (2026-07-19), no change needed.**
     `(x, y)` -> `WorldPipeline.get(i1, j1, ...)` with `i1=y*512` (row),
     `j1=x*512` (col) is right. Measured seam-vs-interior gradient ratio
     across a tile boundary: **1.004**, i.e. the boundary is statistically
     indistinguishable from the tile interior. No axis flip. This question
     is closed; do not re-open it without new evidence.
   - **Scale semantics**: `DiffusionConfig.scale` (1 => 30m/px, 8 =>
     3.75m/px in our `tile_codec.py`) is passed straight through as
     terrain-diffusion's own upsample `scale` factor (relative to the
     pinned checkpoint's `native_resolution` config field). Confirm the
     pinned checkpoint's `native_resolution` actually makes `scale=1` mean
     "no upsampling" (i.e. native 30m/px) before trusting scale=1 output;
     bring up scale=8 as a separate `DiffusionConfig`/`provider_id` per the
     note in step 3 above, once scale=1 is confirmed good.
   - **Per-tile reseed cost**: `TerrainDiffusionBackend` calls `WorldPipeline
     .change_seed()` whenever the requested `seed` differs from the
     pipeline's currently-bound seed (expensive — rebuilds the tile
     hierarchy). Fine for one world seed per server process (the expected
     production shape), but confirm pregen/serving patterns don't
     interleave multiple seeds against one process.

## 5. Confirm real tile outputs — DONE, and one thing was wrong

**Answered on 2026-07-19.** The channel *set* was right; the **units were
not**. Climate channels come out as **raw WorldClim bioclim units**, not
normalised to `[0, 1]` as originally assumed. `EXPECTED_CHANNELS` has been
corrected accordingly — this is exactly the "config edit, not a redesign"
outcome the two-outcome branch below anticipated.

Per-tile inference measured at **22.5 s on a 4090** (25 tiles ≈ 9.4 min,
289 ≈ 108 min). This is the number the pregen budget comes from.

`tools/probe_tile.py` is this step, as a file rather than a pasted snippet
(pasting multi-line heredocs into a web terminal silently re-indents them and
hangs the shell on an `EOF` that never matches — this is why every step in
the bring-up is now a committed file). It is run automatically as the final
gate of `tools/bootstrap_pod.sh`. The equivalent by hand:

```python
from terrain_service.providers.diffusion import DiffusionProvider, validate_model_output, ModelOutputMismatch

provider = DiffusionProvider(config=config, dry_run=False)
raster = provider._call_model(seed=1, x=0, y=0, scale=1)   # raw model output, pre-adapter

try:
    validate_model_output(raster, config.channel_mapping)
    print("MATCHES our assumption — no adaptation needed")
except ModelOutputMismatch as e:
    for issue in e.issues:
        print("-", issue)
```

Two outcomes:

- **Matches**: proceed to step 6 as-is.
- **Differs** (different channel count/names/dtype/ranges): this is a
  `DiffusionConfig.channel_mapping` edit (if it's a naming/count mismatch —
  e.g. the model calls it `precip_var` not `precip_variability`, or splits
  temperature into day/night) or an `EXPECTED_CHANNELS` + `adapt_raster_to_tile`
  edit (if it's a genuinely different channel *set*, e.g. no seasonality
  channel at all). Either way it's a config/provider-layer change, not a
  redesign — see `docs/m4-plan.md`'s "HARD DEPENDENCY before biome tuning"
  section, which this step directly closes out. Update `docs/m4-plan.md` and
  `docs/status.md`'s backlog row with what you found.

Once `validate_model_output` passes, sanity-check the adapted tile visually
(dump `tile.elevation`/`tile.climate` as a PNG or just eyeball min/max/mean)
before trusting the full pregen run in step 6 — validation catches
structural mismatches, not "the mountains are inverted."

## 6. Golden-tile pin + pregen the launch radius

```python
import hashlib
from terrain_service import tile_codec

tile = provider.generate(seed=1, x=0, y=0, scale=1)
print(hashlib.sha256(tile_codec.encode(tile)).hexdigest())   # pin this, generating machine only
```

Record that hash as a comment near `config` (informational only — unlike the
synthetic provider's `GOLDEN_SHA256`, diffusion output is NOT required to
reproduce cross-machine; cache-distribution is what makes that a non-issue,
per the module docstring).

**Do not pregen at the origin without scanning first.** Tile (0,0) of seed
20260719 is **entirely underwater** (elevation max −8.9 m), so an
origin-centred radius-2 pregen buys 25 tiles of open ocean. Every new seed
needs a land scan before any pregen is paid for.

`tools/generate_world.sh` does scan → choose origin → pregen → package in one
command. It uses `tools/scan_land.py` (strided sampling — one tile is 15.4 km
across, so stride 3 reaches ~230 km for 25 samples) and `tools/pick_origin.py`
to score for a **coastal** origin. Note that ranking by raw land fraction, the
obvious choice, is actively wrong: it selects the tile *furthest from water*,
i.e. a featureless plateau with no coast and no river mouths.

The `pregen` CLI is deliberately not used here: `--provider diffusion` would
construct the UNPINNED default config and be refused by the sha256 gate.
Wiring pinned-config selection into `_make_provider` in `app.py` remains a
follow-up.

Copy/sync `./tile-cache` off the GPU box (it's just files, content-addressed
by provider_id/seed/scale/x/y — see `cache.py`) to wherever the production
Flask server's `TERRAIN_CACHE_DIR` lives, or point the server directly at a
shared volume/bucket.

## 7. Shut the GPU down

Stop (not just pause) the rented instance. If it's a scale-to-zero
serverless deployment left running for production, confirm it actually
scales to zero on idle before walking away — the cost model above assumes
it does.

Also: **destroy** the Vast instance rather than stopping it. Storage bills
for as long as the instance exists. Rent with **≥ 60 GB disk** — the size is
fixed at rental time and cannot be grown afterwards.

## Session budget

The first session (2026-07-19) took **~6 hours**, almost all of it
archaeology: the failed `pip install -e`, the training-requirements
download, the CUDA mismatch, the `MODEL_PATHS` misreading, the ETOPO
filename, the WorldClim stdin prompt, a hung heredoc paste, and a bracketed
sha256. Every one of those is now handled by
`tools/bootstrap_pod.sh`.

A repeat run on a new seed should be **~20–40 min of mostly-download
bring-up** plus **~9 min scan + ~9 min pregen** at radius 2 (or ~108 min at
radius 8), driven by two pasted commands — or by `ssh` from the dev machine
with no terminal at all. See `docs/pod-bringup-commands.md`.
