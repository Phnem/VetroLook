// Vetro Look, GPL-3.0-or-later.
// Video Mode. See videomode.h.
#include "videomode.h"
#include "preview.h"
#include "subtitles.h"
#include "mediastate.h"
#include "streaming.h"
#include "ai.h"
#include <shlobj.h>
#include <cwctype>
#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
using Microsoft::WRL::ComPtr;

namespace{

// A poster is a first look, not a master: a frame large enough to fill a 4K
// window would cost more to fetch from the shell than the engine will cost to
// show the real one.
constexpr unsigned PosterEdge=1024;

struct Request{std::wstring path;uint64_t generation=0;};
struct Result{uint64_t generation=0;MediaRoute route;MediaFacts facts;std::shared_ptr<Image> poster;};

std::mutex mx;
std::condition_variable cv;
std::thread worker;
bool running=false,stopping=false;
Request pending;                       // latest-wins: one slot, not a queue
bool hasPending=false;
Result finished;
bool hasFinished=false;
std::atomic<uint64_t> liveGeneration{0};
HWND notifyWindow=nullptr;UINT notifyMessage=0;

// The surface's own state. Written only from the thread that owns the window.
std::wstring surfacePath;
MediaRoute surfaceRoute;
MediaFacts surfaceFacts;
std::shared_ptr<Image> surfacePoster;
ComPtr<ID2D1Bitmap> posterTexture;
uint64_t surfaceGeneration=0;

// The engine, and what the shell has been told about it.
std::unique_ptr<IPlaybackEngine> engine;
// What the shell has decided about quality. Kept here rather than in the engine
// because it outlives any one engine: the decisions are the viewer's, and an
// engine created later must start out under them.
struct Quality{
 PresentationTarget target;
 PresentationPlan plan;
 StreamBudget budget;
 EnhancementPlan enhancement;
};
Quality quality;
std::wstring engineError;
bool engineTried=false,contentAttached=false;
PlaybackSnapshot snapshot;
unsigned surfaceWidth=0,surfaceHeight=0;

// The transport controls.
//
// Geometry is laid out during paint and read by the hit tests, the way the rest
// of the interface works. The values come from Apple's Human Interface
// Guidelines and from the measured macOS control anatomy: a capsule panel, a
// circular lift plate under the primary action, concentric radii, 44-point touch
// targets, and press feedback that scales rather than flashes.
enum Control{
 CtrlNone,CtrlPlay,CtrlBack,CtrlForward,CtrlVolume,CtrlVolumeTrack,CtrlExpand,
 CtrlTrack,CtrlSettings,CtrlAudio,CtrlSubtitles,CtrlPip,CtrlCount
};
struct Hit{Control id=CtrlNone;D2D1_RECT_F rect{};};
D2D1_RECT_F barRect{},trackRect{},volumeTrackRect{};
Hit hits[CtrlCount];int hitCount=0;
Control hovered=CtrlNone,pressed=CtrlNone;
Spring hoverLift[CtrlCount],pressLift[CtrlCount];
bool springsReady=false;
bool scrubbing=false,volumeDragging=false;
// Where the pointer is asking about, and the card that answers. The timeline
// never moves the film blindly (18.2): while the pointer is over it, what is
// under the cursor is decoded and shown above it.
Spring previewReveal{0,PanelK,PanelC};
double previewTime=0;               // the position the card is answering for
float previewX=0;                   // where on the track the pointer is
bool previewWanted=false;
D2D1_RECT_F previewRect{};
std::shared_ptr<PreviewFrame> previewFrame;
ComPtr<ID2D1Bitmap> previewTexture;
const PreviewFrame* previewTextureOf=nullptr;
bool previewOpen=false;
// Every commit gets one. Work started for an older position -- a preview, a
// prefetch -- has no right to come back as current (20.2).
uint64_t seekGeneration=0;

// The Vetro Bubble. One piece of glass that morphs between lines and collapses
// into a point after a real silence (25); its geometry is sprung so a new line
// retargets the shape rather than replacing it.
BubbleState bubble;
Spring bubbleWidth{0,PanelK,PanelC},bubbleHeight{0,PanelK,PanelC};
float bubbleScale=1,bubbleTextAlpha=1;
std::wstring bubbleText;
D2D1_RECT_F bubbleRect{};
bool bubbleGeometryReady=false;
// Who is drawing the subtitles at this moment. Decided per cue, because one ASS
// track carries plain dialogue and authored signs together (23.2).
CueKind cueKind=CueKind::None;
bool engineDrawsSubtitles=false;
double subtitleCollapseAfter=3.0;     // 25.7
std::wstring subtitleLanguages,audioLanguages;

// What the viewer remembers about this film (mediastate.h): where it had got
// to, which tracks were chosen, how far the delays were nudged. Applied once,
// when the engine has told us enough about the film to apply it.
MediaState state;
bool stateApplied=false,resumeOffered=false;
double resumedTo=0;
// The A-B loop, in the film's own time. Negative means unset.
double loopFrom=-1,loopTo=-1;
// A network source (Appendix G). What the resolver said about it travels with
// every reopen, and the reconnect state is the viewer's, not the engine's: the
// engine retries a dropped connection inside one load; this retries the load.
OpenOptions streamOptions;
struct Network{
 int attempts=0;               // since the stream last played
 int reconnects=0;             // ones that worked, for Diagnostics
 bool waiting=false;           // a retry is scheduled
 bool restoring=false;         // a retry is in flight
 bool everPlayed=false,gaveUp=false,wasLive=false;
 double retryAt=0,lastPosition=0;
 StreamFailure failure=StreamFailure::None;
 std::wstring lastError;
 // The stall watchdog: when the engine last gave up on the line, and where the
 // playhead stood at that moment.
 std::wstring logSeen;
 double giveUpAt=0,giveUpPosition=0;
};
Network net;
std::wstring streamNotice;         // one message for the shell to show, taken once
// Live is shown relative to its window (G.1): the timeline starts where the
// kept buffer starts, so the bar is the DVR range and not a sliver at the end
// of a clock that began before the viewer joined. Every seek from the timeline
// adds this back.
double timelineBase=0;
// AI subtitles and silence skip (27, 28). Subtitles are asked for per film and
// forgotten with it: generating costs power, and tomorrow's film should not
// start doing it unasked. Silence skip is a setting the shell hands over.
bool aiSubtitles=false,aiOpened=false;
SilenceSkip silenceSkip=SilenceSkip::Off;
double lastSilenceSkip=0;
int silenceSkips=0;double silenceSkipped=0;   // for Diagnostics
// Defined with the rest of the state handling, below the mode's own lifecycle,
// and needed by it: leaving a film is the last chance to write its position.
void RememberState();
double scrubTarget=0;
double lastScrubSeek=0;
void (*expandToggle)()=nullptr;
bool (*expandState)()=nullptr;
void (*pipToggle)()=nullptr;
bool (*pipState)()=nullptr;

// The two extra controls are useful only when they lead somewhere. Their card
// stays attached to the transport, so it reads as one physical object instead
// of a second, unrelated window on top of the film.
enum Popup{PopupNone,PopupSpeed,PopupAudio,PopupSubtitles};
Popup popup=PopupNone;
// Keep the outgoing kind until its spring is actually gone. Otherwise the
// compositor can leave a frame of clear blur without its tinted material.
Popup closingPopup=PopupNone;
Spring popupReveal{0,PanelK,PanelC};
struct PopupHit{D2D1_RECT_F rect{};double speed=0;std::wstring kind;int64_t id=0;};
std::vector<PopupHit> popupHits;
int popupHovered=-1;
double videoZoom=1.0;

Popup VisiblePopup(){return popup!=PopupNone?popup:closingPopup;}
void ClosePopup(){if(popup!=PopupNone)closingPopup=popup;popup=PopupNone;popupHovered=-1;popupReveal.To(0.f);}
void TogglePopup(Popup next){if(popup==next){ClosePopup();return;}popup=next;closingPopup=PopupNone;popupHovered=-1;popupReveal.To(1.f);}

void Add(Control id,D2D1_RECT_F rect){
 if(hitCount<int(sizeof hits/sizeof hits[0]))hits[hitCount++]={id,rect};
}
bool Inside(const D2D1_RECT_F& r,float x,float y){
 return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom;
}
Control At(float x,float y){
 for(int i=0;i<hitCount;i++)if(Inside(hits[i].rect,x,y))return hits[i].id;
 return CtrlNone;
}
bool InPopup(float x,float y){
 for(const auto& hit:popupHits)if(Inside(hit.rect,x,y))return true;
 return false;
}

void Run(){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 while(true){
  Request request;
  {
   std::unique_lock lock(mx);
   cv.wait(lock,[]{return stopping||hasPending;});
   if(stopping)break;
   request=pending;hasPending=false;
  }
  // Two shell calls, in the order the surface needs them: the facts are what
  // the caption shows, the poster is what fills the window. Between them the
  // generation is checked again, because the thumbnail is the slower of the two
  // and a user walking through a folder will have moved on.
  Result result;
  result.generation=request.generation;
  // The probe the window thread deliberately did not do. It names the container
  // for the caption, and it is how a still photograph wearing a film's
  // extension gets sent back to Image Mode.
  // An address is not a file: the shell has no facts or poster for it, and
  // asking would put a network request behind a property handler.
  if(LooksLikeUrl(request.path)){
   result.route=RouteForUrl(request.path);
  }else{
   result.route=RouteForPath(request.path);
   result.facts=ReadMediaFacts(request.path);
   if(liveGeneration.load()==request.generation)
    result.poster=ShellPoster(request.path,PosterEdge);
  }
  {
   std::lock_guard lock(mx);
   if(stopping)break;
   finished=std::move(result);hasFinished=true;
  }
  if(notifyWindow)PostMessageW(notifyWindow,notifyMessage,0,0);
 }
 CoUninitialize();
}

std::wstring Join(const std::vector<std::wstring>& parts,const wchar_t* separator){
 std::wstring out;
 for(const auto& part:parts){
  if(part.empty())continue;
  if(!out.empty())out+=separator;
  out+=part;
 }
 return out;
}
// The caption: what this file is, in the order a viewer reads it. Anything the
// property handler did not report is simply absent -- no zeros, no "unknown".
std::wstring Caption(){
 std::vector<std::wstring> parts;
 if(!surfaceRoute.container.empty())parts.push_back(surfaceRoute.container);
 unsigned width=snapshot.width?snapshot.width:surfaceFacts.width;
 unsigned height=snapshot.height?snapshot.height:surfaceFacts.height;
 if(width&&height)parts.push_back(std::to_wstring(width)+L"×"+std::to_wstring(height));
 double rate=snapshot.frameRate>0.1?snapshot.frameRate:surfaceFacts.frameRate;
 if(rate>0.1){
  wchar_t buffer[32];
  swprintf_s(buffer,rate<100?L"%.3g fps":L"%.0f fps",rate);
  parts.push_back(buffer);
 }
 auto duration=FormatDuration(snapshot.duration>0?snapshot.duration:surfaceFacts.seconds);
 if(!duration.empty())parts.push_back(duration);
 return Join(parts,L"  ·  ");
}
// Aspect-correct fit, centred, never upscaled far past the poster's own pixels:
// a 320-pixel thumbnail stretched across a 4K window is worse than a small sharp
// one, and the real frame is seconds away.
D2D1_RECT_F FitPoster(D2D1_RECT_F viewport){
 float vw=viewport.right-viewport.left,vh=viewport.bottom-viewport.top;
 float pw=float(surfacePoster->w),ph=float(surfacePoster->h);
 if(pw<=0||ph<=0||vw<=0||vh<=0)return viewport;
 float scale=(std::min)((std::min)(vw/pw,vh/ph),2.f);
 float w=pw*scale,h=ph*scale;
 float cx=(viewport.left+viewport.right)/2,cy=(viewport.top+viewport.bottom)/2;
 return D2D1::RectF(cx-w/2,cy-h/2,cx+w/2,cy+h/2);
}

double Now(){return double(GetTickCount64())/1000.0;}
double Clamp01(double v){return v<0?0:(v>1?1:v);}
// The card's frame is 168 DIP wide, so 336 device pixels covers a 2x display
// without asking the decoder for more than will ever be shown.
unsigned PreviewEdge(){return 336;}
// The pointer's question, and the request that answers it. Requests are
// quantised and coalesced inside the preview engine, so this may be called on
// every mouse move without anything being decoded twice.
void UpdatePreview(float x,bool wanted);

std::wstring Clock(double seconds){
 auto text=FormatDuration(seconds);
 return text.empty()?L"0:00":text;
}
void UpdatePreview(float x,bool wanted){
 // Live has no finite film to preview, and a second connection to the same
 // broadcast would only compete with the first for the line.
 bool usable=wanted&&snapshot.duration>0&&!snapshot.live&&trackRect.right>trackRect.left+8;
 previewWanted=usable;
 if(!usable){previewReveal.To(0);return;}
 float span=(std::max)(1.f,trackRect.right-trackRect.left);
 double fraction=Clamp01(double((x-trackRect.left)/span));
 previewTime=fraction*snapshot.duration;
 previewX=(std::max)(trackRect.left,(std::min)(trackRect.right,x));
 PreviewRequest(previewTime,PreviewEdge());
 previewReveal.To(1);
}

// The engine is created on the first film and kept afterwards. Creating one
// costs a library load and a device; doing that per file would put a hitch on
// every step through a folder of clips.
bool EnsureEngine(){
 if(engine)return true;
 if(engineTried)return false;
 engineTried=true;
 // The colour of the output is a creation parameter, so whatever the shell has
 // already told us about the display is used here rather than applied after the
 // fact. See playback.h for why that one cannot wait.
 engine=CreatePlaybackEngine(notifyWindow,notifyMessage,quality.target,engineError);
 if(engine){
  engine->SetPresentationPlan(quality.plan);
  if(quality.budget.forwardBytes)engine->SetStreamBudget(quality.budget);
  engine->SetEnhancement(quality.enhancement);
 }
 return engine!=nullptr;
}
void AttachContent(){
 if(!engine||contentAttached)return;
 IUnknown* content=engine->PresentationContent();
 if(!content)return;
 if(GfxSetVideoContent(content))contentAttached=true;
}
void DetachContent(){
 if(!contentAttached)return;
 GfxSetVideoContent(nullptr);
 contentAttached=false;
}

}

