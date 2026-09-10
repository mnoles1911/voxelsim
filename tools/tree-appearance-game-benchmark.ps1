param([int]$Repeats=2,[switch]$NaniteDiagnostic)
$ErrorActionPreference='Stop'
if ($Repeats -lt 1 -or $Repeats -gt 4) { throw 'Repeats must be 1 through 4' }
$root='D:\voxelsim'
$bench=Join-Path $root '.scratch\appearance-benchmark'
$prepared=Get-Content -LiteralPath (Join-Path $bench 'appearance-renderer.json') -Raw | ConvertFrom-Json
if ([bool]$prepared.nanite -ne [bool]$NaniteDiagnostic) { throw 'Prepared mesh renderer does not match the requested benchmark. Rebuild the isolated maps first.' }
$folder=if ($NaniteDiagnostic) { 'benchmark-sm6' } else { 'benchmark-nonnanite' }
$out=Join-Path $root "asset-forge\out\tree-appearance-pilot\game-v2\$folder"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$rows=@()
for ($repeat=1; $repeat -le $Repeats; $repeat++) {
    $modes=if ($repeat % 2 -eq 1) { @('opaque','mask') } else { @('mask','opaque') }
    foreach ($species in @('temperate_oak','scots_pine')) {
        foreach ($mode in $modes) {
            if (Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) { throw 'Another Unreal session is open; leave it untouched and defer measurements.' }
            $label="$species-$mode-$repeat"
            $started=Get-Date
            $map="/Game/Voxel/AppearancePilot/V2/Bench_${species}_0004_$mode"
            $log=Join-Path $out "$label.log"
            $shot=(Join-Path $out "$label.png").Replace('\','/')
            $argsRun=@((Join-Path $bench 'AppearanceBenchmark.uproject'),$map,'-game','-dx12','-RenderOffscreen','-unattended','-nosplash','-nop4','-ResX=1280','-ResY=720','-csvCaptureFrames=1800','-csvGpuStats','-csvCompression=0','-ExitAfterCsvProfiling','-ExecCmds="t.MaxFPS 60,r.VSync 0"',('-csvExecCmds="900:HighResShot 1280x720 filename={0}"' -f $shot),"-abslog=$log")
            $proc=Start-Process -FilePath 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' -ArgumentList $argsRun -WindowStyle Hidden -PassThru
            if (-not $proc.WaitForExit(300000)) { Stop-Process -Id $proc.Id; throw "Owned benchmark $label timed out" }
            if ($proc.ExitCode -ne 0) { throw "Owned benchmark $label exited with code $($proc.ExitCode)" }
            $csvMatches=@(Select-String -LiteralPath $log -Pattern 'Writing CSV to file : (.+\.csv)')
            if ($csvMatches.Count -ne 1) { throw "Expected one CSV completion for $label" }
            $csvPath=$csvMatches[0].Matches[0].Groups[1].Value.Trim()
            if ((Get-Item -LiteralPath $csvPath).LastWriteTime -lt $started) { throw 'Stale CSV capture' }
            if (-not (Select-String -LiteralPath $log -Pattern 'shaderplatform="PCD3D_SM6"' -Quiet)) { throw 'Benchmark did not use Shader Model 6' }
            if (Select-String -LiteralPath $log -Pattern 'LoadErrors:|Fatal error:|Error:.*material|missing usage flag' -Quiet) { throw 'Benchmark asset load or rendering failed' }
            Copy-Item -LiteralPath $csvPath -Destination (Join-Path $out "$label.csv")
            if (-not (Test-Path -LiteralPath $shot)) { throw "Missing camera verification screenshot: $shot" }
            if ((Get-Item -LiteralPath $shot).LastWriteTime -lt $started) { throw 'Stale camera screenshot' }
            $rows+=@{species=$species;mode=$mode;repeat=$repeat;csv="$label.csv";screenshot="$label.png";log="$label.log"}
            @{status='captured; camera and warmup/GPU series validation required';renderer=$(if ($NaniteDiagnostic) { 'nanite-static-diagnostic' } else { 'non-nanite-full-source-static-baseline' });runs=$rows}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $out 'manifest.json')
            Write-Output "CAPTURED $label"
        }
    }
}
