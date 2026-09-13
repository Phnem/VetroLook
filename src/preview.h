#pragma once
// Vetro Look, GPL-3.0-or-later.
// The preview engine: frames for the timeline, from anywhere in the film,
// without the playback engine ever being disturbed.
//
// A person dragging the timeline is asking "what is there?", and the only
// honest answer is the frame itself (18.2: no blind seeking). The playing
// engine cannot be the one to answer -- seeking it would stop the film the
// question is about -- so previews come from a decoder of their own that does
// nothing but controlled random access: no audio, no subtitles, no output, no
// hardware decoder to fight over.
//
// Three rules run through everything here:
//   * latest wins. A frame for a position the pointer has already left is
//     dropped rather than drawn (19.3).
//   * positions are quantised. A pointer produces hundreds of distinct
//     timestamps a second and every one of them would be a cache key (H.1).
//   * the pointer is faster than any decoder, so a request coalesces into the
//     newest useful one rather than queueing (19.4).
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One preview frame, in the viewer's own pixel order, already tone mapped down
// to SDR by the decoder that produced it (19.8).
struct PreviewFrame{
 unsigned width=0,height=0;
 double time=0;             // the position this frame actually came from
 bool exact=false;          // decoded for this bucket, rather than a neighbour
 std::vector<uint8_t> bgra; // width*height*4
};

// Opens a preview source for `path`. `signature` identifies the media for the
// cache, `duration` sets how finely positions are quantised. Cheap: the decoder
// itself is not created until the first request, and is dropped again when the
// timeline has been idle.
void PreviewOpen(const std::wstring& path,const std::wstring& signature,double duration,
                 HWND notify,UINT message);
// Leaves the film. Anything in flight becomes stale and is never drawn.
void PreviewClose();
void PreviewStop();        // shutdown, joins the worker

// Asks for the frame at `seconds`. Returns the bucket the request landed in,
// which is what the caller compares against a frame it already has.
int64_t PreviewRequest(double seconds,unsigned maxEdge);
// The best frame for `seconds` that exists now: the exact bucket if it has been
// decoded, otherwise the nearest neighbour within a bucket or two, so a dragged
// pointer shows something immediately and sharpens in place (19.5).
std::shared_ptr<PreviewFrame> PreviewBest(double seconds);
// True when a preview source could be opened at all. False for a film the
// decoder refused, and for a source that cannot be seeked: the timeline then
// shows a timestamp and no picture rather than a fake one (18.2).
bool PreviewAvailable();
// True while a decode for the newest request is still running, so the interface
// can say "working" rather than "nothing there".
bool PreviewBusy();

// Position quantisation (H.1). Pure, and shared with the tests: a six-hour
// recording and a ninety-second clip cannot have the same preview density
// (19.6), and the granularity is what decides both cache size and how often
// the decoder is asked for anything.
double PreviewGranularity(double duration);
int64_t PreviewBucket(double seconds,double granularity);
double PreviewBucketTime(int64_t bucket,double granularity);

// The Resource Governor's permissions, in the terms this engine works in
// (12.4, steps 1 and 2). `scale` shrinks the frames asked for, `coalesceMs`
// is how long a moving pointer is allowed to settle before anything is decoded,
// and `allowed` false stops new decoding entirely -- cached frames still show.
void PreviewSetPolicy(bool allowed,float scale,int coalesceMs);
// What the session cache is allowed to hold.
void PreviewSetMemoryBudget(size_t bytes);
size_t PreviewCacheBytes();
