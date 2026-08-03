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
#   EXPECT_PROVIDER_ID   the world this pod must be able to EXTEND. Step 7
#                        computes the provider_id this pod's artifacts +
#                        checkpoint would generate, and DIES if it differs.
#                        Set it whenever the pod is meant to add tiles to an
#                        existing world -- the alternative is finding out after
#                        the bake, when the only remaining "fix" is
#                        --provider-id-override, which is not a fix: it puts
#                        two different planets in one namespace with a seam in
#                        the middle and no error anywhere.

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
  # --ignore-installed is NOT optional on a Debian/Ubuntu system python.
  #
  # Ubuntu ships some of these (blinker, and whatever else the base image's
  # apt layer pulled in) into /usr/lib/python3/dist-packages WITHOUT the
  # .dist-info RECORD file pip needs to uninstall a package. When pip decides
  # it must upgrade one, it aborts the WHOLE transaction with
  #     Cannot uninstall blinker 1.7.0, RECORD file not found.
  #     Hint: The package was installed by debian.
  # after having already downloaded everything -- which is how this failed on
  # 2026-08-01, ~10 minutes and 2.5 GB in.
  #
  # --ignore-installed sidesteps it by installing fresh copies into
  # /usr/local/lib/python3.*/dist-packages, which precedes dist-packages on
  # sys.path, instead of trying to remove the apt-owned ones. It costs a few
  # redundant downloads and is immune to whichever packages the next base
  # image happens to apt-install. Targeting the one offending package by name
  # would just move the failure to the next image.
  python3 -m pip install --no-input --ignore-installed "${DEPS[@]}" \
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
# 7. conditioning data -- PINNED BYTES, verified, or this pod stops here
#
# This step used to BUILD two of the six files compute_conditioning_digest()
# hashes, and that is why a world could not be extended by a second machine.
# Measured on a fresh pod:
#
#     etopo_10m.tif            9,344,975 B here vs 8,442,844 B on the pod
#     synthetic_map_stats.json    12,297 B      vs    12,570 B
#
# -- which moves the conditioning digest, which moves provider_id, which makes
# the new tiles a different world.
#
# The 2026-08-02 diagnosis went further than "the build drifts", and the
# finding changed the fix (docs/measurements/etopo-build-not-reproducible-
# 2026-08-02.txt, and the module docstring of
# terrain_service/conditioning_artifacts.py):
#
#   THE SHIPPING WORLD'S etopo_10m.tif WAS NEVER A BUILD OUTPUT. It is
#   uncompressed (2160x1080 float32 = 9,331,200 B + 13,775 B of tags is exactly
#   its length) and carries ETOPO's own vertical datum -- "WGS 84 + EGM2008
#   height", GDAL_NODATA -99999 -- neither of which fetch_etopo.py can emit,
#   because it copies its profile from wc2.1_10m_bio_1.tif and writes deflate.
#   Its mtime also predates fetch_etopo.py's first commit by 24 days.
#
# So pinning the build was never going to recover it. The two files have to be
# DOWNLOADED as artifacts, exactly the way the checkpoint already is, and that
# is what tools/fetch_conditioning.py does: hash what is here against
# data/conditioning-artifacts.json, fetch what is missing from the pinned URLs,
# verify the sha256 of what arrived, and EXIT NON-ZERO -- naming every file and
# both hashes -- if the result is not byte-for-byte the pinned set.
#
# THE HOSTING DECISION IS STILL OPEN, on purpose. The "sources" arrays for the
# two built artifacts are empty and fetch_conditioning.py FAILS LOUDLY with the
# options rather than picking one: a ~9 MB binary committed to this repo is
# permanent in every clone forever. See "hosting_decision_required" in
# data/conditioning-artifacts.json for the recommendation and what it costs.
# Until it is filled in, a fresh pod cannot reproduce the pinned world -- and
# it now SAYS SO instead of quietly building a second planet.
# ---------------------------------------------------------------------------
say "7/8 conditioning data (pinned artifacts)"
mkdir -p data/global

# WorldClim first: its four rasters ARE reproducible downloads and upstream's
# own fetcher is the only published way to get them (they ship as one zip, so
# there is no per-file URL to pin -- the sha256 pins are the check on what the
# fetcher produced, not a substitute for it).
#
# It auto-downloads on first pipeline run and PROMPTS FOR CONSENT on stdin --
# which hangs an unattended script forever. `yes` answers it.
#
# CALL THE DOWNLOADER DIRECTLY, do not run an inference probe for its side
# effect. This used to be `probe_tile.py ... || true`, relying on the probe
# failing LATE -- after _compute_map_stats had fetched WorldClim on its way to
# the missing ETOPO file. That stopped working when identity schema v2
# (2026-07-22) added the conditioning gate: probe_tile.py builds a
# DiffusionConfig with no conditioning_digest, so it defaults to UNVERIFIED and
# verify_conditioning_digest refuses BEFORE any download happens. The `|| true`
# swallowed it and the next line died with "WorldClim did not land", with
# nothing in the log explaining why. A fresh pod could not bootstrap at all.
#
# _ensure_wc_files is the function whose entire job is this download. It has no
# identity gate and nothing to regress when the schema changes again.
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

