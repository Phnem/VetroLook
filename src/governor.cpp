// Vetro Look, GPL-3.0-or-later. See governor.h.
#include "governor.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace{
double Clamp01(double v){return v<0?0:(v>1?1:v);}

// Sustained, not instantaneous (§12.5). Degrading is quicker than recovering on
// purpose: a late frame has already been seen by the viewer, while a tier that
// climbs back too eagerly produces the RTX ON/OFF flapping the plan warns about.
constexpr double Pressed=0.60;      // above this, pressure is accumulating
constexpr double Rested=0.25;       // below this, headroom is accumulating
constexpr double Severe=0.90;
constexpr double DegradeAfter=1.2;  // seconds of pressure before giving something up
constexpr double DegradeFast=0.4;   // ... unless it is already severe
constexpr double RecoverAfter=5.0;  // seconds of headroom before taking it back
constexpr double MinDwell=2.0;      // no two changes closer than this

std::mutex policyMx;
GovernorPolicy published;
}

double PressureScore(const GovernorSample& sample,const wchar_t** reason){
 struct Term{const wchar_t* name;double value;};
 double interval=sample.interval>0.001?sample.interval:0.25;
 // Memory is pressure whether or not anything is playing: it is the one signal
 // that ruins the application rather than merely the picture.
 Term terms[]={
  {L"memory",Clamp01(sample.ramPressure)},
  {L"video memory",Clamp01(sample.vramPressure)},
  {L"frame deadline",0},
  {L"late frames",0},
  {L"dropped frames",0},
  {L"a/v sync",0},
 };
 if(sample.playing){
  double budget=sample.frameBudgetMs>1?sample.frameBudgetMs:16.67;
  // Half the interval left is comfortable; nothing left is not. Below zero our
  // own drawing alone does not fit, which is the definition of no headroom.
  double half=budget*0.5;
  terms[2].value=Clamp01((half-sample.deadlineMarginMs)/half);
  // Two late frames a second is a picture that is visibly not keeping up.
  terms[3].value=Clamp01((double(sample.lateFrames)/interval)/2.0);
  terms[4].value=Clamp01((double(sample.droppedFrames+sample.decoderDrops)/interval)/3.0);
  // 20 ms of drift is inaudible, 80 ms is lips out of step.
  double drift=fabs(sample.avSyncError);
  terms[5].value=Clamp01((drift-0.02)/0.06);
 }
 // The worst signal decides. A governor that averages its inputs is a governor
 // that ignores the one thing actually going wrong.
 const Term* worst=&terms[0];
 for(const auto& term:terms)if(term.value>worst->value)worst=&term;
 double score=worst->value;
 const wchar_t* name=score>0.001?worst->name:L"headroom";
 // A machine holding its clocks down or saving battery has less to give even
 // when nothing has gone wrong yet.
 if(sample.throttled){score+=0.15;if(score>Pressed&&!wcscmp(name,L"headroom"))name=L"thermal ceiling";}
 if(sample.batterySaver)score+=0.10;
 score=Clamp01(score);
 if(reason)*reason=name;
 return score;
}

