#!/usr/bin/env bash
#
# COMMAND 2: scan for land -> choose an origin -> pregen the launch grid ->
# package it, in one pasted command. Assumes bootstrap_pod.sh has already
# run and left $WORK/bringup.env behind.
#
#   bash /workspace/voxelsim/terrain-service/tools/generate_world.sh --seed 20260719
#
# ORIGIN SELECTION -- the one real judgement call in here.
#
# Doing nothing is not an option: tile (0,0) of seed 20260719 is entirely
# underwater (elevation max -8.9 m), so an origin-centred pregen buys 25
# tiles of open ocean. Every new seed has to be scanned.
#
# But naive auto-picking is also wrong. "Highest land fraction" selects, by
# construction, the tile FURTHEST from any water -- a featureless plateau
# interior with no coast and no river mouth. So this script auto-picks using
# tools/pick_origin.py, which scores for a COASTAL tile (land fraction near
# 0.70) with real relief in a land-heavy neighbourhood, rather than for
# maximum land.
#
# Auto-pick is the DEFAULT because the alternative -- stopping to ask -- turns
# a two-command bring-up into a three-command one and strands a rented GPU
# waiting on a human. But the human still wins whenever they want to: the
# scan prints its full ASCII land map and the scored ranking before pregen
# starts, and `--origin X,Y` overrides the pick outright. Re-running with a
# different origin is cheap, because the scan JSON is reused and overlapping
# tiles come from the cache.
#
# Restartable: the scan reuses its JSON if complete, and pregen_at.py skips
# tiles already in the cache. A dropped connection costs nothing but the
# tile that was in flight.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="${WORK:-/workspace}"
ENV_FILE="$WORK/bringup.env"

RED=$'\033[31m'; GRN=$'\033[32m'; BLD=$'\033[1m'; RST=$'\033[0m'
say()  { printf '%s\n' "${BLD}==> $*${RST}"; }
ok()   { printf '%s\n' "${GRN}    ok: $*${RST}"; }
die()  { printf '%s\n' "${RED}${BLD}FAILED: $*${RST}" >&2; exit 1; }

SEED=20260719
RADIUS=2
SCAN_RADIUS=2
STRIDE=3
ORIGIN=""
RESCAN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --seed)         SEED="$2"; shift 2 ;;
    --radius)       RADIUS="$2"; shift 2 ;;
    --scan-radius)  SCAN_RADIUS="$2"; shift 2 ;;
    --stride)       STRIDE="$2"; shift 2 ;;
    --origin)       ORIGIN="$2"; shift 2 ;;
    --rescan)       RESCAN=1; shift ;;
    -h|--help)      sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *)              die "unknown argument: $1" ;;
  esac
done

[ -f "$ENV_FILE" ] || die "$ENV_FILE not found -- run bootstrap_pod.sh first."
# shellcheck disable=SC1090
. "$ENV_FILE"

: "${CKPT_DIR:?CKPT_DIR missing from $ENV_FILE; re-run bootstrap_pod.sh}"
: "${CKPT_SHA:?CKPT_SHA missing from $ENV_FILE; re-run bootstrap_pod.sh}"

# The sha256 is filled in from the env file, never pasted -- on 2026-07-19 a
# placeholder went in with its angle brackets still attached, and a wrong
# hash does not fail loudly: it silently changes provider_id, which is both
# the tile-cache namespace and the edit-log provider stamp.
[ "${#CKPT_SHA}" = "64" ] || die "CKPT_SHA is not 64 chars: '$CKPT_SHA'"

CACHE_DIR="$WORK/tile-cache"
SCAN_JSON="$WORK/scan-seed${SEED}.json"
OUT_TAR="$WORK/tile-cache-seed${SEED}.tar.gz"
LOG="$WORK/generate-seed${SEED}.log"

exec > >(tee -a "$LOG") 2>&1

# Relative paths again: WorldClim and ETOPO are opened relative to cwd.
cd "$TS_DIR"

say "seed $SEED  |  checkpoint $CKPT_DIR  |  log $LOG"

# ---------------------------------------------------------------------------
# 1. scan for land
# ---------------------------------------------------------------------------
if [ -n "$ORIGIN" ]; then
  say "1/3 scan skipped (--origin $ORIGIN given)"
else
  n=$(( (2 * SCAN_RADIUS + 1) * (2 * SCAN_RADIUS + 1) ))
  if [ -f "$SCAN_JSON" ] && [ "$RESCAN" = "0" ]; then
    ok "reusing $SCAN_JSON (pass --rescan to discard it)"
  else
    [ "$RESCAN" = "1" ] && rm -f "$SCAN_JSON"
    say "1/3 scanning $n tiles, stride $STRIDE (~$((n * 23 / 60)) min)"
    # Strided, not contiguous: one tile is 512 px x 30 m = 15.4 km, so
    # stride 3 reaches ~46 km per step. The whole problem is that the
    # neighbourhood of the origin is ocean, so reach beats density.
    python3 tools/scan_land.py "$CKPT_DIR" "$CKPT_SHA" \
      --seed "$SEED" --radius "$SCAN_RADIUS" --stride "$STRIDE" \
      --json "$SCAN_JSON" \
      || die "scan failed. It is restartable -- re-run this script."
  fi

  say "scoring candidates for a coastal origin"
  ORIGIN="$(python3 tools/pick_origin.py "$SCAN_JSON" --explain)" \
    || die "no tile in the scan had usable land.
    Widen the search and try again:
      bash $0 --seed $SEED --scan-radius 3 --stride 5 --rescan"
  ok "auto-picked origin $ORIGIN  (override with --origin X,Y)"
fi

# ---------------------------------------------------------------------------
# 2 + 3. pregen the launch grid and package it
# ---------------------------------------------------------------------------
tiles=$(( (2 * RADIUS + 1) * (2 * RADIUS + 1) ))
say "2/3 pregen $tiles tiles at origin $ORIGIN (~$((tiles * 23 / 60)) min at 22.5 s/tile)"

# pregen_at.py writes the manifest (provider_id, checkpoint sha256, per-tile
# hashes) and the tarball itself, so nothing has to be recovered from
# scrollback if this terminal dies.
python3 tools/pregen_at.py "$CKPT_DIR" "$CKPT_SHA" \
  --origin "$ORIGIN" --seed "$SEED" --radius "$RADIUS" \
  --cache "$CACHE_DIR" --out "$OUT_TAR" \
  || die "pregen failed. Re-run this script with --origin $ORIGIN --
    tiles already in the cache are skipped, so only the missing ones cost."

say "3/3 done"
printf '\n%s\n' "${GRN}${BLD}=============== WORLD READY ===============${RST}"
printf '  seed     : %s\n' "$SEED"
printf '  origin   : %s   radius %s   (%s tiles)\n' "$ORIGIN" "$RADIUS" "$tiles"
printf '  tarball  : %s\n' "$OUT_TAR"
printf '  manifest : %s/manifest.json\n' "$CACHE_DIR"
printf '\n%s\n\n' "${BLD}NEXT -- from the DEV MACHINE, download then destroy the pod:${RST}"
printf '  scp -P <pod-ssh-port> root@<pod-host>:%s D:\\voxelsim\\\n\n' "$OUT_TAR"
printf '%s\n' "  Then DESTROY the instance (not just stop it) -- Vast bills storage for"
printf '%s\n\n' "  as long as the instance exists."
