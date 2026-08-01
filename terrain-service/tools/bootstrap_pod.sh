#!/usr/bin/env bash
#
# Take a fresh Vast.ai / Runpod PyTorch pod from nothing to a VALIDATED
# terrain-diffusion inference environment, in one pasted command.
#
#   COMMAND 1 (paste this on the pod):
#     git clone https://github.com/mnoles1911/voxelsim /workspace/voxelsim \
#       || git -C /workspace/voxelsim pull; \
#     bash /workspace/voxelsim/terrain-service/tools/bootstrap_pod.sh
#
# Everything here is a FILE, never a heredoc: pasting a multi-line heredoc
# into a browser terminal silently re-indents it, and the shell then hangs
# forever waiting on a closing EOF that will never match. That cost us a
# session on 2026-07-19.
#
# Properties this script is required to have:
#   * IDEMPOTENT / RESTARTABLE -- every expensive step drops a stamp file in
#     $STAMPS. Re-running after a dropped connection skips completed work.
#     Deleting a stamp forces that one step to redo.
#   * NON-INTERACTIVE -- nothing reads stdin. pip gets --no-input; the
#     WorldClim fetch (which prompts for consent on stdin and would hang an
#     unattended run forever) is fed from `yes`.
#   * FAILS LOUDLY -- it ends by running a real one-tile inference through
#     probe_tile.py and exits non-zero if validation does not pass. A pod
#     that reports success has actually generated a tile.
#
# Overridable by environment:
#   WORK          workspace root                 (default /workspace)
#   CKPT_REPO     HF bundle id                   (default xandergos/terrain-diffusion-30m)
#   PROBE_SEED    seed used for the probe tile   (default 20260719)
#   ALLOW_SMALL_DISK=1   skip the 60 GB check (you will regret this)

set -euo pipefail

WORK="${WORK:-/workspace}"
CKPT_REPO="${CKPT_REPO:-xandergos/terrain-diffusion-30m}"
PROBE_SEED="${PROBE_SEED:-20260719}"
MIN_DISK_GB="${MIN_DISK_GB:-60}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"          # terrain-service/
REPO_ROOT="$(cd "$TS_DIR/.." && pwd)"

TD_DIR="$WORK/terrain-diffusion"
CKPT_DIR="$WORK/ckpt/$(basename "$CKPT_REPO")"
STAMPS="$WORK/.bringup"
ENV_FILE="$WORK/bringup.env"
LOG="$WORK/bringup.log"

mkdir -p "$STAMPS" "$WORK/ckpt"

# ---------------------------------------------------------------------------
# plumbing
# ---------------------------------------------------------------------------
RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; BLD=$'\033[1m'; RST=$'\033[0m'

say()  { printf '%s\n' "${BLD}==> $*${RST}"; }
ok()   { printf '%s\n' "${GRN}    ok: $*${RST}"; }
warn() { printf '%s\n' "${YLW}    warning: $*${RST}"; }
die()  { printf '%s\n' "${RED}${BLD}FAILED: $*${RST}" >&2; exit 1; }

have() { [ -f "$STAMPS/$1" ]; }
mark() { touch "$STAMPS/$1"; }

# Tee everything to a log so a dropped terminal does not lose the diagnosis.
exec > >(tee -a "$LOG") 2>&1
say "bootstrap starting $(date -u +%FT%TZ)  (log: $LOG)"

# ---------------------------------------------------------------------------
# 1. preflight -- fail on things that cannot be fixed later
# ---------------------------------------------------------------------------
say "1/8 preflight"

command -v python3 >/dev/null || die "no python3 on this image."
command -v git     >/dev/null || die "no git on this image."

if ! command -v nvidia-smi >/dev/null; then
  die "nvidia-smi not found -- this is not a GPU pod. Destroy and re-rent."
fi
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader \
  || die "nvidia-smi failed. The GPU is not usable; destroy and re-rent."

