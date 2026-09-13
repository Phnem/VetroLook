VETRO LOOK 2.1 — NATIVE IMAGE AND VIDEO VIEWER

Launch VetroLook.exe, then drop a photo or a film onto the window or press Ctrl+O.
You can also pass a path or a web address on the command line, or use Windows
Open with. Windows 10/11 x64 is the intended platform.

WHAT IS IN THIS FOLDER
VetroLook.exe             the viewer
libmpv-2.dll              the playback engine; keep it beside VetroLook.exe
lensfun-db\               lens profiles for photo metadata
THIRD_PARTY_NOTICES.txt   licences of everything shipped here

EXPLORER PREVIEW
Keep VetroLook running. Select an image in Explorer's file list and press Space.
Space again or Escape hides preview. A tray icon remains available.
VetroLook.exe --background starts only the tray/Space listener.

PHOTOS
Left/Right: previous/next file in the folder
Wheel or +/-: zoom; drag: pan; double-click: fit/100%
0: fit; 1: 100%; R / Shift+R: rotate right/left
Rotation affects only the view and never changes your source image.

FILMS
Space: play/pause            Left/Right: seek (5 or 10 s, set in the … menu)
, and . : one frame back/forward (the film pauses itself)
PageUp/PageDown: chapters    Z / Shift+Z, X / Shift+X: subtitle and audio delay
S: save the frame            L: A-B loop (start, end, off)
P: picture in picture        T: pin picture in picture on top
A: AI subtitles on/off       Shift+A: save AI subtitles beside the film
End: back to the live edge of a live stream
Ctrl+V: open a web address from the clipboard
Hover the timeline to preview frames. A film reopens where you left it.

AI SUBTITLES
Open the … menu and choose AI subtitles · Download (about 1.2 GB, once).
The speech runtime, speech detector and model are fetched from their official
sources, checked against their published checksums and stored under
%LOCALAPPDATA%\VetroLook. Audio never leaves your computer. The same row
removes them again. An NVIDIA GPU makes transcription much faster.

STREAMS
Direct links, HLS and DASH play by address. Links to web pages need yt-dlp.exe
in a folder named resolver beside VetroLook.exe; it is not included.

FORMATS
Photos: JPEG, PNG, GIF, WebP, AVIF, EXR, BMP, TIFF, PSD/PSB and supported
LibRaw camera formats. HEIC/HEIF uses installed Windows codecs.
Films and audio: MP4, MKV, MOV, WebM, AVI, TS and the other containers and
codecs the playback engine supports, with embedded and sidecar subtitles.

Retain THIRD_PARTY_NOTICES.txt and lensfun-db/COPYING.CC_BY-SA_3.0 when
redistributing this package.
