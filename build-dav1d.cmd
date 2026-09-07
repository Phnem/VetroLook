@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set PYTHONPATH=%~dp0tools
set PATH=%~dp0tools\bin;%PATH%
python -m mesonbuild.mesonmain setup "%~dp0build-dav1d" "%~dp0dav1d" --default-library=static -Db_vscrt=mt -Denable_asm=false -Denable_tests=false -Denable_tools=false
if errorlevel 1 exit /b 1
python -m mesonbuild.mesonmain compile -C "%~dp0build-dav1d" -j 8
