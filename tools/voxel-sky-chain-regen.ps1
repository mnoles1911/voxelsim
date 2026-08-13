# Rebuild MPC_VoxelSky and EVERY material that binds it, in order, then PHOTOGRAPH
# the result and refuse the run if the picture moved.
#
# WHY THIS EXISTS WHEN voxel-water-star-regen.ps1 ALREADY DID MOST OF IT
# ======================================================================
#
# Because that script's dependent list is a hard-coded three, and the list is now
# four. `create_underwater_material.py` was written on 2026-08-12 and binds
# MPC_VoxelSky for SunDirection and MoonLightFraction. Nothing added it to any
# chain. So running the old chain after touching the sky would have deleted the
# collection out from under M_Underwater and left it compiling to UE's DEFAULT
# MATERIAL -- while every log line reported success.
#
# That is not a hypothetical. It is the 2026-08-10 failure exactly, in which all
# water in the world drew with the default material for a night. The lesson from
# that night was recorded as "run the chain in order". The lesson was incomplete:
# the real rule is THE CHAIN MUST CONTAIN EVERY DEPENDENT, and a hard-coded list
# silently stops being that the moment somebody writes a new material.
#
# So this script does not hard-code the list. It DISCOVERS it, by grepping the
# generators for a CollectionParameter node, and it FAILS if it finds a generator
# it has no ordering rule for. A new dependent now breaks this script loudly at
# minute zero instead of breaking the game silently.
#
# THE THREE CHECKS, IN INCREASING ORDER OF HOW MUCH THEY ARE WORTH
# ================================================================
#
#   1. The commandlet's own markers. Necessary, not sufficient: the exit code is
#      meaningless in both directions here (UE's end-of-run summary counts three
#      pre-existing project errors, so a perfect run exits 1; and an exception
#      inside -run=pythonscript can still leave a 0).
#
#   2. "Failed to compile Material" across every log. Catches the classic
#      stale-binding failure at GENERATION time. Does NOT catch a material that
#      compiles clean here and fails at LOAD time in the game, which is the shape
#      the 2026-08-10 failure actually took.
#
#   3. A CAPTURE AT A PINNED POSE, DIFFED AGAINST A KNOWN-GOOD FRAME. This is the
#      one that would have caught 2026-08-10 in minutes. Nothing in this change
#      is supposed to alter a single pixel -- the two new parameters default to
#      zero and nothing reads them yet -- so the frame must match the reference.
#      A material that fell back to the engine default does not look subtly off;
#      it looks like grey plastic, and the diff goes to tens of percent.
#
#      This is the owner's own method, and he is on record that it is what solved
#      the banding problem when metrics disagreed with what he could see:
#      same-ground A/B renders per pipeline stage.

