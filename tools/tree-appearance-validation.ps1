param([int]$WaitForImportPid=0)
$ErrorActionPreference='Stop'
$root='D:\voxelsim'
$ue='D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project=Join-Path $root 'ue-project\VoxelEarth.uproject'
$out=Join-Path $root 'asset-forge\out\tree-appearance-pilot\game-v2'
if ($WaitForImportPid -gt 0) { Wait-Process -Id $WaitForImportPid -ErrorAction SilentlyContinue }
if (Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) { throw 'Another Unreal session is open; defer the isolated test.' }
$env:TREE_APPEARANCE_EXPORT_DIR=$out
$env:TREE_APPEARANCE_ONLY='temperate-oak-0004,scots-pine-0004'
$importLog=Join-Path $out 'unreal-import.log'
$argsImport=@($project,'-run=pythonscript',"-script=$root/ue-project/Tools/import_tree_appearance_pilot.py",'-unattended','-nop4','-nosplash','-AllowCommandletRendering','-dx12',"-abslog=$importLog")
$proc=Start-Process -FilePath $ue -ArgumentList $argsImport -WindowStyle Hidden -PassThru
$proc.WaitForExit()
if (-not (Select-String -LiteralPath $importLog -Pattern 'TREE_APPEARANCE_IMPORT_COMPLETE:2' -Quiet)) { throw "Import did not complete; inspect $importLog" }
$captureLog=Join-Path $out 'unreal-capture.log'
$argsCapture=@($project,"-ExecutePythonScript=$root/ue-project/Tools/capture_tree_appearance_pilot.py",'-unattended','-nop4','-nosplash','-dx12','-RenderOffscreen','-ResX=1280','-ResY=720',"-abslog=$captureLog")
$proc=Start-Process -FilePath $ue -ArgumentList $argsCapture -WindowStyle Hidden -PassThru
if (-not $proc.WaitForExit(600000)) { Stop-Process -Id $proc.Id; throw 'Owned diagnostic capture timed out after ten minutes.' }
if (-not (Select-String -LiteralPath $captureLog -Pattern 'TREE_APPEARANCE_CAPTURE_COMPLETE:8' -Quiet)) { throw "Capture did not complete; inspect $captureLog" }
Write-Output "Diagnostic captures ready for visual review: $out/captures. Game GPU benchmarking remains separate."
