# Asset Forge desktop launcher (plan P1, owner rulings 1+2, 2026-09-05).
#
# What it does, in order:
#   1. PREFLIGHT: checks python and the three packages the forge needs
#      (numpy, scipy, pillow) and fails in plain English naming the fix --
#      a missing scipy must not surface as a traceback mid-use.
#   2. SINGLE INSTANCE: compares the running server's startup fingerprint to
#      the current checkout. Reuses the latest server, restarts stale Forge.
#   3. Otherwise starts `python -m forge.cli serve`, which opens the browser
#      itself. The console window stays visible ON PURPOSE: ctrl-c in it is
#      how you stop the forge.
#
# The desktop shortcut (see install-shortcut.ps1) points here.

param([int]$Port = 8731)

$ErrorActionPreference = "Stop"
$forgeDir = Split-Path $PSScriptRoot -Parent   # tools -> asset-forge
$url = "http://127.0.0.1:$Port/"

# --- 1. preflight ------------------------------------------------------------
$python = Get-Command python -ErrorAction SilentlyContinue
if ($null -eq $python) {
    $installedPython = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python*\python.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($installedPython) { $python = @{ Source = $installedPython.FullName } }
}
if ($null -eq $python) {
    Write-Host ""
    Write-Host "Asset Forge cannot start: Python was not found on this machine's PATH." -ForegroundColor Red
    Write-Host "Fix: install Python 3.12+ from https://www.python.org/downloads/ and re-run."
    exit 1
}

# one interpreter start for all three; find_spec imports nothing heavy
$missing = & $python.Source -c "import importlib.util as u; print(','.join(m for m in ('numpy','scipy','PIL') if u.find_spec(m) is None))"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Asset Forge cannot start: '$($python.Source)' failed to run at all." -ForegroundColor Red
    exit 1
}
if ($missing) {
    $pipNames = ($missing -split ',') | ForEach-Object { if ($_ -eq 'PIL') { 'pillow' } else { $_ } }
    Write-Host ""
    Write-Host "Asset Forge cannot start: the Python package$(if ($pipNames.Count -gt 1) {'s'}) '$($pipNames -join `"', '`")' $(if ($pipNames.Count -gt 1) {'are'} else {'is'}) not installed." -ForegroundColor Red
    Write-Host "Fix: python -m pip install $($pipNames -join ' ')"
    Write-Host "Then run this launcher again."
    exit 1
}

# --- 2. already running, and current? ----------------------------------------
Push-Location $forgeDir
try {
    $expected = (& $python.Source -m forge.server_version) | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$expected.fingerprint) { throw 'Cannot identify current Forge code.' }
} finally { Pop-Location }
$running = $null
try {
    $running = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/server-version" -TimeoutSec 5
} catch { }
# The probe timeout is GENEROUS on purpose: /api/kinds re-reads every spec and
# measures ~10 s on this library, while a forge that is NOT running refuses the
# connection instantly -- so the long timeout costs nothing in the cold case
# and prevents the disaster case: stdlib http.server sets SO_REUSEADDR, and on
# Windows that lets a second serve silently STEAL the port from the first
# instead of failing to bind. The probe is the single-instance mechanism.
$alive = $false
try {
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/kinds" -UseBasicParsing -TimeoutSec 30
    if ($r.StatusCode -eq 200) { $alive = $true }
} catch { }

if ($alive) {
    if ($running.application -eq 'asset-forge' -and
        $running.root -eq $expected.root -and
        $running.fingerprint -eq $expected.fingerprint) {
        Write-Host "Asset Forge is current at $url -- opening your browser."
        Start-Process $url
        exit 0
    }
    # A legacy server has no version endpoint. Verify its actual listener and
    # command line before restarting; never stop another app using this port.
    $listeners = @(Get-NetTCPConnection -LocalPort $Port -State Listen)
    $forgeProcesses = @()
    foreach ($ownerId in @($listeners.OwningProcess | Sort-Object -Unique)) {
        $ownerProcess = Get-CimInstance Win32_Process -Filter "ProcessId = $ownerId"
        $command = $ownerProcess.CommandLine
        $isForge = $command -match '-m\s+forge\.cli\s+serve(?:\s|$)'
        $rightPort = $command -match "--port(?:\s+|=)$Port(?:\s|$)" -or
            ($Port -eq 8731 -and $command -notmatch '--port')
        if (!$isForge -or !$rightPort) {
            throw "Port $Port belongs to another command; it was not stopped."
        }
        $forgeProcesses += $ownerProcess
    }
    Write-Host "Restarting Asset Forge to load the latest code. Saved library entries are preserved."
    foreach ($ownerProcess in $forgeProcesses) { Stop-Process -Id $ownerProcess.ProcessId -ErrorAction Stop }
    $deadline = (Get-Date).AddSeconds(10)
    while (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
        if ((Get-Date) -gt $deadline) { throw "Port $Port did not close after stopping the old Forge server." }
        Start-Sleep -Milliseconds 100
    }
}

# A listener that did NOT answer the probe is either a hung forge or another
# program on our port. Starting serve anyway would hijack the socket (see
# above), so refuse with a name instead.
$busy = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($busy) {
    $owners = (@($busy) | ForEach-Object OwningProcess | Sort-Object -Unique) -join ', '
    Write-Host ""
    Write-Host "Port $Port is taken by process id $owners, but it did not answer as Asset Forge." -ForegroundColor Red
    Write-Host "Either stop that process, or start the forge on another port:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Port 8732"
    exit 1
}

# --- 3. start the forge ------------------------------------------------------
Write-Host "=============================================="
Write-Host "  Asset Forge"
Write-Host "  $url"
Write-Host "  This window is the forge. Press ctrl-c here"
Write-Host "  to stop it; closing the browser tab does not."
Write-Host "=============================================="
Set-Location $forgeDir
& $python.Source -m forge.cli serve --port $Port
exit $LASTEXITCODE
