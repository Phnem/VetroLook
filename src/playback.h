#pragma once
// Vetro Look, GPL-3.0-or-later.
// The playback engine boundary.
//
// Everything above this line -- Video Mode, the shell, the controls -- speaks in
// positions, durations and commands. Everything below it is one engine's
// business. No type from a media library appears in this header, and nothing
// outside `mpvengine.cpp` includes one: the day a better engine exists, the
// viewer around it does not get rewritten.
//
// The engine is driven from the thread that owns the window. It wakes that
// thread with a posted message when something happened, and `Pump()` is where
// its events are turned into the snapshot the interface reads.
#include <windows.h>
#include <unknwn.h>   // IUnknown: the one type the presentation seam passes
#include <memory>
#include <string>
#include <vector>

enum class SeekMode{
 Fast,    // nearest useful position; what a dragged timeline wants
 Exact,   // decode to the requested timestamp; what a frame or a grab wants
};

// How the engine is asked to decide when a frame is shown. The names are the
// viewer's, not an engine's: what each one means in a particular engine's
// vocabulary is that engine's business.
enum class SyncMode{
 AudioClock,       // frames follow the audio clock; cheapest, and never drifts
 DisplayResample,  // audio is resampled so frames land on the display's cadence
 DisplayDrop,      // frames follow the display, dropping rather than resampling
};
// The pacing decision, as the engine receives it. `displayHz` is the one thing
// the engine cannot find out for itself: in composition mode it has no window,
// so it has no display to ask. See pacing.h for how this is arrived at.
struct PresentationPlan{
 SyncMode sync=SyncMode::AudioClock;
 bool interpolate=false;     // blend across display frames for a judder cadence
 double displayHz=0;         // 0: the engine is told nothing and keeps to audio
 bool preferEfficiency=false;// battery or a throttled machine: spend less
};
// What the output actually is, for colour rather than for timing. Composition
// mode hides the display from the engine just as completely here, so HDR, peak
// luminance and paper white have to be handed over or the picture is graded for
// a display nobody owns.
struct PresentationTarget{
 bool hdr=false,wideGamut=false;
 double maxNits=0,minNits=0,sdrWhiteNits=0;
 double refreshHz=0;
 bool operator==(const PresentationTarget& o)const{
  return hdr==o.hdr&&wideGamut==o.wideGamut&&maxNits==o.maxNits&&
         minNits==o.minNits&&sdrWhiteNits==o.sdrWhiteNits;
 }
};
// Enhancement, as the engine receives it (17). What to turn on is decided in
// enhance.h; the engine is only told the result.
struct EnhancementPlan{
 bool superResolution=false;  // the GPU vendor's video super resolution
 double scale=1;              // output over source, already quantised
 bool operator==(const EnhancementPlan& o)const{return superResolution==o.superResolution&&scale==o.scale;}
};
// How much memory the engine's own queues may hold. Every queue bounded, §14.2.
struct StreamBudget{
 uint64_t forwardBytes=0;    // demuxed and waiting to be decoded
 uint64_t backBytes=0;       // kept behind the playhead for a cheap step back
 double seconds=0;           // how far ahead to read when the source allows it
};

// What a stream needs besides its address: a separate sound stream when a site
// splits them, the two request headers a site may insist on, and the name a
// person would call it. Headers beyond these two are not passed on: anything
// else a resolver asks for is not something the engine should be sending.
struct OpenOptions{
 std::wstring audioUrl,title,userAgent,referrer;
 bool live=false;             // the resolver already knows; the engine is told
 double startAt=-1;           // resume here once open; negative for the start or the live edge
};

// A track as the viewer needs to describe it. `id` is the engine's own handle
// for it and means nothing outside the engine.
struct TrackInfo{
 int64_t id=0;
 std::wstring kind;      // "video", "audio", "sub"
 std::wstring title,language,codec;
 bool selected=false,external=false;
};

// What the interface draws. Read on the window thread, never partially updated:
// the engine publishes a whole snapshot or none.
struct PlaybackSnapshot{
 bool opened=false;          // a file is loaded
 bool hasVideo=false;
 bool paused=true;
 bool seeking=false;
 bool buffering=false;
 bool endReached=false;
 double position=0,duration=0;
 double speed=1;
 double frameRate=0;
 unsigned width=0,height=0;
 double volume=100;
 bool muted=false;
 std::wstring videoCodec,audioCodec,hwdec;
 // The film's own transfer function -- "pq", "hlg", "bt.1886" and so on. It is
 // how a high dynamic range film announces itself, and it decides whether the
 // picture is being shown as graded or tone mapped down to this display.
 std::wstring transfer;
 // The subtitle cue on screen now, in both forms: as the author wrote it, tags
 // and all, and as plain text. The first is what the classifier reads to decide
 // who should draw this cue; the second is what the viewer's own bubble shows.
 std::wstring subtitleAss,subtitleText,subtitleCodec;
 double subtitleStart=0,subtitleEnd=0;
 bool subtitleTrack=false;   // a subtitle track is selected at all
 std::wstring renderer;      // which rung of the render ladder is in use
 std::wstring error;         // human-readable, already worth showing