void VideoModeEnter(const std::wstring& path,const MediaRoute& route,uint64_t generation,
                    HWND notify,UINT message){
 surfacePath=path;surfaceRoute=route;surfaceGeneration=generation;
 state=MediaState{};stateApplied=false;resumeOffered=false;resumedTo=0;
 loopFrom=loopTo=-1;
 if(aiOpened)AiClose();
 aiOpened=false;aiSubtitles=false;
 if(!route.IsUrl()){
  // A URL has no signature worth keeping: the same address can be a different
  // film tomorrow, and the file it points at is not ours to stat.
  auto signature=MediaSignature(path);
  if(const MediaState* remembered=MediaStateFind(signature))state=*remembered;
  state.signature=signature;
 }
 surfaceFacts=MediaFacts{};
 surfacePoster.reset();posterTexture.Reset();
 snapshot=PlaybackSnapshot{};
 scrubbing=volumeDragging=false;
 hovered=pressed=CtrlNone;hitCount=0;
 barRect=trackRect=volumeTrackRect=D2D1::RectF(0,0,0,0);
 popup=closingPopup=PopupNone;popupHovered=-1;popupReveal.Reset(0);popupHits.clear();
 videoZoom=1.0;
 RememberState();
 MediaStateFlush();
 bubble=BubbleState{};bubbleText.clear();bubbleGeometryReady=false;
 bubbleRect=D2D1::RectF(0,0,0,0);cueKind=CueKind::None;engineDrawsSubtitles=false;
 previewReveal.Reset(0);previewWanted=false;previewFrame.reset();
 previewTexture.Reset();previewTextureOf=nullptr;
 if(previewOpen){PreviewClose();previewOpen=false;}
 liveGeneration.store(generation);
 {
  std::lock_guard lock(mx);
  notifyWindow=notify;notifyMessage=message;
  pending=Request{path,generation};hasPending=true;
  hasFinished=false;
  if(!running&&!stopping){running=true;worker=std::thread(Run);}
 }
 cv.notify_one();
 // The picture from the previous film must not be left on screen underneath the
 // new one's poster while this one opens.
 DetachContent();
 if(EnsureEngine()){
  if(surfaceWidth&&surfaceHeight)engine->SetSurfaceSize(surfaceWidth,surfaceHeight);
  engine->SetVideoZoom(videoZoom);
  if(route.IsUrl())engine->OpenStream(path,streamOptions);
  else engine->Open(path);
 }
}
void VideoModeSetStreamOptions(const OpenOptions& options){
 streamOptions=options;
 net=Network{};streamNotice.clear();
}

bool VideoModeCollect(uint64_t generation){
 Result result;
 {
  std::lock_guard lock(mx);
  if(!hasFinished)return false;
  result=std::move(finished);hasFinished=false;
 }
 // A result for a film the viewer has left is dropped here rather than
 // anywhere closer to the screen. This is the only place it can be dropped
 // cheaply, and the only place where both generations are known.
 if(result.generation!=generation||result.generation!=surfaceGeneration)return false;
 surfaceFacts=result.facts;
 if(result.route.kind!=MediaKind::Unsupported)surfaceRoute=result.route;
 if(result.poster){surfacePoster=result.poster;posterTexture.Reset();}
 return true;
}

void VideoModeLeave(){
 liveGeneration.store(0);
 DetachContent();
 if(engine)engine->Close();
 snapshot=PlaybackSnapshot{};
 surfacePath.clear();
 surfaceRoute=MediaRoute{};
 surfaceFacts=MediaFacts{};
 surfacePoster.reset();
 posterTexture.Reset();
 surfaceGeneration=0;
 scrubbing=volumeDragging=false;
 hovered=pressed=CtrlNone;hitCount=0;
 barRect=trackRect=volumeTrackRect=D2D1::RectF(0,0,0,0);
 popup=closingPopup=PopupNone;popupHovered=-1;popupReveal.Reset(0);popupHits.clear();
 videoZoom=1.0;
 streamOptions=OpenOptions{};net=Network{};streamNotice.clear();
 if(aiOpened)AiClose();
 aiOpened=false;aiSubtitles=false;
 std::lock_guard lock(mx);
 hasPending=false;hasFinished=false;
}

void VideoModeReleaseTextures(){posterTexture.Reset();previewTexture.Reset();previewTextureOf=nullptr;}

void VideoModeStop(){
 PreviewStop();
 DetachContent();
 engine.reset();
 {
  std::lock_guard lock(mx);
  if(!running){stopping=true;return;}
  stopping=true;
 }
 cv.notify_all();
 if(worker.joinable())worker.join();
 running=false;
}

namespace{
// Which renderer owns the cue on screen right now, and the plain text of it
// when the answer is "ours". Runs once per pump rather than per frame: the cue
// changes a few times a minute.
void ClassifyCurrentCue(){
 if(!engine)return;
 // Generated subtitles take the bubble while they are on. They are dialogue by
 // construction, never typesetting, and the film's own track is not drawn
 // underneath them.
 if(aiSubtitles){
  AiCue cue;
  std::wstring text;
  if(AiCueAt(snapshot.position,cue)){
   text=cue.text;
   size_t first=0;
   while(first<text.size()&&iswspace(text[first]))first++;
   text.erase(0,first);
   while(!text.empty()&&iswspace(text.back()))text.pop_back();
  }
  cueKind=text.empty()?CueKind::None:CueKind::Simple;
  bubbleText=text;
  if(engineDrawsSubtitles){engine->SetSubtitleRendering(false);engineDrawsSubtitles=false;}
  return;
 }
 if(!snapshot.subtitleTrack){
  cueKind=CueKind::None;bubbleText.clear();
  if(engineDrawsSubtitles){engine->SetSubtitleRendering(false);engineDrawsSubtitles=false;}
  return;
 }
 if(CodecIsBitmap(snapshot.subtitleCodec)){
  // A picture has no text to lay out again, so it goes to the renderer that
  // knows where the author put it (26.2).
  cueKind=CueKind::Bitmap;bubbleText.clear();
  if(!engineDrawsSubtitles){engine->SetSubtitleRendering(true);engineDrawsSubtitles=true;}
  return;
 }
 const std::wstring& source=snapshot.subtitleAss.empty()?snapshot.subtitleText:snapshot.subtitleAss;
 cueKind=ClassifyCue(source);
 bool authored=cueKind==CueKind::ComplexAss;
 if(authored!=engineDrawsSubtitles){
  engine->SetSubtitleRendering(authored);
  engineDrawsSubtitles=authored;
 }
 bubbleText=authored?std::wstring():PlainFromAss(source.empty()?snapshot.subtitleText:source);
}

// Everything the viewer remembered about this film, applied at the first moment
// the engine knows enough for it to mean anything. Once, and never again for
// this film: a second application would fight whatever the viewer has since
// chosen for themselves.
void ApplyRememberedState(){
 if(stateApplied||!engine||snapshot.duration<=0)return;
 stateApplied=true;
 if(state.signature.empty())return;
 if(state.audioTrack>=0)engine->SelectTrack(L"audio",state.audioTrack);
 if(state.subtitleTrack>=0)engine->SelectTrack(L"sub",state.subtitleTrack);
 if(state.audioDelay!=0)engine->SetAudioDelay(state.audioDelay);
 if(state.subtitleDelay!=0)engine->SetSubtitleDelay(state.subtitleDelay);
 if(MediaStateResumable(state)){
  // Exact, because this is the frame the viewer is going to look at, and it is
  // the one they left off on (20.1).
  engine->Seek(state.position,SeekMode::Exact);
  resumedTo=state.position;resumeOffered=true;
 }
}

void RememberState(){
 if(state.signature.empty()||snapshot.duration<=0)return;
 state.position=snapshot.position;
 state.duration=snapshot.duration;
 MediaStateRemember(state);
}

// Appendix G, the part that happens after a stream has been opened: a failure is
// classified from what the engine said, and either retried with backoff or
// reported in a sentence. Runs on every pump, before the timeline is derived.
void WatchNetwork(){
 if(!surfaceRoute.IsUrl()||!engine)return;
 // Two ways a dropped line ends a stream: short of its length, or -- when a read
 // past what is held fails -- with the playhead parked at the very end, which is
 // indistinguishable from finishing unless the engine's log says the line went.
 bool lineDropped=snapshot.endReached&&ClassifyEngineFailure(snapshot.failureLog)==StreamFailure::Network;
 bool premature=snapshot.endReached&&snapshot.opened&&
                (lineDropped||EndedPrematurely(snapshot.live,snapshot.position,snapshot.duration));
 // The third way, and the quiet one: the engine's last word is that it gave up
 // on the line, nothing is buffered, and the film has not played on since.
 // Marked when the give-up is first said, and cleared only by the film playing
 // on (below): the engine keeps talking after it gives up, and a later line
 // about something else does not mean the line came back.
 if(snapshot.failureLog!=net.logSeen){
  net.logSeen=snapshot.failureLog;
  if(EngineGaveUpOnNetwork(snapshot.failureLog)&&net.giveUpAt<=0){
   net.giveUpAt=Now();
   net.giveUpPosition=snapshot.position;
   // The last place the film really was. Taken now, before a failed read parks
   // the playhead at the end and before any seek starts from that fiction.
   bool parked=!snapshot.live&&snapshot.duration>0&&snapshot.position>=snapshot.duration-0.5;
   if(!parked&&snapshot.position>0.5)net.lastPosition=snapshot.position;
  }
 }
 // Data came back on its own: the film has played on a little from where it
 // stopped, with something buffered. A jump is not playing on -- a failed read
 // can park the playhead at the end in one step.
 if(net.giveUpAt>0&&snapshot.cacheAhead>1.0&&
    snapshot.position>net.giveUpPosition+1.0&&snapshot.position<net.giveUpPosition+30.0)
  net.giveUpAt=0;
 bool stalled=net.giveUpAt>0&&Now()-net.giveUpAt>2.5&&snapshot.cacheAhead<1.0;
 bool failed=!snapshot.error.empty()||premature||stalled;
 // Only a playhead with data under it is a position worth coming back to: a
 // failed read can park it at the very end, and that is not where the viewer was.
 bool parkedAtEnd=snapshot.network&&!snapshot.live&&snapshot.duration>0&&
                  snapshot.position>=snapshot.duration-0.5;
 bool playingWithData=snapshot.opened&&snapshot.position>0.5&&!snapshot.endReached&&!parkedAtEnd&&
                      !snapshot.seeking&&!snapshot.buffering&&(snapshot.cacheAhead>0.5||!snapshot.network);
 if(!failed&&playingWithData&&net.giveUpAt<=0){
  if(net.restoring){net.restoring=false;net.reconnects++;streamNotice=T(S_Reconnected);}
  net.attempts=0;net.everPlayed=true;net.gaveUp=false;
  net.lastPosition=snapshot.position;net.wasLive=snapshot.live;
 }
 if(!failed||net.waiting||net.gaveUp)return;
 net.lastError=stalled?std::wstring(L"the engine gave up on the connection"):
               premature?std::wstring(L"the stream ended before its end"):snapshot.error;
 if(stalled&&(net.failure==StreamFailure::None||net.failure==StreamFailure::Unknown))net.failure=StreamFailure::Network;
 net.giveUpAt=0;
 net.failure=ClassifyEngineFailure(snapshot.failureLog+L"\n"+snapshot.error);
 if(premature&&(net.failure==StreamFailure::Unknown||net.failure==StreamFailure::None))
  net.failure=StreamFailure::Network;
 if(net.failure==StreamFailure::None)net.failure=StreamFailure::Unknown;
 // An unexplained failure after a stream has played is treated as the line; one
 // before it ever played is treated as the address, and not hammered.
 bool retry=FailureRetryable(net.failure)||(net.everPlayed&&net.failure==StreamFailure::Unknown);
 int allowed=net.everPlayed?ReconnectAttempts:FirstConnectAttempts;
 if(retry&&net.attempts<allowed){
  net.waiting=true;
  net.retryAt=Now()+ReconnectDelay(net.attempts);
  net.attempts++;
  streamNotice=T(net.everPlayed?S_Reconnecting:S_WaitingNetwork);
 }else{
  net.gaveUp=true;net.restoring=false;
 }
}

long long SelectedAudioTrack(){
 if(!engine)return 0;
 for(const auto& track:engine->Tracks())if(track.kind==L"audio"&&track.selected)return track.id;
 return 0;
}
// The AI session follows the film: opened the first time anything is wanted and
// the length is known, told where the playhead is on every pump, and asked where
// the next silence ends when silence skip is on. Local films only -- a stream's
// audio is not ours to fetch a second time.
void SyncAi(){
 bool local=!surfaceRoute.IsUrl()&&!surfacePath.empty()&&snapshot.duration>0&&!snapshot.live;
 if(!local)return;
 bool wanted=aiSubtitles||silenceSkip!=SilenceSkip::Off;
 if(wanted&&!aiOpened){
  aiOpened=true;
  AiOpen(surfacePath,MediaSignature(surfacePath),snapshot.duration,SelectedAudioTrack(),notifyWindow,notifyMessage);
 }
 if(!aiOpened)return;
 AiSetWanted(aiSubtitles,silenceSkip!=SilenceSkip::Off);
 AiSetPlayhead(snapshot.position);
 if(silenceSkip!=SilenceSkip::Off&&engine&&!snapshot.paused&&!snapshot.seeking&&!scrubbing&&
    Now()-lastSilenceSkip>1.0){
  double target=AiSilenceTarget(snapshot.position,silenceSkip);
  if(target>snapshot.position+0.3){
   lastSilenceSkip=Now();
   silenceSkips++;silenceSkipped+=target-snapshot.position;
   seekGeneration++;
   engine->Seek(target,SeekMode::Fast);
  }
 }
}

}

