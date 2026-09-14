# Vetro Look Clean Uninstaller
# Removes legacy VetroLook installations and their registrations before a
# clean install. It intentionally does not touch the repository's `dist`
# folder, so this utility can sit beside a freshly built release.

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Administrator)) {
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated'
    )
    exit $LASTEXITCODE
}

$log = Join-Path $env:TEMP 'VetroLook-clean-uninstall.log'
function Write-Log([string] $message) {
    "$(Get-Date -Format s)  $message" | Add-Content -LiteralPath $log -Encoding utf8
}
function Remove-ExactPath([string] $path) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Continue
        Write-Log "Removed path: $path"
    }
}
function Remove-ExactKey([string] $path) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Continue
        Write-Log "Removed registry key: $path"
    }
}

Write-Log 'Clean uninstall started.'

# The old executable cannot be removed while it is still hosting a film.
Get-Process -Name 'VetroLook' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction Continue

# Only exact, historical VetroLook installation and data locations are
# removed. Nothing is inferred from a wildcard or an arbitrary parent folder.
$installPaths = @(
    'C:\Program Files\VetroLook',
    'C:\Program Files (x86)\VetroLook',
    (Join-Path $env:LOCALAPPDATA 'Programs\VetroLook'),
    (Join-Path $env:APPDATA 'VetroLook'),
    (Join-Path $env:LOCALAPPDATA 'VetroLook')
)
foreach ($path in $installPaths) { Remove-ExactPath $path }

# VetroLook's own ProgIDs, capabilities, App Paths and per-user application
# entries. The extension defaults (UserChoice) are deliberately not altered:
# Windows owns those choices, and another app must not suddenly lose default.
$registryKeys = @(
    'HKCU:\Software\Classes\Applications\VetroLook.exe',
    'HKCU:\Software\Classes\VetroLook.Media',
    'HKCU:\Software\Classes\VetroLook.Image',
    'HKCU:\Software\VetroLook',
    'HKLM:\Software\Classes\Applications\VetroLook.exe',
    'HKLM:\Software\Classes\VetroLook.Media',
    'HKLM:\Software\Classes\VetroLook.Image',
    'HKLM:\Software\VetroLook',
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\App Paths\VetroLook.exe'
)
foreach ($key in $registryKeys) { Remove-ExactKey $key }

foreach ($runKey in @(
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run',
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run'
)) {
    if (Test-Path -LiteralPath $runKey) {
        Remove-ItemProperty -LiteralPath $runKey -Name 'VetroLook' -ErrorAction SilentlyContinue
    }
}
foreach ($appsKey in @(
    'HKCU:\Software\RegisteredApplications',
    'HKLM:\Software\RegisteredApplications'
)) {
    if (Test-Path -LiteralPath $appsKey) {
        Remove-ItemProperty -LiteralPath $appsKey -Name 'Vetro Look' -ErrorAction SilentlyContinue
        Remove-ItemProperty -LiteralPath $appsKey -Name 'VetroLook' -ErrorAction SilentlyContinue
    }
}

# Remove only VetroLook's Open With values; other applications and the default
# handler for every extension remain untouched.
$extensions = @(
    '.jpg','.jpeg','.jfif','.png','.gif','.webp','.avif','.exr','.bmp','.tif','.tiff','.ico','.heic','.heif',
    '.psd','.psb','.cr2','.cr3','.nef','.arw','.dng','.raf','.rw2','.orf','.pef',
    '.mp4','.m4v','.mov','.mkv','.webm','.avi','.wmv','.flv','.mpeg','.mpg','.ts','.m2ts','.mts',
    '.mp3','.m4a','.aac','.flac','.wav','.ogg','.opus','.wma'
)
foreach ($root in @('HKCU:\Software\Classes','HKLM:\Software\Classes')) {
    foreach ($extension in $extensions) {
        $openWith = Join-Path $root "$extension\OpenWithProgids"
        if (Test-Path -LiteralPath $openWith) {
            Remove-ItemProperty -LiteralPath $openWith -Name 'VetroLook.Media' -ErrorAction SilentlyContinue
            Remove-ItemProperty -LiteralPath $openWith -Name 'VetroLook.Image' -ErrorAction SilentlyContinue
        }
    }
}

# Remove obsolete Apps & Features records only when their display name is this
# product. This avoids guessing a product code or deleting another vendor's
# uninstall record.
foreach ($root in @(
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
)) {
    if (-not (Test-Path -LiteralPath $root)) { continue }
    Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue | ForEach-Object {
        $entry = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
        if ($entry.DisplayName -match '^Vetro\s*Look') {
            Remove-Item -LiteralPath $_.PSPath -Recurse -Force -ErrorAction Continue
            Write-Log "Removed uninstall record: $($_.PSChildName)"
        }
    }
}

# Ask Explorer to discard cached association/icon metadata.
Add-Type -Namespace Native -Name Shell -MemberDefinition @'
    [System.Runtime.InteropServices.DllImport("shell32.dll")]
    public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);
'@ -ErrorAction SilentlyContinue
[Native.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Log 'Clean uninstall completed.'

[System.Windows.Forms.MessageBox]::Show(
    "The old VetroLook installation and registrations were removed.`n`nYou can now run the new Setup.exe or MSI package.",
    'VetroLook Clean Uninstaller',
    [System.Windows.Forms.MessageBoxButtons]::OK,
    [System.Windows.Forms.MessageBoxIcon]::Information
) | Out-Null