 // Timing, as the engine sees it, and the numbers the pacing plan is judged by.
 // Drops are counted twice over on purpose: a frame the decoder abandoned and a
 // frame the output showed late are different failures with different answers.
 int64_t droppedFrames=0;    // frames the output never showed
 int64_t decoderDrops=0;     // frames the decoder abandoned before that
 int64_t delayedFrames=0;    // frames the output presented late
 double avSync=0;            // audio ahead of video, in seconds
 double displayFps=0;        // frames per second actually being shown
 double vsyncJitter=0;       // how irregular the presentation interval is
 std::wstring syncMode;      // the pacing plan in force, for Diagnostics
 std::wstring colourTarget;  // the output colour the engine was told about
 // True when the display's colour has changed in a way the engine cannot be
 // retuned for while it is running: the next film picks it up. Stage 3's one
 // deliberate seam, because a colour pipeline is chosen when the output is
 // created and composition mode leaves us no way to recreate it in place.
 bool targetPending=false;

 // Network sources (9, Appendix G). `live` is a source without a finite length;
 // its seekable window is what has been kept since joining, and that window --
 // not an invented duration -- is what the timeline may offer.
 bool network=false;
 bool live=false;
 bool seekable=true;
 double liveStart=0,liveEnd=0;  // the seekable window, in the film's own time
 double cacheAhead=0;           // seconds demuxed ahead of the playhead
 // The engine's last warnings for this source, oldest first. Diagnostics shows
 // them; the failure classifier reads them. Never the message itself.
 std::wstring failureLog;

 // What enhancement is actually running, in words, or empty. `enhancementFailed`
 // is set when the driver refused it: it is then not asked for again this film.
 std::wstring enhancement;
 bool enhancementFailed=false;
};

// A chapter, as the container names it.
struct ChapterInfo{
 double start=0;
 std::wstring title;
};

struct IPlaybackEngine{
 virtual ~IPlaybackEngine()=default;

 // Starts opening `path`. Returns false only when the request could not be
 // made at all; a file that turns out to be unplayable reports through the
 // snapshot's error, because that is what the viewer shows.
 virtual bool Open(const std::wstring& path)=0;
 // A network address, with what a resolver said about it. The engine's own
 // page resolver stays off: resolving is a separate, bounded process (9.5).
 virtual bool OpenStream(const std::wstring& url,const OpenOptions& options)=0;
 virtual void Close()=0;

 virtual void Play()=0;
 virtual void Pause()=0;
 virtual void TogglePause()=0;
 // Absolute position, in seconds.
 virtual void Seek(double seconds,SeekMode mode)=0;
 // Relative, in seconds; negative goes back.
 virtual void SeekBy(double seconds,SeekMode mode)=0;

 // One displayed frame forward or back, from wherever the film is now. Pauses
 // playback first, because a film that keeps running underneath a frame step is
 // not stepping. Backwards is the expensive direction: see 21.3.
 virtual void StepFrame(int direction)=0;

 virtual void SetVolume(double percent)=0;
 virtual void SetMuted(bool muted)=0;
 virtual void SetSpeed(double speed)=0;

 // The surface the engine renders into, in device pixels. The shell owns the
 // window, so the engine is told how large its output should be rather than
 // measuring anything itself.
 virtual void SetSurfaceSize(unsigned width,unsigned height)=0;
 // The colour the engine paints where the film does not reach, so the window's
 // ground stays the window's, not a black rectangle.
 virtual void SetBackground(uint8_t r,uint8_t g,uint8_t b)=0;

 // The engine's presentation content, for the shell's composition tree, or null
 // until the first frame has somewhere to go. Ownership stays with the engine.
 virtual IUnknown* PresentationContent()=0;

 // Drains the engine's events and republishes the snapshot. Called on the
 // window thread after the engine's wake-up message arrives.
 virtual void Pump()=0;
 virtual PlaybackSnapshot Snapshot()const=0;
 virtual std::vector<TrackInfo> Tracks()const=0;
 virtual void SelectTrack(const std::wstring& kind,int64_t id)=0;

