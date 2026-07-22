# GPU pod bring-up — the runbook

Generate terrain-diffusion tiles for a seed on a rented GPU. **Two commands.**

This replaces an earlier version of this file that was written before any
real run; most of what it guessed turned out to be wrong. Everything below
is from the 2026-07-19 session, which took ~6 hours, almost all of it
archaeology. None of that should ever be repeated.

Narrative background: `terrain-service/docs/diffusion-bringup.md`.

---

## Rent the pod

Vast.ai (or Runpod), **RTX 4090, PyTorch template, disk ≥ 60 GB**.

> **The disk size is chosen at rental time and cannot be grown later.** Too
> small means destroying the instance and starting over. 60 GB is the floor.

Cost is roughly $0.35–0.70/hr; the whole run below is well under an hour of
GPU time once the downloads are done.

---

## Command 1 — bring the pod up

Paste this into the pod's terminal:

```sh
git clone https://github.com/mnoles1911/voxelsim /workspace/voxelsim || git -C /workspace/voxelsim pull; bash /workspace/voxelsim/terrain-service/tools/bootstrap_pod.sh
```

Takes ~20–40 min, mostly downloading. It is **restartable** — if the browser
terminal drops, paste the exact same line again and it skips what finished.

It does, in order, and says so as it goes:

1. **Preflight** — GPU present, ≥ 60 GB free. Fails immediately if not.
2. **Clones terrain-diffusion** to `/workspace/terrain-diffusion`.
3. **Installs the inference deps** (~20 packages).
4. **Reinstalls torch + torchvision** from the wheel index matching the
   host driver.
5. **Writes `/workspace/bringup.env`** — paths and the checkpoint hash.
6. **Downloads the checkpoint** `xandergos/terrain-diffusion-30m`.
7. **Builds the reference rasters** (WorldClim, then ETOPO).
8. **Generates one real tile and validates it.** This is the gate.

Success looks like `================ POD READY ================` followed by
the checkpoint sha256 and the exact text of command 2. **If you do not see
that banner, command 2 is not safe to run.**

Full log: `/workspace/bringup.log`.

---

## Command 2 — scan, pregen, package

The bring-up banner prints this line with everything filled in. It is:

```sh
bash /workspace/voxelsim/terrain-service/tools/generate_world.sh --seed 20260719
```

~9 min to scan for land, then ~9 min to pregen 25 tiles. Also restartable —
completed tiles come from the cache on a re-run.

1. **Scans** 25 strided tiles (~230 km reach) and prints an ASCII land map.
2. **Picks a coastal origin** and prints the scored ranking.
3. **Pregens** a 5×5 grid there, writing a manifest with `provider_id`, the
   checkpoint hash and per-tile hashes.
4. **Packages** it to `/workspace/tile-cache-seed<SEED>.tar.gz`.

Useful flags:

| flag | why |
|---|---|
| `--origin X,Y` | override the automatic pick — skips the scan entirely |
| `--radius 8`   | 289 tiles instead of 25 (~108 min) |
| `--scan-radius 3 --stride 5 --rescan` | if the scan found no land |

### Why it auto-picks the origin

**Something must pick.** Tile (0,0) of seed 20260719 is entirely underwater
(max −8.9 m), so "just use the origin" buys 25 tiles of ocean. Every new seed
has to be scanned.

**But highest-land-fraction is the wrong answer** — it selects the tile
furthest from any water, i.e. a featureless plateau with no coast, no river
mouth, nothing to navigate by. So `tools/pick_origin.py` scores for a
*coastal* tile (land fraction near 0.70) with real relief in a land-heavy
neighbourhood.

It auto-picks **by default** so a rented GPU never sits idle waiting on a
human. **You still get the final say**: the land map and the full ranking are
printed before anything is spent, and re-running with `--origin X,Y` is cheap
because overlapping tiles come from the cache. If the map looks interesting
somewhere the scorer didn't pick — an archipelago, a big bay — take it.

---

## Then: download, and destroy the pod

From the **dev machine**:

```powershell
scp -P <pod-ssh-port> root@<pod-host>:/workspace/tile-cache-seed20260719.tar.gz D:\voxelsim\
tar xzf D:\voxelsim\tile-cache-seed20260719.tar.gz -C D:\voxelsim
```

Then **destroy** the instance — not stop, not idle. Vast bills storage for as
long as the instance exists.

Point the game at the cache leaf directory (`provider_id` and the seed-hex
directory are both in `manifest.json`):

```powershell
& "D:\UE5\Engine\Binaries\Win64\UnrealEditor.exe" "D:\voxelsim\ue-project\VoxelEarth.uproject" `
  -game -windowed -resx=1280 -resy=720 -log -unattended -nosplash `
  -VoxelTileDir="D:\voxelsim\tile-cache\<provider_id>\000000000135276f\s1" `
  -VoxelScreenshotAfter=25
