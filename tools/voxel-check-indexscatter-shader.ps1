# voxel-check-indexscatter-shader.ps1 -- does VoxelMarchIndexScatter.usf
# compile, at every entry point?
#
# THE SAME REASON THE OTHER voxel-check-*-shader.ps1 SCRIPTS EXIST, and the
# same rule. ue-project\Shaders is shared, the editor compiles global shaders
# AT BOOT, and this file now carries THREE IMPLEMENT_GLOBAL_SHADER entry
# points (VoxelMarchChunkIndex.cpp): the delta scatter, the Phase 2 publish,
# and -- since 2026-08-27 -- the anySolid refine/audit kernel. A half-finished
# .usf here does not fail a build; it fails an editor LAUNCH, and costs a whole
# leg to discover. A half-finished .cpp cannot do that.
#
# WHAT THIS PROVES: the three kernels parse, type-check and generate DXIL under
# SM6.0 with the defines their host class pushes.
#
# WHAT IT DOES NOT PROVE, stated so nobody reads more into a green run than is
# there: nothing about whether the refine kernel CLEARS THE RIGHT BITS. That is
# what voxel.March.IndexAnySolidAudit is for, and what
# voxel.March.IndexAnySolidPoison exists to show that audit can fail. A
# compiling kernel that deletes terrain compiles exactly as cleanly as a
# correct one.

param([string]$Root = '')

