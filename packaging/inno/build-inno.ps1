param(
    [Parameter(Mandatory)] [string]$ExePath,
    [Parameter(Mandatory)] [string]$OutDir,
    [string]$Version = '1.0.0',
    [string]$RepoRoot,
    [string]$ScriptDir = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'

function Resolve-InnoSetup {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }

    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($winget) {
        Write-Host '[Inno] Inno Setup not found -- installing via winget (JRSoftware.InnoSetup)...'
        & winget install --id JRSoftware.InnoSetup -e --silent --accept-package-agreements --accept-source-agreements | Out-Null
        foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    }
    return $null
}

$iscc = Resolve-InnoSetup
if (-not $iscc) {
    Write-Warning '[Inno] Inno Setup (ISCC.exe) not found and could not be auto-installed. Install it from https://jrsoftware.org/isinfo.php (or run: winget install JRSoftware.InnoSetup) and re-run. Skipping Inno Setup build.'
    exit 1
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$iss = Join-Path $ScriptDir 'VetroLook.iss'
$lensDbPath = Join-Path (Split-Path $ExePath -Parent) 'lensfun-db'
$readmePath = Join-Path (Split-Path $ExePath -Parent) 'README.txt'
$noticesPath = Join-Path (Split-Path $ExePath -Parent) 'THIRD_PARTY_NOTICES.txt'
foreach ($required in @($lensDbPath, $readmePath, $noticesPath)) {
    if (-not (Test-Path $required)) { throw "[Inno] Required runtime payload is missing: $required" }
}

Write-Host "[Inno] ISCC.exe: $iscc"
& $iscc "/DMyAppVersion=$Version" "/DMyExePath=$ExePath" "/DMyLensDbPath=$lensDbPath" "/DMyReadmePath=$readmePath" "/DMyNoticesPath=$noticesPath" $iss
if ($LASTEXITCODE) { throw 'ISCC.exe failed' }

Write-Host "[Inno] Built $(Join-Path $OutDir "VetroLook-$Version-Setup.exe")"
