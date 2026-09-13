#pragma once
// Vetro Look, GPL-3.0-or-later.
// Frame pacing: when a frame should be shown, and what that costs us.
//
// Decoding a frame quickly is not enough -- it has to be presented at the right
// moment relative to the media clock and to the display (§11). The engine does
// the presenting; this file decides the policy it presents under, and measures
// the one part of the picture the engine does not control: our own interface
// frames, which share the same GPU and the same 16 ms.
//
// `PlanPacing` is pure. A cadence decision that can only be tested by watching a
// film is a decision nobody ever checks, so the whole table is arithmetic on two
// numbers plus the state of the machine.
#include "playback.h"
#include <string>

// §11.3. Most people see only these three.
enum class SyncPolicy{
 Auto,         // the viewer decides from the cadence and the headroom
 Smoothness,   // spend to make motion even
 LowLatency,   // spend nothing; react fast
};

struct PacingInputs{
 double contentFps=0;        // the film's own rate, 0 when not known yet
 double displayHz=0;         // the output's exact rate, 0 when not known
 bool hasAudio=true;         // without audio there is no audio clock to follow
 bool hardwareDecode=true;
 bool onBattery=false,batterySaver=false,throttled=false;
 // What has actually been going wrong, per minute of playback. Sustained, not
 // instantaneous: one late frame when a window was dragged is not a policy.
 double lateFramesPerMinute=0;
 bool constrained=false;     // the Governor says there is no headroom to spend
};

struct PacingPlan{
 PresentationPlan engine;    // what the engine is told
 double cadence=0;           // display frames per content frame
 bool integerCadence=false;  // 24 in 120, say: every frame shown equally long
 bool judder=false;          // 24 in 60: a 3:2 pattern the eye can see
 const wchar_t* reason=L"";  // one short phrase, for Diagnostics
};

// The whole decision, as arithmetic. §11.2's list of rates reduces to this:
// whether the display's cadence divides the content's, and whether there is
// headroom to pay for smoothing it when it does not.
PacingPlan PlanPacing(SyncPolicy policy,const PacingInputs& in);

// A film's rate rounded to the family it belongs to, so 23.976 and 24 do not
// produce two different plans. Returns 0 for an unknown or variable rate.
double NominalFrameRate(double measured);
// True when `displayHz` is a whole multiple of `contentFps`, within the
// tolerance that 59.94-against-23.976 needs.
bool CadenceIsInteger(double contentFps,double displayHz);

// ----------------------------------------------------- our own frame cost ----
// The interface's frames, not the film's. The engine presents the picture on its
// own layer, so our drawing does not hold a film frame back directly -- but it
// shares the same GPU, and chrome that has stopped fitting in a frame is the
// clearest sign this machine has run out of room. Measured, not assumed.
struct FrameStats{
 int samples=0,overBudget=0;
 double p50=0,p95=0,max=0,mean=0;   // milliseconds
};
void PacingRecordUiFrame(double milliseconds);
// The budget the interface's own frames are judged against, from the display's
// rate. Not the same thing as the film's deadline: the film is presented by the
// engine on its own layer, and a 240 Hz display does not make our chrome late.
// What the interface owes a person is sixty frames a second; above that is a
// bonus, and judging our drawing against a 4 ms interval would report pressure
// on the fastest machines in the world.
void PacingSetBudget(double displayHz);
double PacingBudgetMs();
FrameStats PacingUiStats();
// Milliseconds of the display interval still unspent by our own drawing, at the
// 95th percentile. Negative means the interface alone does not fit in a frame.
double PacingDeadlineMargin();
void PacingReset();
std::wstring PacingReport(const PacingPlan& plan);
