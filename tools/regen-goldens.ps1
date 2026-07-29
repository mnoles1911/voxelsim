# Regenerate voxel-core golden digests after a deliberate worldgen change.
#
# WHY THIS EXISTS. A kWorldGenVersion bump invalidates every pinned digest at
# once, and updating seven of them by hand -- reading a decimal "actual" out of
# a failure message, converting to hex, finding the right line -- is both
# tedious and exactly the kind of transcription a tired person gets wrong. A
# wrong golden is worse than no golden: it pins the bug.
#
# THIS IS NOT A "MAKE THE TESTS PASS" BUTTON. It rewrites the pinned values to
# whatever the code currently produces, so it is only ever correct to run it
# when you have already decided the output SHOULD have changed and you have
# verified the change some other way (vxc_terrainprobe, vxc_climateprobe, a
# screenshot). Run it on a dirty tree you are about to review, never on CI.
#
# Usage:  pwsh tools/regen-goldens.ps1 [-BuildDir D:\voxelsim\build\voxel-core]
param(
    [string]$BuildDir = "D:\voxelsim\build\voxel-core",
    [string]$RepoRoot = "D:\voxelsim"
)

$ErrorActionPreference = "Stop"

Write-Host "Building..." -ForegroundColor Cyan
cmake --build $BuildDir --config Release 2>&1 |
    Select-String -Pattern "error|Error C" | Select-Object -First 10

$exe = Join-Path $BuildDir "tests\Release\vxc_tests.exe"
if (-not (Test-Path $exe)) { throw "test binary not found: $exe" }

Write-Host "Running tests to collect actual digests..." -ForegroundColor Cyan
# Route output through a file rather than `2>&1` into the pipeline. In Windows
# PowerShell, redirecting a native executable's stderr wraps each line in an
# ErrorRecord and re-wraps it at the console width, which splits the very
# "(actual vs expected)" tail this script has to parse.
$log = Join-Path ([System.IO.Path]::GetTempPath()) "vxc_goldens.txt"
$p = Start-Process -FilePath $exe -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err"
$out = @(Get-Content $log) + @(Get-Content "$log.err" -ErrorAction SilentlyContinue)

# Failure lines look like:
#   FAIL <file>:<line> in <test>: d.h == 0x<expected>ull (<actual> vs <expected-dec>)
$pairs = @{}
foreach ($line in $out) {
    $m = [regex]::Match($line, "FAIL (.*?):(\d+) in (\w+): .*?0x([0-9A-Fa-f]+)ull \((\d+) vs (\d+)\)")
    if (-not $m.Success) { continue }
    $test = $m.Groups[3].Value
    $old = "0x" + $m.Groups[4].Value
    $new = "0x{0:X16}" -f [uint64]$m.Groups[5].Value
    if ($old -eq $new) { continue }
    $pairs[$old] = $new
    Write-Host ("  {0,-42} {1} -> {2}" -f $test, $old, $new)
}

if ($pairs.Count -eq 0) {
    Write-Host "No golden digest mismatches to update." -ForegroundColor Green
    exit 0
}

$patched = 0
Get-ChildItem (Join-Path $RepoRoot "voxel-core\tests\*.cpp") | ForEach-Object {
    $t = Get-Content $_.FullName -Raw
    $n = 0
    foreach ($k in $pairs.Keys) {
        if ($t -match [regex]::Escape($k)) { $t = $t -replace [regex]::Escape($k), $pairs[$k]; $n++ }
    }
    if ($n -gt 0) {
        Set-Content -Path $_.FullName -Value $t -Encoding utf8 -NoNewline
        Write-Host ("  patched {0} ({1})" -f $_.Name, $n) -ForegroundColor Yellow
        $patched += $n
    }
}

Write-Host "`n$patched digest(s) rewritten. REVIEW THE DIFF, then rebuild and re-run." -ForegroundColor Cyan
Write-Host "Remaining failures after this are real -- goldens are not the only pinned values." -ForegroundColor Cyan
