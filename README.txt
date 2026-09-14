VETRO LOOK 1.1 — NATIVE IMAGE VIEWER

Launch VetroLook.exe, then drop an image onto the window or press Ctrl+O.
You can also pass an image path on the command line or use Windows Open with.
No installation, network connection, runtime download, or bundled DLL is needed.
Windows 10/11 x64 is the intended platform.

EXPLORER PREVIEW
Keep VetroLook running. Select an image in Explorer's file list and press Space.
Space again or Escape hides preview. A tray icon remains available.
VetroLook.exe --background starts only the tray/Space listener.
Right-click the tray icon and choose Quit to stop it.
Normal viewer close exits the application; launch it again to restore Space support.

CONTROLS
Left/Right: previous/next sibling image
Wheel or +/-: zoom; drag: pan; double-click: fit/100%
0: fit; 1: 100%; R / Shift+R: rotate right/left
Ctrl+O: open; Escape: close; drag top edge: move; drag window edges: resize
Floating controls fade after inactivity. Move the mouse to reveal them.
Rotation affects only the view and never changes your source image.

FORMATS
JPEG, PNG, GIF, WebP, AVIF, EXR, BMP, TIFF, PSD/PSB, and supported LibRaw
camera formats are supported. JPEG, PNG, TIFF, static WebP, PSD and PSB use
screen-sized decode/scale paths where their codecs support it. RAW browsing
selects a useful embedded preview before a full decode when possible.
WIC fallback can use installed Windows codecs; HEIC/HEIF support is not guaranteed.
GIF, WebP and AVIF display their first frame. JPEG XL is not in this build.

The Lensfun profile database beside this executable enables lens identification.
Retain THIRD_PARTY_NOTICES.txt and lensfun-db/COPYING.CC_BY-SA_3.0 when
redistributing this portable package.
