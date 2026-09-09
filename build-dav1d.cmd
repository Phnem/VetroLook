@echo off
if exist "%~dp0build-dav1d\src\libdav1d.a" exit /b 0
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set PYTHONPATH=%~dp0tools
set PATH=%~dp0tools\bin;%PATH%
set DAV1D_SRC=%~dp0dav1d
if not exist "%DAV1D_SRC%\meson.build" set DAV1D_SRC=%~dp0..\dav1d
python -m mesonbuild.mesonmain setup "%~dp0build-dav1d" "%DAV1D_SRC%" --default-library=static -Db_vscrt=mt -Denable_asm=false -Denable_tests=false -Denable_tools=false
if errorlevel 1 exit /b 1
python -m mesonbuild.mesonmain compile -C "%~dp0build-dav1d" -j 8