bool VideoModeNetworkTick(){
 // A stalled engine sends no wake-ups, so a network source is pumped on this
 // clock as well: otherwise the watchdog above would never get to look.
 if(engine&&surfaceRoute.IsUrl()&&!net.waiting)VideoModePump();
 if(!net.waiting||!engine||Now()<net.retryAt)return false;
 net.waiting=false;net.restoring=true;
 // A live stream comes back at the live edge; a film comes back where it was.
 // Either way, nothing buffered before the drop is replayed (Appendix G).
 auto options=streamOptions;
 options.startAt=(net.wasLive||!net.everPlayed)?-1:net.lastPosition;
 DetachContent();
 engine->OpenStream(surfacePath,options);
 return true;
}
bool VideoModeTakeStreamNotice(std::wstring& text){
 if(streamNotice.empty())return false;
 text=std::move(streamNotice);streamNotice.clear();
 return true;
}
std::wstring VideoModeFailureText(StreamFailure failure){
 switch(failure){
  case StreamFailure::NotFound:return T(S_StreamNotFound);
  case StreamFailure::Forbidden:return T(S_StreamForbidden);
  case StreamFailure::Certificate:return T(S_StreamCertificate);
  case StreamFailure::Protected:return T(S_StreamDrm);
  case StreamFailure::SignInRequired:return T(S_SignIn);
  case StreamFailure::NoPublicStream:return T(S_NoPublicStream);
  case StreamFailure::ResolverMissing:return T(S_NoResolver);
  default:return T(S_StreamFailed);
 }
}
bool VideoModeJumpToLive(){
 if(!engine||!snapshot.live||snapshot.liveEnd<=0)return false;
 seekGeneration++;
 previewFrame.reset();previewTextureOf=nullptr;
 engine->Seek((std::max)(0.0,snapshot.liveEnd-1.0)+timelineBase,SeekMode::Fast);
 return true;
}
std::wstring VideoModeNetworkReport(){
 if(!surfaceRoute.IsUrl()||!engine)return {};
 wchar_t line[320];
 std::wstring out;
 // The engine's own numbers, not the timeline's: Diagnostics is where the
 // window's real position in the stream is worth seeing.
 const PlaybackSnapshot raw=engine->Snapshot();
 swprintf_s(line,L"Network: %ls%ls, %.1f s read ahead%ls%ls%ls\n",snapshot.live?L"live":L"on demand",
            snapshot.seekable?L"":L", not seekable",snapshot.cacheAhead,
            raw.endReached?L", ended":L"",raw.seeking?L", seeking":L"",raw.buffering?L", buffering":L"");
 out+=line;
 swprintf_s(line,L"%ls: %.1f to %.1f s (%.0f s kept), engine length %.1f s, position %.1f s\n",
            raw.live?L"Live window":L"Seekable range",raw.liveStart,raw.liveEnd,
            (std::max)(0.0,raw.liveEnd-raw.liveStart),raw.duration,raw.position);
 out+=line;
 if(!streamOptions.title.empty())out+=L"Resolved: "+streamOptions.title+
  (streamOptions.audioUrl.empty()?L"":L", separate audio stream")+L"\n";
 swprintf_s(line,L"Watchdog: waiting %d, restoring %d, attempts %d, gave up %d, played %d, give-up seen %ls, at %.1f s, resume at %.1f s\n",
            int(net.waiting),int(net.restoring),net.attempts,int(net.gaveUp),int(net.everPlayed),
            net.giveUpAt>0?std::to_wstring(int(Now()-net.giveUpAt)).append(L" s ago").c_str():L"no",
            net.giveUpPosition,net.lastPosition);
 out+=line;
 out+=L"Reconnects: "+std::to_wstring(net.reconnects);
 if(net.waiting){
  swprintf_s(line,L", attempt %d in %.0f s",net.attempts,(std::max)(0.0,net.retryAt-Now()));
  out+=line;
 }
 out+=L"\n";
 if(net.failure!=StreamFailure::None)
  out+=std::wstring(L"Last failure: ")+StreamFailureName(net.failure)+L" ("+net.lastError+L")\n";
 if(!snapshot.failureLog.empty())out+=L"Engine said:\n"+snapshot.failureLog;
 return out;
}

bool VideoModeToggleAiSubtitles(){
 aiSubtitles=!aiSubtitles;
 if(aiSubtitles)SyncAi();
 else{
  bubbleText.clear();cueKind=CueKind::None;
  if(aiOpened)AiSetWanted(false,silenceSkip!=SilenceSkip::Off);
 }
 return aiSubtitles;
}
bool VideoModeAiSubtitles(){return aiSubtitles;}
bool VideoModeAiAvailableHere(){return !surfaceRoute.IsUrl()&&!surfacePath.empty();}
void VideoModeSetSilenceSkip(SilenceSkip mode){
 silenceSkip=mode;
 if(aiOpened)AiSetWanted(aiSubtitles,silenceSkip!=SilenceSkip::Off);
}
SilenceSkip VideoModeSilenceSkip(){return silenceSkip;}
std::wstring VideoModeAiExport(){
 if(!VideoModeAiAvailableHere())return {};
 std::filesystem::path film(surfacePath);
 auto status=AiStatusNow();
 // movie.ru.ai.srt: found by every player's own "subtitles beside the film"
 // search, and marked as generated so it is never mistaken for a human track.
 std::wstring spoken=status.language.empty()?L"und":status.language;
 auto target=film.parent_path()/(film.stem().wstring()+L"."+spoken+L".ai.srt");
 return AiExport(target.wstring(),false)?target.wstring():std::wstring();
}
std::wstring VideoModeAiReport(){
 if(!AiRuntimePresent()&&AiStatusNow().state!=AiState::Downloading)return L"AI subtitles: not downloaded\n";
 auto s=AiStatusNow();
 const wchar_t* states[]={L"no runtime",L"model not downloaded",L"downloading",L"idle",
                          L"loading the model",L"working",L"held by the governor",L"failed"};
 wchar_t line[400];
 swprintf_s(line,L"AI: %ls, subtitles %ls, silence skip %ls; runtime %ls, model %ls%ls%ls\n",
            states[int(s.state)],aiSubtitles?L"on":L"off",
            silenceSkip==SilenceSkip::Off?L"off":silenceSkip==SilenceSkip::Gentle?L"gentle":L"aggressive",
            s.runtimeVersion.empty()?L"not loaded":s.runtimeVersion.c_str(),s.modelName.c_str(),
            s.device.empty()?L"":L", ",s.device.c_str());
 std::wstring out=line;
 if(aiOpened){
  swprintf_s(line,L"AI coverage: %.0f s transcribed, %.0f s listened to, %d cues, language %ls%ls; last chunk %.0f s of audio in %.2f s\n",
             s.covered,s.analysed,s.cues,s.language.empty()?L"not yet known":s.language.c_str(),
             s.fromCache?L", from cache":L"",s.lastChunkSeconds,s.lastChunkWall);
  out+=line;
 }
 if(silenceSkip!=SilenceSkip::Off||silenceSkips){
  swprintf_s(line,L"Silence skip: %d jumps, %.1f s skipped\n",silenceSkips,silenceSkipped);
  out+=line;
 }
 if(s.state==AiState::Downloading){swprintf_s(line,L"AI model download: %.0f%%\n",s.downloadFraction*100);out+=line;}
 if(!s.error.empty())out+=L"AI error: "+s.error+L"\n";
 return out;
}

