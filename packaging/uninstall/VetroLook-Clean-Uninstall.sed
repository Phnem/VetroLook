[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=1
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=..\..\release\VetroLook-Clean-Uninstall.exe
FriendlyName=VetroLook Clean Uninstaller
AppLaunched=powershell.exe -NoProfile -ExecutionPolicy Bypass -File cleanup-old-vetrolook.ps1
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
SourceFiles=SourceFiles
[Strings]
FILE0="cleanup-old-vetrolook.ps1"
[SourceFiles]
SourceFiles0=.
[SourceFiles0]
%FILE0%=
