// Vetro Look, GPL-3.0-or-later. See pacing.h.
#include "pacing.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace{
// The rates §11.2 asks for, as the families they belong to. 23.976 and 24 are
// the same decision; measuring them apart only produces two plans where one
// belongs.
const double standardRates[]={23.976,24,25,29.97,30,48,50,59.94,60,72,75,90,100,119.88,120,144,240};

std::mutex frameMx;
// Four seconds of interface frames at 60 Hz. Long enough that one hitch does not
// own the percentile, short enough that the number still describes now.
constexpr int FrameWindow=240;
double frameSamples[FrameWindow]{};
int frameCount=0,frameNext=0;
double frameBudgetMs=1000.0/60.0;
}

double NominalFrameRate(double measured){
 if(measured<=0||measured>300)return 0;
 for(double rate:standardRates)
  if(fabs(measured-rate)<=0.06)return rate;
 // Not a broadcast rate: keep the measurement. A file really can be 18 fps, and
 // pretending otherwise would pace it against a cadence it does not have.
 return measured;
}

bool CadenceIsInteger(double contentFps,double displayHz){
 if(contentFps<=0||displayHz<=0)return false;
 double ratio=displayHz/contentFps;
 if(ratio<0.98)return false;
 double whole=std::round(ratio);
 // 59.94/23.976 is exactly 2.5, and 60/24 exactly 2.5 as well: the tolerance is
 // there for rounded rates, not to call 2.5 an integer.
 return fabs(ratio-whole)<=0.02;
}

PacingPlan PlanPacing(SyncPolicy policy,const PacingInputs& in){
 PacingPlan plan;
 double content=NominalFrameRate(in.contentFps);
 double display=in.displayHz;
 plan.engine.displayHz=display>0?display:0;
 plan.engine.preferEfficiency=in.onBattery||in.batterySaver||in.throttled;
 if(content>0&&display>0){
  plan.cadence=display/content;
  plan.integerCadence=CadenceIsInteger(content,display);
  // A cadence between whole numbers means some frames are held longer than
  // others -- 24 in 60 is the familiar 3:2 -- and that is what the eye reads as
  // judder. Below one there are not enough display frames to go round at all.
  plan.judder=!plan.integerCadence&&plan.cadence>=1;
 }

 // Nothing to pace against. The audio clock is not a compromise here: it is the
 // only clock there is, and it never drifts from itself.
 if(display<=0||content<=0){
  plan.engine.sync=SyncMode::AudioClock;
  plan.reason=display<=0?L"display rate unknown":L"film rate unknown";
  return plan;
 }
 // Whatever the policy, a machine that is already losing frames is not asked to
 // do more work. §12.3: playback correctness is above every smoothing trick.
 bool losing=in.lateFramesPerMinute>60;
 bool poor=in.constrained||in.batterySaver||in.throttled||!in.hardwareDecode;

 if(policy==SyncPolicy::LowLatency){
  plan.engine.sync=SyncMode::AudioClock;
  plan.reason=L"low latency";
  return plan;
 }
 if(losing){
  plan.engine.sync=SyncMode::AudioClock;
  plan.reason=L"late frames, following audio";
  return plan;
 }
 // Auto gives way to the machine; an explicit Smoothness preference gives way
 // only in the Governor's emergency (§F.4): a preference is not permission to
 // drop frames indefinitely.
 if(poor&&(policy==SyncPolicy::Auto||in.constrained)){
  plan.engine.sync=SyncMode::AudioClock;
  plan.reason=in.batterySaver?L"battery saver":
              in.throttled?L"machine throttled":
              !in.hardwareDecode?L"software decode":L"no headroom";
  return plan;
 }
 // More content frames than the display can show. Dropping on a schedule beats
 // resampling audio for a rate that cannot be reached.
 if(plan.cadence<0.98){
  plan.engine.sync=SyncMode::DisplayDrop;
  plan.reason=L"display slower than film";
  return plan;
 }
 plan.engine.sync=SyncMode::DisplayResample;
 if(!in.hasAudio)plan.reason=L"display cadence, no audio";
 else if(plan.integerCadence)plan.reason=L"exact cadence";
 else plan.reason=L"uneven cadence";
 // Interpolation is the expensive half of smoothness and it only earns its cost
 // against a judder cadence. On battery it is never worth it (§12.6), and in Auto
 // it waits for headroom.
 bool affordable=!in.onBattery&&!in.batterySaver&&!in.throttled&&!in.constrained;
 if(plan.judder&&affordable)
  plan.engine.interpolate=policy==SyncPolicy::Smoothness;
 if(plan.engine.interpolate)plan.reason=L"uneven cadence, interpolated";
 return plan;
}

