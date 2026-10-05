# Changelog

## 2.4.0 вЂ” 2026-10-05

Changes since the last published release, 2.2:

- Added a mandatory first-run English/Russian language chooser. The question is in English; close, Alt+F4 and Escape cannot dismiss it before an explicit choice. The saved preference is honored on subsequent starts, including direct-file and background launches.
- Reworked the gallery and viewer around the Tokyo Glass palette, bold typography and rounded frosted surfaces; retained glass and restored image/video ambient reflection while preserving transparency for cutout images.
- Added the existing glass sidebar to library navigation: Recent, Favorites, pinned folders, system-location icons and a compact current-path stack.
- Added folder pin/unpin actions, Recycle Bin deletion, spring-loaded drag navigation and Current folder / Subfolders / Everywhere search scopes.
- Moved folder reads and searches to background workers and improved gallery initialization and thumbnail handling.
- Added local six-class Smart Gallery inference with a persistent model-bound cache, Show all/category controls and persistent manual Show/Hide overrides.
- Staged the first complete gallery review until the user applies it. Added progress, approximate remaining time, popup-to-toast animation, completion confirmation and a smooth gallery fade.
- Preserved staged labels after Later and offered application again on the next launch. After Yes, newly added files apply automatically.
- Retried files that change while queued using their latest metadata, and reset application consent when the model identity changes.
- Bundled the exact model/policy in EXE, MSI, MSIX and portable packaging. Published its validation limitation in the model card; the original scientific gate remains failed and is not presented as a pass.
- Preserved settings, favorites and gallery corrections in the legacy clean uninstaller.
- Updated README, release documentation and package scripts to work in both the development workspace and a standalone repository checkout.

## 2.2 вЂ” 2026-09-14

- Rebuilt Video Mode chrome as dense macOS-style frosted matte material: live backdrop blur, restrained vibrancy and contrast, neutral tint, micro-grain, inner highlight, bright hairline and soft shadow.
- Replaced the transport glyphs with a consistent Phosphor Icons family and removed the ornamental disc around the primary playback action.
- Split audio tracks and subtitles into independent animated popups whose height follows their content and whose rows have real hover feedback.
- Kept the transport's DirectComposition blur stable while popups, timeline previews and subtitle bubbles appear or disappear.
- Added a dedicated picture-in-picture control and fixed repeated PiP entry, maximized-window restoration and mode-exit state leakage.
- Made the player window follow the video's aspect ratio and use the real Windows maximized state, without compact-window corner artefacts.
- Added real video magnification through mpv, controlled by the wheel and the `+`, `-`, `0` and `1` keys.
- Activated Send, Save, Save As and Delete for video files, while removing image-only Print, power and Space-preview settings from Video Mode.
- Improved video information, loading-state handling, popup close animation and the compact title treatment.
- Added release-grade MSI, Inno Setup and portable ZIP packaging plus a standalone clean uninstaller for stale legacy installations and shell registrations.
- Fixed the build pipeline retaining a cached review output directory and silently packaging an older executable.

## 2.1 вЂ” 2026-09-13