# Vast disk is chosen at rental time and CANNOT be grown afterwards, so a
# small disk is a re-rent, not a fixable problem. Better to learn that in
# the first 10 seconds than 40 minutes into a checkpoint download.
avail_gb=$(df -BG --output=avail "$WORK" | tail -1 | tr -dc '0-9')
if [ "${avail_gb:-0}" -lt "$MIN_DISK_GB" ]; then
  if [ "${ALLOW_SMALL_DISK:-0}" = "1" ]; then
    warn "only ${avail_gb} GB free on $WORK; continuing because ALLOW_SMALL_DISK=1"
  else
    die "only ${avail_gb} GB free on $WORK, need >= ${MIN_DISK_GB} GB.
    Vast disk size is fixed at rental time and cannot be resized. Destroy
    this instance and re-rent with >= ${MIN_DISK_GB} GB.
    (Set ALLOW_SMALL_DISK=1 to override, at your own risk.)"
  fi
fi
ok "GPU present, ${avail_gb} GB free on $WORK"

# ---------------------------------------------------------------------------
# 2. terrain-diffusion source
# ---------------------------------------------------------------------------
say "2/8 terrain-diffusion source"
# PINNED, not --depth 1 of whatever main happens to be. Two reasons:
#   * the patch below applies to exactly this tree, and a moved upstream would
#     make it fail (loudly now -- see the die) or, worse, apply with fuzz;
#   * "the world depends on an unpinned third-party HEAD" is the same class of
#     bug the conditioning digest exists to prevent.
# Bump both the SHA and the patch together, never one alone.
TD_COMMIT="82a0431"
TD_PATCH="$TS_DIR/patches/terrain-diffusion-worldgen.patch"

if have clone && [ -d "$TD_DIR/terrain_diffusion" ]; then
  ok "already cloned at $TD_DIR"
else
  rm -rf "$TD_DIR"
  git clone https://github.com/xandergos/terrain-diffusion "$TD_DIR" \
    || die "clone of terrain-diffusion failed (network?)."
  git -C "$TD_DIR" checkout --quiet "$TD_COMMIT" \
    || die "terrain-diffusion has no commit $TD_COMMIT -- upstream rewrote
    history, or the pin is wrong. Do not proceed on whatever main is now:
    the worldgen patch was written against $TD_COMMIT."

  # OUR worldgen changes: the orographic rain shadow and the elevation tail
  # stretch. They live here rather than in a fork because voxelsim is the repo
  # that owns the decision; the fork would be one more thing to keep in sync.
  #
  # THIS MUST NOT BE SKIPPED SILENTLY. Upstream's WorldPipeline.__init__ ends
  # in **deprecated_kwargs, which reads only histogram_raw -- so if the patch
  # is absent, `orographic=` and `elev_gain=` are ACCEPTED AND DISCARDED. The
  # pod would generate a world with no rain shadow and unstretched relief,
  # stamped with a provider_id that says otherwise. There is no way to tell
  # from the tiles. Hence: hard failure, not a warning.
  [ -f "$TD_PATCH" ] || die "missing $TD_PATCH -- the worldgen patch is part of
    this repo; a checkout that lacks it cannot produce the shipping world."
  git -C "$TD_DIR" apply --check "$TD_PATCH" \
    || die "worldgen patch does not apply cleanly to $TD_COMMIT.
    Regenerate it against the pinned commit; do NOT force it."
  git -C "$TD_DIR" apply "$TD_PATCH" \
    || die "worldgen patch failed to apply after --check passed (disk?)."

  mark clone
  ok "cloned to $TD_DIR at $TD_COMMIT + worldgen patch"
fi

# NOTE: do NOT `pip install -e $TD_DIR`. The repo ships no setup.py and no
# pyproject.toml, so that command fails outright. It is imported by path:
export PYTHONPATH="$TD_DIR:${PYTHONPATH:-}"

# ---------------------------------------------------------------------------
# 3. inference dependencies
# ---------------------------------------------------------------------------
say "3/8 inference dependencies"

# NOT `pip install -r $TD_DIR/requirements.txt`. That file is the TRAINING
# stack -- wandb, optuna, cartopy, earthengine, lpips and friends. On
# 2026-07-19 it crawled at 29 kB/s and killed the session outright. None of
# it is needed to run inference.
#
# This list is instead everything the inference path actually imports,
# discovered one traceback at a time during that session. torch and
# torchvision are deliberately ABSENT: they are installed together in step 4
# from a CUDA-matched index, and installing them here from PyPI would just
# have to be undone.
DEPS=(
  diffusers accelerate huggingface_hub safetensors
  numpy scipy h5py numba einops ema_pytorch omegaconf
  tqdm pillow scikit-image rasterio matplotlib
  infinite-tensor pyfastnoiselite
  flask pytest
)

