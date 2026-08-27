#!/usr/bin/env bash
# Read a direction sweep's logs and print ONE table: pose, arm, view=, march ms,
# ratio-to-down, and the state of every invalidator.
#
# Usage:
#   bash tools/march-direction-summary.sh DIR1              # by sweep prefix (reads the manifest)
#   bash tools/march-direction-summary.sh ZZ-pitch-90 ZZ-pitch0 ZZ-pitch30
#                                                            # or by bare log names
#
# ============================================================================
# WHY THIS IS NOT `grep | tail -1`
# ============================================================================
#
# tools/leg-summary.sh's header documents what that costs: the run keeps logging
# for LingerSec after the flight ends and every windowed counter reads zero
# there, so `tail -1` lands in the linger window and reports "the cache is
# enabled and doing nothing". It produced two wrong conclusions in one session
# and a `holes 0 -> 10` regression that reversed sign when read over the flight.
#
# The lines this script reads fall into three different classes and each needs a
# different rule. They are labelled at each site:
#
#   ONE-SHOT      ProfileGPU rows, "STATIC pose pinned", "view=", the command
#                 line. Written once. Any match is the match.
#   CUMULATIVE    "Voxel march: mode=" (frames / emitFrames since boot),
#                 DOUBLE GRANT and GATE LEAK totals. The LAST line is the
#                 leg's answer, and tail -1 is correct here -- only here.
#   WINDOWED      the 5 s LogVoxelPerf lines, including "Voxel march holes".
#                 tail -1 is FORBIDDEN: it reads the linger window. This script
#                 reports the last window that carried rays, and says so.
#
# ============================================================================
# WHAT THE ms COLUMN IS
# ============================================================================
#
# `VoxelMarch.March` from ProfileGPU, and nothing else. Not marchMs from
# voxel.March.Stats (a running mean; it read 6.348 on a leg where ProfileGPU
# read 7.178). Not frame time. The frame column is printed as CONTEXT and
# labelled that way.
#
# ============================================================================
# A VOID IS LOUD
# ============================================================================
#
# A leg failing any invalidator prints its ms in brackets, its status in the
# VOID column, and every failed check by name underneath. This script exits 1
# if any leg is void, so a caller cannot mistake a broken sweep for a result.

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SAVED="$REPO/Saved"

