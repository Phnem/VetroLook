$ErrorActionPreference = 'Stop'
$iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
if (-not (Test-Path -LiteralPath $iscc)) { throw 'Inno Setup 6 ISCC.exe is required' }
$script = Join-Path $PSScriptRoot 'migration-fixture.iss'
$result = Join-Path $PSScriptRoot 'migration-fixture-results.txt'
Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
& $iscc $script | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
$exe = Join-Path $PSScriptRoot 'out\VetroLookMigrationFixture.exe'
$process = Start-Process -FilePath $exe -ArgumentList @('/VERYSILENT', "/RESULT=$result") -Wait -PassThru -WindowStyle Hidden
if (-not (Test-Path -LiteralPath $result)) { throw 'Migration fixture harness did not write a result' }
$text = Get-Content -LiteralPath $result -Raw
$text | Write-Host
if ($text -notmatch 'SUMMARY PASS') { throw 'Migration fixture regression failed' }
