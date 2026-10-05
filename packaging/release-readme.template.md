# VetroLook — release packages

Version {{VERSION}}, Windows 10/11 x64. Four independent, standard distribution
formats. None of them ships a separate uninstaller executable — each one
manages its own removal through the mechanism Windows already provides for
that format.

## VetroLook-{{VERSION}}-Setup.exe (Inno Setup)

Standard interactive installer. Installs to `Program Files\VetroLook`,
creates Start Menu / optional desktop shortcuts, and registers the app under
**Settings → Apps → Installed apps → VetroLook**.

Inno Setup generates its own uninstaller (`unins000.exe`) inside the install
folder automatically — there is nothing separate to ship or run by hand.
Uninstall from **Installed apps**, or run that executable directly.

Silent install / uninstall (standard Inno Setup switches, no custom flags):

```text
VetroLook-{{VERSION}}-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
"%ProgramFiles%\VetroLook\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

## VetroLook-{{VERSION}}-x64.msi (WiX / Windows Installer)

Standard MSI package. Install/uninstall is handled entirely by Windows
Installer — there is no bundled uninstaller executable. The app appears
under **Settings → Apps → Installed apps** exactly like any other MSI-based
program, and removal goes through its ProductCode:

```text
msiexec /i VetroLook-{{VERSION}}-x64.msi /qn
msiexec /x VetroLook-{{VERSION}}-x64.msi /qn
```

<!-- msix -->
## VetroLook-{{VERSION}}-x64.msix (MSIX / Microsoft Store)

Built unsigned, as required for Microsoft Store submission — the Store signs
it during ingestion. Once installed (via the Store, or sideloaded with a
trusted signature), install/update/uninstall lifecycle is fully owned by
Windows / the Store; there is no separate uninstaller. Remove it the same
way as any other Store app (**Settings → Apps**, or right-click → Uninstall).

A locally-signed copy for sideload testing only (never part of this release)
can be produced with `.\build-packages.ps1 -LocalTestSign`; see
`packaging/msix/local-test/`. That dev certificate must not be distributed —
it exists purely so this package can be installed on a development machine
before a real Store signature is available.
<!-- /msix -->

## First launch and Smart Gallery

Choose English or Russian at the mandatory first-run language question.
The bundled Smart Gallery model runs locally. First review results stay staged
until you choose Yes, apply. GOT IT collapses progress into a toast; Later
preserves labels and asks again next launch. Manual Show/Hide decisions win
over classification. Read `models/MODEL_CARD.md` for the classifier's measured
limits; the original scientific quality gate remains failed.

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

`VetroLook-{{VERSION}}-Portable.zip` contains `VetroLook.exe`, the playback
engine `libmpv-2.dll`, the Lensfun database, the local Smart Gallery model/policy and the license/notices — no
installation and no uninstaller. The viewer keeps its settings and caches under
`%LOCALAPPDATA%\VetroLook`.
