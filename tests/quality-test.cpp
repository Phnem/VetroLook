// Vetro Look, GPL-3.0-or-later.
// Stage 3: frame pacing, the Resource Governor and the capability matrix.
//
// Every decision under test is a pure function of a struct, which is the whole
// reason those functions are shaped that way. A governor that can only be
// observed by watching a film on one machine is a governor nobody ever checks,
// and the flapping it is built to avoid is exactly the behaviour a person
// watching would not notice for minutes at a time.
//
// The last section probes the machine this runs on and prints what it found. It
// asserts only what must be true of any machine, because the point of a
// capability probe is that the answers differ.
#include "../src/pacing.h"
#include "../src/governor.h"
#include "../src/capability.h"
#include "../src/subtitles.h"
#include "../src/mediastate.h"
#include <windows.h>
#include <iostream>
#include <string>

namespace{
int failures=0;
void Check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;}
void Show(const std::wstring& text){
 int bytes=WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);
 std::string out(bytes>0?bytes-1:0,'\0');
 if(bytes>1)WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,out.data(),bytes,nullptr,nullptr);
 std::cout<<out;
}

PacingInputs Film(double fps,double hz){
 PacingInputs in;in.contentFps=fps;in.displayHz=hz;return in;
}

// A sample that is in perfect health, so each test can spoil exactly one thing.
GovernorSample Healthy(double now){
 GovernorSample sample;
 sample.now=now;sample.interval=0.25;sample.playing=true;
 sample.deadlineMarginMs=14;sample.frameBudgetMs=16.67;
 return sample;
}
// Runs `seconds` of samples through a governor, with one field already spoiled.
void Feed(Governor& governor,GovernorSample sample,double from,double seconds){
 for(double t=from;t<from+seconds;t+=0.25){
  sample.now=t;
  governor.Observe(sample);
 }
}
}

