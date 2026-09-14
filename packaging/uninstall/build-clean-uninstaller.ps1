param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) '..\release')
)

$ErrorActionPreference = 'Stop'
$iexpress = Join-Path $env:WINDIR 'System32\iexpress.exe'
if (-not (Test-Path -LiteralPath $iexpress)) { throw 'IExpress is unavailable on this Windows installation.' }
$sed = Join-Path $PSScriptRoot 'VetroLook-Clean-Uninstall.sed'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Push-Location $PSScriptRoot
try {
    & $iexpress /N $sed
    # IExpress is a legacy GUI tool: on success it may leave $LASTEXITCODE
    # unset rather than writing an explicit zero. The artifact check below is
    # the reliable success condition in both cases.
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "IExpress failed with exit code $LASTEXITCODE." }
}
finally { Pop-Location }

$artifact = Join-Path $OutDir 'VetroLook-Clean-Uninstall.exe'
if (-not (Test-Path -LiteralPath $artifact)) { throw "Expected artifact was not produced: $artifact" }
Get-Item -LiteralPath $artifact
