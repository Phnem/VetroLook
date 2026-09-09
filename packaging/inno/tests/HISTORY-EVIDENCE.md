# Historical installer evidence

The public repository history was inspected at commits `eebdc12` (1.0.0) and
`55c09ea` (1.0.1). The pre-Inno installer registered exactly:

- key: `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\VetroLook`
- `DisplayName=VetroLook`
- `Publisher=Phnem`
- `InstallLocation=%LOCALAPPDATA%\Programs\VetroLook`
- installed uninstaller: `%LOCALAPPDATA%\Programs\VetroLook\Uninstall.exe`
- 1.0.1 `QuietUninstallString`: quoted `Uninstall.exe` plus `/silent`

Version 1.0.0 lacked `QuietUninstallString`, but its same custom uninstaller
accepts `/silent`, `/verysilent`, and `/quiet`. It removes only the historical
installation directory and shell registrations. Settings under
`HKCU\Software\VetroLook` and the library index/cache outside that directory
are not deletion targets in that uninstaller.

The fixture harness validates this exact identity and path rather than trusting
an arbitrary command stored under a similarly named registry key.
