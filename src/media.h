#pragma once
// Vetro Look, GPL-3.0-or-later.
// Media routing: what kind of thing does this path hold, and therefore which
// presentation mode owns it.
//
// The router is deliberately separate from every decoder. A photograph and a
// film are two presentation modes of one viewer, so the decision of which mode
// to enter must not be made by whichever subsystem happens to try opening the
// file first. It is also not made by the extension alone: a `.mp4` that is
// really a Matroska stream, or a `.dat` that is really an MP4, should open.
#include <cstdint>
#include <string>
#include <vector>

// What a path holds, in the terms the shell needs to choose a mode.
//
// AnimatedImage is a distinct kind even though Stage 1 routes it to Image Mode:
// the classification is a property of the file, not of what the viewer can
// currently do with it.
enum class MediaKind{Unsupported,Image,AnimatedImage,Video,Audio};

// Where the bytes come from. A URL is classified from its shape alone -- the
// router never reaches out to the network to decide which mode to enter.
enum class MediaOrigin{LocalFile,Url};

struct MediaRoute{
 MediaKind kind=MediaKind::Unsupported;
 MediaOrigin origin=MediaOrigin::LocalFile;
 // True when the signature was read, rather than the name trusted.
 bool probed=false;
 // True when the extension claimed one kind and the bytes proved another. The
 // bytes win; this flag exists so Diagnostics can say why.
 bool mislabelled=false;
 // Container or codec family, for Diagnostics. Never shown as an error.
 std::wstring container;
 bool IsVideo()const{return kind==MediaKind::Video;}
 bool IsImage()const{return kind==MediaKind::Image||kind==MediaKind::AnimatedImage;}
 bool Playable()const{return kind==MediaKind::Video||kind==MediaKind::Audio;}
 bool IsUrl()const{return origin==MediaOrigin::Url;}
};

// The extension hint. Cheap, synchronous, wrong for mislabelled files, and the
// only thing available for a path that does not exist yet.
MediaKind KindFromExtension(const std::wstring& path);

// Signature classification over a header. `size` may be short; every probe
// checks what it needs before reading. This is the whole probe logic, kept free
// of file and COM dependencies so it can be tested against synthetic headers.
MediaRoute RouteForBytes(const uint8_t* bytes,size_t size);

// Extension hint, then a bounded header read, then the signature's answer where
// it has one. Reads at most HeaderProbeBytes and never blocks on more.
MediaRoute RouteForPath(const std::wstring& path);

// True for something that is an address rather than a path. Shape only: this is
// asked on the window thread, before anything is opened, and it must not touch
// the network or the disk.
bool LooksLikeUrl(const std::wstring& text);

// URL shape only: scheme, path extension, and the manifest extensions that mark
// a stream. No request is made.
MediaRoute RouteForUrl(const std::wstring& url);

// How much of a file the probe is allowed to read. Enough for an Ogg codec
// identification header and a Matroska DocType, small enough to be one read.
constexpr size_t HeaderProbeBytes=4096;

// The canonical extension lists. `Supported()` in decoders.cpp is the image
// decode gate and defers to the first of these, so the two cannot drift.
bool ImageExtensionSupported(const std::wstring& extensionWithDot);
// The same lists, enumerated. The shell registration needs to walk them, and a
// second hand-written copy of the extensions is a copy that drifts -- which is
// exactly how a viewer ends up registered for a format it cannot open, or
// silently not offered for one it can.
std::vector<std::wstring> ExtensionsFor(MediaKind kind);
bool VideoExtensionSupported(const std::wstring& extensionWithDot);
bool AudioExtensionSupported(const std::wstring& extensionWithDot);

// Anything the unified viewer will show or play: the set that folder navigation
// and the filmstrip walk through. Image-only callers keep using `Supported()`.
bool SupportedMedia(const std::wstring& path);

// A stable identity for per-file state: normalised path, size and write time.
// Two files with the same signature are the same media item as far as resume
// position, track selection and preview cache are concerned.
std::wstring MediaSignature(const std::wstring& path);
