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
& "$PSScriptRoot/build-dav1d.cmd"
if($LASTEXITCODE){throw 'dav1d build failed'}
& $cmake -S $PSScriptRoot -B "$PSScriptRoot/build" -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE){throw 'Configure failed'}
& $cmake --build "$PSScriptRoot/build" --config Release --target VetroLook VetroLookSetup --parallel 8
if($LASTEXITCODE){throw 'Build failed'}

