param()
$ErrorActionPreference='Stop'
$exe=Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'dist/VetroLook.exe'
$log=Join-Path $PSScriptRoot 'runtime-final.log'
$fixtures=(Get-ChildItem "$PSScriptRoot/images" -File | Sort-Object Name).FullName
$wallpaper='C:/Windows/Web/4K/Wallpaper/Windows/img0_1920x1200.jpg'
if(Test-Path $wallpaper){$fixtures=@($wallpaper)+$fixtures}
$arguments=@('--self-test',('"'+$log+'"'))+@($fixtures | ForEach-Object {'"'+$_+'"'})
$process=Start-Process $exe -ArgumentList $arguments -PassThru
if(!$process.WaitForExit(60000)){throw 'Runtime test timed out'}
Get-Content $log
if($process.ExitCode -ne 0){throw "Runtime test failed: $($process.ExitCode)"}
