#define MyOutputDir ".\out"
[Setup]
AppId=VetroLookMigrationFixture
AppName=VetroLookMigrationFixture
AppVersion=1
DefaultDirName={tmp}\VetroLookMigrationFixture
Uninstallable=no
CreateAppDir=no
OutputDir={#MyOutputDir}
OutputBaseFilename=VetroLookMigrationFixture
PrivilegesRequired=lowest
DisableWelcomePage=yes

[Code]
#include "..\migration.issinc"

var
  Failures: Integer;
  Report: String;

procedure Check(Value: Boolean; const Name: String);
begin
  if Value then
    Report := Report + 'PASS ' + Name + #13#10
  else begin
    Report := Report + 'FAIL ' + Name + #13#10;
    Failures := Failures + 1;
  end;
end;

procedure CheckFixtures;
var
  FileName, Parameters, Root: String;
begin
  Root := 'C:\Users\fixture user\AppData\Local\Programs\VetroLook';
  Check(BuildMigrationPlan(True, Root, 'VetroLook', 'Phnem', Root,
    '"' + Root + '\Uninstall.exe" /silent',
    '"' + Root + '\Uninstall.exe"', False, FileName, Parameters) and
    SameText(FileName, Root + '\Uninstall.exe') and SameText(Parameters, '/silent'),
    'custom entry prefers QuietUninstallString');

  Check(BuildMigrationPlan(True, Root, 'VetroLook', 'Phnem', Root, '',
    '"' + Root + '\Uninstall.exe"', False, FileName, Parameters) and
    (Pos('/silent', Lowercase(Parameters)) > 0),
    'custom entry adds historical quiet flag');

  Check(BuildMigrationPlan(False, Root, 'VetroLook', 'VetroLook', Root, '',
    '"' + Root + '\unins000.exe"', False, FileName, Parameters) and
    (Pos('/verysilent', Lowercase(Parameters)) > 0) and
    (Pos('/suppressmsgboxes', Lowercase(Parameters)) > 0) and
    (Pos('/norestart', Lowercase(Parameters)) > 0),
    'Inno entry receives silent flags');

  Check(BuildMigrationPlan(False, Root, 'VetroLook', 'VetroLook', Root,
    '"' + Root + '\unins001.exe" /SILENT',
    '"' + Root + '\unins001.exe"', False, FileName, Parameters) and
    SameText(Parameters, '/SILENT'),
    'Inno entry prefers registered QuietUninstallString');

  Check(BuildMigrationPlan(True, Root, ' VetroLook ', ' Phnem ', Root + '\', '',
    '"' + Root + '\Uninstall.exe" /quiet', False, FileName, Parameters) and
    SameText(Parameters, '/quiet'),
    'custom fallback preserves an existing historical quiet flag');

  Check(not BuildMigrationPlan(True, Root, 'Not VetroLook', 'Phnem', Root, '',
    '"' + Root + '\Uninstall.exe"', False, FileName, Parameters),
    'reject wrong display name');
  Check(not BuildMigrationPlan(True, Root, 'VetroLook', 'Other publisher', Root, '',
    '"' + Root + '\Uninstall.exe"', False, FileName, Parameters),
    'reject wrong custom publisher');
  Check(not BuildMigrationPlan(True, Root, 'VetroLook', 'Phnem', 'C:\Other', '',
    '"' + Root + '\Uninstall.exe"', False, FileName, Parameters),
    'reject wrong install location');
  Check(not BuildMigrationPlan(True, Root, 'VetroLook', 'Phnem', Root, '',
    '"C:\Windows\System32\cmd.exe" /c anything', False, FileName, Parameters),
    'reject uninstaller outside VetroLook directory');
  Check(not BuildMigrationPlan(False, Root, 'VetroLook', 'VetroLook', Root, '',
    '"' + Root + '\Uninstall.exe"', False, FileName, Parameters),
    'reject non-Inno executable for Inno key');
  Check(not BuildMigrationPlan(True, Root, 'VetroLook', 'Phnem', Root, '',
    '"' + Root + '\VetroLook-Uninstall.exe"', False, FileName, Parameters),
    'reject guessed custom uninstaller filename');
  Check(not BuildMigrationPlan(False, Root, 'VetroLook', 'VetroLook', Root, '',
    '"' + Root + '\uninsABC.exe"', False, FileName, Parameters),
    'reject malformed Inno uninstaller filename');
end;

function InitializeSetup(): Boolean;
var
  ResultPath: String;
begin
  CheckFixtures;
  ResultPath := ExpandConstant('{param:RESULT|migration-fixture-results.txt}');
  if Failures = 0 then Report := Report + 'SUMMARY PASS' + #13#10
  else Report := Report + 'SUMMARY FAIL ' + IntToStr(Failures) + #13#10;
  SaveStringToFile(ResultPath, Report, False);
  Result := False;
end;