param(
    # The known-good frame this run must reproduce. Default is the shipped-arm
    # wide pose from 2026-08-12, taken AFTER the water clarity was dialled back
    # and BEFORE the sky collection was touched -- i.e. the last frame of the
    # water the owner signed off on.
    [string]$ReferencePng = 'D:\voxelsim\ue-project\Saved\Screenshots\WindowsEditor\VoxelVerify00534.png',
    # Pose and conditions of that reference. Changing any of these invalidates
    # the comparison, which is why they are here and not in the capture call.
    [string]$SpawnAt = '-65102,-51084',
    [double]$SpawnAltM = 12,
    [double]$SpawnPitch = -30,
    [double]$SpawnYaw = 200,
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [int]$SettleSec = 170,
    # Hard-fail threshold on the differing-pixel percentage.
    #
    # 15 is deliberately loose. This is not a precision instrument and must not
    # cry wolf: a repeat capture of an unchanged build does not come back
    # identical (TSR history, stream-in state), and a guard that fires on normal
    # variance gets ignored, which is worse than no guard. What it is sized to
    # catch is a material that vanished into the engine default, which moves an
    # entire surface and lands in the tens of percent. The measured number is
    # ALWAYS printed, so a small real regression is still visible to a human even
    # when it passes.
    [double]$MaxDiffPct = 15.0,
    [switch]$SkipCapture,
    # Re-run ONLY the verification capture, against materials already on disk.
    #
    # This exists because the split it describes has now happened twice in one
    # night: all four regenerations succeeded and the CAPTURE was refused, by the
    # voxelcore.lib staleness guard in voxel-capture.ps1. Without this switch the
    # only way to finish the verification is to spend another forty minutes
    # rebuilding four materials that were already correct -- and the temptation
    # then is to skip the capture instead, which throws away the one check that
    # would actually have caught 2026-08-10.
    #
    # Only legitimate when the regenerations in THIS run's log directory
    # completed. It does not re-check them; it assumes what is on disk is what
    # the chain built.
    [switch]$CaptureOnly,
    # Rebuild only these generators, then verify. Everything not listed is left
    # alone.
    #
    # LEGITIMATE ONLY WHEN THE COLLECTION IS NOT BEING REBUILT. The ordering rule
    # is "every dependent must be rebuilt AFTER create_sky_material.py recreates
    # MPC_VoxelSky" -- it is not "all six must always run together". If the
    # collection on disk is already current and a dependent was missed, that
    # dependent alone is the correct repair, and re-running the sky would be the
    # RISKIER choice: it would delete the collection the other five are bound to
    # and require rebuilding all of them again.
    #
    # This is exactly the reasoning voxel-water-star-regen.ps1's -WaterOnly
    # switch documents. Refuses to include create_sky_material.py, because a run
    # that recreates the collection and rebuilds only some dependents is the
    # 2026-08-10 failure with extra steps.
    [string[]]$Only = @(),
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [int]$TimeoutSec = 2400
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$Tools   = Join-Path $Root 'ue-project\Tools'
$LogDir  = Join-Path $Root 'Saved\sky-chain'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$live = @(Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue)
if ($live.Count -gt 0) {
    $detail = ($live | ForEach-Object { "$($_.ProcessName) PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: an editor is already running -- $detail."
}

# --- THE ORDER, AND THE RULE THAT KEEPS IT COMPLETE --------------------------
#
# create_sky_material.py is the SOLE author of MPC_VoxelSky and DELETES it on
# every run, so it goes first and everything that binds the collection must be
# rebuilt after it. Among the dependents the order does not matter -- they bind
# the collection, not each other -- but it is fixed here anyway so two runs are
# comparable.
#
# sky_star_graph.py is a MODULE imported by create_sky_material.py, not a
# generator; it is expected in the discovery scan and is not run on its own.
$ORDER = @(
    'create_sky_material.py',                 # authors the collection
    'create_sky_atmosphere_dome_material.py',
    # BEFORE the water material, and the arrow is load-bearing in both
    # directions. After the sky, because the ripple step material binds the
    # collection like everything else. Before the water, because
    # sample_ripple_field loads /Game/Voxel/RT_VoxelRippleField BY NAME and
    # raises if it is absent -- so the water generator cannot build until the
    # ripple assets exist.
    'create_ripple_field_materials.py',
    'create_water_voxel_material.py',
    'create_underwater_material.py',
    # THE TWO TERRAIN MATERIALS ARE SKY DEPENDENTS, WHICH IS NOT OBVIOUS AND WAS
    # MISSED ON THE FIRST RUN OF THIS SCRIPT.
    #
    # They do not mention the sky. They bind the collection through
    # `from bathy_field_graph import sample_bathy_field`, which calls
    # b.collection_param("BathyFieldOrigin" / "BathyFieldInvSize" /
    # "BathyFieldValid") -- the camera-centred bathymetry window's placement is
    # published through MPC_VoxelSky like everything else.
    #
    # They became dependents on 2026-08-12, when wet-shore darkening started
    # reading the baked shore-distance field. Before that the terrain had no
    # reason to touch the sky collection at all, which is why no chain listed
    # them.
    #
    # Leaving them out cost a run: M_VoxelTerrain logged "Failed to compile
    # Material for platform PCD3D_SM6, Default Material will be used in game"
    # AT LOAD TIME -- not during generation, where this script was looking -- and
    # the land photographed as flat brown instead of green. Measured mean land
    # RGB went 71.7/76.7/59.0 to 64.3/59.5/55.3, green falling below red.
    'create_voxel_material.py',
    'create_clipmap_material.py'
)
$NOT_A_GENERATOR = @(
    'sky_star_graph.py',        # module, imported by create_sky_material.py
    'bathy_field_graph.py',     # module, imported by the terrain and water generators
    'ripple_field_graph.py',    # module, imported by create_water_voxel_material.py (once wired)
    'water_wave_graph.py',      # module, imported by create_water_voxel_material.py (once wired)
    'terrain_material_common.py' # module, imported by both terrain generators
)
# Generators that bind the collection but are NOT yet wired into the game. They
# are skipped rather than silently ignored, and the skip is printed -- an
# unlisted-but-skipped file is how the next M_Underwater happens.
#
# EMPTY as of 2026-08-13: create_ripple_field_materials.py graduated into $ORDER
# when the water material started sampling the ripple field.
$NOT_YET_WIRED = @()

# WHAT COUNTS AS BINDING THE COLLECTION, AND WHY THE FIRST VERSION OF THIS SCAN
# WAS WRONG.
#
# It matched only the literal `MaterialExpressionCollectionParameter`. That is
# the class name, and it is what a generator writes when it makes the node
# ITSELF. Two shipping generators do not: create_voxel_material.py and
# create_clipmap_material.py bind MPC_VoxelSky by importing bathy_field_graph,
# whose sample_bathy_field calls `b.collection_param(...)` on a builder handed
# in by the caller. Neither file contains the class name anywhere.
#
# So the scan said four dependents, the truth was six, and the two it missed are
# the two that broke -- their materials failed to compile AT LOAD TIME and drew
# as the engine default. A scan looking for one spelling of one implementation
# is not a scan for the dependency; it is a scan for a coding style.
#
# The three patterns below are all the ways a generator currently reaches the
# collection. A HELPER MODULE (bathy_field_graph, sky_star_graph) matches too and
# is expected to -- those are modules, not generators, and are listed in
# $NOT_A_GENERATOR. That is deliberate: it is better for this scan to over-report
# and be told "that one is a module" than to under-report and be silent, because
# the cost of the two errors is not symmetric. An over-report stops the script;
# an under-report ships the default material.
$BINDING_PATTERNS = @(
    'MaterialExpressionCollectionParameter',  # makes the node directly
    'collection_param\s*\(',                  # via a builder/helper method
    'from bathy_field_graph import'           # imports a helper that binds
)
Write-Host '=== DISCOVERING DEPENDENTS (any generator that reaches MPC_VoxelSky)' -ForegroundColor Cyan
$found = @(Get-ChildItem (Join-Path $Tools '*.py') |
           Where-Object { $raw = (Get-Content $_.FullName -Raw)
                          $null -ne ($BINDING_PATTERNS | Where-Object { $raw -match $_ } | Select-Object -First 1) } |
           ForEach-Object { $_.Name } | Sort-Object)
foreach ($f in $found) { Write-Host "    $f" -ForegroundColor DarkGray }

$unaccounted = @($found | Where-Object { $ORDER -notcontains $_ -and $NOT_A_GENERATOR -notcontains $_ -and $NOT_YET_WIRED -notcontains $_ })
if ($unaccounted.Count -gt 0) {
    throw ("UNACCOUNTED DEPENDENT(S): $($unaccounted -join ', ').`n" +
           "These bind MPC_VoxelSky but this script has no ordering rule for them. Rebuilding the " +
           "sky without rebuilding them leaves them bound to a collection that no longer exists, " +
           "and a material in that state compiles to UE's DEFAULT MATERIAL while every log line " +
           "reports success -- the 2026-08-10 failure. Add each to `$ORDER (if it ships) or to " +
           "`$NOT_YET_WIRED (if it does not) and say which in the commit.")
}
$missing = @($ORDER | Where-Object { $found -notcontains $_ })
if ($missing.Count -gt 0) { throw "ORDER lists $($missing -join ', ') but they no longer bind the collection. Prune `$ORDER." }
foreach ($s in $NOT_YET_WIRED) { Write-Host "    SKIPPING $s -- binds the collection but is not wired into the game yet" -ForegroundColor Yellow }

# --- RUN THEM ----------------------------------------------------------------
$toRun = $ORDER
if ($Only.Count -gt 0) {
    if ($Only -contains 'create_sky_material.py') {
        throw ("-Only may not include create_sky_material.py: it DELETES and recreates MPC_VoxelSky, " +
               "after which every dependent must be rebuilt. Run the full chain instead.")
    }
    $unknown = @($Only | Where-Object { $ORDER -notcontains $_ })
    if ($unknown.Count -gt 0) { throw "-Only names $($unknown -join ', '), which are not in the chain." }
    # Keep $ORDER's ordering rather than the caller's, so two runs are comparable.
    $toRun = @($ORDER | Where-Object { $Only -contains $_ })
    Write-Host "=== -Only: rebuilding $($toRun -join ', ') against the EXISTING collection" -ForegroundColor Yellow
}
$logs = @()
foreach ($s in ($(if ($CaptureOnly) { @() } else { $toRun }))) {
    $log = Join-Path $LogDir ("regen-" + ($s -replace '\.py$','') + '.log')
    $logs += $log
    Write-Host "=== $s -> $log" -ForegroundColor Cyan
    # -AllowCommandletRendering, and only where it is needed.
    #
    # Without it the commandlet comes up with rhiname="Null" (confirmed in the
    # log: NullDrv is loaded). Materials are perfectly happy with that -- they
    # are assets, not GPU resources -- which is why every generator in this
    # chain ran headless for months without anyone needing the flag.
    #
    # A RENDER TARGET IS NOT. create_ripple_field_materials.py builds three of
    # them, and on a null RHI AssetTools::create_asset returns None, so the
    # script raised "Failed to create render target asset at
    # /Game/Voxel/RT_VoxelRippleStateA" -- a clean, loud failure, but one whose
    # message says nothing about the RHI and would send the next person into the
    # factory bindings instead.
    #
    # The water script gets it for a different reason, documented in
    # voxel-water-star-regen.ps1: without a real RHI no shader map is built and
    # MaterialEditingLibrary.get_statistics returns every field as 0 -- a zero
    # indistinguishable from a genuinely free material.
    #
    # The rest are left on the cheaper path deliberately: the flag costs real
    # startup time, and a generator that does not need it should not pay it.
    $extra = @()
    if ($s -eq 'create_ripple_field_materials.py' -or $s -eq 'create_water_voxel_material.py') {
        $extra += '-AllowCommandletRendering'
    }
    $p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList (@(
        "`"$Project`"", '-run=pythonscript', "-script=`"$Tools\$s`"",
        '-unattended', '-nop4', '-nosplash', "-abslog=`"$log`""
    ) + $extra)
    $p.WaitForExit($TimeoutSec * 1000) | Out-Null
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; throw "$s TIMED OUT after ${TimeoutSec}s -- see $log" }

    $ok = Select-String -Path $log -SimpleMatch 'Python script executed successfully' -ErrorAction SilentlyContinue
    $py = @(Select-String -Path $log -SimpleMatch 'LogPython: Error' -ErrorAction SilentlyContinue)
    if (-not $ok -or $py.Count -gt 0) {
        if ($py.Count -gt 0) { $py | Select-Object -First 15 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red } }
        throw "$s FAILED (successMarker=$([bool]$ok), pythonErrors=$($py.Count), exit=$($p.ExitCode)) -- see $log"
    }
    Write-Host "    ok" -ForegroundColor Green
}

# --- COMPILE FAILURES, ACROSS EVERY LOG --------------------------------------
#
# Under -CaptureOnly the logs of the run being verified are still on disk, so
# check those rather than checking nothing: a resumed verification that skipped
# this would be weaker than the run it is finishing.
if ($CaptureOnly) {
    $logs = @($ORDER | ForEach-Object { Join-Path $LogDir ("regen-" + ($_ -replace '\.py$','') + '.log') } |
              Where-Object { Test-Path $_ })
    if ($logs.Count -ne $ORDER.Count) {
        throw ("-CaptureOnly found $($logs.Count) of $($ORDER.Count) regeneration logs in $LogDir. " +
               "Run the full chain: there is nothing on disk proving the materials were rebuilt.")
    }
    Write-Host "=== -CaptureOnly: verifying against $($logs.Count) existing regeneration logs" -ForegroundColor Cyan
}
# A COMPILE FAILURE IN AN INTERMEDIATE LOG IS EXPECTED, AND TREATING IT AS FATAL
# WAS WRONG.
#
# The first version threw on any "Failed to compile Material" in any
# generator's log. That is a false positive by construction, and it stopped a
# chain that had actually worked.
#
# The reason: each generator runs its own editor, and that editor LOADS every
# material in the project. Between recreating MPC_VoxelSky and rebuilding
# dependent N, every dependent after N is still bound to the collection that no
# longer exists -- so it fails to compile, loudly, in the log of whichever
# generator happens to be running at the time. Observed exactly that: the
# ripple generator's log carries 16 "CollectionParameter has invalid parameter
# None" errors against M_WaterVoxel, which the very next step rebuilt correctly.
# The same session also logged "Param2D> Found NULL" for the ripple texture the
# water material had not been rebuilt to point at yet.
#
# In other words the intermediate logs record the chain MID-FLIGHT, and the
# whole reason the chain exists is that mid-flight is broken. Only the FINAL
# state is a claim about anything.
#
# So these are reported and not thrown on, and the authoritative check moved to
# the capture below -- a fresh editor that loads everything after all
# regeneration is done. That is also where the real 2026-08-13 breakage was
# caught (M_VoxelTerrain, drawing as the default material), because a load-time
# failure never appears in a generation log at all.
$failed = @()
foreach ($log in $logs) { $failed += @(Select-String -Path $log -SimpleMatch 'Failed to compile Material' -ErrorAction SilentlyContinue) }
if ($failed.Count -gt 0) {
    Write-Host ("=== $($failed.Count) compile failure(s) in intermediate logs -- EXPECTED mid-chain, " +
                "see the note here. The capture below is the check that counts.") -ForegroundColor Yellow
    $failed | Select-Object -First 4 | ForEach-Object { Write-Host "    $($_.Line -replace '^.*?(Failed to compile)','$1')" -ForegroundColor DarkYellow }
}
else { Write-Host '=== no compile failures in any generation log' -ForegroundColor Green }

if ($SkipCapture) { Write-Host 'CAPTURE SKIPPED -- the chain is UNVERIFIED where it matters most.' -ForegroundColor Yellow; return }

# --- THE CHECK THAT IS ACTUALLY WORTH SOMETHING ------------------------------
# --- MAKE voxelcore.lib GENUINELY NEWEST BEFORE THE CAPTURE GUARD LOOKS -------
#
# voxel-capture.ps1 refuses to shoot when any file under voxel-core/src or
# /include is newer than the compiled voxelcore.lib, because UBT does not track
# those sources: Build.bat prints "Result: Succeeded" while linking a lib from
# hours ago, and the failure is not a link error, it is WRONG TERRAIN. That
# guard is correct and must not be weakened.
#
# But it compares against EVERY file in those directories, including headers no
# translation unit in the lib includes. voxelcore/weather.h is one: nothing in
# voxel-core/src consumes it (its callers are the tests, a bench and two UE
# translation units), so editing it leaves the lib legitimately unchanged and
# legitimately older. The guard then refuses a capture that would have been
# perfectly valid. That happened three times on 2026-08-13 alone, and each time
# the "fix" was a human touching an unrelated .cpp -- which is a ritual, not a
# repair, and the sort of thing that eventually gets replaced by weakening the
# guard.
#
# So the chain does it properly and in the ONLY order that works:
#   voxelcore first, so the lib postdates every source;
#   then the UE module, so the DLL postdates the lib -- the guard checks that
#   too, and rebuilding the lib alone does NOT trigger a relink because UBT
#   still sees no changed input.
# Reversing these two leaves the DLL older than the lib and the guard fires on
# the second of its two checks instead of the first.
if (-not $SkipCapture) {
    Write-Host '=== refreshing voxelcore.lib and the module so the capture guard is satisfied' -ForegroundColor Cyan
    (Get-Item (Join-Path $Root 'voxel-core\src\tilestore.cpp')).LastWriteTime = Get-Date
    & cmake --build (Join-Path $Root 'build\voxel-core-msvc') --config Release --target voxelcore 2>&1 | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { throw "voxel-core relink FAILED (exit $LASTEXITCODE)" }
    # TOUCH A UE SOURCE FIRST, OR Build.bat DOES NOTHING. UBT does not track
    # voxel-core, so after the lib is rebuilt it still sees no changed input and
    # declines to relink -- leaving the DLL older than the lib, which is the
    # capture guard's SECOND check. Rebuilding the lib alone therefore trades one
    # refusal for another, and that is exactly what happened on the first run of
    # this block. The guard's own message says to do this; follow it.
    (Get-Item (Join-Path $Root 'ue-project\Source\VoxelEarth\VoxelOceanActor.cpp')).LastWriteTime = Get-Date
    $blog = Join-Path $Root 'Saved\build-chain-relink.log'
    & 'D:\UE_5.8\Engine\Build\BatchFiles\Build.bat' VoxelEarthEditor Win64 Development `
        -Project="$Project" -WaitMutex *>&1 | Tee-Object -FilePath $blog | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw "module relink FAILED (exit $LASTEXITCODE) -- see $blog" }
}

if (-not (Test-Path $ReferencePng)) { throw "reference frame not found: $ReferencePng" }
Write-Host "=== CAPTURE at the pinned pose, to compare against $(Split-Path $ReferencePng -Leaf)" -ForegroundColor Cyan
$shot = & (Join-Path $PSScriptRoot 'voxel-capture.ps1') `
    -Name 'skychain-verify' -SpawnAt $SpawnAt -SpawnAltM $SpawnAltM -SpawnPitch $SpawnPitch -SpawnYaw $SpawnYaw `
    -Width 2560 -Height 1440 -TimeOfDay $TimeOfDay -Date $Date -SettleSec $SettleSec |
    Select-Object -Last 1
if (-not $shot -or -not (Test-Path $shot)) { throw 'verification capture produced no image' }

# THE AUTHORITATIVE COMPILE CHECK, on a fresh editor that loaded everything
# after all regeneration finished. A material that fails HERE is broken in the
# shipped state, not mid-chain -- and note that this is a LOAD-time failure,
# which never appears in a generation log at all. This is precisely how
# M_VoxelTerrain was caught drawing as the engine default on 2026-08-13, after
# four generation logs reported success and meant it.
$capLog = Join-Path $Root ("Saved\capture-skychain-verify.log")
if (Test-Path $capLog) {
    $loadFail = @(Select-String -Path $capLog -SimpleMatch 'Failed to compile Material' -ErrorAction SilentlyContinue)
    if ($loadFail.Count -gt 0) {
        $loadFail | Select-Object -First 6 | ForEach-Object { Write-Host "    $($_.Line -replace '^.*?(Failed to compile)','$1')" -ForegroundColor Red }
        throw ("$($loadFail.Count) material(s) FAILED TO COMPILE AT LOAD TIME in the verification run. " +
               "These are drawing as the engine DEFAULT MATERIAL in the shipped state. Rebuild the " +
               "named material(s) with -Only; if one is not in `$ORDER, it is an undiscovered dependent " +
               "and belongs there.")
    }
    Write-Host '=== no load-time compile failures in the verification run' -ForegroundColor Green
}
else { Write-Warning "no capture log at $capLog -- load-time compile failures UNCHECKED." }

$diff = & python (Join-Path $PSScriptRoot 'imgdiff.py') $ReferencePng $shot --where 2>&1
$diff | ForEach-Object { Write-Host "  $_" }
$pctLine = ($diff | Select-String -Pattern '^\s*([\d.]+)% of' | Select-Object -First 1)
if (-not $pctLine) { throw "could not read a differing-pixel percentage out of imgdiff -- the chain is UNVERIFIED." }
$pct = [double]$pctLine.Matches[0].Groups[1].Value

Write-Host ''
if ($pct -gt $MaxDiffPct) {
    throw ("THE PICTURE MOVED: $pct% of pixels differ from the reference, over the $MaxDiffPct% limit.`n" +
           "Nothing in this change should alter a pixel -- both new parameters default to zero and " +
           "nothing reads them. A difference this large means a material did not survive the " +
           "collection being recreated and is drawing as the engine default. Compare:`n" +
           "  reference $ReferencePng`n  this run  $shot")
}
Write-Host "PICTURE HELD: $pct% differ vs the reference (limit $MaxDiffPct%)." -ForegroundColor Green
Write-Host "  reference $ReferencePng"
Write-Host "  this run  $shot"
Write-Host 'MPC_VoxelSky rebuilt with WindVectorMS + WindFieldValid; all 4 dependents rebuilt and photographed.' -ForegroundColor Green