# The DERIVED conditioning cache is one of the six the gate below checks, but
# on a pod that has never had it there is nothing to verify and no URL for it
# yet, so upstream's builder still has to run FIRST.
#
# THAT EXISTS TO BREAK A DEADLOCK. Upstream writes this file lazily, on the
# first pipeline run, from _load_stats_cache's miss path. But
# verify_conditioning_digest refuses to run inference while any listed file is
# absent -- so on a fresh pod nothing can ever create it, and step 8 dies with
# "conditioning data missing" naming a file no documented command produces.
#
# It needs etopo_10m.tif to exist, so it runs after the fetch attempt below
# only if the fetch could not supply it. Ordering note: the gate runs LAST, so
# whatever path produced these files, the same check decides.
#
# NOTE: takes a couple of minutes -- it reads all five global rasters and
# builds 64-knot quantile tables per channel. It is deterministic given the
# rasters and the config's own frequency_mult/drop_water_pct, which are read
# from WorldShapeConfig rather than repeated, so this cannot drift from what
# inference will actually ask for. "Deterministic GIVEN THE RASTERS" is doing
# real work in that sentence: it inherits etopo_10m.tif's bytes exactly, which
# is why it drifted on the pod.
build_stats() {
  if [ -f data/global/synthetic_map_stats.json ]; then return 0; fi
  [ -f data/global/etopo_10m.tif ] || return 0
  say "    building synthetic_map_stats.json (reads all five rasters; ~2 min)"
  python3 -c 'from terrain_service.providers.diffusion import DiffusionConfig; from terrain_diffusion.inference.synthetic_map import make_synthetic_map_factory; ws = DiffusionConfig().world_shape; make_synthetic_map_factory(frequency_mult=list(ws.frequency_mult), seed=0, drop_water_pct=ws.drop_water_pct)' \
    || die "could not build synthetic_map_stats.json. Without it the
    conditioning digest cannot be computed and no inference can run."
  [ -f data/global/synthetic_map_stats.json ] \
    || die "synthetic_map_stats.json still absent after the build ran.
    STATS_CACHE_PATH is CWD-RELATIVE ('data/global/...'), so this must run
    from $TS_DIR -- check the working directory before anything else."
}

# Pull whatever of the pinned set has a URL. Non-fatal here on purpose: the
# stats file may still need building from a just-downloaded etopo, and the
# same gate runs again below to decide.
say "    fetching pinned conditioning artifacts"
python3 tools/fetch_conditioning.py || true
build_stats

# THE GATE. Verifies all six by sha256 and fails with a full per-file report.
#
# NO `|| true`, NO warn-and-continue, and deliberately NOT stamped: a stamp
# would let a pod whose data/global was later edited report success from a
# touch file. Hashing 25 MB takes under a second against 22.5 s per tile.
#
# EXPECT_PROVIDER_ID (optional) is the world this pod has been asked to EXTEND.
# When set, the id these artifacts + this checkpoint would produce is computed
# and compared, so "this pod cannot generate into that world" is discovered
# now, in one second, instead of after a bake. It is a hard failure, and the
# answer to it is never --provider-id-override: that flag returns a namespace
# verbatim and would put two different planets in one directory with a seam in
# the middle and no error anywhere.
say "    verifying the six conditioning files against their pins"
if ! python3 tools/fetch_conditioning.py --verify-only \
       --checkpoint-sha256 "$SHA" \
       ${EXPECT_PROVIDER_ID:+--expect-provider-id "$EXPECT_PROVIDER_ID"}; then
  die "conditioning data is NOT the pinned set -- this pod cannot generate
  tiles that join the pinned world, so it stops here rather than producing a
  second planet under the first one's seed.

  Read the per-file report above: it names which of the six differ and gives
  both hashes. Then, depending on what it said:

    * 'No download URL is pinned for ...' -- the hosting decision in
      data/conditioning-artifacts.json has not been made yet. Make it, put the
      URLs in that file's 'sources' arrays, commit, and re-run.
    * a file is present but WRONG -- this box built its own. Move it aside
      (do not delete it: it is the only evidence of what any tiles already
      here were generated from) and re-run.
    * you are deliberately starting a NEW world -- then say so:
        python3 tools/fetch_etopo.py --i-am-starting-a-new-world
      and rebuild synthetic_map_stats.json from it. The new world gets its own
      provider_id and its own namespace, which is correct.

  Do NOT reach for --provider-id-override to make a mismatch go away."
fi
ok "all six conditioning files match their pins"

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
