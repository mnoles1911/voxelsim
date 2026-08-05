# Shoot ONE arm of one pose, foreground, and print the evidence that decides
# whether the frame is usable. Split out of bv12-river-captures.ps1 because a
# backgrounded driver was killed mid-run and left a pose half-shot: a capture
# set that cannot be resumed frame-by-frame has to be restarted whole.
#
# THE SETTLE RULE THIS PRINTS IS ALTITUDE-DEPENDENT, and that is the point.
# RefreshImplicitWater's disc is 52 x 52 x 26 m centred on the CAMERA, so at any
# altitude above ~26 m there is no implicit water in it and the DRAINED line is
# structurally impossible. voxel-capture.ps1 warns about its absence anyway.
#   * near-field (<= ~26 m):  require `RefreshImplicitWater: DRAINED`
#   * altitude:               require `River ribbons: DRAINED build`, and read
#                             the reach/quad counts off it
# Reading a structurally-absent line as a failed settle wastes a re-shoot; worse,
# treating the warning as noise at 10 m lets a genuinely unsettled near-field
# frame through. Both lines are printed here so the caller judges the right one.

param(
    [Parameter(Mandatory=$true)][string]$Name,   # e.g. knick-1km
    [Parameter(Mandatory=$true)][string]$At,
    [Parameter(Mandatory=$true)][double]$Alt,
    [Parameter(Mandatory=$true)][double]$Pitch,
    [Parameter(Mandatory=$true)][double]$Yaw,
    [Parameter(Mandatory=$true)][ValidateSet('on','off')][string]$Arm,
    [int]$SettleSec = 300
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$Out  = Join-Path $Root 'docs\water-map\captures'
$full = "bv12-$Name-$Arm"
$dest = Join-Path $Out "$full.png"

$extra = @(
    '-VoxelTileDir=D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-80b9ca451a23eae4\000000000135276f\s1',
    '-VoxelFineTileDir=D:\voxelsim\tile-cache',
    '-VoxelFineTileProviderId=terrain-diffusion-unlabeled-80b9ca451a23eae4-b52995abb',
    '-VoxelFineTileGateFatal=0')
if ($Arm -eq 'off') { $extra += '-VoxelRiverRibbons=0' }

& "$Root\tools\voxel-capture.ps1" -Name $full -SpawnAt $At -SpawnAltM $Alt `
    -SpawnPitch $Pitch -SpawnYaw $Yaw -SettleSec $SettleSec -TimeoutSec 500 `
    -ExtraArgs $extra | Out-Null

# Take the shot the SCRIPT reported, from its own "wrote <path>" line, rather
# than the newest png in the directory -- a stale file would otherwise be copied
# under this pose's name and nothing would say so.
$L = Join-Path $Root "Saved\capture-$full.log"
$shot = (Get-ChildItem (Join-Path $Root 'ue-project\Saved\Screenshots\WindowsEditor') -Filter *.png |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1)
Copy-Item $shot.FullName $dest -Force

function L1($pat) { $m = Select-String $L -Pattern $pat | Select-Object -Last 1; if ($m) { $m.Line } else { $null } }
$rib = L1 'River ribbons: DRAINED build.*'
$dis = L1 'River ribbons: DISABLED.*'
$str = L1 'Voxel streaming: loaded=.*jobsInFlight.*'
$fin = L1 'Fine tier \(5s window\).*'
$cam = L1 'Capture: cam loc=.*'

Write-Host "FILE     : $dest"
Write-Host "SETTLE   : $(if ($str) { ($str -replace '.*Voxel streaming: ','') } else { 'MISSING' })"
Write-Host "RIBBON   : $(if ($rib) { ($rib -replace '.*River ribbons: ','') } elseif ($dis) { 'DISABLED (control armed)' } else { 'MISSING' })"
Write-Host "IMPLICIT : DRAINED x$(@(Select-String $L -Pattern 'RefreshImplicitWater: DRAINED').Count), STILL DRAINING x$(@(Select-String $L -Pattern 'STILL DRAINING').Count)"
Write-Host "SHEETS   : x$(@(Select-String $L -Pattern 'Lake sheets: DRAINED').Count)"
Write-Host "TILES    : refused $(@(Select-String $L -Pattern 'was REFUSED').Count), undrawn $(@(Select-String $L -Pattern 'Chunk left undrawn').Count)"
Write-Host "FINE     : $(if ($fin) { ($fin -replace '.*Fine tier ','') } else { 'MISSING' })"
Write-Host "SHUTTER  : $(if ($cam) { ($cam -replace '.*Capture: ','') } else { 'MISSING' })"
