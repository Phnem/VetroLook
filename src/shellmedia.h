#pragma once
// Vetro Look, GPL-3.0-or-later.
// What Windows already knows about a media file.
//
// Before a playback engine exists -- and, later, before one has finished opening
// -- the shell can answer the questions the viewer needs to show something
// truthful: how long is this, how large is the picture, and what does its first
// frame look like. Both answers come from the Windows thumbnail cache and the
// property handler for the file, so they cost a fraction of opening a decoder
// and they never pull a video subsystem into the process.
//
// Neither call is for the UI thread. Both reach the shell, and the shell reaches
// disk.
#include "image.h"
#include <cstdint>
#include <memory>
#include <string>

struct MediaFacts{
 bool ready=false;
 double seconds=0;            // 0 when the handler does not report a duration
 unsigned width=0,height=0;   // the stored frame size, before any display scaling
 double frameRate=0;
 unsigned bitrate=0;          // bits per second, total
 unsigned channels=0,sampleRate=0;
 uint64_t bytes=0;
 std::wstring videoCodec,audioCodec,title;
};

// Duration, frame size, codecs. Never throws; an absent property is left at its
// default rather than guessed.
MediaFacts ReadMediaFacts(const std::wstring& path);

// The poster frame, from the shell's thumbnail pipeline, at most `maxEdge` on
// the long side. Returns null when no thumbnail exists: a generic file icon is
// not a poster frame, and pretending otherwise would put a filmstrip tile on
// screen that shows nothing about the film.
std::shared_ptr<Image> ShellPoster(const std::wstring& path,unsigned maxEdge);

// Human-readable duration: `7:32`, `1:04:11`. Empty for a duration of zero.
std::wstring FormatDuration(double seconds);
