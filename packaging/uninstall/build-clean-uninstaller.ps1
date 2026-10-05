param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) '..\release')
)

$ErrorActionPreference = 'Stop'
$iexpress = Join-Path $env:WINDIR 'System32\iexpress.exe'
if (-not (Test-Path -LiteralPath $iexpress)) { throw 'IExpress is unavailable on this Windows installation.' }
$sed = Join-Path $PSScriptRoot 'VetroLook-Clean-Uninstall.sed'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$artifact = Join-Path ([System.IO.Path]::GetFullPath($OutDir)) 'VetroLook-Clean-Uninstall.exe'
$generatedSed = Join-Path $OutDir 'clean-uninstaller-build.sed'
$sedText = [System.IO.File]::ReadAllText($sed)
$sedText = [regex]::Replace($sedText, '(?m)^TargetName=.*$', ('TargetName=' + $artifact))
[System.IO.File]::WriteAllText($generatedSed, $sedText, [System.Text.Encoding]::Default)
Push-Location $PSScriptRoot
try {
    # IExpress's legacy parser does not accept a quoted SED path. Run from the
    # SED's directory and supply a simple filename; source paths stay absolute.
    $generatedSedText = $sedText.Replace('SourceFiles0=.', ('SourceFiles0=' + $PSScriptRoot))
    [System.IO.File]::WriteAllText($generatedSed, $generatedSedText, [System.Text.Encoding]::ASCII)
    $builder = Start-Process -FilePath $iexpress -WorkingDirectory ([System.IO.Path]::GetFullPath($OutDir)) -ArgumentList @('/N','/Q','clean-uninstaller-build.sed') -WindowStyle Hidden -Wait -PassThru
    if ($builder.ExitCode -ne 0) { throw "IExpress failed with exit code $($builder.ExitCode)." }
}
finally { Pop-Location }

if (-not (Test-Path -LiteralPath $artifact)) { throw "Expected artifact was not produced: $artifact" }
Get-Item -LiteralPath $artifact