int main(){
 // ------------------------------------------------------------- rates ------
 {
  Check(NominalFrameRate(23.976)==23.976,"a broadcast rate is itself");
  Check(NominalFrameRate(23.98)==23.976,"a measured 23.98 is the 23.976 family");
  Check(NominalFrameRate(29.970029)==29.97,"29.970029 is 29.97");
  Check(NominalFrameRate(0)==0&&NominalFrameRate(-5)==0,"no rate is no rate");
  Check(NominalFrameRate(18)==18,"an unusual rate is kept rather than rounded into a family");

  Check(CadenceIsInteger(24,48),"24 in 48 is exact");
  Check(CadenceIsInteger(23.976,119.88),"23.976 in 119.88 is exact");
  Check(CadenceIsInteger(30,60),"30 in 60 is exact");
  Check(!CadenceIsInteger(24,60),"24 in 60 is not exact: that is the 3:2 pattern");
  Check(!CadenceIsInteger(25,60),"25 in 60 is not exact");
  Check(!CadenceIsInteger(60,50),"60 in 50 has fewer display frames than film frames");
 }

 // ------------------------------------------------------------ pacing ------
 {
  auto plan=PlanPacing(SyncPolicy::Auto,Film(24,120));
  Check(plan.engine.sync==SyncMode::DisplayResample&&plan.integerCadence&&!plan.engine.interpolate,
        "an exact cadence follows the display and needs no interpolation");
  Check(plan.engine.displayHz==120,"the engine is told the display rate it cannot see");

  plan=PlanPacing(SyncPolicy::Auto,Film(24,60));
  Check(plan.judder&&plan.engine.sync==SyncMode::DisplayResample&&!plan.engine.interpolate,
        "Auto follows the display through a 3:2 cadence but does not pay to smooth it");

  plan=PlanPacing(SyncPolicy::Smoothness,Film(24,60));
  Check(plan.engine.interpolate,"Smoothness pays for interpolation where there is judder to smooth");

  plan=PlanPacing(SyncPolicy::Smoothness,Film(24,120));
  Check(!plan.engine.interpolate,"even Smoothness does not interpolate an exact cadence");

  plan=PlanPacing(SyncPolicy::LowLatency,Film(24,60));
  Check(plan.engine.sync==SyncMode::AudioClock&&!plan.engine.interpolate,
        "low latency keeps to the audio clock");

  plan=PlanPacing(SyncPolicy::Auto,Film(0,60));
  Check(plan.engine.sync==SyncMode::AudioClock,"an unknown film rate falls back to the audio clock");
  plan=PlanPacing(SyncPolicy::Auto,Film(24,0));
  Check(plan.engine.sync==SyncMode::AudioClock&&plan.engine.displayHz==0,
        "an unknown display rate falls back to the audio clock and tells the engine nothing");

  plan=PlanPacing(SyncPolicy::Auto,Film(60,50));
  Check(plan.engine.sync==SyncMode::DisplayDrop,
        "more film frames than the display can show drops on a schedule");

  auto late=Film(24,60);late.lateFramesPerMinute=120;
  plan=PlanPacing(SyncPolicy::Smoothness,late);
  Check(plan.engine.sync==SyncMode::AudioClock,
        "frames already arriving late end display sync whatever the preference");

  auto battery=Film(24,60);battery.onBattery=true;
  plan=PlanPacing(SyncPolicy::Smoothness,battery);
  Check(!plan.engine.interpolate&&plan.engine.preferEfficiency,
        "on battery nothing is interpolated and the cheap path is asked for");

  auto saver=Film(24,60);saver.batterySaver=true;
  plan=PlanPacing(SyncPolicy::Auto,saver);
  Check(plan.engine.sync==SyncMode::AudioClock,"battery saver keeps Auto on the audio clock");

  auto software=Film(24,60);software.hardwareDecode=false;
  plan=PlanPacing(SyncPolicy::Auto,software);
  Check(plan.engine.sync==SyncMode::AudioClock,"software decode keeps Auto on the audio clock");

  auto squeezed=Film(24,60);squeezed.constrained=true;squeezed.throttled=true;
  plan=PlanPacing(SyncPolicy::Smoothness,squeezed);
  Check(plan.engine.sync==SyncMode::AudioClock,
        "an explicit preference still gives way in the Governor's emergency");
 }

 // --------------------------------------------------------- our frames -----
 {
  PacingReset();
  PacingSetBudget(60);
  for(int i=0;i<100;i++)PacingRecordUiFrame(4.0);
  for(int i=0;i<10;i++)PacingRecordUiFrame(25.0);
  auto stats=PacingUiStats();
  Check(stats.samples==110&&stats.overBudget==10,"frames over the display's interval are counted");
  Check(stats.p50<5&&stats.max>=25,"one hitch does not own the median");
  Check(PacingDeadlineMargin()<16.67,"the margin is what is left of the interval, not the interval");
  PacingReset();
  Check(PacingUiStats().samples==0,"the window can be emptied between films");
 }

 // ----------------------------------------------------------- pressure -----
 {
  auto healthy=Healthy(0);
  const wchar_t* reason=nullptr;
  Check(PressureScore(healthy,&reason)<0.2,"a healthy sample is not pressure");

  auto overrun=Healthy(0);overrun.deadlineMarginMs=-2;
  Check(PressureScore(overrun)>=0.95,"no margin left is full pressure");

  auto late=Healthy(0);late.lateFrames=1;     // four a second
  Check(PressureScore(late,&reason)>=0.6&&!wcscmp(reason,L"late frames"),
        "late frames are pressure, and are named as the cause");

  auto memory=Healthy(0);memory.ramPressure=0.9;
  Check(PressureScore(memory,&reason)>=0.9&&!wcscmp(reason,L"memory"),"memory is weighed too");

  auto idle=Healthy(0);idle.playing=false;idle.deadlineMarginMs=-5;
  Check(PressureScore(idle)<0.2,"with nothing playing there is no frame deadline to miss");

  auto drift=Healthy(0);drift.avSyncError=0.09;
  Check(PressureScore(drift,&reason)>=0.9&&!wcscmp(reason,L"a/v sync"),"lips out of step is pressure");

  auto warm=Healthy(0);warm.throttled=true;
  Check(PressureScore(warm)>PressureScore(healthy),"a throttled machine has less to give");
 }

 // -------------------------------------------------------------- tiers -----
 {
  GovernorSample sample=Healthy(0);
  auto headroom=PolicyForTier(GovernorTier::Headroom,sample);
  auto balanced=PolicyForTier(GovernorTier::Balanced,sample);
  auto constrained=PolicyForTier(GovernorTier::Constrained,sample);
  auto emergency=PolicyForTier(GovernorTier::Emergency,sample);
  // The ladder of 12.4, in order: coalescing and preview size first, background
  // work next, the blur after that, and the picture never.
  Check(headroom.glass&&headroom.neighbourPrefetch&&headroom.previewGeneration,
        "with headroom everything is allowed");
  Check(balanced.previewScale<headroom.previewScale&&balanced.previewCoalesceMs>headroom.previewCoalesceMs,
        "the first thing given up is preview size and eagerness");
  Check(balanced.glass&&balanced.neighbourPrefetch,"prefetch and glass survive the first step");
  Check(!constrained.previewGeneration&&!constrained.backgroundIndexing&&!constrained.neighbourPrefetch,
        "constrained starts no new background work");
  Check(constrained.glass&&constrained.glassBlurScale<balanced.glassBlurScale,
        "constrained simplifies the blur rather than removing the glass");
  Check(!emergency.glass&&!emergency.enhancement&&!emergency.aiTranscription,
        "in an emergency only playback is left");
  Check(!constrained.enhancement&&!constrained.aiTranscription,
        "enhancement and transcription go before the interface does");

  auto onBattery=Healthy(0);onBattery.onBattery=true;
  Check(!PolicyForTier(GovernorTier::Headroom,onBattery).enhancement,
        "enhancement is never offered on battery, however much headroom there is");
  auto hidden=Healthy(0);hidden.windowHidden=true;
  Check(!PolicyForTier(GovernorTier::Headroom,hidden).previewGeneration&&
        PolicyForTier(GovernorTier::Headroom,hidden).backgroundIndexing,
        "a hidden window stops previews but is a good moment to index");
  auto hiddenBattery=Healthy(0);hiddenBattery.windowHidden=true;hiddenBattery.onBattery=true;
  Check(!PolicyForTier(GovernorTier::Headroom,hiddenBattery).backgroundIndexing,
        "a hidden window on battery stops indexing as well");
 }

 // --------------------------------------------------------- hysteresis -----
 {
  Governor governor;
  governor.Reset();
  Feed(governor,Healthy(0),0,10);
  Check(governor.CurrentTier()==GovernorTier::Headroom&&governor.Changes()==0,
        "a healthy machine is left alone");

  // Pressed, but not severe: a little over the threshold, which is what the
  // 1.2 second wait is for.
  auto pressed=Healthy(0);pressed.deadlineMarginMs=3;
  Feed(governor,pressed,10,0.75);
  Check(governor.CurrentTier()==GovernorTier::Headroom,
        "a moment of pressure changes nothing: that is the hysteresis");
  Feed(governor,pressed,10.75,1.0);
  Check(governor.CurrentTier()==GovernorTier::Balanced&&governor.Changes()==1,
        "sustained pressure gives up one tier, not all of them");

  Feed(governor,pressed,12,6);
  Check(governor.CurrentTier()<GovernorTier::Balanced,"pressure that continues keeps stepping down");
  int stepped=governor.Changes();
  Feed(governor,Healthy(20),20,3);
  Check(governor.Changes()==stepped,"three seconds of calm is not yet a recovery");
  Feed(governor,Healthy(23),23,20);
  Check(governor.CurrentTier()==GovernorTier::Headroom,"sustained headroom takes every tier back");

  // The flapping test: pressure that alternates with calm on a one-second
  // rhythm must not produce a tier change per cycle.
  governor.Reset();
  double now=0;
  for(int cycle=0;cycle<20;cycle++){
   auto spike=Healthy(now);spike.deadlineMarginMs=1;spike.lateFrames=1;
   Feed(governor,spike,now,0.5);now+=0.5;
   Feed(governor,Healthy(now),now,0.5);now+=0.5;
  }
  Check(governor.Changes()<=2,"alternating pressure does not flap the tier");

  governor.Reset();
  auto emergency=Healthy(0);emergency.deadlineMarginMs=-4;emergency.lateFrames=4;
  Feed(governor,emergency,0,0.6);
  Check(governor.CurrentTier()==GovernorTier::Balanced,
        "severe pressure acts inside a second, without skipping a tier");

  governor.Reset();
  auto saver=Healthy(0);saver.batterySaver=true;
  Feed(governor,saver,0,30);
  Check(governor.CurrentTier()<=GovernorTier::Balanced,"battery saver keeps a ceiling on the tier");
 }

 // ------------------------------------------------------- the pipeline -----
 {
  // 13.2's first example: HEVC Main10 HDR on an NVIDIA part with an HDR display.
  Capabilities machine;
  machine.adapter.vendor=L"NVIDIA";machine.adapter.dedicatedVideoMemory=8ull<<30;
  machine.decode.hevc8=machine.decode.hevc10=machine.decode.h264=true;
  machine.display.hdr=true;machine.display.maxNits=1000;
  MediaProfile film;film.codec=L"hevc";film.bitDepth=10;film.hdr=true;
  film.width=3840;film.height=2160;
  PowerFacts mains;
  auto choice=ChoosePipeline(machine,film,mains);
  Check(choice.hardwareDecode&&choice.hdrOutput&&!choice.toneMap&&choice.enhancementCandidate,
        "HDR film, HDR display, capable part: hardware decode, HDR out, enhancement offered");

  PowerFacts battery;battery.onBattery=true;
  Check(!ChoosePipeline(machine,film,battery).enhancementCandidate,
        "the same machine on battery is not an enhancement candidate");

  // The second example: AV1 10-bit on an old integrated part with battery saver.
  Capabilities old;
  old.adapter.vendor=L"Intel";old.adapter.dedicatedVideoMemory=128ull<<20;
  old.adapter.integrated=true;
  old.decode.h264=true;
  MediaProfile av1;av1.codec=L"av1";av1.bitDepth=10;av1.width=3840;av1.height=2160;
  PowerFacts saving;saving.batterySaver=true;
  auto poor=ChoosePipeline(old,av1,saving);
  Check(!poor.hardwareDecode&&!poor.highQualityScaler&&!poor.enhancementCandidate,
        "no AV1 decoder and no power to spare: software decode and the cheap scaler");

  MediaProfile hdrFilm=film;
  Capabilities sdr=machine;sdr.display.hdr=false;
  Check(ChoosePipeline(sdr,hdrFilm,mains).toneMap,"an HDR film on an SDR display is tone mapped");

  DecodeFacts decode;decode.h264=true;decode.hevc8=true;
  Check(DecodeSupported(decode,L"h264",8)&&!DecodeSupported(decode,L"h264",10),
        "ten-bit H.264 is not the same profile as eight-bit");
  Check(DecodeSupported(decode,L"hevc",8)&&!DecodeSupported(decode,L"hevc",10),
        "HEVC Main does not imply Main10");
  Check(!DecodeSupported(decode,L"av1",8),"an absent decoder is absent");
 }

 // ---------------------------------------------------------- subtitles -----
 {
  // The classifier decides who draws a cue, per cue rather than per file (23.2).
  // Raw literals throughout: these are ASS override tags, and every one of them
  // begins with the backslash an ordinary escape sequence would eat.
  Check(ClassifyCue(LR"(Hello there.)")==CueKind::Simple,"plain dialogue is the viewer's own");
  Check(ClassifyCue(LR"({\i1}Hello{\i0} there.)")==CueKind::Simple,
        "italics are styling, not placement: still dialogue");
  Check(ClassifyCue(LR"({\pos(320,120)}SHOP)")==CueKind::ComplexAss,
        "an explicit position is authored typesetting");
  Check(ClassifyCue(LR"({\move(0,0,100,100)}x)")==CueKind::ComplexAss,"so is a move");
  Check(ClassifyCue(LR"({\clip(0,0,50,50)}x)")==CueKind::ComplexAss,"so is a clip");
  Check(ClassifyCue(LR"({\k50}la{\k30}la)")==CueKind::ComplexAss,"karaoke timing is authored");
  Check(ClassifyCue(LR"({\frz30}tilted)")==CueKind::ComplexAss,"a rotation is authored");
  Check(ClassifyCue(LR"({\an8}Overhead)")==CueKind::ComplexAss,
        "a caption pinned to the top of the frame is answering something up there");
  Check(ClassifyCue(LR"({\an2}Bottom centre)")==CueKind::Simple,
        "the ordinary bottom alignment is still dialogue");
  Check(ClassifyCue(L"")==CueKind::None&&ClassifyCue(L"   ")==CueKind::None,"nothing is nothing");
  Check(CodecIsBitmap(L"hdmv_pgs_subtitle")&&CodecIsBitmap(L"dvd_subtitle")&&!CodecIsBitmap(L"ass"),
        "bitmap subtitles are recognised by their codec");

  Check(PlainFromAss(LR"({\i1}Hello{\i0}\Nthere)")==L"Hello\nthere",
        "override blocks go, hard breaks become breaks");
  Check(PlainFromAss(LR"(a\hb)")==L"a b","a hard space is a space");

  // The bubble is a physical object: it holds through the gaps of a conversation
  // and collapses only after a real silence (25.2, 25.3).
  BubbleState state;
  auto open=AdvanceBubble(state,L"First line",0.0,3.0,false);
  Check(open.phase==BubblePhase::Opening&&open.scale<1,"it opens from a point");
  AdvanceBubble(state,L"First line",0.5,3.0,false);
  Check(state.phase==BubblePhase::Reading,"and settles into reading");
  auto morph=AdvanceBubble(state,L"Second line",1.0,3.0,false);
  Check(morph.retarget&&state.phase==BubblePhase::Reading,
        "a new line retargets the same object rather than opening a second one");
  AdvanceBubble(state,L"",1.5,3.0,false);
  Check(state.phase==BubblePhase::Holding,"a gap is a hold, not a disappearance");
  AdvanceBubble(state,L"Third line",2.0,3.0,false);
  Check(state.phase==BubblePhase::Reading,"and the next line comes back from the held shape");
  AdvanceBubble(state,L"",2.5,3.0,false);
  AdvanceBubble(state,L"",4.0,3.0,false);
  Check(state.phase==BubblePhase::Holding,"three seconds of silence have not passed yet");
  AdvanceBubble(state,L"",5.6,3.0,false);
  Check(state.phase==BubblePhase::Collapsing,"after the silence it collapses");
  auto collapsing=AdvanceBubble(state,L"",5.72,3.0,false);
  Check(collapsing.textAlpha<1&&collapsing.scale<1&&collapsing.textAlpha<collapsing.scale,
        "and the text leaves before the shape does");
  auto rescued=AdvanceBubble(state,L"Fourth line",5.75,3.0,false);
  Check(state.phase==BubblePhase::Opening&&rescued.retarget,
        "a line arriving mid-collapse cancels it rather than queueing behind it");
  AdvanceBubble(state,L"",6.0,3.0,false);
  AdvanceBubble(state,L"",9.5,3.0,false);
  AdvanceBubble(state,L"",10.5,3.0,false);
  Check(state.phase==BubblePhase::Hidden&&state.text.empty(),"and eventually it is gone");

  BubbleState still;
  auto reduced=AdvanceBubble(still,L"No motion",0.0,3.0,true);
  Check(reduced.scale==1.f&&reduced.textAlpha==1.f,"Reduce Motion means no animation at all");
 }

 // ------------------------------------------------------------ resume -----
 {
  // What is worth coming back to. The rule is small and entirely about how a
  // person watches: the opening seconds are not a place anyone resumes from,
  // and a film that reached its credits is a film to start again.
  auto film=[](double position,double duration){
   MediaState state;state.signature=L"x";state.position=position;state.duration=duration;
   return state;
  };
  Check(MediaStateResumable(film(600,3600)),"the middle of a film is worth resuming");
  Check(!MediaStateResumable(film(12,3600)),"the first half minute is not");
  Check(!MediaStateResumable(film(3590,3600)),"nor is the last");
  Check(!MediaStateResumable(film(3540,3600)),"nor the closing credits at 98 per cent");
  Check(!MediaStateResumable(film(600,0)),"a film of no known length has nothing to resume");
  Check(MediaStateResumable(film(45,120)),"a short clip follows the same rule");
 }

 // ------------------------------------------------------- this machine -----
 {
  auto display=ProbeDisplay(nullptr);
  auto power=ReadPower();
  auto memory=ReadMemory(nullptr);
  Check(display.refreshHz>20&&display.refreshHz<1000,"the display reports a believable refresh rate");
  Check(display.sdrWhiteNits>0,"paper white always has a value, even if it is the sRGB default");
  Check(memory.totalRam>0&&memory.availableRam>0,"the machine reports its memory");
  Check(memory.ramPressure>=0&&memory.ramPressure<=1,"memory pressure is a fraction");
  Check(power.clockHeadroom>0&&power.clockHeadroom<=1,"the clock ceiling is a fraction of the maximum");
  Capabilities here;
  here.display=display;
  Show(L"\n"+CapabilityReport(here,power,memory));
  auto plan=PlanPacing(SyncPolicy::Auto,Film(23.976,display.refreshHz));
  Show(PacingReport(plan)+L"\n");
 }

 std::cout<<(failures?"FAILURES: ":"all passed: ")<<failures<<"\n";
 return failures?1:0;
}