 // Audio endpoints as the engine names them. `SetAudioDevice({})` returns to the
 // system default, which is also what happens when the chosen one disappears.
 virtual std::vector<std::pair<std::wstring,std::wstring>> AudioDevices()const=0;
 virtual void SetAudioDevice(const std::wstring& id)=0;

 // ------------------------------------------------------- product -----------
 // Nudging one stream against the other, for a film whose sound or subtitles
 // were mastered slightly out of step. Seconds; positive means later.
 virtual void SetAudioDelay(double seconds)=0;
 virtual void SetSubtitleDelay(double seconds)=0;
 // Chapters as the container lists them, and the one the playhead is inside.
 virtual std::vector<ChapterInfo> Chapters()const=0;
 virtual int CurrentChapter()const=0;
 // Writes the frame on screen, exactly as it is being shown, to `path`. Returns
 // false when the engine could not produce one.
 virtual bool WriteScreenshot(const std::wstring& path,bool withSubtitles)=0;
 // The A-B loop. A negative time clears that end; clearing A clears both.
 virtual void SetLoop(double from,double to)=0;

 // ------------------------------------------------------------ quality ------
 // The three things the shell knows and the engine cannot: when to present, what
 // it is presenting onto, and how much memory it may hold while doing it. All
 // three are cheap to repeat with an unchanged value.
 // Who draws the subtitles. True hands them to the engine's own renderer,
 // which is what authored typesetting and bitmap subtitles need; false keeps
 // the engine silent so the viewer can draw the dialogue itself.
 virtual void SetSubtitleRendering(bool engineDraws)=0;
 // Preferred subtitle and audio languages, in order. A global preference (22.3).
 virtual void SetLanguagePreference(const std::wstring& subtitles,const std::wstring& audio)=0;

 virtual void SetPresentationPlan(const PresentationPlan& plan)=0;
 virtual void SetPresentationTarget(const PresentationTarget& target)=0;
 virtual void SetStreamBudget(const StreamBudget& budget)=0;
 // Cheap to repeat with an unchanged plan; a change rebuilds the filter chain.
 virtual void SetEnhancement(const EnhancementPlan& plan)=0;
};

// Creates the libmpv-backed engine, loading the library on first use. Returns
// null with `error` filled when the library is missing or too old -- Image Mode
// must keep working on a machine where video never does.
//
// The presentation target is a creation parameter rather than only a setter
// because the colour pipeline is fixed when the engine's output is built. An
// engine created for an SDR display cannot be retuned into an HDR one while it
// runs; it reports `targetPending` instead, and the next film gets the new one.
std::unique_ptr<IPlaybackEngine> CreatePlaybackEngine(HWND notify,UINT message,
 const PresentationTarget& target,std::wstring& error);

// True once the engine library has been loaded into this process. Nothing loads
// it but a video open: a photograph must not pay for a video stack.
bool PlaybackEngineLoaded();
// Exclusive access to the audio device: the film's samples reach the hardware
// untouched by the system mixer, at the cost of every other sound on the
// machine falling silent. Off by default for exactly that reason, and read
// before the engine is created because an output cannot be reopened underneath
// a playing film.
void PlaybackSetAudioExclusive(bool exclusive);

// Exclusive access to the audio device: the film's samples reach the hardware
// untouched by the system mixer, at the cost of every other sound on the machine
// falling silent. Off by default for exactly that reason, and read before the
// engine is created because an output cannot be reopened under a playing film.
void PlaybackSetAudioExclusive(bool exclusive);

// Where the engine's own log lines go. Off unless a sink is set, because the
// engine is chatty and the viewer is not a console application; with one set,
// a failure to open a stream can say why rather than only that (Appendix G).
void PlaybackSetLogSink(void(*sink)(const std::wstring& line));

// Loads the library without creating an engine, for the preview decoder and for
// the probes. Returns false with `error` filled when it is missing or too old.
bool PlaybackEnsureLibrary(std::wstring& error);
// The loaded library itself, for the one other part of the viewer that needs a
// decoder: the preview engine (preview.h). Loading policy -- which file, from
// where, and which version is acceptable -- stays here, in one place; a second
// consumer binds the handful of entry points it needs from the module this
// returns. Null until a film has been opened.
HMODULE PlaybackEngineModule();
// The engine's version string for Diagnostics, or empty when it is not loaded.
std::wstring PlaybackEngineVersion();
