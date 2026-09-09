<#
Builds VetroLook.exe (if needed) and then packages it three ways, in
parallel. Output lands in .\release\:

  release\VetroLook-<version>-Setup.exe   (Inno Setup; creates its own standard uninstaller)
  release\VetroLook-<version>-x64.msi     (WiX; removal goes through Windows Installer)
  release\VetroLook-<version>-x64.msix    (unsigned, for Microsoft Store submission)
  release\SHA256SUMS.txt
  release\VetroLook-<version>-Portable.zip   (unless -SkipPortable)

None of these ship a separate uninstaller executable -- each format manages
its own removal (Inno's generated uninstaller, msiexec, or Windows/Store for
MSIX) and shows up under Settings > Apps > Installed apps.

Usage:
  .\build-packages.ps1                  # build app if missing, then package all three
  .\build-packages.ps1 -SkipAppBuild    # reuse the existing dist\VetroLook.exe
  .\build-packages.ps1 -LocalTestSign   # also produce a locally-signed MSIX copy
                                         # under packaging\msix\local-test\ for
                                         # sideload testing (never placed in release\)
  .\build-packages.ps1 -SkipPortable    # skip the portable .zip
#>
param(
    [switch]$SkipAppBuild,
    [string]$Version,
    [switch]$LocalTestSign,
    [switch]$SkipPortable
)
$ErrorActionPreference = 'Stop'
$RepoRoot = $PSScriptRoot
$DistDir = Join-Path $RepoRoot 'dist'
$ExePath = Join-Path $DistDir 'VetroLook.exe'
$ReleaseDir = Join-Path $RepoRoot 'release'
$PackagingDir = Join-Path $RepoRoot 'packaging'

if (-not $Version) {
    $cmakeText = Get-Content (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
    if ($cmakeText -match 'project\(VetroLook VERSION ([\d\.]+)') { $Version = $Matches[1] } else { $Version = '1.0.0' }
}

Write-Host "VetroLook release packaging -- version $Version"

if (-not $SkipAppBuild) {
    Write-Host 'Building VetroLook.exe (CMake/MSVC)...'
    & (Join-Path $RepoRoot 'build.ps1')
    if ($LASTEXITCODE) { throw 'Application build failed' }
}

if (-not (Test-Path $ExePath)) {
    throw "VetroLook.exe not found at $ExePath (pass -SkipAppBuild only once it has been built)"
}

New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null

Copy-Item (Join-Path $RepoRoot 'LICENSE') (Join-Path $ReleaseDir 'LICENSE') -Force
Copy-Item (Join-Path $DistDir 'THIRD_PARTY_NOTICES.txt') (Join-Path $ReleaseDir 'THIRD_PARTY_NOTICES.txt') -Force

$targets = @(
    @{ Name = 'MSI';  File = "VetroLook-$Version-x64.msi";  Script = 'wix\build-msi.ps1' }
    @{ Name = 'MSIX'; File = "VetroLook-$Version-x64.msix"; Script = 'msix\build-msix.ps1' }
    @{ Name = 'Inno'; File = "VetroLook-$Version-Setup.exe"; Script = 'inno\build-inno.ps1' }
)

Write-Host 'Building MSI, MSIX and Inno Setup installers in parallel...'
$jobs = foreach ($t in $targets) {
    $scriptPath = Join-Path $PackagingDir $t.Script
    $scriptDir = Split-Path $scriptPath -Parent
    if ($t.Name -eq 'MSIX') {
        Start-Job -Name $t.Name -FilePath $scriptPath -ArgumentList $ExePath, $ReleaseDir, $Version, $RepoRoot, $LocalTestSign.IsPresent, $scriptDir
    }
    else {
        Start-Job -Name $t.Name -FilePath $scriptPath -ArgumentList $ExePath, $ReleaseDir, $Version, $RepoRoot, $scriptDir
    }
}

Wait-Job -Job $jobs | Out-Null

foreach ($j in $jobs) {
    Write-Host "----- $($j.Name) -----"
    try {
        Receive-Job -Job $j -ErrorAction Continue 2>&1 | ForEach-Object { Write-Host $_ }
    }
    catch {
        Write-Host "[$($j.Name)] $_"
    }
    Remove-Job -Job $j -Force
}

if (-not $SkipPortable) {
    Write-Host "----- Portable -----"
    $portableZip = Join-Path $ReleaseDir "VetroLook-$Version-Portable.zip"
    $stagePortable = Join-Path $PackagingDir 'portable-stage'
    if (Test-Path $stagePortable) { Remove-Item $stagePortable -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stagePortable | Out-Null
    Copy-Item $ExePath (Join-Path $stagePortable 'VetroLook.exe') -Force
    foreach ($doc in 'README.txt', 'THIRD_PARTY_NOTICES.txt') {
        $src = Join-Path $DistDir $doc
        if (Test-Path $src) { Copy-Item $src (Join-Path $stagePortable $doc) -Force }
    }
    # The Lensfun database is read at runtime from beside the executable. Ship
    # it, or lens identification silently reports "database not installed".
    $lensDb = Join-Path $DistDir 'lensfun-db'
    if (Test-Path $lensDb) {
        Copy-Item $lensDb (Join-Path $stagePortable 'lensfun-db') -Recurse -Force
    } else {
        Write-Warning 'lensfun-db not found beside the executable; lens profiles will be unavailable.'
    }
    if (Test-Path $portableZip) { Remove-Item $portableZip -Force }
    Compress-Archive -Path (Join-Path $stagePortable '*') -DestinationPath $portableZip -CompressionLevel Optimal
    Remove-Item $stagePortable -Recurse -Force
    Write-Host "[Portable] Built $portableZip"
    $targets += @{ Name = 'Portable'; File = "VetroLook-$Version-Portable.zip"; Script = $null }
}

$readmeTemplate = Join-Path $PackagingDir 'release-readme.template.md'
if (Test-Path $readmeTemplate) {
    $readme = ([System.IO.File]::ReadAllText($readmeTemplate, [System.Text.Encoding]::UTF8)).Replace('{{VERSION}}', $Version)
    [System.IO.File]::WriteAllText((Join-Path $ReleaseDir 'README.md'), $readme, (New-Object System.Text.UTF8Encoding($false)))
}

Write-Host ''
Write-Host 'Summary:'
$allOk = $true
$hashLines = @()
foreach ($t in $targets) {
    $p = Join-Path $ReleaseDir $t.File
    if (Test-Path $p) {
        $item = Get-Item $p
        $size = [math]::Round($item.Length / 1MB, 2)
        $hash = (Get-FileHash -Path $p -Algorithm SHA256).Hash.ToLower()
        Write-Host ("  [OK]   {0,-9} -> {1} ({2} MB)" -f $t.Name, $p, $size)
        $hashLines += "$hash  $($item.Name)"
    }
    else {
        Write-Host ("  [FAIL] {0,-9} -> {1} was not produced" -f $t.Name, $p)
        $allOk = $false
    }
}

$sumsPath = Join-Path $ReleaseDir 'SHA256SUMS.txt'
$hashLines | Set-Content -Path $sumsPath -Encoding ASCII
Write-Host "  [OK]   SHA256SUMS -> $sumsPath"

if (-not $allOk) { exit 1 }

$releaseAssets = "VetroLook-$Version-Setup.exe, VetroLook-$Version-x64.msi, VetroLook-$Version-x64.msix, SHA256SUMS.txt"
if (-not $SkipPortable) { $releaseAssets += ", VetroLook-$Version-Portable.zip (optional)" }
Write-Host "`nGitHub Release assets (release\): $releaseAssets"