void VideoModePump(){
 if(!engine)return;
 engine->Pump();
 snapshot=engine->Snapshot();
 WatchNetwork();
 // Live has no length to pretend to (G.1). The timeline is the window kept since
 // joining, and the label says LIVE.
 timelineBase=0;
 if(snapshot.live){
  double start=snapshot.liveEnd>snapshot.liveStart?snapshot.liveStart:snapshot.position;
  double end=(std::max)(snapshot.liveEnd,snapshot.position);
  timelineBase=start;
  snapshot.position=(std::max)(0.0,snapshot.position-start);
  snapshot.duration=(std::max)(0.0,end-start);
  snapshot.liveStart=0;snapshot.liveEnd=snapshot.duration;
 }
 ClassifyCurrentCue();
 ApplyRememberedState();
 RememberState();
 AttachContent();
 // The preview source can only be opened once the film's length is known: the
 // length is what decides how finely the timeline is divided (19.6).
 if(!previewOpen&&snapshot.duration>0&&!snapshot.live&&!surfacePath.empty()){
  previewOpen=true;
  PreviewOpen(surfacePath,MediaSignature(surfacePath),snapshot.duration,notifyWindow,notifyMessage);
 }
 SyncAi();
}
void VideoModeSurface(unsigned widthPixels,unsigned heightPixels,float cornerRadiusPixels){
 surfaceWidth=widthPixels;surfaceHeight=heightPixels;
 GfxSetVideoCorners(cornerRadiusPixels);
 if(engine)engine->SetSurfaceSize(widthPixels,heightPixels);
}
void VideoModeBackground(uint8_t r,uint8_t g,uint8_t b){
 if(engine)engine->SetBackground(r,g,b);
}
void VideoModeTogglePlay(){
 if(!engine)return;
 // At the end of a film, the play button starts it again rather than doing
 // nothing: that is what a viewer means by pressing play on a finished file.
 if(snapshot.endReached&&snapshot.duration>0)engine->Seek(0,SeekMode::Fast);
 engine->TogglePause();
}
void VideoModeSeekBy(double seconds){
 if(!engine)return;
 // The commit sequence of 20.3, in the order it is written there: the
 // generation moves first so that anything already in flight is stale before
 // the engine is touched, and the preview the pointer was looking at is
 // released rather than drawn over the new position.
 seekGeneration++;
 previewFrame.reset();previewTextureOf=nullptr;
 // Where the viewer asked to be is where a stream that drops during this seek
 // comes back to -- not wherever the failed read left the playhead.
 // Not while a drop is pending: the playhead a seek would start from is where a
 // failed read left it, not where the film was.
 if(surfaceRoute.IsUrl()&&!snapshot.live&&net.giveUpAt<=0&&!net.waiting&&!net.restoring){
  double target=snapshot.position+seconds;
  if(snapshot.duration>0)target=(std::min)(target,snapshot.duration);
  net.lastPosition=(std::max)(0.0,target);
 }
 engine->SeekBy(seconds,SeekMode::Fast);
}
// One displayed frame at a time. The film pauses itself: stepping while it
// plays is not stepping, and mpv steps in real presentation order, so this is
// correct for variable frame rate as well (21.5).
void VideoModeStepFrame(int direction){
 if(!engine||!snapshot.hasVideo)return;
 seekGeneration++;
 previewFrame.reset();previewTextureOf=nullptr;
 engine->StepFrame(direction);
}
uint64_t VideoModeSeekGeneration(){return seekGeneration;}

double VideoModeSubtitleDelay(){return state.subtitleDelay;}
double VideoModeAudioDelay(){return state.audioDelay;}
void VideoModeAdjustSubtitleDelay(double delta){
 if(!engine)return;
 state.subtitleDelay=delta==0?0:state.subtitleDelay+delta;
 engine->SetSubtitleDelay(state.subtitleDelay);
 RememberState();
}
void VideoModeAdjustAudioDelay(double delta){
 if(!engine)return;
 state.audioDelay=delta==0?0:state.audioDelay+delta;
 engine->SetAudioDelay(state.audioDelay);
 RememberState();
}

bool VideoModeJumpChapter(int direction,std::wstring& title){
 if(!engine)return false;
 auto chapters=engine->Chapters();
 if(chapters.size()<2)return false;
 double now=snapshot.position;
 size_t target=0;
 if(direction>0){
  while(target<chapters.size()&&chapters[target].start<=now+0.4)target++;
  if(target>=chapters.size())return false;
 }else{
  // Back goes to the start of this chapter first, the way every other player
  // does it, and only then to the one before.
  size_t current=0;
  while(current+1<chapters.size()&&chapters[current+1].start<=now+0.4)current++;
  bool nearStart=now-chapters[current].start<3.0;
  if(nearStart&&current==0)return false;
  target=nearStart?current-1:current;
 }
 seekGeneration++;
 engine->Seek(chapters[target].start,SeekMode::Exact);
 title=chapters[target].title;
 if(title.empty())title=std::to_wstring(target+1);
 return true;
}
size_t VideoModeChapterCount(){
 if(!engine)return 0;
 return engine->Chapters().size();
}

std::wstring VideoModeScreenshot(){
 if(!engine||!snapshot.hasVideo)return {};
 PWSTR pictures=nullptr;
 std::wstring folder;
 if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures,0,nullptr,&pictures))&&pictures){
  folder=std::wstring(pictures)+L"\\Vetro Look";
  CoTaskMemFree(pictures);
 }
 if(folder.empty())return {};
 std::error_code ec;
 std::filesystem::create_directories(folder,ec);
 auto stem=std::filesystem::path(surfacePath).stem().wstring();
 if(stem.empty())stem=L"frame";
 // The position, not the wall clock: a screenshot is of a moment in the film,
 // and two of them from the same scene should sort next to each other.
 int total=int(snapshot.position);
 wchar_t stamp[32];
 swprintf_s(stamp,L"%02d.%02d.%02d",total/3600,(total/60)%60,total%60);
 auto path=folder+L"\\"+stem+L" "+stamp+L".png";
 // With the engine drawing the subtitles, they are part of the picture; with
 // the viewer drawing them, they are not, and a screenshot of the film is the
 // film.
 return engine->WriteScreenshot(path,engineDrawsSubtitles)?path:std::wstring();
}

int VideoModeToggleLoopPoint(){
 // A loop needs two fixed points; a live window slides out from under them.
 if(!engine||snapshot.duration<=0||snapshot.live)return 0;
 if(loopFrom<0){
  loopFrom=snapshot.position;loopTo=-1;
  engine->SetLoop(loopFrom,-1);
  return 1;
 }
 if(loopTo<0&&snapshot.position>loopFrom+0.2){
  loopTo=snapshot.position;
  engine->SetLoop(loopFrom,loopTo);
  return 2;
 }
 loopFrom=loopTo=-1;
 engine->SetLoop(-1,-1);
 return 0;
}
bool VideoModeLoopActive(){return loopFrom>=0&&loopTo>=0;}
bool VideoModeLoop(double& from,double& to){
 from=loopFrom;to=loopTo;
 return loopFrom>=0;
}
bool VideoModeResumed(double& position){
 if(!resumeOffered)return false;
 resumeOffered=false;position=resumedTo;
 return true;
}
bool VideoModeShowingVideo(){return contentAttached&&snapshot.hasVideo;}
const PlaybackSnapshot& VideoModeSnapshot(){return snapshot;}

bool VideoModeOverControls(float x,float y){
 return (barRect.right>barRect.left&&Inside(barRect,x,y))||InPopup(x,y);
}
bool VideoModePointerDown(float x,float y){
 if(!engine||barRect.right<=barRect.left)return false;
 // Popup rows are real playback actions, not ornamental menu items. Clicking
 // the current rate still gives a tactile close, while stream rows select the
 // engine track by its opaque id.
 for(const auto& item:popupHits)if(Inside(item.rect,x,y)){
  if(item.speed>0){engine->SetSpeed(item.speed);snapshot.speed=item.speed;}
  else if(item.kind==L"ai"){
   // Without a model there is nothing to turn on; the shell's toast says where
   // to get one, so the row asks the same question the key does.
   if(AiInstalled())VideoModeToggleAiSubtitles();
   else if(notifyWindow)PostMessageW(notifyWindow,WM_KEYDOWN,'A',0);
  }
  else if(!item.kind.empty()){
   engine->SelectTrack(item.kind,item.id);
   // Chosen by hand, so it is this film's choice from now on (22.3).
   if(item.kind==L"audio")state.audioTrack=item.id;
   else if(item.kind==L"sub")state.subtitleTrack=item.id;
   RememberState();
  }
  ClosePopup();return true;
 }
 auto control=At(x,y);
 pressed=control;
 switch(control){
  case CtrlPlay:VideoModeTogglePlay();return true;
  case CtrlBack:VideoModeSeekBy(-10);return true;
  case CtrlForward:VideoModeSeekBy(10);return true;
  case CtrlVolume:VideoModeToggleMute();return true;
  case CtrlExpand:if(expandToggle)expandToggle();return true;
  case CtrlPip:if(pipToggle)pipToggle();return true;
  case CtrlSettings:TogglePopup(PopupSpeed);return true;
  case CtrlAudio:TogglePopup(PopupAudio);return true;
  case CtrlSubtitles:TogglePopup(PopupSubtitles);return true;
  case CtrlVolumeTrack:volumeDragging=true;VideoModePointerMove(x,y);return true;
  case CtrlTrack:
   if(snapshot.duration<=0)return true;
   scrubbing=true;VideoModePointerMove(x,y);return true;
  default:break;
 }
 return VideoModeOverControls(x,y);
}
void VideoModePointerMove(float x,float y){
 hovered=At(x,y);
 popupHovered=-1;
 for(size_t i=0;i<popupHits.size();i++)if(Inside(popupHits[i].rect,x,y)){popupHovered=int(i);break;}
 // Hovering the timeline is a question, and it is answered whether or not the
 // pointer is held down.
 UpdatePreview(x,hovered==CtrlTrack||scrubbing);
 if(volumeDragging&&engine){
  float span=(std::max)(1.f,volumeTrackRect.right-volumeTrackRect.left);
  double level=Clamp01(double((x-volumeTrackRect.left)/span))*100.0;
  engine->SetMuted(false);
  engine->SetVolume(level);
  snapshot.volume=level;snapshot.muted=false;
  return;
 }
 if(!scrubbing||!engine||snapshot.duration<=0)return;
 float span=(std::max)(1.f,trackRect.right-trackRect.left);
 double fraction=Clamp01(double((x-trackRect.left)/span));
 scrubTarget=fraction*snapshot.duration;
 // While the pointer is down the engine is asked for the cheap seek, and not on
 // every mouse movement: the timeline follows the pointer, the decoder does not
 // have to. The exact position is committed on release.
 if(Now()-lastScrubSeek>0.08){
  lastScrubSeek=Now();
  engine->Seek(scrubTarget+timelineBase,SeekMode::Fast);
 }
}
void VideoModePointerUp(float x,float y){
 pressed=CtrlNone;
 volumeDragging=false;
 if(!scrubbing){UpdatePreview(x,hovered==CtrlTrack);return;}
 scrubbing=false;
 if(engine&&snapshot.duration>0){
  // Commit (20.3): the generation first, then the exact seek. Exact is right
  // here and nowhere else in a drag -- this is the position the viewer chose,
  // and it is the one frame they will actually look at.
  seekGeneration++;
  if(surfaceRoute.IsUrl()&&!snapshot.live)net.lastPosition=scrubTarget;
  engine->Seek(scrubTarget+timelineBase,SeekMode::Exact);
 }
 UpdatePreview(x,hovered==CtrlTrack);
 (void)y;
}

bool VideoModeStep(float seconds){
 if(!springsReady)return false;
 bool moving=popupReveal.Step(seconds);
 if(popup==PopupNone&&popupReveal.v<=.004f&&!popupReveal.Moving()){
  closingPopup=PopupNone;popupHits.clear();
 }
 moving|=previewReveal.Step(seconds);
 // The bubble's own clock. Its phase is decided from the cue and the silence
 // since the last one; its geometry follows in springs, so a retarget bends the
 // shape it already has instead of starting a new one (25.5).
 auto advance=AdvanceBubble(bubble,bubbleText,Now(),subtitleCollapseAfter,reducedMotion);
 bubbleScale=advance.scale;bubbleTextAlpha=advance.textAlpha;
 if(advance.phase!=BubblePhase::Hidden)moving=true;
 moving|=bubbleWidth.Step(seconds);
 moving|=bubbleHeight.Step(seconds);
 // While the pointer rests on the timeline and nothing is being decoded, the
 // buckets on either side are prepared. It is the cheapest kind of prefetch:
 // it only ever runs when the decoder is idle and the viewer is already
 // looking at the answer for where they are.
 if(previewWanted&&!PreviewBusy()&&snapshot.duration>0){
  auto exact=PreviewBest(previewTime);
  if(exact&&exact->exact){
   double granularity=PreviewGranularity(snapshot.duration);
   double ahead=previewTime+granularity,behind=previewTime-granularity;
   if(ahead<snapshot.duration)PreviewRequest(ahead,PreviewEdge());
   else if(behind>0)PreviewRequest(behind,PreviewEdge());
  }
 }
 for(int i=0;i<CtrlCount;i++){
  hoverLift[i].To(hovered==Control(i)?1.f:0.f);
  pressLift[i].To(pressed==Control(i)?1.f:0.f);
  moving|=hoverLift[i].Step(seconds);
  moving|=pressLift[i].Step(seconds);
 }
 return moving;
}

