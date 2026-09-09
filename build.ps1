param()
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$vswhere="${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path "$PSScriptRoot/tools/mesonbuild")) {
 python -m pip install --target "$PSScriptRoot/tools" meson==1.12.0 ninja==1.13.2
 if($LASTEXITCODE){throw 'Build-tool installation failed'}
}

# NASM, for libjpeg-turbo's SIMD kernels. Without it libjpeg-turbo falls back to
# scalar C and JPEG decoding takes roughly twice as long — which is most of the
# time this viewer spends opening the format it opens most. The fallback is
# allowed, but never silently: VETRO_ALLOW_SCALAR_JPEG has to be set on purpose.
$nasmVersion='2.16.03'
$nasmDir=Join-Path $PSScriptRoot "tools/nasm/nasm-$nasmVersion"
$nasm=Join-Path $nasmDir 'nasm.exe'
if (!(Test-Path $nasm)) {
 $onPath=Get-Command nasm -ErrorAction SilentlyContinue
 if ($onPath) { $nasm=$onPath.Source }
 else {
  Write-Host "Fetching NASM $nasmVersion for libjpeg-turbo SIMD..."
  $zip=Join-Path $env:TEMP "nasm-$nasmVersion.zip"
  try {
   Invoke-WebRequest -Uri "https://www.nasm.us/pub/nasm/releasebuilds/$nasmVersion/win64/nasm-$nasmVersion-win64.zip" `
     -OutFile $zip -TimeoutSec 180 -UseBasicParsing
   Expand-Archive -Path $zip -DestinationPath (Join-Path $PSScriptRoot 'tools/nasm') -Force
   Remove-Item $zip -ErrorAction SilentlyContinue
  } catch {
   $message = @"
NASM could not be fetched: $($_.Exception.Message)

libjpeg-turbo needs it to build its SIMD kernels. Without SIMD, decoding a
36 megapixel JPEG takes about 320 ms instead of about 160 ms on this machine.

Either:
  * install NASM and put nasm.exe on PATH, or
  * unpack https://www.nasm.us/pub/nasm/releasebuilds/$nasmVersion/win64/nasm-$nasmVersion-win64.zip
    into $PSScriptRoot/tools/nasm, or
  * set VETRO_ALLOW_SCALAR_JPEG=1 to build the slower scalar version on purpose.
"@
   if (!$env:VETRO_ALLOW_SCALAR_JPEG) { throw $message }
   Write-Warning $message
  }
 }
}
if (Test-Path $nasm) { $env:NASM_PATH=Split-Path $nasm -Parent }

& "$PSScriptRoot/build-dav1d.cmd"
if($LASTEXITCODE){throw 'dav1d build failed'}
& $cmake -S $PSScriptRoot -B "$PSScriptRoot/build" -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE){throw 'Configure failed'}
& $cmake --build "$PSScriptRoot/build" --config Release --target VetroLook --parallel 8
if($LASTEXITCODE){throw 'Build failed'}

# Confirm what was actually built rather than what was asked for.
$simd=Select-String -Path "$PSScriptRoot/build/CMakeCache.txt" -Pattern '^VETRO_NASM:FILEPATH=' -ErrorAction SilentlyContinue
if ($simd -and $simd -notmatch 'NOTFOUND') { Write-Host "libjpeg-turbo SIMD: enabled ($($simd -replace '.*=',''))" }
else { Write-Warning 'libjpeg-turbo SIMD: DISABLED - JPEG decoding will be roughly twice as slow.' }