if have deps; then
  ok "already installed (rm $STAMPS/deps to force)"
else
  python3 -m pip install --no-input --upgrade pip >/dev/null || true
  python3 -m pip install --no-input "${DEPS[@]}" \
    || die "dependency install failed. Re-run this script; it resumes here.
    If one package is the problem, install it alone to see the real error."
  mark deps
  ok "installed ${#DEPS[@]} packages"
fi

# ---------------------------------------------------------------------------
# 4. CUDA-correct torch -- the expensive-to-miss step
# ---------------------------------------------------------------------------
say "4/8 CUDA-correct torch"

cuda_ok() {
  python3 -c "import torch, torchvision; import sys; sys.exit(0 if torch.cuda.is_available() else 1)" 2>/dev/null
}

if cuda_ok; then
  ok "torch $(python3 -c 'import torch; print(torch.__version__)') already sees the GPU"
  mark torch
else
  # The stock Vast PyTorch image shipped torch 2.13.0+cu130 against a 12.8
  # host driver. That does not crash -- cuda.is_available() just returns
  # False and every tile silently renders on CPU while the 4090 bills.
  warn "torch cannot see the GPU; reinstalling from a driver-matched index"
  mapfile -t INDICES < <(python3 "$SCRIPT_DIR/cuda_index.py") \
    || die "could not determine a wheel index from nvidia-smi."

  installed=0
  for idx in "${INDICES[@]}"; do
    say "    trying $idx"
    # torch AND torchvision from the SAME index, in ONE command. Installing
    # torchvision separately (or from PyPI) drags a non-CUDA torch back in
    # as a dependency and silently undoes this whole step.
    if python3 -m pip install --no-input --force-reinstall \
         torch torchvision --index-url "$idx"; then
      if cuda_ok; then
        ok "cuda works with $idx"
        installed=1
        break
      fi
      warn "installed from $idx but cuda.is_available() is still False"
    else
      warn "pip install from $idx failed"
    fi
  done
  [ "$installed" = "1" ] || die "no wheel index produced a working CUDA torch.
    Tried: ${INDICES[*]}
    Check 'nvidia-smi' reports a CUDA Version banner, and that the host
    driver is not older than every published wheel."
  mark torch
fi

python3 -c "import torch; print('    torch', torch.__version__, '| cuda', torch.version.cuda, '|', torch.cuda.get_device_name(0))"

# ---------------------------------------------------------------------------
# 5. environment file
# ---------------------------------------------------------------------------
say "5/8 environment file"
# Rewritten every run (cheap, and keeps it correct if paths moved).
{
  echo "# generated by bootstrap_pod.sh $(date -u +%FT%TZ)"
  echo "export PYTHONPATH=\"$TD_DIR:\${PYTHONPATH:-}\""
  echo "export TS_DIR=\"$TS_DIR\""
  echo "export CKPT_DIR=\"$CKPT_DIR\""
  echo "export WORK=\"$WORK\""
} > "$ENV_FILE"
ok "wrote $ENV_FILE"

# Everything from here runs from terrain-service/. This is not cosmetic:
# WorldClim and ETOPO are both opened by terrain-diffusion through RELATIVE
# paths (data/global/...), so the current directory decides whether they are
# found at all.
cd "$TS_DIR"

# ---------------------------------------------------------------------------
# 6. checkpoint
# ---------------------------------------------------------------------------
say "6/8 checkpoint bundle"
if have ckpt && [ -d "$CKPT_DIR/coarse_model" ]; then
  ok "already at $CKPT_DIR"
else
  python3 tools/fetch_checkpoint.py --repo "$CKPT_REPO" --dest "$CKPT_DIR" \
    || die "checkpoint download/verify failed. Re-run to resume the download
    (snapshot_download reuses what is already on disk)."
  mark ckpt
fi

SHA="$(python3 tools/checkpoint_sha256.py "$CKPT_DIR")" \
  || die "could not hash the checkpoint at $CKPT_DIR"
[ "${#SHA}" = "64" ] || die "checkpoint sha256 is not 64 chars: '$SHA'"
echo "export CKPT_SHA=\"$SHA\"" >> "$ENV_FILE"
ok "checkpoint sha256 $SHA"

# ---------------------------------------------------------------------------
# 7. WorldClim + ETOPO reference rasters
# ---------------------------------------------------------------------------
say "7/8 reference rasters (WorldClim + ETOPO)"
mkdir -p data/global

