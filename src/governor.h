#pragma once
// Vetro Look, GPL-3.0-or-later.
// The Resource Governor.
//
// It does not answer "is the GPU busy" (§12.1). It answers: will the next frame
// that has to be there arrive on time, and what can be given up so that it does.
// Everything it can give up is secondary by construction -- previews, prefetch,
// indexing, enhancement, transcription, the blur behind the glass -- and
// playback correctness is never the first thing sacrificed (§12.4).
//
// Two properties matter more than the exact thresholds. It must not flap: a tier
// that changes every frame is worse than no tier at all, so degrading needs
// sustained pressure and recovering needs sustained headroom (§12.5). And it must
// be testable without a film, which is why the decision is a pure function of one
// sample struct and the clock.
#include <string>
#include <cstdint>

// Appendix F.2.
// Named for the Governor because the decode pipeline already has tiers of its
// own (pipeline.h): one word, two entirely unrelated ladders.
enum class GovernorTier{
 Emergency=0,     // playback and nothing else
 Constrained=1,   // cached work only, no new background decode
 Balanced=2,      // background work at reduced size
 Headroom=3,      // everything the machine is allowed to do
};

// What the Governor is shown. All deltas are since the previous sample, so the
// caller's polling interval is the only time base involved.
struct GovernorSample{
 double now=0;                 // seconds, monotonic
 double interval=0;            // seconds since the previous sample
 bool playing=false;           // a film is running; otherwise there is no deadline
 bool windowHidden=false;      // minimised or not on screen
 // Frame timing. `deadlineMarginMs` is what is left of the display interval
 // after our own drawing; negative means the interface alone overruns it.
 double deadlineMarginMs=16.0;
 double frameBudgetMs=16.67;
 int lateFrames=0,droppedFrames=0,decoderDrops=0;
 double avSyncError=0;         // absolute, seconds
 double ramPressure=0,vramPressure=0;
 bool onBattery=false,batterySaver=false,throttled=false;
};

// What the rest of the application is allowed to do. Every field is a permission
// the owner of that work asks about; none of them is a command.
struct GovernorPolicy{
 GovernorTier tier=GovernorTier::Headroom;
 bool glass=true;              // frosted panels over the film at all
 float glassBlurScale=1.f;     // how expensive the blur may be
 bool neighbourPrefetch=true;  // decode the photographs either side
 bool backgroundIndexing=true; // walk the library
 bool previewGeneration=true;  // stage 4's timeline previews
 float previewScale=1.f;
 int previewCoalesceMs=16;     // how hard to coalesce preview requests
 bool sceneAnalysis=true;
 bool aiTranscription=true;    // stage 8
 bool enhancement=true;        // stage 9
};

// 0 comfortable .. 1 out of room. Pure, and the only place the inputs of §12.2
// are weighed against each other. `reason`, when asked for, names the signal
// that decided it -- which is the only part of this a person ever reads.
double PressureScore(const GovernorSample& sample,const wchar_t** reason=nullptr);
// The ladder of §12.4, as the tier it belongs to. Pure.
GovernorPolicy PolicyForTier(GovernorTier tier,const GovernorSample& sample);

class Governor{
public:
 void Observe(const GovernorSample& sample);
 void Reset();
 const GovernorPolicy& Policy()const{return policy_;}
 GovernorTier CurrentTier()const{return tier_;}
 double Pressure()const{return pressure_;}
 int Changes()const{return changes_;}        // how often the tier has moved
 std::wstring Explain()const;
private:
 GovernorTier tier_=GovernorTier::Headroom;
 GovernorPolicy policy_;
 double pressure_=0;
 // -1 rather than 0: a sample at time zero is a real sample, and a sentinel
 // that collides with it makes the first second of a session behave differently
 // from every second after it.
 double pressedSince_=-1,restedSince_=-1,changedAt_=-1;
 int changes_=0;
 const wchar_t* because_=L"start";
};

// The application's one Governor. Observed on the window thread; its policy is
// read from anywhere, which is why the read is a plain copy of small values.
Governor& TheGovernor();
GovernorPolicy GovernorNow();