namespace{
// Everything below is the transport's own drawing vocabulary. Over a film the
// palette is fixed rather than themed: the backdrop is the picture, not the
// window, so light and dark chrome would both be read against the same thing.
constexpr float PanelRadius=34.f,PanelHeight=126.f,PanelMargin=22.f;
constexpr float BlurRadius=22.f;                  // device pixels, Gaussian sigma
D2D1_COLOR_F White(float a){return D2D1::ColorF(1,1,1,a);}

// Phosphor Icons, Regular weight (MIT). These are the upstream SVG paths in
// their native 256-unit viewbox, rendered through PhosphorIcon below. Keeping
// the transport in one family gives its small controls a consistent, rounded
// optical weight.
const wchar_t* PhPlay=L"M232.4,114.49,88.32,26.35a16,16,0,0,0-16.2-.3A15.86,15.86,0,0,0,64,39.87V216.13A15.94,15.94,0,0,0,80,232a16.07,16.07,0,0,0,8.36-2.35L232.4,141.51a15.81,15.81,0,0,0,0-27ZM80,215.94V40l143.83,88Z";
const wchar_t* PhPause=L"M200,32H160a16,16,0,0,0-16,16V208a16,16,0,0,0,16,16h40a16,16,0,0,0,16-16V48A16,16,0,0,0,200,32Zm0,176H160V48h40ZM96,32H56A16,16,0,0,0,40,48V208a16,16,0,0,0,16,16H96a16,16,0,0,0,16-16V48A16,16,0,0,0,96,32Zm0,176H56V48H96Z";
const wchar_t* PhBack=L"M199.81,34a16,16,0,0,0-16.24.43L64,109.23V40a8,8,0,0,0-16,0V216a8,8,0,0,0,16,0V146.77l119.57,74.78A15.95,15.95,0,0,0,208,208.12V47.88A15.86,15.86,0,0,0,199.81,34ZM192,208,64.16,128,192,48.07Z";
const wchar_t* PhForward=L"M200,32a8,8,0,0,0-8,8v69.23L72.43,34.45A15.95,15.95,0,0,0,48,47.88V208.12a16,16,0,0,0,24.43,13.43L192,146.77V216a8,8,0,0,0,16,0V40A8,8,0,0,0,200,32ZM64,207.93V48.05l127.84,80Z";
const wchar_t* PhVolume=L"M155.51,24.81a8,8,0,0,0-8.42.88L77.25,80H32A16,16,0,0,0,16,96v64a16,16,0,0,0,16,16H77.25l69.84,54.31A8,8,0,0,0,160,224V32A8,8,0,0,0,155.51,24.81ZM32,96H72v64H32ZM144,207.64,88,164.09V91.91l56-43.55Zm54-106.08a40,40,0,0,1,0,52.88,8,8,0,0,1-12-10.58,24,24,0,0,0,0-31.72,8,8,0,0,1,12-10.58ZM248,128a79.9,79.9,0,0,1-20.37,53.34,8,8,0,0,1-11.92-10.67,64,64,0,0,0,0-85.33,8,8,0,1,1,11.92-10.67A79.83,79.83,0,0,1,248,128Z";
const wchar_t* PhMuted=L"M155.51,24.81a8,8,0,0,0-8.42.88L77.25,80H32A16,16,0,0,0,16,96v64a16,16,0,0,0,16,16H77.25l69.84,54.31A8,8,0,0,0,160,224V32A8,8,0,0,0,155.51,24.81ZM32,96H72v64H32ZM144,207.64,88,164.09V91.91l56-43.55Zm101.66-61.3a8,8,0,0,1-11.32,11.32L216,139.31l-18.34,18.35a8,8,0,0,1-11.32-11.32L204.69,128l-18.35-18.34a8,8,0,0,1,11.32-11.32L216,116.69l18.34-18.35a8,8,0,0,1,11.32,11.32L227.31,128Z";
const wchar_t* PhTracks=L"M80,64a8,8,0,0,1,8-8H216a8,8,0,0,1,0,16H88A8,8,0,0,1,80,64Zm136,56H88a8,8,0,0,0,0,16H216a8,8,0,0,0,0-16Zm0,64H88a8,8,0,0,0,0,16H216a8,8,0,0,0,0-16ZM44,52A12,12,0,1,0,56,64,12,12,0,0,0,44,52Zm0,64a12,12,0,1,0,12,12A12,12,0,0,0,44,116Zm0,64a12,12,0,1,0,12,12A12,12,0,0,0,44,180Z";
const wchar_t* PhSubtitles=L"M48,48H208A16,16,0,0,1,224,64V176A16,16,0,0,1,208,192H48A16,16,0,0,1,32,176V64A16,16,0,0,1,48,48ZM72,96v16h72V96Zm0,40v16h112v-16Z";
const wchar_t* PhPip=L"M48,40H208A16,16,0,0,1,224,56V184A16,16,0,0,1,208,200H48A16,16,0,0,1,32,184V56A16,16,0,0,1,48,40Zm0,16V184H208V56ZM128,120h64v48H128Z";
// Phosphor Gauge: speed is a rate, not a settings concept.
const wchar_t* PhGauge=L"M128,40A88,88,0,1,0,216,128A88,88,0,0,0,128,40ZM88,160l56-48,8,8-48,56ZM128,80a8,8,0,1,1,0,16,8,8,0,0,1,0-16Zm-48,40a8,8,0,1,1,0,16,8,8,0,0,1,0-16Zm96,0a8,8,0,1,1,0,16,8,8,0,0,1,0-16Z";
const wchar_t* PhSettings=L"M128,80a48,48,0,1,0,48,48A48.05,48.05,0,0,0,128,80Zm0,80a32,32,0,1,1,32-32A32,32,0,0,1,128,160Zm109.94-52.79a8,8,0,0,0-3.89-5.4l-29.83-17-.12-33.62a8,8,0,0,0-2.83-6.08,111.91,111.91,0,0,0-36.72-20.67,8,8,0,0,0-6.46.59L128,41.85,97.88,25a8,8,0,0,0-6.47-.6A112.1,112.1,0,0,0,54.73,45.15a8,8,0,0,0-2.83,6.07l-.15,33.65-29.83,17a8,8,0,0,0-3.89,5.4,106.47,106.47,0,0,0,0,41.56,8,8,0,0,0,3.89,5.4l29.83,17,.12,33.62a8,8,0,0,0,2.83,6.08,111.91,111.91,0,0,0,36.72,20.67,8,8,0,0,0,6.46-.59L128,214.15,158.12,231a7.91,7.91,0,0,0,3.9,1,8.09,8.09,0,0,0,2.57-.42,112.1,112.1,0,0,0,36.68-20.73,8,8,0,0,0,2.83-6.07l.15-33.65,29.83-17a8,8,0,0,0,3.89-5.4A106.47,106.47,0,0,0,237.94,107.21Zm-15,34.91-28.57,16.25a8,8,0,0,0-3,3c-.58,1-1.19,2.06-1.81,3.06a7.94,7.94,0,0,0-1.22,4.21l-.15,32.25a95.89,95.89,0,0,1-25.37,14.3L134,199.13a8,8,0,0,0-3.91-1h-.19c-1.21,0-2.43,0-3.64,0a8.08,8.08,0,0,0-4.1,1l-28.84,16.1A96,96,0,0,1,67.88,201l-.11-32.2a8,8,0,0,0-1.22-4.22c-.62-1-1.23-2-1.8-3.06a8.09,8.09,0,0,0-3-3.06l-28.6-16.29a90.49,90.49,0,0,1,0-28.26L61.67,97.63a8,8,0,0,0,3-3c.58-1,1.19-2.06,1.81-3.06a7.94,7.94,0,0,0,1.22-4.21l.15-32.25a95.89,95.89,0,0,1,25.37-14.3L122,56.87a8,8,0,0,0,4.1,1c1.21,0,2.43,0,3.64,0a8.08,8.08,0,0,0,4.1-1l28.84-16.1A96,96,0,0,1,188.12,55l.11,32.2a8,8,0,0,0,1.22,4.22c.62,1,1.23,2,1.8,3.06a8.09,8.09,0,0,0,3,3.06l28.6,16.29A90.49,90.49,0,0,1,222.9,142.12Z";
const wchar_t* PhExpand=L"M216,48V88a8,8,0,0,1-16,0V56H168a8,8,0,0,1,0-16h40A8,8,0,0,1,216,48ZM88,200H56V168a8,8,0,0,0-16,0v40a8,8,0,0,0,8,8H88a8,8,0,0,0,0-16Zm120-40a8,8,0,0,0-8,8v32H168a8,8,0,0,0,0,16h40a8,8,0,0,0,8-8V168A8,8,0,0,0,208,160ZM88,40H48a8,8,0,0,0-8,8V88a8,8,0,0,0,16,0V56H88a8,8,0,0,0,0-16Z";
const wchar_t* PhCompress=L"M152,96V48a8,8,0,0,1,16,0V88h40a8,8,0,0,1,0,16H160A8,8,0,0,1,152,96ZM96,152H48a8,8,0,0,0,0,16H88v40a8,8,0,0,0,16,0V160A8,8,0,0,0,96,152Zm112,0H160a8,8,0,0,0-8,8v48a8,8,0,0,0,16,0V168h40a8,8,0,0,0,0-16ZM96,40a8,8,0,0,0-8,8V88H48a8,8,0,0,0,0,16H96a8,8,0,0,0,8-8V48A8,8,0,0,0,96,40Z";

// Big-Sur-style frosted material: the compositor has already prepared a live,
// softened and vibrant backdrop. This routine adds only the neutral tint,
// perimeter light, inner highlight and micro texture. It must never become a
// fog layer over the film.
void MatteNoise(const D2D1_ROUNDED_RECT& shape,float alpha){
 auto target=Dc();
 if(!target)return;
 static ID2D1DeviceContext* owner=nullptr;
 static ComPtr<ID2D1Bitmap> bitmap;
 static ComPtr<ID2D1BitmapBrush> brush;
 if(owner!=target){owner=target;bitmap.Reset();brush.Reset();}
 if(!bitmap){
  UINT32 pixels[64*64];UINT32 state=0x6D2B79F5u;
  for(auto& pixel:pixels){
   state=state*1664525u+1013904223u;
   UINT32 tone=108u+((state>>24)&63u);
   pixel=0xFF000000u|(tone<<16)|(tone<<8)|tone;
  }
  D2D1_BITMAP_PROPERTIES props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
  if(FAILED(target->CreateBitmap(D2D1::SizeU(64,64),pixels,64*sizeof(UINT32),props,&bitmap))||!bitmap)return;
 }
 if(!brush){
  D2D1_BITMAP_BRUSH_PROPERTIES props=D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,D2D1_EXTEND_MODE_WRAP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  if(FAILED(target->CreateBitmapBrush(bitmap.Get(),props,&brush))||!brush)return;
 }
 brush->SetOpacity(.028f*alpha);
 target->FillRoundedRectangle(shape,brush.Get());
}
void FrostedSurface(const D2D1_ROUNDED_RECT& shape,float alpha){
 auto target=Dc();
 if(!target)return;
 // rgba(28,28,32,0.46) is the material's neutral core. Its small vertical
 // variation creates depth without a cloudy, uniform grey fill.
 float density=GfxGlassBackdropAvailable()?.46f:.54f;
 D2D1_GRADIENT_STOP colours[]={
  {0.f,  D2D1::ColorF(.125f,.125f,.142f,(density-.05f)*alpha)},
  {.18f,D2D1::ColorF(.110f,.110f,.126f,density*alpha)},
  {.72f,D2D1::ColorF(.100f,.100f,.116f,(density+.025f)*alpha)},
  {1.f, D2D1::ColorF(.082f,.082f,.098f,(density+.045f)*alpha)},
 };
 ComPtr<ID2D1GradientStopCollection> stops;
 ComPtr<ID2D1LinearGradientBrush> gradient;
 if(SUCCEEDED(target->CreateGradientStopCollection(colours,UINT(sizeof(colours)/sizeof(colours[0])),D2D1_GAMMA_2_2,
                                                     D2D1_EXTEND_MODE_CLAMP,&stops))&&stops&&
    SUCCEEDED(target->CreateLinearGradientBrush(
     D2D1::LinearGradientBrushProperties(D2D1::Point2F(shape.rect.left,shape.rect.top),
                                         D2D1::Point2F(shape.rect.left,shape.rect.bottom)),
     stops.Get(),&gradient))&&gradient){
  target->FillRoundedRectangle(shape,gradient.Get());
 }else{
  Ink()->SetColor(D2D1::ColorF(.110f,.110f,.126f,density*alpha));
  target->FillRoundedRectangle(shape,Ink());
 }
 MatteNoise(shape,alpha);
 // Thin perimeter light plus a softer inset highlight. These identify a dense
 // frosted surface without turning it into glossy liquid glass.
 auto inset=D2D1::RectF(shape.rect.left+.75f,shape.rect.top+.75f,shape.rect.right-.75f,shape.rect.bottom-.75f);
 Ink()->SetColor(D2D1::ColorF(.96f,.97f,1.f,.14f*alpha));
 target->DrawRoundedRectangle(D2D1::RoundedRect(inset,shape.radiusX-.75f,shape.radiusY-.75f),Ink(),1.f);
 Ink()->SetColor(D2D1::ColorF(.98f,.99f,1.f,.055f*alpha));
  target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(shape.rect.left+1.5f,shape.rect.top+1.5f,shape.rect.right-1.5f,shape.rect.bottom-1.5f),
                                                     shape.radiusX-1.5f,shape.radiusY-1.5f),Ink(),1.f);
}

