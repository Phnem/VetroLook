param(
    [Parameter(Mandatory)] [string]$ExePath,
    [Parameter(Mandatory)] [string]$OutDir,
    [string]$Version = '1.0.0',
    [string]$RepoRoot,
    [string]$ScriptDir = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'

# Modern WiX Toolset (v4+) only -- no candle.exe/light.exe, no .NET Framework
# 3.5 / NetFx3 dependency. Distributed as a `dotnet tool`, so it needs the
# .NET SDK (6+) to install, not just a runtime.

function Get-DotnetSdkMajor {
    $sdks = & dotnet --list-sdks 2>$null
    if ($LASTEXITCODE -or -not $sdks) { return $null }
    $best = $sdks | ForEach-Object { ($_ -split ' ')[0] } | Sort-Object { [version]$_ } -Descending | Select-Object -First 1
    if (-not $best) { return $null }
    return [int]([version]$best).Major
}

function Install-DotnetSdk {
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) { return $false }
    Write-Host '[MSI] .NET SDK 6+ not found -- installing .NET SDK 8 (LTS) via winget...'
    & winget install --id Microsoft.DotNet.SDK.8 -e --silent --accept-package-agreements --accept-source-agreements | Out-Null
    return $true
}

function Find-WixExe {
    $cmd = Get-Command wix.exe -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command wix -ErrorAction SilentlyContinue }
    if ($cmd) { return $cmd.Source }
    $toolPath = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
    if (Test-Path $toolPath) { return $toolPath }
    return $null
}

function Resolve-Wix {
    $wix = Find-WixExe
    if ($wix) { return $wix }

    $sdkMajor = Get-DotnetSdkMajor
    if (-not $sdkMajor -or $sdkMajor -lt 6) {
        if (-not (Install-DotnetSdk)) {
            Write-Warning '[MSI] No .NET SDK 6+ available and winget is missing -- cannot install the modern WiX Toolset (a dotnet global tool). Install a .NET SDK (https://dotnet.microsoft.com/download) and re-run.'
            return $null
        }
        # dotnet tool install needs a fresh process to see a just-installed SDK
        # (its PATH/registration is only picked up by new processes), so we
        # can't `dotnet tool install` in this same process yet.
        Write-Warning '[MSI] .NET SDK installation was triggered. Re-run this script once (a new process/session is required to see the newly installed SDK) so the WiX tool can be installed.'
        return $null
    }

    Write-Host '[MSI] WiX Toolset (modern, dotnet tool) not found -- installing with: dotnet tool install --global wix'
    & dotnet tool install --global wix 2>&1 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        # Already-installed is not a real failure; re-check for the binary either way.
    }
    return Find-WixExe
}

$wix = Resolve-Wix
if (-not $wix) {
    Write-Warning '[MSI] WiX Toolset is unavailable. Skipping MSI.'
    exit 1
}

Write-Host "[MSI] wix.exe: $wix"
$verOut = [string](& $wix --version 2>&1 | Out-String).Trim()
Write-Host "[MSI] WiX Toolset version: $verOut"
$wixMajor = 0
if ($verOut -match '(\d+)\.\d+\.\d+') { $wixMajor = [int]$Matches[1] }

# The upgrade close action and the complete wizard are WiX extensions. Cache
# matching major versions once so a clean build machine creates the same MSI.
if ($wixMajor -gt 0) {
    foreach ($extensionId in 'WixToolset.Util.wixext', 'WixToolset.UI.wixext') {
        $extension = "$extensionId/$wixMajor.0.0"
        & $wix extension add -g $extension 2>&1 | ForEach-Object { Write-Host "[MSI] $_" }
        if ($LASTEXITCODE -ne 0) { throw "Unable to install required WiX extension $extension" }
    }
}

# WiX v6+ requires accepting FireGiant's Open Source Maintenance Fee EULA
# once per machine before it will build anything. This just persists a
# one-time acceptance file (`wix eula accept wixN`); it is not a purchase.
if ($wixMajor -ge 6) {
    $eulaId = "wix$wixMajor"
    & $wix eula accept $eulaId 2>&1 | ForEach-Object { Write-Host "  $_" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$wxs = Join-Path $ScriptDir 'VetroLook.wxs'
$distDir = Split-Path $ExePath -Parent
$lensDbPath = Join-Path $distDir 'lensfun-db'
$readmePath = Join-Path $distDir 'README.txt'
$noticesPath = Join-Path $distDir 'THIRD_PARTY_NOTICES.txt'
foreach ($required in @($lensDbPath, $readmePath, $noticesPath)) {
    if (-not (Test-Path $required)) { throw "[MSI] Required runtime payload is missing: $required" }
}
$lensFragment = Join-Path $ScriptDir 'VetroLook.lensfun.wxi'
$xml = New-Object System.Collections.Generic.List[string]
$xml.Add('<Include>')
$xml.Add('<Fragment xmlns="http://wixtoolset.org/schemas/v4/wxs">')
$xml.Add('  <DirectoryRef Id="LensfunDbFolder">')
$xml.Add('    <Component Id="LensfunDatabase" Guid="BD0C4A9F-CA21-45CB-9EF3-4D7784E55A20">')
Get-ChildItem $lensDbPath -Recurse -File | Sort-Object FullName | ForEach-Object {
    $source = [System.Security.SecurityElement]::Escape($_.FullName)
    $xml.Add(('        <File Source="{0}" />' -f $source))
}
$xml.Add('    </Component>')
$xml.Add('  </DirectoryRef>')
$xml.Add('</Fragment>')
$xml.Add('</Include>')
[System.IO.File]::WriteAllLines($lensFragment, $xml, (New-Object System.Text.UTF8Encoding($false)))
$outMsi = Join-Path $OutDir "VetroLook-$Version-x64.msi"
if (Test-Path $outMsi) { Remove-Item $outMsi -Force }

# wix build has been observed to intermittently fail right after a fresh
# `dotnet tool install` (transient, no reproducible cause found) -- retry
# once before giving up.
$buildOk = $false
for ($attempt = 1; $attempt -le 2; $attempt++) {
    & $wix build -arch x64 -ext WixToolset.Util.wixext -ext WixToolset.UI.wixext -d "ExePath=$ExePath" -d "LensfunDbPath=$lensDbPath" -d "ReadmePath=$readmePath" -d "NoticesPath=$noticesPath" -d "ProductVersion=$Version" -pdbtype none -out $outMsi $wxs
    if ($LASTEXITCODE -eq 0) { $buildOk = $true; break }
    Write-Warning "[MSI] wix build attempt $attempt failed (exit $LASTEXITCODE), retrying..."
    Start-Sleep -Seconds 2
}
if (-not $buildOk) { throw 'wix build failed' }

Write-Host "[MSI] Built $outMsi"