- Added media routing: a file's kind is decided by extension and, where the name is misleading, by its container signature, so a film named `.jpg` opens as a film and a photograph named `.mp4` opens as a photograph.
- Added Video Mode as a second presentation mode of the same window, with its poster frame and media facts; playback itself arrives with the engine.
- Made folder navigation and the filmstrip walk images and video together, with poster-frame tiles for films.
- Made the toolbar follow what the current mode can do, so zoom, crop, rotate and fit are absent for video rather than inert.
- Added video playback: films play in the same window, composited underneath the interface rather than in a window of their own.
- Added transport controls in the viewer's own visual language: play and pause, elapsed and total time, and a timeline that can be dragged.
- Made Space play and pause, the arrow keys seek by ten seconds, and Ctrl with an arrow move to the neighbouring file.
- Made the window take the film's shape: a 16:9 film gets a 16:9 window, a phone recording gets a tall one, and dragging an edge keeps that shape.
- Added frosted glass over video: the transport, the file name, the toolbars and the filmstrip are backed by the film itself, blurred by the compositor.
- Rebuilt the transport controls around volume, skip, play, skip, expand and a timeline, in Apple Human Interface Guidelines proportions.
- Made the playback library load only when a film is opened, so opening a photograph costs nothing and a machine without the library remains a complete image viewer.
- Added frame pacing: films are shown on the display's own cadence where that is exact, and the viewer tells the playback engine the refresh rate, which it cannot discover for itself while compositing into our window.
- Added a resource governor that protects playback: when a machine runs short, background work, prefetch, library indexing and the cost of the glass give way before the picture does, and none of it flaps back and forth.
- Added a capability probe: the adapter, its hardware decode profiles, the exact refresh rate, HDR state and paper white are read once and drive what is attempted.
- Made the engine's memory queues follow the machine's free memory rather than a fixed default.
- Added battery and thermal behaviour: on battery nothing is interpolated, enhancement is not offered, and a hidden window stops indexing the library.
- Added HDR output for displays that have it, with the film's colour graded for the panel actually in front of the viewer.
- Added a Synchronization and a Power setting while a film is open, and a Diagnostics row that copies a sanitised report of what the machine, the display and playback are doing.
- Fixed Open with: the viewer published itself to Windows as an image viewer only, so a film handed to it through the shell was opened as a damaged picture. Video and audio formats are now registered as well, under their own document type.
- Made the default-viewer row show whether this build's registration is the one published, so a build that has learned to open more formats asks to publish them.
- Added timeline previews: hovering or dragging the timeline shows the frame you are heading for, decoded by a second, output-less decoder that never disturbs playback.
- Added frame stepping with the comma and full stop keys, or Shift with the arrows: one displayed frame at a time, in both directions, with the film pausing itself.
- Made the arrow keys' seek step a setting of five or ten seconds.
- Added subtitles in the viewer's own material: plain dialogue is drawn as a frosted bubble that morphs between lines and collapses into a point after a silence, instead of blinking on and off.
- Made authored subtitle typesetting stay authored: a positioned sign, a rotation or karaoke timing is left to the engine's own renderer, decided per cue rather than per file.
- Added automatic loading of subtitle files sitting beside a film, including language-suffixed ones and subs folders.
- Added resume: a film reopens where it was left off, unless it was barely started or watched to the end.
- Added chapters, subtitle and audio delay, frame screenshots, an A-B loop and picture in picture, each on a key and each remembered with the film where that makes sense.
- Made a finished film hand over to the next one in the folder.
- Added the Windows media controls: the film appears on the lock screen and in the volume overlay, with its own timeline, and the keyboard's media keys drive it.
- Added playback from an address: a direct link, an HLS or a DASH stream opens from the command line or from the clipboard.
- Made the playback engine's own warnings reach the session log when debugging, so a stream that will not open can say why.
- Added links to web pages: a page address is handed to a separate resolver process (yt-dlp, supplied by the user), bounded by a job object, a memory limit and a 45-second timeout, and killed with the viewer. The page's title names the window, and sites that split picture and sound play both.
- Added live streams: a live HLS or DASH source says LIVE instead of a length, its timeline is the window kept since joining, and End returns to the live edge.
- Added reconnecting: a stream whose connection drops is reopened with backoff вЂ” 1, 2, 4, 8, 16 seconds, then every 30 вЂ” at the position it had reached, or at the live edge for a broadcast, and says so while it tries and when it is back.
- Made a stream that cannot play say why in a sentence: the address is gone, access was refused, the certificate did not verify, the page needs an account, or the source is DRM-protected. The engine's own words stay in Diagnostics.
- Made DRM-only services recognised by their address, so they get a clear message at once instead of a retry loop.
- Fixed the session log going silent after its first non-English line.
- Added AI subtitles, generated on this computer: press A, or choose them in the stream card, and the film's speech is transcribed a few minutes ahead of where you are and shown in the same bubble as ordinary subtitles. Audio never leaves the device.
- Added a single AI subtitles download in the вЂ¦ menu: the speech runtime, the speech detector and the model (about 1.2 GB together) arrive in one step with a progress bar, each checked against its published checksum, and can be removed again. Nothing AI-related is part of the installers.
- Made generated subtitles careful: silent stretches are never sent to the model, and credits it learned from subtitle files, lines it gets stuck repeating and doubtful words over non-speech are dropped.
- Made transcripts persistent: a film transcribed once opens with its subtitles ready, and a different model or language makes a separate transcript rather than reusing the wrong one.
- Added Shift+A to save the generated subtitles beside the film as movie.<language>.ai.srt.
- Added Skip silence, Gentle or Aggressive, for lectures and recordings with long pauses. It is off by default and never jumps over audio it could not read.
- Made transcription give way to playback: it runs below normal priority, pauses between chunks once it is well ahead, and waits whenever the resource governor asks.
- Added RTX Video Super Resolution: on an NVIDIA RTX card, a film shown larger than its own resolution is upscaled by the driver's model, automatically on mains and never in a small picture-in-picture. A Video enhancement setting chooses Off, Auto or On.
- Made enhancement step aside for the rest of a film once the governor has had to take it away, instead of switching on and off.
- Added session restore: if Vetro Look ends unexpectedly while a film is open, the next plain start brings the film back where it was.
- Made picture in picture remember where it was put on each arrangement of screens, settle into a corner when let go near one, and pin or unpin with T.
- Fixed the transport losing its frosted glass whenever a subtitle was on screen: every glass surface now has its own blur, so the bubble no longer takes it from the panel.
- Fixed the player's controls leaving an empty pane of glass behind as they faded: the glass now fades with them, and the panel shrinks into its own base as it goes.
- Fixed the subtitle bubble going blank when the controls were hidden: subtitles are no longer drawn as part of the controls.
- Made messages such as "AI subtitles on" appear above the transport and the subtitles instead of on top of them, on the same frosted glass, growing in as they appear.

## 1.1.0 вЂ” 2026-09-09

- Improved screen-sized decode and scaling paths for JPEG, PNG, TIFF, static WebP, PSD and PSB.
- Added RAW embedded-preview selection by useful dimensions and richer decoder/performance verification.
- Kept the trusted AVIF decode path after rejecting a platform-codec shortcut that did not meet fidelity checks.
- Made all Windows distribution formats carry the Lensfun profile database, README and third-party notices.
- Updated release documentation and package verification information.

## 1.0.1

- Added silent installer support for WinGet.

## 1.0.0

- Initial public release.
