# The height pyramid's IMAGE gate -- A/B captures against a noise floor.
#
# ============================================================================
# WHY THE IMAGE OUTRANKS THE MILLISECOND HERE
# ============================================================================
#
# This project shipped a "-7.6% win" on this very renderer that was the marcher
# DELETING A MOUNTAIN, and the timing INVERTED to +3.1% once the image was made
# honest. A speedup on a renderer is a claim about the picture, and the height
# pyramid's one failure mode -- a bound that is too LOW -- removes terrain
# silently and gets FASTER by exactly the amount it removed.
#
# So: image before timing, and the arm does not proceed to a timing sweep until
# these pairs are clean.
#
# ============================================================================
# THE NOISE FLOOR COMES FIRST, AND IT IS NOT A FORMALITY
# ============================================================================
#
# Two captures of the SAME arm at the SAME pose are not byte-identical: TSR
# carries temporal state, streaming order varies, and the pawn settles to a
# slightly different sub-voxel position. Without measuring that first, any
# control-vs-armed difference is uninterpretable -- and the temptation is
# always to read a small number as "clean". The control pair is what makes
# "clean" a threshold instead of an opinion.
#
# ============================================================================
# PAIR BY EXPLICIT FILENAME. NEVER BY `ls -t | head -2`.
# ============================================================================
#
# voxel-capture.ps1 writes into a shared Screenshots directory and a run can
# leave more than one PNG there. Sorting by time and taking the last two has
# already paired a shot with the wrong partner on this project. This script
# copies each capture to a DETERMINISTIC name as soon as it is taken, and the
# comparison names those files and nothing else.
#
# ============================================================================
# THE TWO YAWS, AND WHY THE AWAY ONE IS THE DANGEROUS ONE
# ============================================================================
#
# The massif at -57440,-57440 sits +x/+y of the spawn. Yaw 45 looks AT it; yaw
# 225 looks away, down the valley that falls to 1,485 m. The pyramid does its
# most aggressive skipping where local ground is FAR below the ray -- i.e.
# looking away -- so that is where a too-low bound deletes terrain first. A
# clean shot toward the massif proves very little on its own.

param(
    [string]$Prefix = 'hpimg',
    [string]$SpawnAt = '-61440,-61440',
    [double]$SpawnAltM = 220,
    [double]$SpawnPitch = 0,
    [int]$SettleSec = 150,
    [int]$Width = 2560,
    [int]$Height = 1440,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$RepoRoot   = (Resolve-Path "$PSScriptRoot\..").Path
$CapScript  = Join-Path $PSScriptRoot 'voxel-capture.ps1'
$ShotDir    = Join-Path $RepoRoot 'ue-project\Saved\Screenshots\WindowsEditor'
$OutDir     = Join-Path $RepoRoot "Saved\$Prefix"
if (-not (Test-Path $CapScript)) { throw "REFUSING TO START: $CapScript is missing." }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# BUILD THE FIELD ON BOTH ARMS. The control differs from the armed capture in
# exactly ONE cvar -- voxel.March.HeightPyramid -- so the CPU fill, its tick
# cost and its streaming interaction are present in both and cannot be mistaken
# for the marcher's doing.
$Base = 'voxel.HeightPyramid.Build 1'

function Shoot([string]$Tag, [string]$Cvars, [double]$Yaw) {
    $name = "$Prefix-$Tag"
    Write-Host ""
    Write-Host "=== CAPTURE $name  (yaw $Yaw) ===" -ForegroundColor Cyan
    Write-Host "    cvars: $Cvars" -ForegroundColor DarkGray
    if ($DryRun) { return }

    $before = @{}
    if (Test-Path $ShotDir) {
        Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue |
            ForEach-Object { $before[$_.Name] = $true }
    }

    & $CapScript -Name $name -SpawnAt $SpawnAt -SpawnAltM $SpawnAltM `
                 -SpawnPitch $SpawnPitch -SpawnYaw $Yaw -SettleSec $SettleSec `
                 -Width $Width -Height $Height -Cvars $Cvars

    # DETERMINISTIC NAME, TAKEN NOW. The set-difference against $before is what
    # makes this a real identification rather than a guess about ordering.
    $new = Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue |
        Where-Object { -not $before.ContainsKey($_.Name) } |
        Sort-Object LastWriteTime -Descending
    if (-not $new) {
        Write-Host "    NO NEW PNG -- this capture produced nothing. Treat as VOID." -ForegroundColor Red
        return
    }
    $dst = Join-Path $OutDir "$Tag.png"
    Copy-Item $new[0].FullName $dst -Force
    Write-Host "    -> $dst  (from $($new[0].Name); $($new.Count) new file(s))" -ForegroundColor Green
}

# ---- the noise floor, twice with the arm OFF -----------------------------
Shoot 'away-ctlA' "$Base, voxel.March.HeightPyramid 0" 225
Shoot 'away-ctlB' "$Base, voxel.March.HeightPyramid 0" 225
# ---- the arm, same pose --------------------------------------------------
Shoot 'away-arm'  "$Base, voxel.March.HeightPyramid 1" 225
# ---- toward the massif ---------------------------------------------------
Shoot 'massif-ctl' "$Base, voxel.March.HeightPyramid 0" 45
Shoot 'massif-arm' "$Base, voxel.March.HeightPyramid 1" 45

Write-Host ""
Write-Host "Compare, NAMING THE FILES (never ls -t):" -ForegroundColor Yellow
Write-Host "  python tools/imgdiff.py $OutDir\away-ctlA.png  $OutDir\away-ctlB.png   # THE NOISE FLOOR"
Write-Host "  python tools/imgdiff.py $OutDir\away-ctlA.png  $OutDir\away-arm.png"
Write-Host "  python tools/imgdiff.py $OutDir\massif-ctl.png $OutDir\massif-arm.png"
Write-Host ""
Write-Host "THE ARMED PAIR MUST NOT EXCEED THE CONTROL PAIR BY A MARGIN THE" -ForegroundColor Yellow
Write-Host "OWNER WOULD SEE. A difference concentrated in a SHAPE -- an arc, a" -ForegroundColor Yellow
Write-Host "ridge line, a missing skyline -- is a hole however small its mean." -ForegroundColor Yellow