// Apple's press feedback is a scale, not a flash: the control gives way under
// the finger and comes back. Four percent at full press, carried by a spring.
template<class Draw> void Press(Control id,D2D1_RECT_F rect,Draw draw){
 float amount=pressLift[id].v;
 auto target=Dc();
 if(amount<=.004f){draw();return;}
 float cx=(rect.left+rect.right)/2,cy=(rect.top+rect.bottom)/2;
 D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
 target->SetTransform(D2D1::Matrix3x2F::Scale(1.f-.04f*amount,1.f-.04f*amount,D2D1::Point2F(cx,cy))*Mat(previous));
 draw();
 target->SetTransform(previous);
}
// A round control sits on a plate that brightens under the pointer. The plate is
// what makes the primary action primary; the others carry none until hovered.
void RoundButton(Control id,D2D1_RECT_F rect,const wchar_t* icon,float iconInset,
                 float restPlate,float alpha,bool fillIcon){
 auto target=Dc();
 float lift=hoverLift[id].v;
 float plate=restPlate+(restPlate>0?.06f:.10f)*lift;
 D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
 // Hover is a literal lift, not a colour-only state. The small travel keeps
 // adjacent transport buttons calm while still making each one feel mounted
 // on the glass rather than printed onto it.
 target->SetTransform(D2D1::Matrix3x2F::Translation(0,-1.5f*lift)*Mat(previous));
 Press(id,rect,[&]{
  if(plate>.004f){
   float radius=(rect.bottom-rect.top)/2;
   auto plateShape=D2D1::RoundedRect(rect,radius,radius);
   Ink()->SetColor(White(plate*alpha));
   target->FillRoundedRectangle(plateShape,Ink());
  }
  PhosphorIcon(icon,D2D1::RectF(rect.left+iconInset,rect.top+iconInset,rect.right-iconInset,rect.bottom-iconInset),
               White((.88f+.12f*lift)*alpha),1.7f,fillIcon);
 });
 target->SetTransform(previous);
}
// Both sliders share one drawing: a thin track, the filled part, and a knob that
// grows while it is held.
void Slider(D2D1_RECT_F rect,float progress,float alpha,bool active,float thickness,float knob){
 auto target=Dc();
 float mid=(rect.top+rect.bottom)/2;
 float half=thickness/2;
 Ink()->SetColor(White(.24f*alpha));
 target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left,mid-half,rect.right,mid+half),half,half),Ink());
 float head=rect.left+(rect.right-rect.left)*float(Clamp01(progress));
 Ink()->SetColor(White(.95f*alpha));
 target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left,mid-half,(std::max)(rect.left,head),mid+half),half,half),Ink());
 float radius=knob*(active?1.18f:1.f)/2;
 Ink()->SetColor(D2D1::ColorF(0,0,0,.18f*alpha));
 target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(head,mid+1.f),radius,radius),Ink());
 Ink()->SetColor(White(alpha));
 target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(head,mid),radius,radius),Ink());
}

// The popup is deliberately a small card with enough depth for one decision.
// It never becomes a settings drawer: transport stays transport, and a video
// remains the primary surface.
D2D1_RECT_F PopupRect(D2D1_RECT_F viewport){
 if(VisiblePopup()==PopupNone&&popupReveal.v<=.004f)return D2D1::RectF(0,0,0,0);
 auto bar=VideoModeControlPanel(viewport);
 if(bar.right<=bar.left)return D2D1::RectF(0,0,0,0);
 constexpr float width=196.f,gap=12.f;
 float height=126.f;
 auto kind=VisiblePopup();
 if((kind==PopupAudio||kind==PopupSubtitles)&&engine){
  int rows=0;
  const bool audio=kind==PopupAudio;
  for(const auto& track:engine->Tracks())if(track.kind==(audio?L"audio":L"sub"))rows++;
  if(!audio&&!surfaceRoute.IsUrl())rows++; // AI subtitles
  rows=(std::max)(1,(std::min)(rows,6));
  height=43.f+rows*30.f;
 }
 float right=bar.right-22.f-34.f-8.f;
 float left=right-width;
 // Narrow windows get the same card, just centred above the primary action.
 left=(std::max)(bar.left+12.f,(std::min)(left,bar.right-width-12.f));
 return D2D1::RectF(left,bar.top-gap-height,left+width,bar.top-gap);
}
void PopupButton(D2D1_RECT_F rect,const std::wstring& label,bool selected,bool hoveredRow,float alpha){
 auto target=Dc();
 if(selected||hoveredRow){
  Ink()->SetColor(White((selected?.17f:.09f)*alpha));
  target->FillRoundedRectangle(D2D1::RoundedRect(rect,10,10),Ink());
  Ink()->SetColor(White((selected?.22f:.11f)*alpha));
  target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left+.5f,rect.top+.5f,rect.right-.5f,rect.bottom-.5f),10,10),Ink(),1.f);
 }
 Write(label,rect,F_Small,White((selected?.98f:hoveredRow?.90f:.72f)*alpha));
}
std::wstring TrackLabel(const TrackInfo& track){
 if(!track.title.empty())return track.title;
 if(!track.language.empty())return track.language;
 if(!track.codec.empty())return track.codec;
 return track.kind==L"audio"?T(S_AudioTrack):track.kind==L"sub"?T(S_SubtitleTrack):L"Video";
}
void PaintPopup(D2D1_RECT_F viewport,float alpha){
 popupHits.clear();
 auto rect=PopupRect(viewport);
 if(rect.right<=rect.left||popupReveal.v<=.004f)return;
 auto target=Dc();
 float reveal=float(Clamp01(popupReveal.v));
 D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
 float cx=(rect.left+rect.right)/2,cy=rect.bottom;
 target->SetTransform(D2D1::Matrix3x2F::Scale(.94f+.06f*reveal,.94f+.06f*reveal,D2D1::Point2F(cx,cy))*Mat(previous));
 auto shape=D2D1::RoundedRect(rect,22,22);
 SoftShadow(shape,.42f*alpha*reveal,1.45f);
 FrostedSurface(shape,alpha*reveal);
 auto kind=VisiblePopup();
 if(kind==PopupSpeed){
  Write(T(S_PlaybackSpeed),D2D1::RectF(rect.left+16,rect.top+14,rect.right-16,rect.top+34),F_Meta,White(.68f*alpha*reveal));
  constexpr double rates[]={.75,1.,1.25};
  for(int i=0;i<3;i++){
   float left=rect.left+14+i*58.f;
   auto button=D2D1::RectF(left,rect.top+43,left+52.f,rect.top+77.f);
   wchar_t label[16];swprintf_s(label,L"%gx",rates[i]);
   PopupButton(button,label,fabs(snapshot.speed-rates[i])<.01,popupHovered==i,alpha*reveal);
   popupHits.push_back({button,rates[i],L"",0});
  }
  Write(T(S_PlaybackRate),D2D1::RectF(rect.left+16,rect.bottom-31,rect.right-16,rect.bottom-13),F_Small,White(.56f*alpha*reveal));
 }else if((kind==PopupAudio||kind==PopupSubtitles)&&engine){
  auto tracks=engine->Tracks();
  const bool audio=kind==PopupAudio;
  Write(audio?T(S_AudioTrack):T(S_SubtitleTrack),D2D1::RectF(rect.left+16,rect.top+12,rect.right-16,rect.top+31),F_Meta,White(.68f*alpha*reveal));
  int row=0;
  // Generated captions belong to subtitles alone. Audio and subtitles have
  // independent docks and never compete for the same three short rows.
  bool aiRow=!audio&&!surfaceRoute.IsUrl();
  int trackRows=aiRow?5:6;
  for(const auto& track:tracks){
   if(track.kind!=(audio?L"audio":L"sub"))continue;
   if(row==trackRows)break;
   float top=rect.top+34.f+row*30.f;
   auto button=D2D1::RectF(rect.left+10,top,rect.right-10,top+27.f);
   PopupButton(button,TrackLabel(track),track.selected,popupHovered==row,alpha*reveal);
   popupHits.push_back({button,0,track.kind,track.id});
   row++;
  }
  if(aiRow){
   float top=rect.top+34.f+row*30.f;
   auto button=D2D1::RectF(rect.left+10,top,rect.right-10,top+27.f);
   PopupButton(button,T(S_AiSubtitles),aiSubtitles,popupHovered==row,alpha*reveal);
   popupHits.push_back({button,0,L"ai",0});
   row++;
  }
  if(!row)Write(T(S_DefaultStream),D2D1::RectF(rect.left+16,rect.top+49,rect.right-16,rect.top+71),F_Small,White(.72f*alpha*reveal));
 }else{
  // While the stream list is arriving, keep the object stable instead of
  // popping it out and back in. It makes the panel feel loaded, not broken.
  Write(T(S_LoadingStreams),D2D1::RectF(rect.left+16,rect.top+12,rect.right-16,rect.top+31),F_Meta,White(.68f*alpha*reveal));
  Write(T(S_LoadingStreams),D2D1::RectF(rect.left+16,rect.top+49,rect.right-16,rect.top+71),F_Small,White(.72f*alpha*reveal));
 }
 target->SetTransform(previous);
}
}

