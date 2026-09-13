#pragma once
// Vetro Look, GPL-3.0-or-later.
// What the viewer remembers about one film.
//
// Where you stopped watching, which audio track you chose, how far the
// subtitles had to be nudged: all of it belongs to the file rather than to the
// application, and none of it belongs in a per-session variable. The key is the
// media signature from media.h -- path, size and write time -- so a renamed file
// is still the same film and a different file at the same path is not.
//
// The store is small, bounded and forgettable: it is a convenience, and losing
// it must cost nothing but the convenience.
#include <string>
#include <vector>
#include <cstdint>

struct MediaState{
 std::wstring signature;
 double position=0;          // where playback had got to, in seconds
 double duration=0;
 double subtitleDelay=0;     // seconds, positive means the text comes later
 double audioDelay=0;
 int64_t audioTrack=-1;      // the engine's own ids, or -1 for "as it came"
 int64_t subtitleTrack=-1;
 uint64_t seen=0;            // FILETIME of the last time this film was opened
};

// Reads the store from disk once. Cheap afterwards.
const MediaState* MediaStateFind(const std::wstring& signature);
// Records what is known now. Writing is debounced: a film being watched updates
// its position constantly and the disk does not need to hear about all of it.
void MediaStateRemember(const MediaState& state);
// Flushes anything pending. Called when a film closes and at shutdown.
void MediaStateFlush();

// True when a remembered position is worth offering. A film abandoned ten
// seconds in, or watched to the end, has nothing to resume: opening it again
// means starting it again.
bool MediaStateResumable(const MediaState& state);
// Where the store lives, for Diagnostics and for the tests.
std::wstring MediaStatePath();
// Empties the store. Only ever called by an explicit request.
void MediaStateClear();
