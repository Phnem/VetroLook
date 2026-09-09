#define MyAppName "VetroLook"
#ifndef MyAppVersion
#define MyAppVersion "1.0.0"
#endif
#ifndef MyExePath
#define MyExePath "..\..\dist\VetroLook.exe"
#endif
#ifndef MyLensDbPath
#define MyLensDbPath "..\..\dist\lensfun-db"
#endif
#ifndef MyReadmePath
#define MyReadmePath "..\..\dist\README.txt"
#endif
#ifndef MyNoticesPath
#define MyNoticesPath "..\..\dist\THIRD_PARTY_NOTICES.txt"
#endif
#define MyAppPublisher "VetroLook"
#define MyAppURL "https://github.com/Phnem/VetroLook"

[Setup]
AppId={{E7F3A1C0-4B2D-4E6F-9A8B-1C2D3E4F5A6B}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
; Without this, {autopf} follows whatever privileges Setup happens to run with and
; a non-elevated run lands in %LOCALAPPDATA%\Programs instead of Program Files.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\..\release
OutputBaseFilename=VetroLook-{#MyAppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\VetroLook.exe
LicenseFile=..\..\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#MyExePath}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyLensDbPath}\*"; DestDir: "{app}\lensfun-db"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MyReadmePath}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyNoticesPath}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\VetroLook.exe"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\VetroLook.exe"; Tasks: desktopicon

[Run]
; Registers file associations/"Open with" entries the same way "VetroLook.exe --register"
; does when run by hand -- runs on every install (including /VERYSILENT) so associations
; are live immediately, without requiring the user to open the app first.
Filename: "{app}\VetroLook.exe"; Parameters: "--register"; Flags: runhidden waituntilterminated
Filename: "{app}\VetroLook.exe"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Mirror image of the [Run] entry above: undo exactly what --register wrote, before
; Setup's own file removal deletes VetroLook.exe. Runs on silent uninstall too.
Filename: "{app}\VetroLook.exe"; Parameters: "--unregister"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterVetroLook"

[Code]
#include "migration.issinc"

// Before Inno, releases 1.0.0/1.0.1 used a custom per-user installer.  Later
// builds used per-user Inno with the current AppId.  Invoke both products'
// registered uninstallers; never recursively delete their installation folder.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := MigrateRegisteredInstall(CustomUninstallKey, 'custom', True);
  if Result = '' then
    Result := MigrateRegisteredInstall(InnoUninstallKey, 'Inno', False);
end;