void PacingSetBudget(double displayHz){
 std::lock_guard lock(frameMx);
 double judged=displayHz>60?60:displayHz;      // see the header
 frameBudgetMs=judged>1?1000.0/judged:1000.0/60.0;
}
double PacingBudgetMs(){
 std::lock_guard lock(frameMx);
 return frameBudgetMs;
}
void PacingRecordUiFrame(double milliseconds){
 if(milliseconds<0)return;
 std::lock_guard lock(frameMx);
 frameSamples[frameNext]=milliseconds;
 frameNext=(frameNext+1)%FrameWindow;
 if(frameCount<FrameWindow)frameCount++;
}
void PacingReset(){
 std::lock_guard lock(frameMx);
 frameCount=0;frameNext=0;
}
FrameStats PacingUiStats(){
 std::vector<double> sorted;
 double budget=0;
 {
  std::lock_guard lock(frameMx);
  sorted.assign(frameSamples,frameSamples+frameCount);
  budget=frameBudgetMs;
 }
 FrameStats stats;
 if(sorted.empty())return stats;
 double total=0;
 for(double value:sorted){total+=value;if(value>budget)stats.overBudget++;}
 std::sort(sorted.begin(),sorted.end());
 auto at=[&](double quantile){
  size_t index=size_t(quantile*double(sorted.size()-1)+0.5);
  return sorted[index<sorted.size()?index:sorted.size()-1];
 };
 stats.samples=int(sorted.size());
 stats.mean=total/double(sorted.size());
 stats.p50=at(0.50);stats.p95=at(0.95);stats.max=sorted.back();
 return stats;
}
double PacingDeadlineMargin(){
 auto stats=PacingUiStats();
 double budget;
 {std::lock_guard lock(frameMx);budget=frameBudgetMs;}
 if(!stats.samples)return budget;
 return budget-stats.p95;
}

std::wstring PacingReport(const PacingPlan& plan){
 auto number=[](double value,const wchar_t* format){
  wchar_t text[32];swprintf_s(text,format,value);return std::wstring(text);
 };
 const wchar_t* mode=plan.engine.sync==SyncMode::AudioClock?L"audio clock":
                     plan.engine.sync==SyncMode::DisplayResample?L"display, audio resampled":
                     L"display, frames dropped";
 auto stats=PacingUiStats();
 std::wstring out=L"Pacing: ";
 out+=mode;
 if(plan.engine.interpolate)out+=L" + interpolation";
 out+=L" (";out+=plan.reason;out+=L")\n";
 if(plan.cadence>0)
  out+=L"Cadence: "+number(plan.cadence,L"%.3f")+L" display frames per film frame"+
       (plan.integerCadence?L", exact\n":plan.judder?L", uneven\n":L"\n");
 if(stats.samples)
  out+=L"Interface frames: p50 "+number(stats.p50,L"%.2f")+L" ms, p95 "+number(stats.p95,L"%.2f")+
       L" ms, max "+number(stats.max,L"%.2f")+L" ms, over budget "+
       std::to_wstring(stats.overBudget)+L" of "+std::to_wstring(stats.samples)+L"\n";
 return out;
}