namespace{
// The preview card. Same material as the transport it grows out of, because it
// is part of the same object: a question asked of the timeline, answered
// directly above the place the pointer is asking about.
constexpr float PreviewWidth=168.f,PreviewPad=7.f,PreviewTimeRow=20.f;

D2D1_RECT_F PreviewCardRect(D2D1_RECT_F viewport){
 if(previewReveal.v<=.004f||trackRect.right<=trackRect.left)return D2D1::RectF(0,0,0,0);
 float aspect=snapshot.width&&snapshot.height?float(snapshot.width)/float(snapshot.height):16.f/9.f;
 if(aspect<.2f||aspect>6.f)aspect=16.f/9.f;
 float frameWidth=PreviewWidth-2*PreviewPad;
 float frameHeight=frameWidth/aspect;
 float height=frameHeight+2*PreviewPad+PreviewTimeRow;
 float left=previewX-PreviewWidth/2;
 // The card stays inside the transport it belongs to, so it never hangs off
 // the panel's edge over the film.
 left=(std::max)(barRect.left+8.f,(std::min)(left,barRect.right-PreviewWidth-8.f));
 // Above the whole transport, not above the track: the card is bigger than the
 // gap between the two rows, and anything drawn into that gap lands on top of
 // the controls the pointer might be heading for.
 float bottom=barRect.top-12.f;
 (void)viewport;
 return D2D1::RectF(left,bottom-height,left+PreviewWidth,bottom);
}

void PaintPreviewCard(D2D1_RECT_F viewport,float alpha){
 auto target=Dc();
 auto rect=PreviewCardRect(viewport);
 if(!target||rect.right<=rect.left)return;
 float reveal=float(Clamp01(previewReveal.v));
 float shown=alpha*reveal;
 if(shown<=.004f)return;
 auto frame=PreviewBest(previewTime);
 if(frame!=previewFrame){previewFrame=frame;previewTextureOf=nullptr;}

 D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
 // Grows from the timeline rather than fading in place: the card is the
 // timeline answering, not a window arriving.
 float cx=(rect.left+rect.right)/2;
 target->SetTransform(D2D1::Matrix3x2F::Scale(.94f+.06f*reveal,.94f+.06f*reveal,D2D1::Point2F(cx,rect.bottom))*Mat(previous));
 auto shape=D2D1::RoundedRect(rect,14.f,14.f);
 SoftShadow(shape,.28f*shown,1.2f);
 FrostedSurface(shape,shown);

 auto frameRect=D2D1::RectF(rect.left+PreviewPad,rect.top+PreviewPad,
                            rect.right-PreviewPad,rect.bottom-PreviewPad-PreviewTimeRow);
 auto frameShape=D2D1::RoundedRect(frameRect,8.f,8.f);
 if(previewFrame&&previewFrame->width&&previewFrame->height){
  if(!previewTexture||previewTextureOf!=previewFrame.get()){
   previewTexture.Reset();
   target->CreateBitmap(D2D1::SizeU(previewFrame->width,previewFrame->height),previewFrame->bgra.data(),
    previewFrame->width*4,
    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE)),
    &previewTexture);
   previewTextureOf=previewFrame.get();
  }
  if(previewTexture){
   ComPtr<ID2D1RoundedRectangleGeometry> clip;
   if(GfxFactory()&&SUCCEEDED(GfxFactory()->CreateRoundedRectangleGeometry(frameShape,&clip))&&clip){
    target->PushLayer(D2D1::LayerParameters(frameRect,clip.Get(),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),nullptr);
    target->DrawBitmap(previewTexture.Get(),frameRect,shown,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr);
    target->PopLayer();
   }else target->DrawBitmap(previewTexture.Get(),frameRect,shown,D2D1_INTERPOLATION_MODE_LINEAR,nullptr);
  }
 }else{
  // No frame yet, or none possible. The card still shows the timestamp: a
  // truthful "here, but I cannot show you" beats a fabricated still (18.2).
  Ink()->SetColor(White(.07f*shown));
  target->FillRoundedRectangle(frameShape,Ink());
  const wchar_t* note=PreviewAvailable()?T(S_PreviewWorking):T(S_PreviewUnavailable);
  float middle=(frameRect.top+frameRect.bottom)/2-9.f;
  Write(note,D2D1::RectF(frameRect.left+6,middle,frameRect.right-6,frameRect.bottom),F_Small,White(.55f*shown));
 }
 // A frame that came from a neighbouring bucket is the truth about roughly
 // where you are, and it is replaced in place the moment the exact one lands.
 auto timeRect=D2D1::RectF(rect.left,rect.bottom-PreviewTimeRow-2.f,rect.right,rect.bottom-2.f);
 // The card's own type, not the timeline's: the transport's 18pt tabular figures
 // are for the row that has to stay readable at a glance across a wide bar, and
 // on a 168-point card they read as shouting.
 Write(Clock(previewTime),timeRect,F_Meta,White((previewFrame&&!previewFrame->exact?.72f:.95f)*shown));
 target->SetTransform(previous);
}
}

namespace{
// The Vetro Bubble (24). Matte glass over the picture, the viewer's own type,
// and a shape that belongs to the text rather than to a fixed plate. No cheap
// black rectangle, and never an effect that outranks the words.
constexpr float BubbleMaxWidthFraction=.74f;
constexpr float BubblePadX=22.f,BubblePadY=13.f;
constexpr float BubbleBottomGap=44.f;

// The bubble's measured shape for the text it is showing. `lift` is how far the
// transport pushes it up, so the two never occupy the same place.
D2D1_RECT_F BubbleTargetRect(D2D1_RECT_F viewport,const std::wstring& text,float lift){
 if(text.empty())return D2D1::RectF(0,0,0,0);
 float available=(viewport.right-viewport.left)*BubbleMaxWidthFraction;
 float width=0,height=0;
 MeasureBlock(text,F_Subtitle,available,width,height);
 if(width<=0||height<=0)return D2D1::RectF(0,0,0,0);
 width=(std::min)(width,available);
 float boxWidth=width+2*BubblePadX,boxHeight=height+2*BubblePadY;
 float cx=(viewport.left+viewport.right)/2;
 float bottom=viewport.bottom-BubbleBottomGap-lift;
 return D2D1::RectF(cx-boxWidth/2,bottom-boxHeight,cx+boxWidth/2,bottom);
}

void PaintBubble(D2D1_RECT_F viewport,float chromeAlpha){
 auto target=Dc();
 if(!target)return;
 if(bubble.phase==BubblePhase::Hidden||bubble.text.empty()){bubbleRect=D2D1::RectF(0,0,0,0);return;}
 // While the transport is up the bubble sits above it: a line hidden behind the
 // controls is a line the viewer had to rewind for.
 float lift=0;
 if(barRect.right>barRect.left)lift=(viewport.bottom-barRect.top)*Clamp01(chromeAlpha)+10.f*Clamp01(chromeAlpha);
 auto measured=BubbleTargetRect(viewport,bubble.text,lift);
 if(measured.right<=measured.left){bubbleRect=D2D1::RectF(0,0,0,0);return;}
 float targetWidth=measured.right-measured.left,targetHeight=measured.bottom-measured.top;
 if(!bubbleGeometryReady){
  bubbleWidth.Reset(targetWidth);bubbleHeight.Reset(targetHeight);bubbleGeometryReady=true;
 }
 bubbleWidth.To(targetWidth);bubbleHeight.To(targetHeight);
 float width=(std::max)(4.f,bubbleWidth.v)*(std::max)(0.f,bubbleScale);
 float height=(std::max)(4.f,bubbleHeight.v)*(std::max)(0.f,bubbleScale);
 float cx=(measured.left+measured.right)/2,bottom=measured.bottom;
 auto rect=D2D1::RectF(cx-width/2,bottom-height,cx+width/2,bottom);
 bubbleRect=rect;
 // Concentric radii: the corner follows the size, so a bubble collapsing
 // towards a point becomes a bead rather than a shrinking rectangle (25.3).
 float radius=(std::min)(20.f,(std::min)(width,height)/2);
 auto shape=D2D1::RoundedRect(rect,radius,radius);
 float alpha=(std::min)(1.f,bubbleScale*1.4f);
 if(alpha<=.004f)return;
 SoftShadow(shape,.30f*alpha,1.3f);
 FrostedSurface(shape,alpha);
 if(bubbleTextAlpha>.004f){
  // The text is laid out in the shape it is going to, not in the shape the
  // glass currently has. A morphing bubble that clipped its own words while it
  // caught up would be an animation that costs reading time, which is the one
  // thing the subtitle animation may never do (25.6).
  auto textRect=D2D1::RectF(measured.left+BubblePadX,measured.top+BubblePadY-1.f,
                            measured.right-BubblePadX,measured.bottom-BubblePadY+1.f);
  // Typography over effect (24.1): the text is plain white at full strength,
  // and the material behind it is what does the work of making it legible.
  Write(bubble.text,textRect,F_Subtitle,White(bubbleTextAlpha*alpha));
 }
}
}

void VideoModePaintMatteSurface(const D2D1_ROUNDED_RECT& shape,float alpha){
 FrostedSurface(shape,alpha);
}

void VideoModePaint(D2D1_RECT_F viewport,const Palette& palette,D2D1_COLOR_F base,float alpha){
 auto target=Dc();
 if(!target)return;
 // Once the engine's picture is composited underneath this layer, the interface
 // layer must stay transparent over it. Painting a ground here would hide the
 // film behind the window's own background, which is the one thing the two-layer
 // composition tree exists to prevent.
 if(VideoModeShowingVideo())return;
 // The letterbox, and the window's own ground. A film does not fill an
 // arbitrary window, and what surrounds it belongs to the window rather than to
 // the file: the shell's base tone, then its sunk tone over it, not black bars.
 Ink()->SetColor(base);
 target->FillRectangle(viewport,Ink());
 Ink()->SetColor(Fade(palette.sunk,.55f*alpha));
 target->FillRectangle(viewport,Ink());
 if(!surfacePoster||!surfacePoster->w||!surfacePoster->h)return;
 if(!posterTexture){
  target->CreateBitmap(D2D1::SizeU(surfacePoster->w,surfacePoster->h),surfacePoster->pixels.data(),
   surfacePoster->w*4,
   D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),
   &posterTexture);
  if(!posterTexture)return;
 }
 auto frame=FitPoster(viewport);
 SoftShadow(D2D1::RoundedRect(frame,6,6),.5f*alpha,1.2f);
 target->DrawBitmap(posterTexture.Get(),frame,alpha,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr);
}

namespace{
// The transport leaves the way the subtitle bubble does, towards the point it
// stands on: a slight shrink into its own bottom centre while it fades, so it
// reads as one object going away rather than a picture losing its opacity.
float TransportScale(float alpha){
 float a=alpha<0?0:(alpha>1?1:alpha);
 float eased=a*a*(3-2*a);
 return .9f+.1f*eased;
}
}

