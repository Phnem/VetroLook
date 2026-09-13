#pragma once
// Vetro Look, GPL-3.0-or-later.
// The web-page resolver (9.5, Appendix T). A separate capability, never part of
// the playback path: it runs out of process, inside a job object that bounds its
// memory and kills it with us, under a timeout, and it is handed one address and
// nothing else -- no cookies, no state, no configuration of its own.
//
// The implementation is yt-dlp, supplied by the user rather than bundled: which
// sites it can read changes weekly and is not Vetro Look's promise to make. It
// is looked for in `resolver\` beside the application, then in
// `%LOCALAPPDATA%\VetroLook\resolver\`, then on PATH.
#include <windows.h>
#include <string>
#include "streaming.h"

struct ResolveOutcome{
 uint64_t generation=0;
 bool ok=false;
 ResolvedStream stream;
 StreamFailure failure=StreamFailure::None;
 std::wstring detail;          // the resolver's own words, for Diagnostics only
 double seconds=0;             // how long it took
};

// Where the resolver is, or empty.
std::wstring ResolverPath();
// Starts resolving `url` on a worker. A request already running is cancelled:
// latest wins. `message` is posted to `notify` when the outcome is ready.
void ResolverStart(const std::wstring& url,uint64_t generation,HWND notify,UINT message);
// Kills the helper, if one is running. Cheap when none is.
void ResolverCancel();
// Takes the finished outcome, if there is one.
bool ResolverCollect(ResolveOutcome& outcome);
bool ResolverBusy();
