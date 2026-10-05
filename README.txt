VETRO LOOK 2.4.0 — TOKYO GLASS

Launch VetroLook.exe, then drop an image onto the window or press Ctrl+O.
You can also pass an image path on the command line or use Windows Open with.
Keep libmpv-2.dll beside the executable for the built-in Vetro Video mode.
Photo viewing works offline. Optional AI subtitles require a separate download.
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
The photo tools sit in the lower dock; zoom is on the lower left and editing
on the lower right. The library has a separate search and view control row.
Rotation affects only the view and never changes your source image.

FILES AND GALLERY
The menu beside VETRO LOOK opens the existing glass sidebar. Recent opens by
default. Favorites, pinned folders, system locations and a compact current
path are available there. Right-click a folder to pin it or delete to the
Recycle Bin. Hover a dragged file over a folder for 500 ms to open it; dropping
copies the file through Windows Shell. Search scope supports current folder,
subfolders and all indexed locations. Folder reads and searches run in workers.

FIRST RUN AND SMART GALLERY
On first launch, choose English or Russian. The question is in English and
cannot be closed until you choose. Your language is saved and can be changed
later in the menu.

Smart Gallery labels images locally with a bundled six-class model. The first
pass shows progress and approximate remaining time. GOT IT collapses the popup
into a toast; after completion, choose Yes, apply or Later. Images remain in
place until you apply. Yes also enables automatic application to new files.
Later keeps the labels and asks again next launch.

Show all and category controls are in the filter panel. Right-click an image
for Always show, Hide from gallery or Return to automatic classification.
Manual corrections persist, uncertain images remain visible, and original
files are never changed. The model's 95% upper bound for hiding a real photo
is 0.15061%, above the original 0.1% target; see models/MODEL_CARD.md.
Personal gallery data and training materials are not distributed.

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