void VideoModePaintOverlay(D2D1_RECT_F viewport,const Palette& palette,float alpha,float dpi){
 auto target=Dc();
 if(!target)return;
 // Subtitles are not chrome and are not drawn here at all: the shell draws them
 // after the chrome's own fading layer (VideoModePaintSubtitles), because a line
 // of dialogue must not fade out with the controls.
 if(alpha<=.004f){
  barRect=D2D1::RectF(0,0,0,0);
  return;
 }
 if(!springsReady){
  for(int i=0;i<CtrlCount;i++){hoverLift[i]=Spring(0,ButtonK,ButtonC);pressLift[i]=Spring(0,ButtonK,ButtonC);}
  springsReady=true;
 }
 barRect=trackRect=volumeTrackRect=D2D1::RectF(0,0,0,0);
 hitCount=0;

 // Nothing to transport yet: say what the file is instead. The same pill the
 // poster stage has always shown, in the same place the controls will appear,
 // so the surface does not reshuffle when playback starts.
 if(!snapshot.opened||snapshot.duration<=0){
  GfxClearGlassBackdrop();
  auto caption=Caption();
  if(caption.empty())return;
  float width=Measure(caption,F_Meta,420.f)+34.f,height=34.f;
  float centreX=(viewport.left+viewport.right)/2;
  auto pill=D2D1::RectF(centreX-width/2,viewport.bottom-height-PanelMargin,centreX+width/2,viewport.bottom-PanelMargin);
  auto pillShape=D2D1::RoundedRect(pill,height/2,height/2);
  SoftShadow(pillShape,.35f*alpha);
  Glass(pillShape,palette,.9f*alpha,D2D1::Matrix3x2F::Identity());
  Write(caption,D2D1::RectF(pill.left,pill.top+9.f,pill.right,pill.bottom),F_Meta,Fade(palette.text,alpha));
  return;
 }

 // ------------------------------------------------------------- geometry ---
 barRect=VideoModeControlPanel(viewport);
 popupHits.clear();
 float cx=(barRect.left+barRect.right)/2;
 auto shape=D2D1::RoundedRect(barRect,PanelRadius,PanelRadius);

 // --------------------------------------------------------------- glass ----
 // The compositor has already frosted the film under this panel; what is drawn
 // here is the material over that blur. A control is not one colour: base
 // density, a lift that catches the light, then the specular edge. Flatten those
 // and the glass stops answering to whatever is behind it.
 (void)dpi;
 // Everything that belongs to the panel shrinks with it; the compositor's glass
 // is given the same rectangle by VideoModeControlGlass. Hit tests keep the
 // resting geometry, so a control does not move under a pointer mid-fade.
 D2D1_MATRIX_3X2_F beforeCollapse;target->GetTransform(&beforeCollapse);
 float collapse=TransportScale(alpha);
 target->SetTransform(D2D1::Matrix3x2F::Scale(collapse,collapse,D2D1::Point2F(cx,barRect.bottom))*Mat(beforeCollapse));
 SoftShadow(shape,.25f*alpha,1.35f);
 FrostedSurface(shape,alpha);

 // ------------------------------------------------------------ first row ---
 float rowY=barRect.top+40.f;                 // centre line of the control row
 float playSize=62.f,skipSize=40.f,smallSize=36.f;
 auto centred=[&](float centreX,float size){
  return D2D1::RectF(centreX-size/2,rowY-size/2,centreX+size/2,rowY+size/2);
 };
 auto play=centred(cx,playSize);
 auto back=centred(cx-playSize/2-28.f-skipSize/2,skipSize);
 auto forward=centred(cx+playSize/2+28.f+skipSize/2,skipSize);
 auto volume=centred(barRect.left+22.f+smallSize/2,smallSize);
 float volumeWidth=(std::min)(96.f,(back.left-volume.right)-24.f);
 volumeTrackRect=D2D1::RectF(volume.right+10.f,rowY-9.f,volume.right+10.f+(std::max)(0.f,volumeWidth),rowY+9.f);
 auto expand=centred(barRect.right-22.f-smallSize/2,smallSize);
 auto pip=centred(expand.left-10.f-smallSize/2,smallSize);
 auto settings=centred(pip.left-10.f-smallSize/2,smallSize);
 auto subtitles=centred(settings.left-10.f-smallSize/2,smallSize);
 auto audio=centred(subtitles.left-10.f-smallSize/2,smallSize);
 bool showExtras=barRect.right-barRect.left>=700.f;

 Add(CtrlPlay,play);Add(CtrlBack,back);Add(CtrlForward,forward);
 Add(CtrlVolume,volume);if(showExtras){Add(CtrlAudio,audio);Add(CtrlSubtitles,subtitles);Add(CtrlSettings,settings);Add(CtrlPip,pip);}Add(CtrlExpand,expand);
 if(volumeTrackRect.right>volumeTrackRect.left+8)Add(CtrlVolumeTrack,volumeTrackRect);

 // Playback remains the spatial centre, but it is no longer wrapped in a
 // separate disc: on frosted material the icon itself is the primary action.
 RoundButton(CtrlPlay,play,snapshot.paused?PhPlay:PhPause,17.f,0.f,alpha,true);
 RoundButton(CtrlBack,back,PhBack,9.f,0.f,alpha,true);
 RoundButton(CtrlForward,forward,PhForward,9.f,0.f,alpha,true);
 RoundButton(CtrlVolume,volume,snapshot.muted?PhMuted:PhVolume,8.f,0.f,alpha,true);
 if(showExtras){
  RoundButton(CtrlAudio,audio,PhTracks,8.f,popup==PopupAudio?.08f:0.f,alpha,true);
  RoundButton(CtrlSubtitles,subtitles,PhSubtitles,8.f,popup==PopupSubtitles?.08f:0.f,alpha,true);
  RoundButton(CtrlSettings,settings,PhGauge,8.f,popup==PopupSpeed?.08f:0.f,alpha,true);
  RoundButton(CtrlPip,pip,PhPip,8.f,pipState&&pipState()?.08f:0.f,alpha,true);
 }else if(popup!=PopupNone){ClosePopup();}
 RoundButton(CtrlExpand,expand,expandState&&expandState()?PhCompress:PhExpand,9.f,0.f,alpha,true);
 if(volumeTrackRect.right>volumeTrackRect.left+8)
  Slider(volumeTrackRect,snapshot.muted?0.f:float(snapshot.volume/100.0),alpha,volumeDragging,4.f,12.f);

 // ----------------------------------------------------------- second row ---
 float timeY=barRect.top+PanelHeight-33.f;
 double shown=scrubbing?scrubTarget:snapshot.position;
 auto elapsed=Clock(shown),total=snapshot.live?std::wstring(T(S_Live)):Clock(snapshot.duration);
 float timeWidth=(std::max)(54.f,Measure(total,F_Timeline,160.f)+10.f);
 auto elapsedRect=D2D1::RectF(barRect.left+24.f,timeY-10.f,barRect.left+24.f+timeWidth,timeY+14.f);
 auto totalRect=D2D1::RectF(barRect.right-24.f-timeWidth,elapsedRect.top,barRect.right-24.f,elapsedRect.bottom);
 Write(elapsed,elapsedRect,F_Timeline,White(.95f*alpha));
 Write(total,totalRect,F_Timeline,White(.60f*alpha));

 trackRect=D2D1::RectF(elapsedRect.right+12.f,timeY-9.f,totalRect.left-12.f,timeY+9.f);
 if(trackRect.right>trackRect.left+8){
  Add(CtrlTrack,D2D1::RectF(trackRect.left-6,trackRect.top-6,trackRect.right+6,trackRect.bottom+6));
  float progress=float(snapshot.duration>0?Clamp01(shown/snapshot.duration):0.0);
  Slider(trackRect,progress,alpha,scrubbing||hovered==CtrlTrack,4.f,13.f);
 }
 target->SetTransform(beforeCollapse);
 PaintPopup(viewport,alpha);
 PaintPreviewCard(viewport,alpha);
}

void VideoModePaintSubtitles(D2D1_RECT_F viewport,float chromeAlpha){
 PaintBubble(viewport,chromeAlpha);
}
float VideoModeSubtitleOpacity(){
 if(bubbleRect.right<=bubbleRect.left)return 0;
 return (std::min)(1.f,(std::max)(0.f,bubbleScale*1.4f));
}
D2D1_RECT_F VideoModeControlGlass(D2D1_RECT_F viewport,float alpha,float& radius){
 auto rest=VideoModeControlPanel(viewport);
 float scale=TransportScale(alpha);
 radius=PanelRadius*scale;
 if(rest.right<=rest.left)return rest;
 float cx=(rest.left+rest.right)/2;
 return D2D1::RectF(cx+(rest.left-cx)*scale,rest.bottom+(rest.top-rest.bottom)*scale,
                    cx+(rest.right-cx)*scale,rest.bottom);
}

// Where the transport will draw, so the shell can have the compositor frost the
// film under it before anything is painted. Geometry only -- the same numbers
// the painter uses, in one place.
D2D1_RECT_F VideoModeControlPanel(D2D1_RECT_F viewport){
 if(!snapshot.opened||snapshot.duration<=0)return D2D1::RectF(0,0,0,0);
 float available=viewport.right-viewport.left-2*PanelMargin;
 // A fixed 440-DIP minimum became a near edge-to-edge slab on high DPI
 // displays. The dock now follows the available surface and only bottoms out
 // at the space needed for its transport cluster.
 float width=(std::min)(820.f,(std::max)(320.f,available*.56f));
 if(width>available)width=available;
 float cx=(viewport.left+viewport.right)/2;
 return D2D1::RectF(cx-width/2,viewport.bottom-PanelHeight-PanelMargin,cx+width/2,viewport.bottom-PanelMargin);
}
D2D1_RECT_F VideoModePopupPanel(D2D1_RECT_F viewport){return PopupRect(viewport);}
float VideoModePopupOpacity(){return float(Clamp01(popupReveal.v));}
D2D1_RECT_F VideoModePreviewPanel(D2D1_RECT_F viewport){return PreviewCardRect(viewport);}
D2D1_RECT_F VideoModeSubtitlePanel(){return bubbleRect;}
std::wstring VideoModeCueDescription(){
 std::wstring out;
 switch(cueKind){
  case CueKind::Simple:out=L"dialogue, drawn by the viewer";break;
  case CueKind::ComplexAss:out=L"authored typesetting, drawn by the engine";break;
  case CueKind::Bitmap:out=L"bitmap, drawn by the engine";break;
  case CueKind::None:out=snapshot.subtitleTrack?L"track selected, nothing on screen":L"no track";break;
 }
 if(!snapshot.subtitleCodec.empty())out+=L" ("+snapshot.subtitleCodec+L")";
 if(!bubbleText.empty()){
  auto sample=bubbleText.substr(0,60);
  for(auto& ch:sample)if(ch==L'\n')ch=L' ';
  out+=L": "+sample;
 }
 return out;
}
void VideoModeSetSubtitlePolicy(double collapseAfterSeconds,const std::wstring& subtitleLanguages,
                                const std::wstring& audioLanguages){
 subtitleCollapseAfter=collapseAfterSeconds>0?collapseAfterSeconds:3.0;
 if(engine)engine->SetLanguagePreference(subtitleLanguages,audioLanguages);
}
void VideoModeSetVolume(double percent){if(engine)engine->SetVolume(percent);}
void VideoModeSetZoom(double scale){
 videoZoom=(std::max)(1.0,(std::min)(scale,8.0));
 if(engine)engine->SetVideoZoom(videoZoom);
}
double VideoModeZoom(){return videoZoom;}
void VideoModeResetZoom(){VideoModeSetZoom(1.0);}
void VideoModeSetTarget(const PresentationTarget& target){
 if(target==quality.target)return;
 quality.target=target;
 if(engine)engine->SetPresentationTarget(target);
}
void VideoModeSetPacing(const PresentationPlan& plan){
 quality.plan=plan;
 if(engine)engine->SetPresentationPlan(plan);
}
void VideoModeSetStreamBudget(const StreamBudget& budget){
 quality.budget=budget;
 if(engine)engine->SetStreamBudget(budget);
}
const PresentationTarget& VideoModeTarget(){return quality.target;}
void VideoModeSetEnhancement(const EnhancementPlan& plan){
 quality.enhancement=plan;
 if(engine)engine->SetEnhancement(plan);
}
void VideoModeToggleMute(){
 if(!engine)return;
 bool next=!snapshot.muted;
 engine->SetMuted(next);
 snapshot.muted=next;
}
void VideoModeSetExpandHandler(void(*toggle)(),bool(*expanded)()){
 expandToggle=toggle;expandState=expanded;
}
void VideoModeSetPipHandler(void(*toggle)(),bool(*active)()){
 pipToggle=toggle;pipState=active;
}

const MediaFacts& VideoModeFacts(){return surfaceFacts;}
const MediaRoute& VideoModeRoute(){return surfaceRoute;}
bool VideoModeHasPoster(){return surfacePoster&&surfacePoster->w&&surfacePoster->h;}
std::wstring VideoModeError(){
 // A stream's failure is a sentence, not the engine's words (9.2): those are in
 // Diagnostics. While a reconnect is pending there is no failure to show yet.
 if(surfaceRoute.IsUrl()){
  if(net.waiting||(net.restoring&&snapshot.error.empty()))return {};
  if(net.gaveUp)return VideoModeFailureText(net.failure);
  if(!snapshot.error.empty())return VideoModeFailureText(ClassifyEngineFailure(snapshot.failureLog+L"\n"+snapshot.error));
  return {};
 }
 if(!snapshot.error.empty())return snapshot.error;
 if(!engine&&engineTried)return engineError;
 return {};
}
