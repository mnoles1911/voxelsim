# The bv13 river capture set: every pose, its no-water control, and the diff.
#
# WHY THIS IS A SCRIPT AND NOT FOURTEEN COMMAND LINES. Each pose is shot TWICE
# -- once with the far-field river ribbon actor on and once with
# -VoxelRiverRibbons=0 -- and the pair is only evidence if literally nothing
# else differed between the two runs. Typing the pose twice is how the sun ends
# up pinned in one arm and drifting in the other, or how the control gets the
# altitude of the previous rung. Here the pose is written once and both arms are
# generated from it.
#
# It also enforces the ordering that the persisted-state note in
# voxel-capture.ps1 is about: ON then OFF, back to back, with the .vxlog and
# .vxwater cleared by the capture script between them. A control run after an
# unrelated run can inherit that run's water and photograph it.
#
# WHAT THE NUMBERS BESIDE EACH POSE ARE. Width, depth and clearance come from
# terrain-service/tools/river_capture_probe.py against the same four bv13 tiles
# the capture loads, so every frame ships with a measured quantity to be checked
# against rather than an impression. Clearance is the distance from the camera
# column to the nearest UNBAKED tile; the working margin is 4.5 km, because a
# pose 4,073 m out produced 2,898 fine-tier gate leaks answering elevation at
# sea level.
#
#   tools\river-bv13-capture-set.ps1                 # everything
#   tools\river-bv13-capture-set.ps1 -Only owner-valley,ladder-1km
#   tools\river-bv13-capture-set.ps1 -WhatIfPoses    # print the plan, shoot nothing

param(
    [string[]]$Only = @(),
    [switch]$WhatIfPoses,
    [int]$SettleSec = 300,
    [string]$OutDir = '',
    [string]$ProjectPath = 'D:\voxelsim\ue-project\VoxelEarth.uproject'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $OutDir) { $OutDir = Join-Path $Root 'docs\images\river-bv13' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Root 'Saved') | Out-Null

# THE TILE SET. bv13 was verified from the TILE BYTES, not the directory date --
# `-bd3d0ddc7` is the newest fine directory here by mtime and is bv8 with no
# water plane at all, and was nearly used as a baseline.
$Fine       = 'D:/voxelsim/tile-cache'
$Provider   = 'terrain-diffusion-unlabeled-80b9ca451a23eae4-ba9c62170'
$Coarse     = 'D:/voxelsim/tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4/000000000135276f/s1'