GovernorPolicy PolicyForTier(GovernorTier tier,const GovernorSample& sample){
 GovernorPolicy policy;
 policy.tier=tier;
 switch(tier){
  case GovernorTier::Headroom:
   break;                                     // everything as built
  case GovernorTier::Balanced:
   policy.glassBlurScale=.75f;
   policy.previewScale=.6f;policy.previewCoalesceMs=33;
   policy.sceneAnalysis=false;
   break;
  case GovernorTier::Constrained:
   // Cached previews only, simplified blur, nothing new started in the
   // background. The glass stays, because it is the shape of the interface --
   // only its cost comes down.
   policy.glassBlurScale=.4f;
   policy.neighbourPrefetch=false;
   policy.backgroundIndexing=false;
   policy.previewGeneration=false;policy.previewScale=.5f;policy.previewCoalesceMs=66;
   policy.sceneAnalysis=false;
   policy.aiTranscription=false;
   policy.enhancement=false;
   break;
  case GovernorTier::Emergency:
   policy.glass=false;policy.glassBlurScale=0;
   policy.neighbourPrefetch=false;
   policy.backgroundIndexing=false;
   policy.previewGeneration=false;policy.previewScale=.5f;policy.previewCoalesceMs=120;
   policy.sceneAnalysis=false;
   policy.aiTranscription=false;
   policy.enhancement=false;
   break;
 }
 // §12.6 and §74. Battery is not pressure -- the machine is coping -- it is a
 // different answer to the same question about what is worth doing.
 if(sample.onBattery){
  policy.enhancement=false;
  policy.sceneAnalysis=false;
 }
 if(sample.batterySaver){
  policy.aiTranscription=false;
  policy.previewScale=(std::min)(policy.previewScale,.5f);
  policy.previewCoalesceMs=(std::max)(policy.previewCoalesceMs,66);
 }
 if(sample.windowHidden){
  // Nothing is being looked at: previews and blur are for a viewer who is
  // watching. Indexing is the exception -- a hidden window on mains is the best
  // moment to walk the library -- unless the battery is paying for it.
  policy.previewGeneration=false;
  policy.glass=false;policy.glassBlurScale=0;
  if(sample.onBattery||sample.batterySaver)policy.backgroundIndexing=false;
 }
 return policy;
}

void Governor::Reset(){
 tier_=GovernorTier::Headroom;policy_=GovernorPolicy{};
 pressure_=0;pressedSince_=restedSince_=changedAt_=-1;changes_=0;because_=L"start";
 std::lock_guard lock(policyMx);published=policy_;
}

void Governor::Observe(const GovernorSample& sample){
 const wchar_t* reason=L"headroom";
 pressure_=PressureScore(sample,&reason);
 if(pressure_>=Pressed){
  if(pressedSince_<0)pressedSince_=sample.now;
  restedSince_=-1;
 }else if(pressure_<=Rested){
  if(restedSince_<0)restedSince_=sample.now;
  pressedSince_=-1;
 }else{
  // The band between the two thresholds is where nothing happens. That gap is
  // the hysteresis: without it the tier would track the noise.
  pressedSince_=restedSince_=-1;
 }
 // Battery saver keeps a ceiling on what the Governor will climb back to, so a
 // laptop on its last ten percent does not end up running every background
 // convenience again the moment a film pauses.
 GovernorTier ceiling=sample.batterySaver?GovernorTier::Balanced:GovernorTier::Headroom;
 bool dwelt=changedAt_<0||sample.now-changedAt_>=MinDwell;
 double needed=pressure_>=Severe?DegradeFast:DegradeAfter;
 if(pressedSince_>=0&&sample.now-pressedSince_>=needed&&dwelt&&tier_>GovernorTier::Emergency){
  tier_=GovernorTier(int(tier_)-1);
  changes_++;changedAt_=sample.now;pressedSince_=sample.now;because_=reason;
 }else if(restedSince_>=0&&sample.now-restedSince_>=RecoverAfter&&dwelt&&tier_<ceiling){
  tier_=GovernorTier(int(tier_)+1);
  changes_++;changedAt_=sample.now;restedSince_=sample.now;because_=L"headroom";
 }
 if(tier_>ceiling){tier_=ceiling;changedAt_=sample.now;because_=L"battery saver";}
 policy_=PolicyForTier(tier_,sample);
 std::lock_guard lock(policyMx);
 published=policy_;
}

std::wstring Governor::Explain()const{
 const wchar_t* names[]={L"Emergency",L"Constrained",L"Balanced",L"Headroom"};
 wchar_t pressure[16];swprintf_s(pressure,L"%.2f",pressure_);
 std::wstring out=L"Governor: ";
 out+=names[int(tier_)];
 out+=L", pressure ";out+=pressure;
 out+=L", ";out+=because_;
 out+=L", ";out+=std::to_wstring(changes_);out+=L" tier changes";
 return out;
}

Governor& TheGovernor(){
 static Governor governor;
 return governor;
}
GovernorPolicy GovernorNow(){
 std::lock_guard lock(policyMx);
 return published;
}
