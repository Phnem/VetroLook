param(
    [Parameter(Mandatory)] [string]$ExePath,
    [Parameter(Mandatory)] [string]$OutDir,
    [string]$Version = '1.0.0',
    [string]$RepoRoot,
    [bool]$LocalTestSign = $false,
    [string]$ScriptDir = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'

# The .msix that lands in $OutDir (release\) is built UNSIGNED: that is the
# artifact meant for Microsoft Store submission -- the Store signs it on
# ingestion. Passing -LocalTestSign additionally produces a second copy,
# signed with a throwaway local dev certificate, under packaging\msix\local-test\
# purely so the package can be sideloaded and smoke-tested on this machine.
# That signed copy and its .cer never go into release\.

function Find-KitTool([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $bases = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "$env:ProgramFiles\Windows Kits\10\bin"
    )
    foreach ($b in $bases) {
        if (-not (Test-Path $b)) { continue }
        $versions = Get-ChildItem $b -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        foreach ($v in $versions) {
            $p = Join-Path $v.FullName "x64\$Name"
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

$makeappx = Find-KitTool 'makeappx.exe'
if (-not $makeappx) {
    Write-Warning '[MSIX] makeappx.exe not found. Install the Windows 10/11 SDK and re-run. Skipping MSIX.'
    exit 1
}

$stageDir = Join-Path $ScriptDir 'stage'
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir 'Assets') | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Copy-Item $ExePath (Join-Path $stageDir 'VetroLook.exe') -Force
$distDir = Split-Path $ExePath -Parent
$lensDb = Join-Path $distDir 'lensfun-db'
foreach ($required in @($lensDb, (Join-Path $distDir 'README.txt'), (Join-Path $distDir 'THIRD_PARTY_NOTICES.txt'))) {
    if (-not (Test-Path $required)) { throw "[MSIX] Required runtime payload is missing: $required" }
}
Copy-Item $lensDb (Join-Path $stageDir 'lensfun-db') -Recurse -Force
Copy-Item (Join-Path $distDir 'README.txt') (Join-Path $stageDir 'README.txt') -Force
Copy-Item (Join-Path $distDir 'THIRD_PARTY_NOTICES.txt') (Join-Path $stageDir 'THIRD_PARTY_NOTICES.txt') -Force

$manifest = (Get-Content (Join-Path $ScriptDir 'AppxManifest.xml') -Raw).Replace('{{VERSION}}', $Version)
Set-Content -Path (Join-Path $stageDir 'AppxManifest.xml') -Value $manifest -Encoding UTF8

Add-Type -AssemblyName System.Drawing

$iconSource = Join-Path $RepoRoot 'icons\icon256.ico'
if (-not (Test-Path $iconSource)) { $iconSource = Join-Path $RepoRoot 'icons\icon512.ico' }
if (-not (Test-Path $iconSource)) { throw "No source icon found under $RepoRoot\icons" }

function New-PngFromIcon([string]$IconPath, [int]$Size, [string]$OutPath) {
    $srcIcon = New-Object System.Drawing.Icon($IconPath, (New-Object System.Drawing.Size(256, 256)))
    $srcBmp = $srcIcon.ToBitmap()
    $dstBmp = New-Object System.Drawing.Bitmap $Size, $Size
    $g = [System.Drawing.Graphics]::FromImage($dstBmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($srcBmp, 0, 0, $Size, $Size)
    $dstBmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $dstBmp.Dispose(); $srcBmp.Dispose(); $srcIcon.Dispose()
}

New-PngFromIcon $iconSource 44 (Join-Path $stageDir 'Assets\Square44x44Logo.png')
New-PngFromIcon $iconSource 150 (Join-Path $stageDir 'Assets\Square150x150Logo.png')
New-PngFromIcon $iconSource 50 (Join-Path $stageDir 'Assets\StoreLogo.png')

$outMsix = Join-Path $OutDir "VetroLook-$Version-x64.msix"
if (Test-Path $outMsix) { Remove-Item $outMsix -Force }

Write-Host "[MSIX] makeappx.exe: $makeappx"
& $makeappx pack /d $stageDir /p $outMsix /o
if ($LASTEXITCODE) { throw 'makeappx pack failed' }
Write-Host "[MSIX] Built (unsigned, Store-submission-ready) $outMsix"

if ($LocalTestSign) {
    $signtool = Find-KitTool 'signtool.exe'
    if (-not $signtool) {
        Write-Warning '[MSIX] -LocalTestSign requested but signtool.exe was not found; skipping local signed copy.'
    }
    else {
        $localTestDir = Join-Path $ScriptDir 'local-test'
        New-Item -ItemType Directory -Force -Path $localTestDir | Out-Null
        $signedMsix = Join-Path $localTestDir "VetroLook-$Version-x64-signed.msix"
        Copy-Item $outMsix $signedMsix -Force

        $subject = 'CN=VetroLook Dev'
        $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
        if (-not $cert) {
            Write-Host '[MSIX] Creating a local self-signed signing certificate (CN=VetroLook Dev) for local testing only...'
            $cert = New-SelfSignedCertificate -Type Custom -Subject $subject -KeyUsage DigitalSignature `
                -FriendlyName 'VetroLook Dev Signing Cert (local test only)' -CertStoreLocation Cert:\CurrentUser\My `
                -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
        }
        $cerPath = Join-Path $localTestDir 'VetroLook-DevCert.cer'
        Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null

        & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $signedMsix
        if ($LASTEXITCODE) { throw 'signtool sign failed' }
        Write-Host "[MSIX] Local test copy signed with thumbprint $($cert.Thumbprint): $signedMsix"
        Write-Host "[MSIX] Trust certificate for sideloading: $cerPath (import into CurrentUser\TrustedPeople on the test machine -- local testing only, do not ship)"
    }
}

Write-Host "[MSIX] Done"