if (-not $Root) {
    $Probe = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    if ($Probe -and (Test-Path (Join-Path $Probe 'ue-project'))) { $Root = $Probe }
    else { $Root = 'D:\voxelsim' }
}
$Root = (Resolve-Path $Root).Path
$Dxc = Join-Path $Root 'tools\dxc\bin\x64\dxc.exe'
# A worktree checkout has no fetched dxc; fall back to the main checkout's.
if (-not (Test-Path $Dxc)) { $Dxc = 'D:\voxelsim\tools\dxc\bin\x64\dxc.exe' }
$Usf = Join-Path $Root 'ue-project\Shaders\VoxelMarchIndexScatter.usf'
$CellUsh = Join-Path $Root 'ue-project\Shaders\VoxelMarchIndexCell.ush'
$IndexHdr = Join-Path $Root 'ue-project\Source\VoxelEarthShaders\Public\VoxelMarchChunkIndex.h'
$PoolHdr = Join-Path $Root 'ue-project\Source\VoxelEarthShaders\Public\VoxelBrickPool.h'
$Stage = Join-Path $env:TEMP 'voxel-indexscatter-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc (run tools/fetch-dxc.ps1)" }
if (-not (Test-Path $Usf)) { throw "VoxelMarchIndexScatter.usf not found at $Usf" }
if (-not (Test-Path $CellUsh)) { throw "VoxelMarchIndexCell.ush not found at $CellUsh" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# THE CONSTANTS ARE READ FROM THE HEADERS, NOT TYPED HERE. The refine kernel's
# host class pushes them as shader defines precisely so there is no hand
# mirror; a script that typed its own copy would go green over exactly the
# drift the defines exist to prevent.
$RecLine = Select-String -Path $PoolHdr -Pattern 'kChunkRecordDwords = (\d+)'
if (-not $RecLine) { throw "kChunkRecordDwords not found in $PoolHdr" }
$RecordDwords = $RecLine.Matches[0].Groups[1].Value

$WordsLine = Select-String -Path $IndexHdr -Pattern 'kRefineStatWords = (\d+)'
if (-not $WordsLine) { throw "kRefineStatWords not found in $IndexHdr" }
$StatWords = $WordsLine.Matches[0].Groups[1].Value

# The stat word ORDER is the enum's order, and the enum is the buffer layout.
# Parsed in order rather than by name-to-number so a reordering shows up here
# as a different set of defines, which is what it is.
$EnumBody = (Get-Content $IndexHdr -Raw)
$EnumMatch = [regex]::Match($EnumBody, 'enum ERefineStatWord : int32\s*\{(.*?)\}', 'Singleline')
if (-not $EnumMatch.Success) { throw "ERefineStatWord not found in $IndexHdr" }
$Names = [regex]::Matches($EnumMatch.Groups[1].Value, 'RefineStat_(\w+)') |
         ForEach-Object { $_.Groups[1].Value }
if ($Names.Count -ne [int]$StatWords) {
    throw "ERefineStatWord has $($Names.Count) entries but kRefineStatWords is $StatWords"
}
# The enum name -> the shader define name. Spelled once, here, and checked for
# completeness below: a new enum entry with no mapping FAILS the script rather
# than silently going uncompiled.
$DefineFor = @{
    'Examined'             = 'VOXEL_REFINE_STAT_EXAMINED'
    'NoMatch'              = 'VOXEL_REFINE_STAT_NOMATCH'
    'RefusedZeroRecord'    = 'VOXEL_REFINE_STAT_ZERO_RECORD'
    'RefusedOrigin'        = 'VOXEL_REFINE_STAT_BAD_ORIGIN'
    'RefusedLevel'         = 'VOXEL_REFINE_STAT_BAD_LEVEL'
    'RefusedCell'          = 'VOXEL_REFINE_STAT_BAD_CELL'
    'RefusedStride'        = 'VOXEL_REFINE_STAT_BAD_STRIDE'
    'RefusedInconsistent'  = 'VOXEL_REFINE_STAT_INCONSISTENT'
    'Cleared'              = 'VOXEL_REFINE_STAT_CLEARED'
    'CasLost'              = 'VOXEL_REFINE_STAT_CAS_LOST'
    'LeftSolid'            = 'VOXEL_REFINE_STAT_LEFT_SOLID'
    'AlreadyClear'         = 'VOXEL_REFINE_STAT_ALREADY_CLEAR'
    'AuditSolid'           = 'VOXEL_REFINE_STAT_AUDIT_SOLID'
}
$RefineDefines = @("VOXEL_MARCH_INDEX_REFINE_RECORD_DWORDS=$RecordDwords",
                   "VOXEL_REFINE_STAT_WORDS=$StatWords",
                   'VOXEL_REFINE_GROUP_SIZE=64')
for ($i = 0; $i -lt $Names.Count; $i++) {
    $N = $Names[$i]
    if (-not $DefineFor.ContainsKey($N)) {
        throw "ERefineStatWord::RefineStat_$N has no shader define in this script -- add it to `$DefineFor and to FVoxelMarchIndexRefineCS::ModifyCompilationEnvironment."
    }
    $RefineDefines += "$($DefineFor[$N])=$i"
}

# The virtual include paths only resolve inside the engine; stage flat copies
# with the includes rewritten. Nothing in the source tree is modified.
Copy-Item $CellUsh (Join-Path $Stage 'VoxelMarchIndexCell.ush')
(Get-Content $Usf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub').
    Replace('"/VoxelEarth/VoxelMarchIndexCell.ush"', '"VoxelMarchIndexCell.ush"') |
    Out-File (Join-Path $Stage 'VoxelMarchIndexScatter.hlsl') -Encoding utf8

$Src = Join-Path $Stage 'VoxelMarchIndexScatter.hlsl'
$Out = Join-Path $Stage 'out.bin'
$Fail = 0
$Total = 0

function Try-Compile($EntryPoint, $Label, [string[]]$Defines = @()) {
    $ArgList = @('-T', 'cs_6_0', '-E', $EntryPoint, '-HV', '2021', '-O3', '-Fo', $script:Out, $script:Src)
    foreach ($D in $Defines) { $ArgList += @('-D', $D) }
    # stderr to a file, not 2>&1: PowerShell 5.1 wraps native stderr lines in
    # NativeCommandError and aborts the run on the first failing kernel.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }
    if ($Failed) {
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 10 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

# ALL THREE GET THE SAME DEFINES, WHICH IS WHAT THE ENGINE DOES AND IS THE
# POINT OF THE FIRST RUN OF THIS SCRIPT.
#
# The three entry points share one .usf, so the refine kernel's BODY is in the
# translation unit for every one of them, macros and all. When only the refine
# shader class pushed the defines -- the obvious arrangement, and the one this
# feature was first written with -- VoxelMarchIndexScatterMain and
# VoxelMarchIndexPublishMain failed with "use of undeclared identifier
# 'VOXEL_REFINE_STAT_BAD_STRIDE'". That is not a build failure; it is an EDITOR
# BOOT fatal, and it costs a whole leg to discover. All three shader classes now
# call VoxelMarchIndexScatterSetDefines, and this models exactly that.
#
# So a FAIL on the scatter or publish line here almost certainly means a class
# in VoxelMarchChunkIndex.cpp lost its ModifyCompilationEnvironment -- look
# there first, not at the kernel the error points into.
$Total += 1; $Fail += Try-Compile 'VoxelMarchIndexScatterMain' 'scatter DXIL  VoxelMarchIndexScatterMain' $RefineDefines
$Total += 1; $Fail += Try-Compile 'VoxelMarchIndexPublishMain' 'publish DXIL  VoxelMarchIndexPublishMain' $RefineDefines
$Total += 1; $Fail += Try-Compile 'VoxelMarchIndexRefineMain'  'refine  DXIL  VoxelMarchIndexRefineMain'  $RefineDefines

# AND ONCE WITHOUT THE HOST'S DEFINES, which is not redundant. The file carries
# #ifndef fallbacks for the three shape defines so it stays readable standalone;
# the stat-word offsets deliberately have NONE, so a dropped SetDefine is a
# COMPILE ERROR rather than a silent write into word 0. This case asserts that
# failure mode still holds: it MUST fail, and the script fails if it succeeds.
$ErrFile = Join-Path $Stage 'dxc-stderr-nodefs.txt'
$Saved = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $Dxc @('-T', 'cs_6_0', '-E', 'VoxelMarchIndexRefineMain', '-HV', '2021', '-O3', '-Fo', $Out, $Src) 2>$ErrFile | Out-Null
$NoDefsOk = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = $Saved
$Total += 1
if ($NoDefsOk) {
    Write-Host 'FAIL  refine  GUARD  VoxelMarchIndexRefineMain compiled WITHOUT the stat-word defines'
    Write-Host '      -- a dropped SetDefine would then write every outcome into word 0 and the'
    Write-Host '         census would read a plausible wrong number instead of failing loudly.'
    Write-Host '         Remove whatever fallback was added for VOXEL_REFINE_STAT_*.'
    $Fail += 1
} else {
    Write-Host 'ok    refine  GUARD  refuses to compile without the stat-word defines'
}

Write-Host ''
if ($Fail -gt 0) {
    Write-Host "$Fail of $Total index-scatter kernels FAILED -- do not start a leg on this tree."
    exit 1
}
Write-Host "All $Total index-scatter kernels compile."
exit 0
