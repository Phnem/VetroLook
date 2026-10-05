<#
Finds installed Vetro Look releases (MSI, Inno, MSIX), shortcuts and portable
executables in the specified search roots. Default: inventory only.
Apply uses official uninstallers before removing verified residual payloads,
installs the specified new Setup.exe, and launches that installed executable.
Settings, favourites, library data and downloaded video components are kept.
Vetro Video is built into Vetro Look; Vetro Probe is a separate product.

  .\Clean-Install-VetroLook.ps1
  .\Clean-Install-VetroLook.ps1 -Apply -InstallerPath ..\release\VetroLook-2.3-Setup.exe
  .\Clean-Install-VetroLook.ps1 -SearchRoots D:\Portable,C:\Users\me\Downloads

Run -Apply from an administrator PowerShell for machine-wide installations.
No directories containing user media are recursively deleted. The source
workspace and new installer are excluded from the search and cleanup.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [string]$InstallerPath,
    [string[]]$SearchRoots = @(
        "$env:LOCALAPPDATA\Programs", "$env:USERPROFILE\Desktop",
        "$env:USERPROFILE\Downloads", 'C:\Program Files', 'C:\Program Files (x86)'
    ),
    [string]$WorkspacePath,
    [string]$ReportPath,
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
if (-not $WorkspacePath) { $WorkspacePath = Split-Path $PSScriptRoot -Parent }
if (-not $ReportPath) { $ReportPath = Join-Path $PSScriptRoot '..\artifacts\vetro-clean-install.json' }
$productPattern = '^Vetro\s*Look(?:\s+(?:version\s+|v)?\d+(?:\.\d+)*)?$'
$workspace = [IO.Path]::GetFullPath($WorkspacePath).TrimEnd('\')
$installer = if ($InstallerPath) { (Resolve-Path -LiteralPath $InstallerPath).Path } else { $null }
$report = [IO.Path]::GetFullPath($ReportPath)
trap {
    $_ | Out-String | Set-Content -LiteralPath ($report+'.error.txt') -Encoding UTF8
    exit 1
}
$events = [Collections.Generic.List[object]]::new()
function Record([string]$action, [string]$target) {
    $events.Add([pscustomobject]@{Time=(Get-Date).ToString('o');Action=$action;Target=$target})
    Write-Host "$action : $target"
}
function Is-Within([string]$path, [string]$parent) {
    $absolute = [IO.Path]::GetFullPath($path).TrimEnd('\')
    $base = [IO.Path]::GetFullPath($parent).TrimEnd('\')
    return $absolute.Equals($base,[StringComparison]::OrdinalIgnoreCase) -or
        $absolute.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)
}
function Test-VetroExe([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    $file = Get-Item -LiteralPath $path
    if ($file.Name -ne 'VetroLook.exe' -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return $false }
    # Earlier builds have no version resource. In that case require the exact
    # executable name together with this application's runtime and notices.
    $info = $file.VersionInfo
    if ($info.ProductName -match $productPattern -or $info.FileDescription -match $productPattern) { return $true }
    return (Test-Path -LiteralPath (Join-Path $file.DirectoryName 'libmpv-2.dll')) -and
        (Test-Path -LiteralPath (Join-Path $file.DirectoryName 'THIRD_PARTY_NOTICES.txt')) -and
        (((Get-Content -LiteralPath (Join-Path $file.DirectoryName 'README.txt') -TotalCount 8 -ErrorAction SilentlyContinue) -join ' ') -match 'Vetro\s*Look')
}
function Find-Portable([string]$root) {
    if (-not (Test-Path -LiteralPath $root -PathType Container) -or (Is-Within $root $workspace)) { return }
    $queue = [Collections.Generic.Queue[string]]::new();$queue.Enqueue([IO.Path]::GetFullPath($root))
    while ($queue.Count) {
        $directory = $queue.Dequeue()
        if (Is-Within $directory $workspace) { continue }
        foreach ($item in Get-ChildItem -LiteralPath $directory -ErrorAction SilentlyContinue) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($item.PSIsContainer) {
                if ($item.Name -notin @('WindowsApps','.git','node_modules')) { $queue.Enqueue($item.FullName) }
            } elseif ($item.Name -eq 'VetroLook.exe' -and (Test-VetroExe $item.FullName)) { $item.FullName }
        }
    }
}
$registrations = @(
    foreach ($root in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')) {
        foreach ($key in Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue) {
            $entry = Get-ItemProperty -LiteralPath $key.PSPath
            if ($entry.DisplayName -match $productPattern) {
                [pscustomobject]@{Name=$entry.DisplayName;Version=$entry.DisplayVersion;
                    Key=$key.PSPath;Code=$key.PSChildName;Location=$entry.InstallLocation;
                    Command=$entry.UninstallString;Msi=($entry.WindowsInstaller -eq 1)}
            }
        }
    }
)
$packages = @(Get-AppxPackage -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -match '^(?:[A-Za-z0-9]+\.)?VetroLook$'
})
$executables = @($SearchRoots | ForEach-Object { Find-Portable $_ } | Sort-Object -Unique)
$shortcuts = @()
$shell = New-Object -ComObject WScript.Shell
foreach ($root in @([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('CommonDesktopDirectory'),
    [Environment]::GetFolderPath('StartMenu'),[Environment]::GetFolderPath('CommonStartMenu'))) {
    if (-not $root) { continue }
    foreach ($link in Get-ChildItem -LiteralPath $root -Filter '*.lnk' -Recurse -ErrorAction SilentlyContinue) {
        if ($link.BaseName -notmatch '^Vetro\s*Look') { continue }
        $target = $shell.CreateShortcut($link.FullName).TargetPath
        if ([IO.Path]::GetFileName($target) -eq 'VetroLook.exe' -and -not (Is-Within $target $workspace)) {
            $shortcuts += [pscustomobject]@{Path=$link.FullName;Target=$target}
        }
    }
}
function Save-Report([string]$status) {
    New-Item -ItemType Directory -Path (Split-Path $report -Parent) -Force | Out-Null
    [pscustomobject]@{Status=$status;PreserveUserData=$true;WorkspaceExcluded=$workspace;
        SearchRoots=$SearchRoots;Registrations=$registrations;Packages=@($packages | ForEach-Object PackageFullName);
        Executables=$executables;Shortcuts=$shortcuts;Events=@($events.ToArray())} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $report -Encoding UTF8
}
Save-Report 'Inventory'
$registrations | Format-Table Name,Version,Location
$executables | ForEach-Object { Write-Host "Executable: $_" }
if (-not $Apply) { Write-Host "Inventory saved: $report"; return }
if (-not $installer -or [IO.Path]::GetFileName($installer) -notmatch '^VetroLook-[\d.]+-Setup\.exe$') {
    throw 'Supply the freshly built VetroLook-version-Setup.exe before applying cleanup.'
}
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Apply requires administrator rights to remove machine-wide MSI/Inno installs. Inventory is unchanged.'
}
$backup = Join-Path (Split-Path $report -Parent) ('VetroLook-settings-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$startupEnabled = $null -ne (Get-ItemProperty -LiteralPath $runKey -Name VetroLook -ErrorAction SilentlyContinue)
# Export only user preferences, not old file associations or installer state.
if (Test-Path 'HKCU:\Software\VetroLook\Settings') {
    & reg.exe export 'HKCU\Software\VetroLook\Settings' (Join-Path $backup 'Settings.reg') /y | Out-Null
    if ($LASTEXITCODE) { throw 'Settings backup failed; cleanup aborted.' }
}
# Retain data in place and keep a second copy of the small persistent records.
foreach ($dataRoot in @("$env:LOCALAPPDATA\VetroLook","$env:APPDATA\VetroLook")) {
    if (Test-Path -LiteralPath $dataRoot) {
        $label = if ($dataRoot.StartsWith($env:LOCALAPPDATA)) { 'Local' } else { 'Roaming' }
        $destination = Join-Path $backup $label;New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Get-ChildItem -LiteralPath $dataRoot -File | Where-Object Length -lt 64MB |
            Copy-Item -Destination $destination -Force
    }
}
Record 'Settings backup' $backup
try {
    foreach ($process in Get-Process -Name VetroLook -ErrorAction SilentlyContinue) {
        if ($process.Path -and (Test-VetroExe $process.Path)) { Stop-Process -Id $process.Id -Force;Record 'Stopped' $process.Path }
    }
    foreach ($entry in $registrations) {
        if ($entry.Msi -or $entry.Command -match '(?i)^MsiExec(?:\.exe)?\s') {
            if ($entry.Code -notmatch '^\{[0-9A-Fa-f-]{36}\}$') { throw 'Invalid MSI product identity.' }
            $process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -WindowStyle Hidden -PassThru -Wait `
                -ArgumentList @('/x',$entry.Code,'/qn','/norestart','/l*v',('"'+(Join-Path $backup ($entry.Code+'.log'))+'"'))
            if ($process.ExitCode -notin @(0,1605,1614,3010)) { throw "MSI removal failed: $($process.ExitCode)" }
        } else {
            if ($entry.Command -notmatch '^"([^"]+\.exe)"(?:\s.*)?$') { throw "Unknown uninstaller: $($entry.Command)" }
            $uninstaller = [IO.Path]::GetFullPath($Matches[1])
            $installDir = Split-Path $uninstaller -Parent
            if ([IO.Path]::GetFileName($uninstaller) -notmatch '^unins\d+\.exe$' -or
                -not (Test-VetroExe (Join-Path $installDir 'VetroLook.exe')) -or (Is-Within $installDir $workspace)) {
                throw "Uninstaller identity could not be verified: $uninstaller"
            }
            $process = Start-Process -FilePath $uninstaller -WindowStyle Hidden -PassThru -Wait `
                -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
            if ($process.ExitCode -ne 0) { throw "Inno removal failed: $($process.ExitCode)" }
        }
        Record 'Uninstalled' ($entry.Name+' '+$entry.Version)
    }
    foreach ($package in $packages) { Remove-AppxPackage -Package $package.PackageFullName;Record 'Removed MSIX' $package.PackageFullName }
    # Remove just verified app payloads from portable/residual copies. Never
    # recursively delete their parent: it can contain the user's photographs.
    foreach ($exe in $executables) {
        if ((Is-Within $exe $workspace) -or -not (Test-VetroExe $exe)) { continue }
        $directory = Split-Path $exe -Parent
        foreach ($name in @('VetroLook.exe','libmpv-2.dll','README.txt','THIRD_PARTY_NOTICES.txt')) {
            $payload = [IO.Path]::GetFullPath((Join-Path $directory $name))
            if (-not (Is-Within $payload $directory) -or (Is-Within $payload $workspace)) { throw 'Unsafe payload path.' }
            if (Test-Path -LiteralPath $payload -PathType Leaf) { Remove-Item -LiteralPath $payload -Force;Record 'Removed old payload' $payload }
        }
        $lensDb = [IO.Path]::GetFullPath((Join-Path $directory 'lensfun-db'))
        if ((Test-Path -LiteralPath (Join-Path $lensDb 'COPYING.CC_BY-SA_3.0')) -and
            (Is-Within $lensDb $directory) -and -not (Is-Within $lensDb $workspace) -and
            -not ((Get-Item -LiteralPath $lensDb).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Remove-Item -LiteralPath $lensDb -Recurse -Force;Record 'Removed old lens database' $lensDb
        }
    }
    foreach ($link in $shortcuts) {
        if (Test-Path -LiteralPath $link.Path) { Remove-Item -LiteralPath $link.Path -Force;Record 'Removed old shortcut' $link.Path }
    }
    $installLog = Join-Path $backup 'install.log'
    $process = Start-Process -FilePath $installer -WindowStyle Hidden -PassThru -Wait -ArgumentList @(
        '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',('/LOG="'+$installLog+'"'))
    if ($process.ExitCode -ne 0) { throw "New installation failed: $($process.ExitCode)" }
    $installedExe = Join-Path $env:ProgramFiles 'VetroLook\VetroLook.exe'
    if (-not (Test-VetroExe $installedExe)) { throw 'Installed executable could not be verified.' }
    $builtExe = Join-Path $workspace 'dist\VetroLook.exe'
    if ((Test-Path -LiteralPath $builtExe) -and
        (Get-FileHash -LiteralPath $builtExe).Hash -ne (Get-FileHash -LiteralPath $installedExe).Hash) {
        throw 'Installed binary differs from the new build.'
    }
    if (Test-Path -LiteralPath (Join-Path $backup 'Settings.reg')) {
        & reg.exe import (Join-Path $backup 'Settings.reg') | Out-Null
        if ($LASTEXITCODE) { throw 'Could not restore saved preferences.' }
    }
    # Rebind optional startup to the installed path after restoring preferences.
    if ($startupEnabled) { Set-ItemProperty -LiteralPath $runKey -Name VetroLook -Value ('"'+$installedExe+'" --background') }
    Record 'Installed and verified' $installedExe
    Save-Report 'Complete'
    if (-not $NoLaunch) { Start-Process -FilePath $installedExe -WindowStyle Normal;Record 'Launched' $installedExe;Save-Report 'Complete' }
} catch {
    if (Test-Path -LiteralPath (Join-Path $backup 'Settings.reg')) {
        & reg.exe import (Join-Path $backup 'Settings.reg') | Out-Null
    }
    Record 'Failed' $_.Exception.Message
    Save-Report 'Failed'
    throw
}