if have rasters && [ -f data/global/etopo_10m.tif ]; then
  ok "data/global/etopo_10m.tif already built"
else
  # WorldClim auto-downloads on first pipeline run and PROMPTS FOR CONSENT
  # on stdin -- which hangs an unattended script forever. `yes` answers it.
  #
  # CALL THE DOWNLOADER DIRECTLY, do not run an inference probe for its side
  # effect. This used to be `probe_tile.py ... || true`, relying on the probe
  # failing LATE -- after _compute_map_stats had fetched WorldClim on its way
  # to the missing ETOPO file. That stopped working when identity schema v2
  # (2026-07-22) added the conditioning gate: probe_tile.py builds a
  # DiffusionConfig with no conditioning_digest, so it defaults to UNVERIFIED
  # and verify_conditioning_digest refuses BEFORE any download happens. The
  # `|| true` swallowed it and the next line died with "WorldClim did not
  # land", with nothing in the log explaining why. A fresh pod could not
  # bootstrap at all.
  #
  # _ensure_wc_files is the function whose entire job is this download. It
  # has no identity gate and nothing to regress when the schema changes again.
  if [ ! -f data/global/wc2.1_10m_bio_1.tif ]; then
    say "    downloading WorldClim (a few hundred MB)"
    yes | timeout 3600 python3 -c \
      'from terrain_diffusion.inference.synthetic_map import _ensure_wc_files; _ensure_wc_files()' \
      || true
  fi
  [ -f data/global/wc2.1_10m_bio_1.tif ] \
    || die "WorldClim did not land in $TS_DIR/data/global/.
    The warm-up run should have fetched it. Check $LOG for what it did
    instead -- if it prompted for something other than consent, answer it
    once by hand with:  cd $TS_DIR && python3 tools/probe_tile.py $CKPT_DIR $SHA"

  # terrain-diffusion's synthetic_map._compute_map_stats opens the RELATIVE
  # path data/global/etopo_10m.tif. Its README tells you to download NOAA's
  # 30 arc-SECOND GeoTIFF, but the filename it reads is a 10 arc-MINUTE
  # downsample that NOAA does not publish -- so it has to be built.
  python3 tools/fetch_etopo.py \
    || die "ETOPO build failed. tools/fetch_etopo.py prints a manual
    download fallback; follow it, then re-run this script."
  mark rasters
  ok "data/global/etopo_10m.tif built"
fi

# ---------------------------------------------------------------------------
# 8. validation -- the gate
# ---------------------------------------------------------------------------
say "8/8 validation probe (one real tile)"
# Deliberately NOT stamped. This is the whole point of the script; it runs
# on every invocation so a "success" message always means a tile was really
# generated on the GPU by the code as it stands right now.
if ! PROBE_SEED="$PROBE_SEED" python3 tools/probe_tile.py "$CKPT_DIR" "$SHA"; then
  die "validation probe did NOT pass -- do not pregen yet.
    * exit 3  => torch cannot see the GPU (see step 4; rm $STAMPS/torch and re-run)
    * MISMATCH lines => EXPECTED_CHANNELS / channel_mapping needs adjusting in
      terrain_service/providers/diffusion.py. This is a config edit, not a
      redesign. Paste the issue lines back to Claude.
    * anything else => read $LOG"
fi

# ---------------------------------------------------------------------------
# done
# ---------------------------------------------------------------------------
printf '\n%s\n' "${GRN}${BLD}================ POD READY ================${RST}"
printf '  checkpoint : %s\n' "$CKPT_DIR"
printf '  sha256     : %s\n' "$SHA"
printf '  env file   : %s   (source it in any new shell)\n' "$ENV_FILE"
printf '  log        : %s\n' "$LOG"
printf '\n%s\n\n' "${BLD}COMMAND 2 -- scan for land, pregen the launch grid, package it:${RST}"
printf '  bash %s/generate_world.sh --seed %s\n\n' "$SCRIPT_DIR" "$PROBE_SEED"
printf '%s\n' "  Add --origin X,Y to override the automatic pick, or --radius N to change"
printf '%s\n' "  the grid size. It is restartable: cached tiles are skipped on re-run."
printf '%s\n\n' "  Budget ~9 min for the scan + ~9 min for a radius-2 pregen at 22.5 s/tile."
