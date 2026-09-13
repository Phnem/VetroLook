# VetroLook — release packages

Version 2.1, Windows 10/11 x64. Four independent, standard distribution
formats. None of them ships a separate uninstaller executable — each one
manages its own removal through the mechanism Windows already provides for
that format.

## VetroLook-2.1-Setup.exe (Inno Setup)

Standard interactive installer. Installs to `Program Files\VetroLook`,
creates Start Menu / optional desktop shortcuts, and registers the app under
**Settings → Apps → Installed apps → VetroLook**.

Inno Setup generates its own uninstaller (`unins000.exe`) inside the install
folder automatically — there is nothing separate to ship or run by hand.
Uninstall from **Installed apps**, or run that executable directly.

Silent install / uninstall (standard Inno Setup switches, no custom flags):

```text
VetroLook-2.1-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
"%ProgramFiles%\VetroLook\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

## VetroLook-2.1-x64.msi (WiX / Windows Installer)

Standard MSI package. Install/uninstall is handled entirely by Windows
Installer — there is no bundled uninstaller executable. The app appears
under **Settings → Apps → Installed apps** exactly like any other MSI-based
program, and removal goes through its ProductCode:

```text
msiexec /i VetroLook-2.1-x64.msi /qn
msiexec /x VetroLook-2.1-x64.msi /qn
```


## AI subtitles

None of the packages contains the speech runtime or model. Open the **…**
menu in Vetro Look and choose **AI subtitles · Download** (about 1.2 GB,
once): the runtime, speech detector and model are fetched from their official
sources, checked against their published SHA-256 and stored under
`%LOCALAPPDATA%\VetroLook`. The same row removes them again.

## Verifying downloads

`SHA256SUMS.txt` in this folder lists the SHA-256 of every file that was
actually produced by the last packaging run.

## Portable build (optional)

`VetroLook-2.1-Portable.zip` contains `VetroLook.exe`, the playback
engine `libmpv-2.dll`, the Lensfun database and the license/notices — no
installation and no uninstaller. The viewer keeps its settings and caches under
`%LOCALAPPDATA%\VetroLook`.