```

(`000000000135276f` is hex for seed 20260719; pass `-VoxelSeed=` for another.)

---

## You can skip the terminal entirely

Both commands are ordinary non-interactive shell scripts that read nothing
from stdin, so a future session doesn't need the browser terminal at all.
Given the pod's SSH host and port from the Vast dashboard, the orchestrator
can drive the whole bring-up from the dev machine:

```powershell
ssh -p <port> root@<host> 'git clone https://github.com/mnoles1911/voxelsim /workspace/voxelsim || git -C /workspace/voxelsim pull; bash /workspace/voxelsim/terrain-service/tools/bootstrap_pod.sh'
ssh -p <port> root@<host> 'bash /workspace/voxelsim/terrain-service/tools/generate_world.sh --seed <SEED>'
scp -P <port> root@<host>:/workspace/tile-cache-seed<SEED>.tar.gz D:\voxelsim\
```

This is strictly better: output lands in your own scrollback, Claude can read
the failures directly, and there is no paste step to corrupt anything. **Hand
Claude the SSH host and port and it can run the entire session.**

---

## When something fails

Read `/workspace/bringup.log`. Then:

| symptom | what it is | fix |
|---|---|---|
| `nvidia-smi not found` | not a GPU pod | destroy, re-rent |
| `only N GB free` | disk too small | destroy, re-rent ≥ 60 GB — it cannot be resized |
| probe exits **3**, `cuda.is_available() is False` | torch/driver mismatch | `rm /workspace/.bringup/torch` and re-run command 1 |
| `MISMATCH -- channel_mapping needs adjusting` | model channels differ from `EXPECTED_CHANNELS` | a config edit in `providers/diffusion.py`, not a redesign — paste the issue lines to Claude |
| `WorldClim did not land` | the consent prompt wasn't answered | run `cd /workspace/voxelsim/terrain-service && python3 tools/probe_tile.py $CKPT_DIR $CKPT_SHA` once by hand |
| ETOPO build failed | NOAA moved the file again | `tools/fetch_etopo.py` prints a manual-download fallback |
| `no tile in the scan had usable land` | seed's neighbourhood is ocean | `--scan-radius 3 --stride 5 --rescan` |
| shell hangs with no output | you pasted a heredoc | Ctrl-C. Never paste heredocs — see below |

Any step can be forced to redo by deleting its stamp from
`/workspace/.bringup/` and re-running command 1.

---

## Confirmed numbers

Measured, not estimated. Don't re-derive these.

- **22.5 s per tile** on a 4090 → 25 tiles ≈ 9.4 min, 289 tiles ≈ 108 min.
- Tiles are ~1.5 MB each.
- One tile is 512 px × 30 m = **15.4 km** across.
- Tile **(0,0) of seed 20260719 is entirely underwater** (max −8.9 m).
- Tile boundaries are **continuous** — seam-vs-interior gradient ratio
  **1.004**. No axis flip is needed; that open question is closed.
- Climate channels are **raw WorldClim bioclim units**, not `[0,1]`. Already
  fixed in `EXPECTED_CHANNELS`.
- `scale` is a **supersample** knob: 1 → 30 m/px, 8 → 11.25 m/px. **A larger
  scale covers LESS ground.** Bring up scale 8 as a separate
  `DiffusionConfig` / `provider_id` if wanted.

---

## Traps, and why the scripts are shaped the way they are

Each of these cost real time on 2026-07-19. They are all handled
automatically now; this list exists so nobody "helpfully" undoes one.

1. **`pip install -e ./terrain-diffusion` fails.** The repo has no `setup.py`
   and no `pyproject.toml`. It is imported by path:
   `export PYTHONPATH=/workspace/terrain-diffusion:$PYTHONPATH`.
2. **Never run terrain-diffusion's `requirements.txt`.** It is the *training*
   stack — wandb, optuna, cartopy, earthengine, lpips. It crawled at 29 kB/s
   and killed a session outright. The real inference deps are the ~20
   packages listed in `bootstrap_pod.sh`.
3. **The image's torch is probably wrong for the driver.** The Vast image
   shipped `torch 2.13.0+cu130` on a **12.8** driver. This does *not* crash —
   `cuda.is_available()` just returns `False` and every tile silently renders
   on **CPU**, at hours per tile, while a 4090 bills. `cuda_index.py` reads
   the driver from `nvidia-smi` and picks a matching wheel index.
   **torchvision must be installed from the same `--index-url`, in the same
   command**, or it drags a PyPI torch back in and undoes the fix.
4. **`MODEL_PATHS` is a red herring.** It lists three separate *training*
   repos, which reads as "download all three". It is not.
   `WorldPipeline.from_pretrained` takes **one** path and pulls
   `coarse_model` / `base_model` / `decoder_model` out of it via
   `subfolder=`. The bundle is **`xandergos/terrain-diffusion-30m`**. A
   `-90m` bundle also exists — different resolution, not a newer version.
5. **ETOPO must be built, not downloaded.** `_compute_map_stats` opens the
   relative path `data/global/etopo_10m.tif`. The README says to download
   NOAA's 30 arc-**second** GeoTIFF, but the filename it reads is a 10
   arc-**minute** downsample NOAA doesn't publish. `tools/fetch_etopo.py`
   resamples one onto the WorldClim grid.
6. **WorldClim prompts for consent on stdin** on first run, which hangs any
   unattended script forever. The bootstrap feeds it from `yes`.
7. **Everything must run from `terrain-service/`.** Both WorldClim and ETOPO
   resolve against the *current directory*.
8. **Never paste a heredoc into a web terminal.** It gets silently
   re-indented, so the closing `EOF` never matches and the shell hangs
   forever with no output. This is why every step is a file in the repo.
9. **Never paste the sha256 by hand.** On 2026-07-19 a placeholder went in
   with its angle brackets still attached (`checkpoint_sha256="<ed06...>"`).
   A wrong hash **does not fail loudly** — it silently changes `provider_id`,
   which is both the tile-cache namespace *and* the edit-log provider stamp.
   The scripts read it from `bringup.env`; the tools reject bracketed or
   wrong-length values.
