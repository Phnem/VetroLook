# Vetro Look, GPL-3.0-or-later.
# Fetches the pinned libmpv development package into third_party/mpv.
#
# The version is pinned on purpose. Video Mode's presentation path depends on
# behaviour that is not part of any stable contract -- composition output and the
# `display-swapchain` property -- so the build must not drift onto whatever
# libmpv happens to be current. Moving the pin means running VetroMpvProbe again
# and reading its report before anything else is believed.
param(
  [string]$Tag     = '20260903',
  [string]$Package = 'mpv-dev-x86_64-20260903-git-69e63f425a.7z',
  [string]$Sha256  = 'FAC135C68A35B7639E39D72C0C365104EDBAEBDEA39A0DFDD8C36E8C8E80FAEF'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root 'third_party/mpv'

if (Test-Path (Join-Path $dest 'mpv.lib')) {
  Write-Host "libmpv already present in $dest"
  exit 0
}

$seven = @("$env:ProgramFiles/7-Zip/7z.exe", "${env:ProgramFiles(x86)}/7-Zip/7z.exe") |
         Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $seven) { throw '7-Zip is required to unpack the libmpv package (install it, or unpack by hand into third_party/mpv).' }

$url = "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/$Tag/$Package"
$archive = Join-Path $env:TEMP $Package
Write-Host "Fetching $Package..."
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing -TimeoutSec 900

$hash = (Get-FileHash $archive -Algorithm SHA256).Hash
if ($hash -ne $Sha256) {
  Remove-Item $archive -ErrorAction SilentlyContinue
  throw "The downloaded package does not match the pinned hash.`nexpected $Sha256`ngot      $hash"
}

New-Item -ItemType Directory -Force $dest | Out-Null
& $seven x $archive "-o$dest" -y | Out-Null
Remove-Item $archive -ErrorAction SilentlyContinue

# The package ships a MinGW import library, which MSVC cannot link. Build one
# from the DLL's own export table instead, so the two can never disagree.
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
cmd /c "`"$vcvars`" >nul && dumpbin /exports `"$dest\libmpv-2.dll`" > `"$dest\exports.txt`""
$names = Get-Content "$dest\exports.txt" |
         Where-Object { $_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(mpv_\S+)' } |
         ForEach-Object { $matches[1] }
if ($names.Count -lt 40) { throw "The DLL exported only $($names.Count) mpv symbols; the package looks wrong." }
@("LIBRARY libmpv-2.dll", "EXPORTS") + ($names | ForEach-Object { "    $_" }) |
  Set-Content "$dest\mpv.def" -Encoding ascii
cmd /c "`"$vcvars`" >nul && lib /nologo /def:`"$dest\mpv.def`" /machine:x64 /out:`"$dest\mpv.lib`""

Write-Host "libmpv ready in $dest ($($names.Count) exports)"