# Poses. `note` is what the frame is FOR; `measured` is the number the frame has
# to be read against. Neither is a claim about how it looks.
$Poses = @(
    @{ name='owner-valley'; x=-160874.5; y=-85643.1; alt=21.5;  pitch=-10; yaw=6.7
       note='THE VALLEY THE OWNER PHOTOGRAPHED. Their frame: world (-160874.5,-85643.1), ~112 m altitude, tile (-11,-6). alt 21.5 m above a 90.46 m surface puts the camera at z=111.96 m. Yaw was NOT recoverable from their report; 6.7 deg aims at the widest fill in range.'
       measured='97.50 m cross-section 272 m out at bearing 6.7 deg; depth p50 0.761 m, max 0.769 m; surface 83.03 m over an 82.26 m floor. Clearance 6,517 m. No sea cell is in line of sight from this camera z on any bearing tested.' }

    @{ name='owner-valley-yaw258'; x=-160874.5; y=-85643.1; alt=21.5; pitch=-10; yaw=258
       note='THE SAME COLUMN AND ALTITUDE AS owner-valley, swung 251 deg. Their yaw is not in the report, so one guess is a guess; two bracket it. This one looks up-valley, away from both the coast and the wide fill.'
       measured='same camera z=111.9 m over the same 90.4 m surface. Clearance 6,517 m. No sea cell in line of sight.' }

    @{ name='valley-width'; x=-160079.1; y=-75599.1; alt=150;   pitch=-30; yaw=110
       note='THE WIDTH SHOT, substituted for the lateral-fill measurement site, which cannot be shot legally (see the writeup). Inland tile (-11,-5); no ground below sea level within 1 km.'
       measured='116.25 m cross-section at azimuth 110 deg; depth p50 0.722 m, max 0.733 m; surface 223.83 m over a 223.10 m floor. Clearance 6,479 m.' }

    @{ name='width-86m'; x=-162959.1; y=-84239.1; alt=150; pitch=-30; yaw=140
       note='THE DOC''S HEADLINE WIDTH, AT A SITE THAT CAN LEGALLY BE SHOT. river-lateral-fill-2026-08-04.txt quotes 86.25 m at tile (-12,-5) row 850 col 2627 -- a spot 1,594.7 m from unbaked coverage, which is 2,905 m inside the gate-leak margin and cannot be photographed. This is a DIFFERENT reach of the same bake whose cross-section measures the same 86.25 m, 6,001 m clear. It is on the coastal tile (-11,-6), but at a 148 m water surface with no sea cell in line of sight on any of 36 bearings from this camera height.'
       measured='86.25 m cross-section at azimuth 140 deg; depth p50 0.526 m, max 0.592 m; surface 148.48 m over a 147.91 m floor. Clearance 6,001 m.' }

    @{ name='ladder-10m';   x=-161999.1; y=-75983.4; alt=10;    pitch=-20; yaw=340
       note='Ladder rung 1 of 4. Altitude is the ONLY variable across the four: same column, same pitch -20, same yaw 340 (along the local river axis). At 10 m the camera sits 9.6 m above the water surface, i.e. INSIDE the +-12.8 m implicit disc, so this rung is the one that needs RefreshImplicitWater: DRAINED.'
       measured='58.12 m cross-section at azimuth 70 deg; depth p50 0.332 m, max 0.477 m; surface 250.12 m over a 249.70 m floor. Clearance 7,009 m.' }
    @{ name='ladder-200m';  x=-161999.1; y=-75983.4; alt=200;   pitch=-20; yaw=340
       note='Ladder rung 2 of 4.'; measured='same column as ladder-10m.' }
    @{ name='ladder-1km';   x=-161999.1; y=-75983.4; alt=1000;  pitch=-20; yaw=340
       note='Ladder rung 3 of 4.'; measured='same column as ladder-10m.' }
    @{ name='ladder-5km';   x=-161999.1; y=-75983.4; alt=5000;  pitch=-20; yaw=340
       note='Ladder rung 4 of 4. The implicit disc is empty air at this height by construction, so the absence of RefreshImplicitWater: DRAINED here is expected; the ribbon drain line is the settle check.'
       measured='same column as ladder-10m. The ribbon scan radius is 4,000 m, so the drawn river is bounded by the actor, not by the tile set.' }

    @{ name='dry-altitude'; x=-160679.1; y=-61799.1; alt=1000;  pitch=-60; yaw=45
       note='THE DISC-IN-THE-SKY REGRESSION CHECK (PR #222). At altitude over dry ground, looking down. The pre-fix failure put a disc of water ~527 m in the air following the camera, so the question is whether there is water ABOVE the terrain. The frame is the weaker half of the answer; the stronger half is the implicit-disc brick count in the log, which is a direct measurement of exactly the quantity the bug moved.'
       measured='ground 1,014.9 m, camera z 2,014.9 m. Clearance 7,079 m. Nearest wet cell 2,700 m (measured ACROSS tiles -- an earlier per-tile transform said 4,684 m and was wrong; this column sits 359 m from its own tile edge with a wet neighbour past it). NOTE: no legal pose in this four-tile set can be drier than 3,401 m, which is inside the 4,000 m ribbon scan radius, so SOME ground-level river is unavoidable in any dry-ground frame here -- and is useful, since it proves the water system was live rather than silently off.' }
)

if ($Only.Count -gt 0) { $Poses = $Poses | Where-Object { $Only -contains $_.name } }
if ($Poses.Count -eq 0) { throw "no poses selected" }

if ($WhatIfPoses) {
    foreach ($p in $Poses) {
        Write-Host "$($p.name): ($($p.x), $($p.y)) alt $($p.alt) m pitch $($p.pitch) yaw $($p.yaw)" -ForegroundColor Cyan
        Write-Host "    $($p.note)"
        Write-Host "    measured: $($p.measured)" -ForegroundColor DarkGray
    }
    return
}

$manifest = @()
foreach ($p in $Poses) {
    foreach ($arm in @('on', 'off')) {
        $nm = "$($p.name)-$arm"
        $a = @{
            Name = $nm; ProjectPath = $ProjectPath
            SpawnAt = "$($p.x),$($p.y)"
            SpawnAltM = $p.alt; SpawnPitch = $p.pitch; SpawnYaw = $p.yaw
            SettleSec = $SettleSec; TimeoutSec = ($SettleSec + 360)
            FineTileDir = $Fine; FineProviderId = $Provider; CoarseTileDir = $Coarse
        }
        if ($arm -eq 'off') { $a['NoRiverRibbons'] = $true }
        Write-Host ("=" * 78) -ForegroundColor DarkGray
        $shot = & "$PSScriptRoot\voxel-capture.ps1" @a
        $dest = Join-Path $OutDir "$nm.png"
        Copy-Item $shot $dest -Force
        Write-Host "  -> $dest" -ForegroundColor Green
        $manifest += [pscustomobject]@{ pose = $p.name; arm = $arm; file = $dest }
    }

    # THE DIFF, RUN IMMEDIATELY so a broken control is caught while the pose is
    # still cheap to re-shoot rather than at the end of a 14-run night.
    $on  = Join-Path $OutDir "$($p.name)-on.png"
    $off = Join-Path $OutDir "$($p.name)-off.png"
    $txt = Join-Path $OutDir "$($p.name)-diff.txt"
    Write-Host "  pixdiff $($p.name):" -ForegroundColor Cyan
    & python "$Root\tools\capture-pixdiff.py" $on $off --out (Join-Path $OutDir "$($p.name)-diff.png") |
        Tee-Object -FilePath $txt
}

$manifest | Format-Table -AutoSize
Write-Host "captures in $OutDir" -ForegroundColor Green
