# Build a target, refusing to start if anything else is using the box.
#
# WHY THIS EXISTS, AND IT IS THE SYMMETRIC HALF OF A GUARD THAT ALREADY EXISTED.
# tools/voxel-run-flight-leg.ps1 refuses to start a leg while a build or another
# editor is running. Nothing refused the REVERSE, and the reverse is the
# expensive direction: a leg holds UnrealEditor-VoxelEarthShaders.dll, the build
# cannot open it for write, and you get
#
#     Link ... Exited with error code 9001
#     LINK : fatal error LNK1104: cannot open file ...UnrealEditor-VoxelEarthShaders.dll
#
# ONE DLL of the pair relinks and the other does not. Nothing afterwards says so,
# and every leg run against that pair measures a binary that does not exist in
# source. This happened three times on 2026-08-24/25, twice to sessions that had
# each just warned the other about it.
#
# THE EXIT CODE IS NOT THE VERDICT. Build.bat's own `Result:` line is. A piped
# Build.bat reports the PAGER's status, so `Build.bat ... | tail` says 0 over a
# `Result: Failed`. This script redirects to a file and greps.
#
# COHERENCE IS PROVED BY A SECOND BUILD, not by the first succeeding: only
# "Target is up to date" shows the tree and the binaries agree. -Verify does it.
param(
    [string]$Target = 'VoxelEarthEditor',
    [string]$Project = 'D:\voxelsim\ue-project\VoxelEarth.uproject',
    [string]$Engine = 'D:\UE_5.8',
    [string]$LogDir = 'D:\voxelsim\Saved',
    [switch]$Verify,
    [switch]$AllowDirty
)
# dotnet is on this list deliberately: UnrealBuildTool runs as dotnet.exe, so a
# build in its UBT phase is invisible to a cl/link/UnrealBuildTool check -- which
# is exactly the window in which the link happens.
$busy = @(Get-Process UnrealEditor-Cmd, UnrealEditor, cl, link, UnrealBuildTool, MSBuild, dotnet -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
    $names = ($busy | Select-Object -Unique ProcessName | ForEach-Object { $_.ProcessName }) -join ', '
    throw ("REFUSING TO BUILD: the box is busy -- $names. A build that starts while an " +
           "editor holds the module DLLs fails its link and leaves ONE half of the pair " +
           "relinked, which silently invalidates every leg run afterwards. Wait for it, " +
           "or stop it. See this script's header.")
}
# AND THE OTHER SESSION'S HALF-WRITTEN FILE, WHICH NO PROCESS CHECK CAN SEE.
#
# The busy-check above is about the BOX. This is about the TREE, and it is the
# failure that actually cost time on 2026-08-25: three sessions share one
# checkout, and background agents edit files in it. An agent halfway through a
# refactor leaves the module inconsistent ON DISK with nothing running, so every
# process check passes and the build fails with what look like ordinary compile
# errors -- in a file the person building has never opened.
#
# That is the expensive part. The failure is INDISTINGUISHABLE from a bug in
# whatever you changed most recently, and on a shared checkout that is usually
# not the thing that is broken. I was only saved by knowing my own agent was
# running; the front-end session would have found C2511 in VoxelRasterAtlas.cpp
# and spent twenty minutes wondering how an SBox change could have caused it.
#
# This does NOT know whether an edit is finished -- nothing here can. It answers
# the weaker question that was still enough for both of us: "does somebody have
# uncommitted work in the module I am about to compile?" Pass -AllowDirty when
# the uncommitted work is yours and you know it is consistent.
# (The check, and the reasoning, are the front-end session's.)
if (-not $AllowDirty) {
    $dirty = @(& git -C (Split-Path $Project) status --short -- (Join-Path (Split-Path $Project) 'Source') 2>$null)
    if ($dirty.Count -gt 0) {
        Write-Host "  uncommitted work in Source/ -- $($dirty.Count) file(s):" -ForegroundColor Yellow
        $dirty | Select-Object -First 8 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        Write-Host ("  If any of that is another session's agent mid-edit, this build will fail with " +
                    "compile errors that are NOT yours. Pass -AllowDirty if it is all yours and consistent.") -ForegroundColor Yellow
    }
}

$log = Join-Path $LogDir ("build-" + $Target + ".log")
$bat = Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat'
& $bat $Target Win64 Development "-project=$Project" -WaitMutex -NoHotReloadFromIDE -DisableAdaptiveUnity *> $log
if (-not (Select-String -Path $log -Pattern '^Result: Succeeded' -Quiet)) {
    Select-String -Path $log -Pattern 'error C\d+|error:|LNK\d+|^Result:' | Select-Object -First 12 | ForEach-Object { $_.Line }
    throw "BUILD FAILED -- see $log. Note: LNK1104 is a COLLISION, not a code error; check the box before blaming the patch."
}
Write-Host "  build ok: $Target" -ForegroundColor Green
if ($Verify) {
    & $bat $Target Win64 Development "-project=$Project" -WaitMutex -NoHotReloadFromIDE -DisableAdaptiveUnity *> "$log.verify"
    if (Select-String -Path "$log.verify" -Pattern 'Target is up to date' -Quiet) {
        Write-Host "  coherence proved: 'Target is up to date'" -ForegroundColor Green
    } else {
        throw "COHERENCE UNPROVEN: a second build did not report 'Target is up to date'. The tree and the binaries disagree."
    }
}
exit 0
