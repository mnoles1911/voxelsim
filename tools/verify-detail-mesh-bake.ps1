param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Editor='D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [ValidateRange(60,7200)][int]$TimeoutSeconds=2400
)
$ErrorActionPreference='Stop'
if(Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue){throw 'An Unreal session is already running; preserve exclusive validation'}
$manifestPath=(Resolve-Path -LiteralPath $Manifest).Path
$outPath=[IO.Path]::GetFullPath($Output)
$projectPath=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../ue-project/VoxelEarth.uproject'))
if(Test-Path -LiteralPath $outPath){throw 'Use a fresh output directory'}
$data=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if($data.schema -notin @(1,2) -or @($data.models).Count -eq 0){throw 'Missing completed supported bake models'}
$verificationScope='Legacy schema 1: fresh-process package hashes, LOD counts/bounds and material persistence only; no attribute parity, cook or gameplay acceptance'
if($data.schema -eq 2){
    if($data.render_attribute_fingerprint_schema -ne 1){throw 'Unsupported render attribute fingerprint contract'}
    foreach($model in $data.models){
        if($model.mesh_facts.fingerprint_schema -ne 1 -or $model.mesh_facts.attribute_sha256 -notmatch '^[a-fA-F0-9]{64}$'){
            throw 'Schema 2 model lacks a valid attribute fingerprint'
        }
    }
    $verificationScope='Schema 2: fresh-process CPU render-buffer attribute/index/section/screen-size persistence and separately pinned material packages; not source geometry, pixels, cooking or gameplay equivalence'
}
if(-not(Test-Path -LiteralPath $Editor)){throw 'Editor executable missing'}
$runArgs=@($projectPath,'-run=VoxelBakeDetailMeshes',"-VerifyManifest=$manifestPath",
    '-unattended','-nosplash','-nop4','-RenderOffscreen',"-abslog=$outPath/unreal.log")
# Start-Process joins ArgumentList on Windows; explicitly preserve spaced paths.
foreach($arg in $runArgs){if($arg -match '["\r\n]'){throw 'Unsupported quote/newline in command argument'}}
$quotedArgs=($runArgs | ForEach-Object { '"'+$_+'"' }) -join ' '
New-Item -ItemType Directory -Path $outPath | Out-Null
$record=[ordered]@{
    scope=$verificationScope
    manifestSchema=[int]$data.schema
    attributeFingerprintSchema=$(if($data.schema -eq 2){1}else{$null})
    manifest=$manifestPath
    manifestSha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    expectedModels=@($data.models).Count
    previewOnly=$data.preview_only
    arguments=$runArgs
    startedUtc=[DateTime]::UtcNow.ToString('o')
    status='running'
}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$outPath/result.json" -Encoding utf8
try {
    $proc=Start-Process -FilePath $Editor -ArgumentList $quotedArgs -WindowStyle Hidden -PassThru
    Write-Output "Fresh-process detail verification PID $($proc.Id)"
    $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while(-not $proc.WaitForExit(1000)){
        if([DateTime]::UtcNow -ge $deadline){$proc.Kill();$proc.WaitForExit();throw 'Verification timed out; no acceptance'}
    }
    $record.exitCode=$proc.ExitCode
    if($proc.ExitCode -ne 0){throw "Editor exited $($proc.ExitCode)"}
    if((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne $record.manifestSha256){throw 'Manifest changed during verification'}
    $log=Get-Content -LiteralPath "$outPath/unreal.log" -Raw
    $pass=[regex]::Matches($log,'DetailBake fresh-process verification PASS: (\d+) models')
    if($pass.Count -ne 1 -or [int]$pass[0].Groups[1].Value -ne $record.expectedModels){throw 'Missing or mismatched completion marker'}
    if($data.schema -eq 2 -and $log -notmatch 'DetailBake fresh-process verification PASS: \d+ models; schema2 attribute persistence'){
        throw 'Missing schema 2 attribute-verification completion marker'
    }
    if($log -match 'Fatal error:|Assertion failed:|DetailBake:'){throw 'Verification logged a failure'}
    $record.status='passed'
} catch {
    $record.status='failed';$record.error=$_.Exception.Message
    throw
} finally {
    $record.finishedUtc=[DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$outPath/result.json" -Encoding utf8
}
Write-Output "Fresh-process schema $($record.manifestSchema) validation passed for $($record.expectedModels) models. $verificationScope"