if [ $# -eq 0 ]; then
  echo "usage: march-direction-summary.sh <sweep-prefix> | <logname> [logname...]" >&2
  exit 2
fi

# ---------------------------------------------------------------------------
# INPUT: a manifest prefix, or bare log names.
# ---------------------------------------------------------------------------
MANIFEST=""
LEGS=()          # each entry: logname<TAB>armtag<TAB>armname<TAB>pitch<TAB>yaw<TAB>repeat<TAB>kind
declare -A ARM_CVARS=()
# US (0x1f) AND NOT TAB AS THE READ SEPARATOR. Tab is IFS-WHITESPACE, so
# `IFS=$'\t' read` COLLAPSES consecutive tabs and an EMPTY FIELD DISAPPEARS --
# which silently shifts every later field left by one. That is exactly how a
# control arm (empty cvars) had its PROOF string read as its cvars and every
# control leg voided with a confident, wrong reason. 0x1f is not whitespace, so
# empty fields survive.
US=$'\x1f'
if [ $# -eq 1 ] && [ -f "$SAVED/$1-manifest.tsv" ]; then
  MANIFEST="$SAVED/$1-manifest.tsv"
  while IFS="$US" read -r kind a b c d e f g h; do
    if [ "$kind" = "arm" ]; then ARM_CVARS["$a"]="${c:-}"; continue; fi
    [ "$kind" = "leg" ] || continue
    LEGS+=("$a$US$b$US$c$US$d$US$e$US$f$US$g")
  done < <(sed "s/\t/$US/g" "$MANIFEST")
  if [ ${#LEGS[@]} -eq 0 ]; then
    # A VOID MUST BE LOUD. An empty table that says "0 legs, none void" is the
    # house failure this project has hit eleven times in one night, so this
    # exits non-zero rather than printing a clean-looking nothing.
    echo "REFUSING TO SUMMARISE: $MANIFEST holds no 'leg' rows." >&2
    echo "  Either the sweep was a -DryRun, or it died before its first leg completed." >&2
    echo "  There is nothing here to read. This is NOT a passing summary." >&2
    exit 1
  fi
else
  for n in "$@"; do
    # Pose is recovered from the log itself below; the name only has to be unique.
    LEGS+=("$n$US?$US?$US?$US?${US}0${US}timing")
  done
fi

# ---------------------------------------------------------------------------
# PROVENANCE. Numbers that cannot be compared across runs because something
# moved underneath are the failure this block exists to make visible: the same
# pose, the same view=, eight hours apart, read 4.548 ms and 5.695 ms.
# ---------------------------------------------------------------------------
if [ -n "$MANIFEST" ]; then
  echo "=============================================================================="
  awk -F'\t' '
    $1=="sweep"         { printf "sweep        %s\n", $2 }
    $1=="when"          { printf "run at       %s\n", $2 }
    $1=="spawn"         { printf "spawn        %s\n", $2 }
    $1=="res"           { printf "requested    %s   (view= below is the ENGINE size and may differ -- that is correct)\n", $2 }
    $1=="timing"        { printf "run shape    %s\n", $2 }
    $1=="base"          { printf "base cvars   %s\n", $2 }
    $1=="dryrun" && $2=="1" { printf "!! DRY RUN -- no leg in this manifest was executed\n" }
    $1=="proofrequired" && $2=="0" { printf "!! PROOF OF TRAFFIC WAS NOT REQUIRED -- this sweep proves nothing about engagement\n" }
    $1=="bin"           { printf "binary       %-40s %s\n", $2, $3 }
    $1=="arm"           { printf "arm %s (%s)\n               cvars: %s\n               proof: %s\n", $2, $3, ($4==""?"(control)":$4), $5 }
    $1=="note"          { printf "note         %s\n", $2 }
  ' "$MANIFEST"
  echo "=============================================================================="
fi

# ---------------------------------------------------------------------------
# ONE LEG -> one tab-separated record. Printed by the table pass below.
# ---------------------------------------------------------------------------
read_leg() {
  local name="$1" armname="$2" wantpitch="$3" wantyaw="$4" kind="$5"
  local mycvars="${6:-}" othercvars="${7:-}"
  local log="$SAVED/$name.log"
  local voids="" warns=""

  if [ ! -f "$log" ]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$armname" "$wantpitch" "$wantyaw" "MISSING" "" "" "" "$kind" \
      "NO LOG FILE (the leg never started)" "" "" "" "" "" "?"
    return
  fi

  # --- ONE-SHOT: the pose the run actually pinned. -----------------------
  # A direction sweep whose legs looked somewhere else is worse than no sweep.
  local poseline pitch yaw
  poseline=$(grep -m1 'STATIC pose pinned at' "$log" || true)
  pitch=""; yaw=""
  if [ -n "$poseline" ]; then
    yaw=$(printf '%s' "$poseline"   | sed -n 's/.*yaw=\(-\?[0-9.]*\) pitch=.*/\1/p')
    pitch=$(printf '%s' "$poseline" | sed -n 's/.*pitch=\(-\?[0-9.]*\).*/\1/p')
  else
    voids="${voids};NO STATIC POSE (the run was not static -- direction unknown)"
  fi
  if [ "$wantpitch" != "?" ] && [ -n "$pitch" ]; then
    if ! awk -v a="$pitch" -v b="$wantpitch" 'BEGIN{exit (a-b<0.05 && b-a<0.05)?0:1}'; then
      voids="${voids};POSE MISMATCH pitch asked $wantpitch got $pitch"
    fi
  fi
  if [ "$wantyaw" != "?" ] && [ -n "$yaw" ]; then
    if ! awk -v a="$yaw" -v b="$wantyaw" 'BEGIN{exit (a-b<0.05 && b-a<0.05)?0:1}'; then
      voids="${voids};POSE MISMATCH yaw asked $wantyaw got $yaw"
    fi
  fi
  [ -n "$pitch" ] || pitch="$wantpitch"
  [ -n "$yaw" ]   || yaw="$wantyaw"

  # --- ONE-SHOT: view=, READ FROM THE ENGINE, NEVER THE REQUEST. ---------
  # view=1552x873 at a requested 2560x1440 is CORRECT: a 60.6% screen
  # percentage that TSR upscales to the owner's 1440p. It is NOT an
  # invalidator on its own, and this script does not "fix" it. What IS an
  # invalidator is the view differing BETWEEN LEGS of one sweep -- then the
  # ms are ray counts of different sizes. That comparison is done after the
  # table, across all legs.
  local view
  view=$(grep -m1 -o 'view=[0-9]*x[0-9]* px' "$log" | sed 's/view=//; s/ px//' || true)
  if [ -z "$view" ]; then
    view="UNKNOWN"
    voids="${voids};NO view= LINE (render size unknown -- do not quote this leg)"
  fi

  # --- ONE-SHOT: the number. ProfileGPU's VoxelMarch.March row. ----------
  # The row carries two time columns (self, then inclusive) and then the pass
  # name; a pass name never contains " ms ", so the LAST match on the line is
  # the inclusive figure. Anchored on the '(' so it cannot pick up
  # VoxelMarch.CompactTiles or a ClearBuffer(VoxelMarch....) row.
  local march frame
  march=$(grep -F 'VoxelMarch.March(' "$log" | grep -oE '[0-9]+\.[0-9]+ ms' | tail -1 | sed 's/ ms//' || true)
  # Frame total, CONTEXT ONLY. The headline is the march pass, not the frame.
  frame=$(grep -E 'LogRHI.*Frame [0-9]+' "$log" | grep -oE '[0-9]+\.[0-9]+ ms' | tail -1 | sed 's/ ms//' || true)
  if [ -z "$march" ] && [ "$kind" = "timing" ]; then
    voids="${voids};NO VoxelMarch.March ROW (no GPU number in this leg)"
  fi

  # --- ONE-SHOT: the capture fired, and fired in the FLIGHT. -------------
  # A ProfileGPU that lands in the post-flight linger window measures having
  # stopped. No wall-clock heuristic catches that; the log's own timestamps do.
  if [ "$kind" = "timing" ]; then
    # TWO MECHANISMS, BOTH ACCEPTED. The sweep uses voxel.DeferExec (whose
    # clock starts at -ExecCmds, i.e. engine startup); the ad-hoc legs in the
    # archive -- ZZ-*, BK-*, BL-*, CENSUS-* -- used -VoxelExecAfter=110 with
    # -VoxelExecCmds=ProfileGPU, whose clock starts at GameMode BeginPlay.
    # Rejecting the second would void the entire archive this table exists to
    # be compared against, so both are read and the landing check below is what
    # actually decides whether the capture was in the flight.
    local capline
    capline=$(grep -m1 -E 'DeferExec: running now: ProfileGPU|VoxelExecAfter: ProfileGPU$' "$log" || true)
    if [ -z "$capline" ]; then
      voids="${voids};ProfileGPU NEVER FIRED (no DeferExec/VoxelExecAfter run line)"
    else
      local tp tc te
      tp=$(printf '%s' "$poseline" | sed -n 's/^\[\([0-9.]*-[0-9.:]*\)\].*/\1/p')
      tc=$(printf '%s' "$capline"  | sed -n 's/^\[\([0-9.]*-[0-9.:]*\)\].*/\1/p')
      te=$(grep 'VoxelPerfRun complete' "$log" | tail -1 | sed -n 's/^\[\([0-9.]*-[0-9.:]*\)\].*/\1/p')
      # Lexicographic compare is exact here: the stamp is fixed-width
      # YYYY.MM.DD-HH.MM.SS:mmm.
      if [ -n "$tp" ] && [ -n "$tc" ] && [ "$tc" \< "$tp" ]; then
        voids="${voids};CAPTURE FIRED BEFORE THE POSE WAS PINNED (photographed the preflight)"
      fi
      if [ -n "$tc" ] && [ -n "$te" ] && [ "$te" \< "$tc" ]; then
        voids="${voids};CAPTURE FIRED AFTER THE RUN COMPLETED (the linger window)"
      fi
    fi
  fi

  # --- ONE-SHOT: the run's own completion witness. -----------------------
  # UVoxelPerfRunSubsystem prints this before RequestExit, so its absence
  # proves FinishRun never ran -- which no timing heuristic can.
  local p50 p95 complete
  complete=$(grep 'VoxelPerfRun complete' "$log" | tail -1 || true)
  if [ -z "$complete" ]; then
    voids="${voids};NO VoxelPerfRun complete (the flight did not finish)"
    p50=""; p95=""
  else
    p50=$(printf '%s' "$complete" | grep -oE 'p50=[0-9.]+ms' | sed 's/p50=//; s/ms//')
    p95=$(printf '%s' "$complete" | grep -oE 'p95=[0-9.]+ms' | sed 's/p95=//; s/ms//')
  fi

  # --- CUMULATIVE: DOUBLE GRANT. The allocator's own correctness gate. ---
  # Non-zero means the GPU handed out dwords somebody already held and the
  # colliding claims were FAILED -- the frame is missing geometry it should
  # have marched, which reads as a saving.
  local dg
  dg=$(grep -c '\[brick-gpualloc\] DOUBLE GRANT' "$log" || true)
  [ "$dg" -eq 0 ] || voids="${voids};DOUBLE GRANT x$dg"

  # --- CUMULATIVE: FINE TIER GATE LEAK. ---------------------------------
  # An elevation query answered from a non-resident tile returns SEA LEVEL, so
  # real ground becomes "provably air" -- and an empty-space mechanism measured
  # against ground that is not there reports a saving it did not make.
  local gl
  gl=$(grep -c 'FINE TIER GATE LEAK' "$log" || true)
  [ "$gl" -eq 0 ] || voids="${voids};FINE TIER GATE LEAK x$gl"

  # --- CUMULATIVE: frames / emitFrames, from voxel.March.Stats. ----------
  # tail -1 is correct here: these counters are cumulative since boot, not
  # windowed. emitFrames BEHIND frames means the emit declined and RDG culled
  # the march -- the timing brackets are NeverCull so they still report,
  # describing work that was thrown away. That is the one way this instrument
  # prints a plausible small number and is believed.
  local statline frames emitframes tiles
  statline=$(grep 'Voxel march: mode=' "$log" | tail -1 || true)
  frames=""; emitframes=""; tiles=""
  if [ -n "$statline" ]; then
    frames=$(printf '%s' "$statline"     | grep -oE ' frames=[0-9]+'     | grep -oE '[0-9]+')
    emitframes=$(printf '%s' "$statline" | grep -oE 'emitFrames=[0-9]+'  | grep -oE '[0-9]+')
    tiles=$(printf '%s' "$statline"      | grep -oE 'tiles total=[0-9]+ drawn=[0-9]+' | grep -oE '[0-9]+' | tr '\n' '/' | sed 's:/$::')
    if [ -n "$frames" ] && [ "$frames" = "0" ]; then
      voids="${voids};MARCH frames=0 (the pass never ran)"
    elif [ -n "$frames" ] && [ -n "$emitframes" ] && [ "$emitframes" -lt "$frames" ]; then
      voids="${voids};emitFrames=$emitframes BEHIND frames=$frames (RDG culled the march)"
    fi
  else
    warns="${warns};no voxel.March.Stats line (no independent witness of traffic)"
  fi

  # --- WINDOWED: image integrity, when HoleStats was on. -----------------
  # tail -1 is FORBIDDEN on a windowed line -- the linger windows read zero and
  # that is the trap this file's header is about. Take the last window that
  # actually carried rays. On a STATIC leg this is weak evidence at best: a
  # parked camera has had time to cover everything it can see. It is here to
  # catch a GROSS change (a mechanism that deleted terrain), not to certify an
  # image. The owner's screenshot outranks it.
  local holes
  holes=$(grep -o 'Voxel march holes (window): .*' "$log" | grep -v 'rays=0 ' | tail -1 || true)

  # --- ONE-SHOT: the cvars the editor actually saw. ----------------------
  # Catches the comma/pipe/quoting family at the only place it matters: what
  # reached the process, not what the script meant to send.
  local exec
  exec=$(grep -m1 'LogInit: Command Line:' "$log" | grep -oE '\-ExecCmds="[^"]*"' | sed 's/-ExecCmds=//' || true)
  if [ -z "$exec" ]; then
    voids="${voids};NO COMMAND LINE IN THE LOG (cannot verify the configuration)"
  else
    # "THE TWO ARMS DIFFERED IN EXACTLY ONE THING" IS A CLAIM. This is its
    # check: the arm's declared cvars must be present in what the editor saw,
    # and the OTHER arm's must be absent. A leg carrying both arms' switches
    # is a cross-contaminated control and reads as a null result.
    if [ -n "$mycvars" ] && ! printf '%s' "$exec" | grep -qF -- "$mycvars"; then
      voids="${voids};THIS ARM'S CVARS ARE NOT IN THE COMMAND LINE THE EDITOR SAW ($mycvars)"
    fi
    if [ -n "$othercvars" ] && printf '%s' "$exec" | grep -qF -- "$othercvars"; then
      voids="${voids};THE OTHER ARM'S CVARS ARE ALSO ON THIS LEG ($othercvars) -- cross-contaminated"
    fi
  fi

  # WHICH KERNEL PERMUTATION THIS LEG TIMED. voxel.March.HoleStats is a shader
  # permutation of the TIMED kernel ("off is FREE: no UAV is created or bound,
  # no groupshared word exists, no atomic runs"), measured at +2.2% at the
  # horizon -- and it is also the switch that makes the arm counters print. Two
  # legs that disagree on it are timing two different shaders. The cross-leg
  # check is in the awk pass; this only records the state.
  local hs
  hs=$(printf '%s' "$exec" | grep -oE 'voxel\.March\.HoleStats +[0-9]+' | grep -oE '[0-9]+$' || true)
  [ -n "$hs" ] || hs="0"

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$armname" "$pitch" "$yaw" "$view" "$march" "$frame" "$p50" \
    "$kind" "${voids#;}" "${warns#;}" "$frames/$emitframes" "$tiles" "$holes" "$exec" "$hs"
}

# ---------------------------------------------------------------------------
# PASS 1: read every leg.
# ---------------------------------------------------------------------------
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
for entry in "${LEGS[@]}"; do
  IFS="$US" read -r name armtag armname pitch yaw repeat kind <<< "$entry"
  [ -n "${kind:-}" ] || kind="timing"
  [ "$armname" != "?" ] || armname="-"
  mine=""; other=""
  if [ -n "$MANIFEST" ]; then
    mine="${ARM_CVARS[$armtag]:-}"
    for t in "${!ARM_CVARS[@]}"; do
      if [ "$t" != "$armtag" ] && [ -n "${ARM_CVARS[$t]}" ]; then other="${ARM_CVARS[$t]}"; fi
    done
  fi
  read_leg "$name" "$armname" "$pitch" "$yaw" "$kind" "$mine" "$other" >> "$TMP"
done

# ---------------------------------------------------------------------------
# PASS 2: the view= consistency check, ACROSS legs.
# One sweep must render one ray count. A leg whose view differs from the rest
# is measuring a different picture, and its ms is not comparable to theirs.
# ---------------------------------------------------------------------------
MAJORITY_VIEW=$(awk -F'\t' '$5!="UNKNOWN" && $5!="MISSING" {print $5}' "$TMP" | sort | uniq -c | sort -rn | head -1 | awk '{print $2}')

# ...and the same for the kernel permutation. voxel.March.HoleStats changes the
# TIMED shader (+2.2% at the horizon, measured), so a sweep whose timing legs
# disagree on it is comparing two shaders. Engagement legs are exempt.
MAJORITY_HOLESTATS=$(awk -F'\t' '$9=="timing" && $5!="MISSING" {print $16}' "$TMP" | sort | uniq -c | sort -rn | head -1 | awk '{print $2}')

# ---------------------------------------------------------------------------
# PASS 3: ratio-to-down, per arm, against that arm's own valid pitch -90 leg.
# The thesis under test is that cost tracks empty space crossed, so every
# direction is quoted against the direction with none of it. A mechanism that
# helps sky and not the horizon must be visible AS THAT in this column.
# ---------------------------------------------------------------------------
echo
printf '%-28s %-8s %6s %5s %-10s %9s %9s %8s %8s  %s\n' \
  "leg" "arm" "pitch" "yaw" "view" "march ms" "/down" "frame*" "p50 ms" "status"
printf -- '------------------------------------------------------------------------------------------------------------------------------\n'

awk -F'\t' -v mv="$MAJORITY_VIEW" -v mh="$MAJORITY_HOLESTATS" '
  # $1 name $2 arm $3 pitch $4 yaw $5 view $6 march $7 frame $8 p50
  # $9 kind $10 voids $11 warns $12 frames/emit $13 tiles $14 holes $15 exec
  # $16 HoleStats state
  {
    n++; for (i=1; i<=16; i++) F[n,i] = $i
    v = $10
    if ($5 != mv && $5 != "MISSING" && mv != "") {
      v = (v=="" ? "" : v ";") "view=" $5 " DISAGREES WITH THE REST OF THE SWEEP (" mv ") -- different ray counts"
    }
    # THE PERMUTATION MUST BE CONSTANT ACROSS THE TIMING LEGS. Engagement legs
    # are deliberately HoleStats 1 and are exempt; a TIMING leg that disagrees
    # with the rest timed a different shader, and its ms is not comparable to
    # theirs at the ~2% level the rest of this table works at.
    if ($9=="timing" && mh != "" && $16 != mh && $5 != "MISSING") {
      v = (v=="" ? "" : v ";") "voxel.March.HoleStats=" $16 " WHILE THE OTHER TIMING LEGS RAN " mh " -- that cvar is a permutation of the TIMED kernel, so these are two different shaders"
    }
    F[n,10] = v
    # The down-pose denominator, per arm, and only from a VALID leg.
    if ($9=="timing" && F[n,10]=="" && $6!="" && $3+0 <= -89.5) { down[$2] = $6 }
  }
  END {
    if (n == 0) {
      # A VOID MUST BE LOUD. "0 legs, none void" is a passing-looking nothing.
      printf "REFUSING TO SUMMARISE: no legs were read. This is NOT a passing summary.\n"
      exit 1
    }
    voidn = 0
    for (i=1; i<=n; i++) {
      ms = F[i,6]; if (ms=="") ms = "n/a"
      ratio = "n/a"
      if (F[i,6] != "" && (F[i,2] in down) && down[F[i,2]]+0 > 0) {
        ratio = sprintf("%.2fx", F[i,6] / down[F[i,2]])
      }
      status = "ok"
      if (F[i,10] != "") { status = "VOID"; voidn++; ms = "(" ms ")" }
      else if (F[i,9] == "engagement") { status = "ok (HoleStats 1 -- ms NOT comparable)"; ms = "(" ms ")"; ratio = "eng" }
      fr = F[i,7]; if (fr=="") fr = "n/a"
      p50 = F[i,8]; if (p50=="") p50 = "n/a"
      printf "%-28s %-8s %6s %5s %-10s %9s %9s %8s %8s  %s\n",
             F[i,1], F[i,2], F[i,3], F[i,4], F[i,5], ms, ratio, fr, p50, status
      if (F[i,10] != "") {
        m = split(F[i,10], vs, ";")
        for (j=1; j<=m; j++) if (vs[j] != "") printf "%-28s   INVALIDATOR: %s\n", "", vs[j]
      }
      if (F[i,11] != "") {
        m = split(F[i,11], ws, ";")
        for (j=1; j<=m; j++) if (ws[j] != "") printf "%-28s   note: %s\n", "", ws[j]
      }
    }
    printf "\n"
    printf "* frame ms is CONTEXT. The number this sweep is for is the march column:\n"
    printf "  VoxelMarch.March from ProfileGPU. Not marchMs from voxel.March.Stats, not frame time.\n"
    printf "\n"
    # Per-arm horizon spread across yaws. The falsifier: an arm whose effect is
    # smaller than the azimuthal spread has not been shown to do anything at
    # the horizon, and the repeat spread on this box is ~2% there.
    for (i=1; i<=n; i++) {
      if (F[i,9]=="timing" && F[i,10]=="" && F[i,6]!="" && F[i,3]+0 > -0.5 && F[i,3]+0 < 0.5) {
        a = F[i,2]; hs[a] += F[i,6]; hn[a]++
        if (!(a in hmin) || F[i,6]+0 < hmin[a]) hmin[a] = F[i,6]+0
        if (!(a in hmax) || F[i,6]+0 > hmax[a]) hmax[a] = F[i,6]+0
      }
    }
    printed = 0
    for (a in hn) {
      if (hn[a] < 2) continue
      if (!printed) { printf "HORIZON (pitch 0) ACROSS YAWS -- the azimuthal spread is the falsifier:\n"; printed = 1 }
      printf "  %-10s n=%d  mean %.3f ms  min %.3f  max %.3f  spread %.1f%%\n",
             a, hn[a], hs[a]/hn[a], hmin[a], hmax[a], 100.0*(hmax[a]-hmin[a])/hmin[a]
    }
    if (printed) {
      printf "  An A/B difference smaller than this spread, or smaller than the ~2%% repeat\n"
      printf "  noise floor measured on BK/BL, has not been shown at the horizon.\n\n"
    }
    if (voidn > 0) {
      printf "%d OF %d LEGS ARE VOID. Do not quote a comparison that crosses one.\n", voidn, n
      exit 1
    }
    printf "%d legs, none void.\n", n
  }
' "$TMP"
RC=$?

# ---------------------------------------------------------------------------
# The cvars the EDITOR saw, per leg. Printed last and in full, because "the two
# arms differed in exactly one thing" is a claim, and this is its evidence.
# ---------------------------------------------------------------------------
echo "-ExecCmds AS THE EDITOR SAW IT (not as the script meant to send it):"
awk -F'\t' '{ printf "  %-28s %s\n", $1, ($15==""?"(NOT IN THE LOG)":$15) }' "$TMP"

# Image integrity, when a leg had HoleStats on. Windowed line, last window that
# carried rays -- never tail -1. Static-pose evidence is weak; see read_leg.
if awk -F'\t' '$14!=""{found=1} END{exit found?0:1}' "$TMP"; then
  echo
  echo "IMAGE INTEGRITY (HoleStats legs only; last window that carried rays, NOT the linger window):"
  echo "  A parked pose has had time to cover everything it can see, so this catches a GROSS"
  echo "  change only. A speedup on a renderer is a claim about the image; the owner's"
  echo "  screenshot outranks this line."
  awk -F'\t' '$14!=""{ printf "  %-28s %s\n", $1, $14 }' "$TMP"
fi

exit $RC
