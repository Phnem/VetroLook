// Vetro Look, GPL-3.0-or-later.
// The libmpv-backed playback engine. The only file in the application that
// knows mpv exists; see playback.h for why.
//
// The library is loaded on demand, by name, the first time a film is opened.
// That is deliberate: opening a photograph must not pull a hundred megabytes of
// media stack into the process, and a machine without the library must still be
// a working image viewer rather than an application that refuses to start.
#include "playback.h"
#include <mpv/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

namespace{

// ------------------------------------------------------------- loading ----
struct Api{
 mpv_handle* (*create)(void);
 int (*initialize)(mpv_handle*);
 void (*terminate_destroy)(mpv_handle*);
 int (*set_option_string)(mpv_handle*,const char*,const char*);
 int (*set_property_string)(mpv_handle*,const char*,const char*);
 int (*set_property)(mpv_handle*,const char*,mpv_format,void*);
 int (*get_property)(mpv_handle*,const char*,mpv_format,void*);
 char* (*get_property_string)(mpv_handle*,const char*);
 int (*command)(mpv_handle*,const char**);
 int (*observe_property)(mpv_handle*,uint64_t,const char*,mpv_format);
 mpv_event* (*wait_event)(mpv_handle*,double);
 void (*set_wakeup_callback)(mpv_handle*,void(*)(void*),void*);
 const char* (*error_string)(int);
 void (*free)(void*);
 void (*free_node_contents)(mpv_node*);
 unsigned long (*client_api_version)(void);
 int (*request_log_messages)(mpv_handle*,const char*);
 int (*command_node)(mpv_handle*,mpv_node*,mpv_node*);
};
Api api{};
HMODULE library=nullptr;
void(*logSink)(const std::wstring&)=nullptr;
bool audioExclusive=false;
std::once_flag loadOnce;
std::wstring loadError,version;

std::wstring Widen(const char* text){
 if(!text||!*text)return {};
 int length=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
 std::wstring out(length?length-1:0,L'\0');
 if(length>1)MultiByteToWideChar(CP_UTF8,0,text,-1,out.data(),length);
 return out;
}
std::string Narrow(const std::wstring& text){
 if(text.empty())return {};
 int length=WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);
 std::string out(length?length-1:0,'\0');
 if(length>1)WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,out.data(),length,nullptr,nullptr);
 return out;
}

template<class T> bool Bind(T& slot,const char* name){
 slot=(T)GetProcAddress(library,name);
 return slot!=nullptr;
}
// Beside the executable first. A player that picks up whichever libmpv happens
// to be on PATH is a player that behaves differently on every machine.
void Load(){
 wchar_t module[MAX_PATH]{};
 if(GetModuleFileNameW(nullptr,module,MAX_PATH)){
  std::error_code ec;
  auto beside=std::filesystem::path(module).parent_path()/L"libmpv-2.dll";
  if(std::filesystem::exists(beside,ec))
   library=LoadLibraryExW(beside.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
 }
 if(!library)library=LoadLibraryW(L"libmpv-2.dll");
 if(!library){
  loadError=L"The playback library (libmpv-2.dll) was not found beside the application.";
  return;
 }
 bool ok=
  Bind(api.create,"mpv_create")&&
  Bind(api.initialize,"mpv_initialize")&&
  Bind(api.terminate_destroy,"mpv_terminate_destroy")&&
  Bind(api.set_option_string,"mpv_set_option_string")&&
  Bind(api.set_property_string,"mpv_set_property_string")&&
  Bind(api.set_property,"mpv_set_property")&&
  Bind(api.get_property,"mpv_get_property")&&
  Bind(api.get_property_string,"mpv_get_property_string")&&
  Bind(api.command,"mpv_command")&&
  Bind(api.observe_property,"mpv_observe_property")&&
  Bind(api.wait_event,"mpv_wait_event")&&
  Bind(api.set_wakeup_callback,"mpv_set_wakeup_callback")&&
  Bind(api.error_string,"mpv_error_string")&&
  Bind(api.free,"mpv_free")&&
  Bind(api.free_node_contents,"mpv_free_node_contents")&&
  Bind(api.client_api_version,"mpv_client_api_version")&&
  Bind(api.request_log_messages,"mpv_request_log_messages")&&
  Bind(api.command_node,"mpv_command_node");
 if(!ok){
  FreeLibrary(library);library=nullptr;
  loadError=L"The playback library is present but does not export the expected interface.";
  return;
 }
 unsigned long number=api.client_api_version();
 version=std::to_wstring(number>>16)+L"."+std::to_wstring(number&0xFFFF);
 // Composition output is what the viewer's presentation path is built on, and
 // it arrived in client API 2.5. Anything older would run in a window of its
 // own, over the top of everything.
 if(number<((2ul<<16)|5ul)){
  FreeLibrary(library);library=nullptr;
  loadError=L"The playback library is too old for this version of Vetro Look.";
  version.clear();
 }
}
bool Ready(){
 std::call_once(loadOnce,Load);
 return library!=nullptr;
}

// The properties the interface reads. Each one is observed rather than polled:
// the clock ticks far more often than anything else changes, and a viewer that
// asked the engine for its state every frame would be interrupting the thing it
// is trying to display.
enum Observed:uint64_t{
 ObsTimePos=1,ObsDuration,ObsPause,ObsCoreIdle,ObsSeeking,ObsEof,ObsCacheWait,
 ObsWidth,ObsHeight,ObsFps,ObsVideoCodec,ObsAudioCodec,ObsHwdec,ObsVolume,ObsMute,ObsSpeed,
 ObsDropped,ObsDelayed,ObsAvSync,ObsDisplayFps,ObsDecoderDrops,ObsJitter,ObsTransfer,
 ObsSubText,ObsSubAss,ObsSubStart,ObsSubEnd,ObsSubCodec,ObsSubId,
 ObsNetwork,ObsSeekable,
};

// The pacing vocabulary translated once, here, where mpv is already known.
const char* SyncOption(SyncMode mode){
 switch(mode){
  case SyncMode::DisplayResample:return "display-resample";
  case SyncMode::DisplayDrop:return "display-vdrop";
  case SyncMode::AudioClock:default:return "audio";
 }
}
const wchar_t* SyncName(SyncMode mode){
 switch(mode){
  case SyncMode::DisplayResample:return L"display-resample";
  case SyncMode::DisplayDrop:return L"display-vdrop";
  case SyncMode::AudioClock:default:return L"audio";
 }
}

class MpvEngine final:public IPlaybackEngine{
public:
 MpvEngine(HWND notify,UINT message,const PresentationTarget& target)
  :notify_(notify),message_(message),target_(target),created_(target){}
 ~MpvEngine()override{Destroy();}

 bool Start(std::wstring& error){
  handle_=api.create();
  if(!handle_){error=L"The playback engine could not be created.";return false;}
  // Vetro Look owns the window, the keyboard and the mouse. mpv is a decoder
  // and a renderer here, nothing else: no window, no OSC, no key bindings, no
  // terminal, and no configuration file picked up from the user's mpv install
  // that could change how the viewer behaves.
  const char* options[][2]={
   {"config","no"},
   {"terminal","no"},
   {"osc","no"},
   {"osd-level","0"},
   {"input-default-bindings","no"},
   {"input-vo-keyboard","no"},
   {"input-media-keys","no"},
   {"idle","yes"},
   {"keep-open","yes"},
   {"force-window","no"},
   {"gpu-context","d3d11"},
   {"d3d11-output-mode","composition"},
   {"d3d11-composition-size","1280x720"},
   // Keep the entire film. gpu-next fills any surrounding space with a
   // softened reflection of this same frame, in the existing GPU renderer.
   {"panscan","0.0"},
   {"border-background","blur"},
   {"background-blur-radius","32"},
   // The decode ladder is the engine's own: auto-safe tries the hardware paths
   // that are known good on this machine and falls back to software rather than
   // failing. Which rung it landed on is reported in the snapshot.
   {"hwdec","auto-safe"},
   {"video-sync","audio"},
   {"audio-client-name","Vetro Look"},
   // Named rather than left to chance: WASAPI is what this application is tuned
   // against, and a machine that would otherwise pick something else should not
   // sound different from every other machine.
   {"ao","wasapi"},
   // External subtitles beside the film, found the way a person would expect:
   // movie.srt, movie.ru.srt, movie.en.ass (22.3). The search happens during
   // load and does not hold up the first frame.
   {"sub-auto","fuzzy"},
   {"sub-file-paths","subs:subtitles:Subs:Subtitles"},
   // The viewer draws plain dialogue itself, so the engine starts silent. It is
   // handed the cue back the moment one turns out to be authored typesetting.
   {"sub-visibility","no"},
   {"title","Vetro Look"},
   {"pause","yes"},
   // Network sources (9.3, Appendix G). The engine's built-in page resolver is
   // off: resolving is a separate process with a timeout, not a script inside
   // the playback path. A dropped connection is retried inside the demuxer
   // first, with its own short backoff, before the viewer's reconnect ever sees
   // it; a stall that outlasts the timeout is reported, not waited out forever.
   {"ytdl","no"},
   {"network-timeout","20"},
   {"stream-lavf-o","reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_delay_max=8"},
   // What has been read stays seekable: for a live stream that is the DVR
   // window, and for a network film it is a seek that does not refetch.
   {"demuxer-seekable-cache","yes"},
   {"force-seekable","yes"},
  };
  // Colour has to be stated before the output is built, and in composition mode
  // the engine has no window with which to find any of it out (stage 2's gate
  // result). An HDR display gets a ten-bit PQ output and the panel's own peak;
  // a wide-gamut SDR display gets Rec.709 mapped into what it actually shows;
  // an ordinary display gets the defaults, which are already right for it.
  char peak[32]{};
  std::vector<std::array<const char*,2>> colour;
  if(target_.hdr){
   colour.push_back({"d3d11-output-format","rgb10_a2"});
   colour.push_back({"d3d11-output-csp","pq"});
   colour.push_back({"target-prim","bt.2020"});
   colour.push_back({"target-trc","pq"});
   if(target_.maxNits>0){
    snprintf(peak,sizeof peak,"%.0f",target_.maxNits);
    colour.push_back({"target-peak",peak});
   }
   snapshot_.colourTarget=L"HDR10 (PQ, BT.2020)";
  }else if(target_.wideGamut){
   colour.push_back({"target-prim","display-p3"});
   snapshot_.colourTarget=L"SDR, wide gamut";
  }else snapshot_.colourTarget=L"SDR (sRGB)";
  auto applyOptions=[&]{
   for(const auto& option:options)api.set_option_string(handle_,option[0],option[1]);
   for(const auto& option:colour)api.set_option_string(handle_,option[0],option[1]);
  if(audioExclusive)api.set_option_string(handle_,"audio-exclusive","yes");
  };
  applyOptions();
  // The render ladder, in the order of §8.2: the modern renderer, then the older
  // one. Both present through the same composition output, so a machine that
  // cannot run the first still plays through a path the shell already knows.
  const char* renderers[]={"gpu-next","gpu"};
  int rc=-1;
  for(const char* renderer:renderers){
   if(api.set_option_string(handle_,"vo",renderer)<0)continue;
   rc=api.initialize(handle_);
   if(rc>=0){snapshot_.renderer=Widen(renderer);break;}
   // A failed initialize leaves the handle unusable, so the next rung needs a
   // fresh one.
   api.terminate_destroy(handle_);
   handle_=api.create();
   if(!handle_){error=L"The playback engine could not be created.";return false;}
   applyOptions();
  }
  if(rc<0){
   error=L"The playback engine refused to start: "+Widen(api.error_string(rc));
   if(handle_){api.terminate_destroy(handle_);handle_=nullptr;}
   return false;
  }
  // Warnings and errors only. At "info" the engine narrates every frame it
  // decodes, which would make the log useless for anything else.
  // Requested always, not only with a sink: a stream's failure is classified
  // from these lines (Appendix G), and that must not depend on a debug switch.
  api.request_log_messages(handle_,"warn");
  api.set_wakeup_callback(handle_,&MpvEngine::Wakeup,this);
  Observe();
  return true;
 }

 bool Open(const std::wstring& path)override{
  if(!handle_)return false;
  // A new file, but the same engine: which renderer it settled on and how loud
  // it is were decided when it started and do not reset with the media.
  auto renderer=snapshot_.renderer;
  auto volume=snapshot_.volume;auto muted=snapshot_.muted;
  auto sync=snapshot_.syncMode;auto colour=snapshot_.colourTarget;
  // The video filter is an engine option and carries over to the next film; the
  // shell re-sends its plan for this one, and a refusal is judged per film.
  auto enhancementText=snapshot_.enhancement;
  enhancementSet_=false;
  snapshot_=PlaybackSnapshot{};
  snapshot_.enhancement=enhancementText;
  snapshot_.renderer=renderer;snapshot_.volume=volume;snapshot_.muted=muted;
  snapshot_.syncMode=sync;snapshot_.colourTarget=colour;
  content_=nullptr;
  recentLog_.clear();logChanged_=true;
  liveHint_=false;lastCachePoll_=0;
  firstDuration_=-1;durationGrew_=false;
  auto utf8=Narrow(path);
  const char* command[]={"loadfile",utf8.c_str(),nullptr};
  int rc=api.command(handle_,command);
  if(rc<0){snapshot_.error=Widen(api.error_string(rc));return false;}
  api.set_property_string(handle_,"pause","no");
  return true;
 }
 bool OpenStream(const std::wstring& url,const OpenOptions& options)override{
  if(!handle_)return false;
  auto renderer=snapshot_.renderer;
  auto volume=snapshot_.volume;auto muted=snapshot_.muted;
  auto sync=snapshot_.syncMode;auto colour=snapshot_.colourTarget;
  // The video filter is an engine option and carries over to the next film; the
  // shell re-sends its plan for this one, and a refusal is judged per film.
  auto enhancementText=snapshot_.enhancement;
  enhancementSet_=false;
  snapshot_=PlaybackSnapshot{};
  snapshot_.enhancement=enhancementText;
  snapshot_.renderer=renderer;snapshot_.volume=volume;snapshot_.muted=muted;
  snapshot_.syncMode=sync;snapshot_.colourTarget=colour;
  content_=nullptr;
  recentLog_.clear();logChanged_=true;
  liveHint_=options.live;lastCachePoll_=0;
  firstDuration_=-1;durationGrew_=false;
  // Per-file options travel with the load, so nothing a resolver said about one
  // stream leaks into the next one opened on this engine.
  auto utf8=Narrow(url);
  std::vector<std::pair<std::string,std::string>> values;
  if(!options.audioUrl.empty())values.push_back({"audio-files",Narrow(options.audioUrl)});
  if(!options.title.empty())values.push_back({"force-media-title",Narrow(options.title)});
  if(!options.userAgent.empty())values.push_back({"user-agent",Narrow(options.userAgent)});
  if(!options.referrer.empty())values.push_back({"referrer",Narrow(options.referrer)});
  if(options.startAt>0){
   char start[32];snprintf(start,sizeof start,"%.3f",options.startAt);
   values.push_back({"start",start});
  }
  std::vector<mpv_node> valueNodes(values.size());
  std::vector<char*> keys(values.size());
  for(size_t i=0;i<values.size();i++){
   valueNodes[i].format=MPV_FORMAT_STRING;
   valueNodes[i].u.string=values[i].second.data();
   keys[i]=values[i].first.data();
  }
  mpv_node_list map{int(values.size()),valueNodes.data(),keys.data()};
  char loadfile[]="loadfile",replace[]="replace";
  mpv_node args[5]{};
  args[0].format=MPV_FORMAT_STRING;args[0].u.string=loadfile;
  args[1].format=MPV_FORMAT_STRING;args[1].u.string=utf8.data();
  args[2].format=MPV_FORMAT_STRING;args[2].u.string=replace;
  args[3].format=MPV_FORMAT_INT64;args[3].u.int64=-1;
  args[4].format=MPV_FORMAT_NODE_MAP;args[4].u.list=&map;
  mpv_node_list list{5,args,nullptr};
  mpv_node command{};command.format=MPV_FORMAT_NODE_ARRAY;command.u.list=&list;
  int rc=api.command_node(handle_,&command,nullptr);
  if(rc<0){snapshot_.error=Widen(api.error_string(rc));return false;}
  api.set_property_string(handle_,"pause","no");
  return true;
 }
 void Close()override{
  if(!handle_)return;
  const char* command[]={"stop",nullptr};
  api.command(handle_,command);
  content_=nullptr;
  snapshot_=PlaybackSnapshot{};
 }
 void Play()override{if(handle_)api.set_property_string(handle_,"pause","no");}
 void Pause()override{if(handle_)api.set_property_string(handle_,"pause","yes");}
 void TogglePause()override{snapshot_.paused?Play():Pause();}
 void Seek(double seconds,SeekMode mode)override{
  if(!handle_)return;
  auto target=std::to_string(seconds);
  const char* command[]={"seek",target.c_str(),mode==SeekMode::Exact?"absolute+exact":"absolute",nullptr};
  api.command(handle_,command);
 }
 void SeekBy(double seconds,SeekMode mode)override{
  if(!handle_)return;
  auto target=std::to_string(seconds);
  const char* command[]={"seek",target.c_str(),mode==SeekMode::Exact?"relative+exact":"relative",nullptr};
  api.command(handle_,command);
 }
 // mpv steps by real presentation order, which is what makes this correct for
 // variable frame rate as well: nothing here assumes a frame lasts 1/fps.
 void StepFrame(int direction)override{
  if(!handle_)return;
  if(!snapshot_.paused)api.set_property_string(handle_,"pause","yes");
  const char* forward[]={"frame-step",nullptr};
  const char* backward[]={"frame-back-step",nullptr};
  api.command(handle_,direction>=0?forward:backward);
 }
 void SetVolume(double percent)override{
  if(!handle_)return;
  double value=percent<0?0:(percent>150?150:percent);
  api.set_property(handle_,"volume",MPV_FORMAT_DOUBLE,&value);
 }
 void SetMuted(bool muted)override{if(handle_)api.set_property_string(handle_,"mute",muted?"yes":"no");}
 void SetSpeed(double speed)override{
  if(!handle_)return;
  api.set_property(handle_,"speed",MPV_FORMAT_DOUBLE,&speed);
 }
 void SetVideoZoom(double scale)override{
  if(!handle_)return;
  // mpv's video-zoom is logarithmic: 0 is the fitted picture, 1 is twice its
  // linear size. Exposing a linear scale keeps the shell's wheel behaviour
  // predictable and gives 1.0 a real reset point.
  scale=(std::max)(1.0,(std::min)(scale,8.0));
  double level=std::log2(scale);
  api.set_property(handle_,"video-zoom",MPV_FORMAT_DOUBLE,&level);
 }
 void SetSurfaceSize(unsigned width,unsigned height)override{
  if(!handle_||!width||!height)return;
  if(width==surfaceWidth_&&height==surfaceHeight_)return;
  surfaceWidth_=width;surfaceHeight_=height;
  auto size=std::to_string(width)+"x"+std::to_string(height);
  api.set_property_string(handle_,"d3d11-composition-size",size.c_str());
 }
 void SetBackground(uint8_t r,uint8_t g,uint8_t b)override{
  if(!handle_)return;
  char colour[16];
  snprintf(colour,sizeof colour,"#%02X%02X%02X",r,g,b);
  api.set_property_string(handle_,"background-color",colour);
 }
 IUnknown* PresentationContent()override{return content_;}

 void Pump()override{
  if(!handle_)return;
  while(true){
   mpv_event* event=api.wait_event(handle_,0);
   if(!event||event->event_id==MPV_EVENT_NONE)break;
   switch(event->event_id){
    case MPV_EVENT_PROPERTY_CHANGE:Apply((mpv_event_property*)event->data,event->reply_userdata);break;
    case MPV_EVENT_FILE_LOADED:
     snapshot_.opened=true;snapshot_.error.clear();snapshot_.endReached=false;
     SyncVideoDimensions();
     TakeContent();
     break;
    case MPV_EVENT_VIDEO_RECONFIG:
     snapshot_.hasVideo=true;
     // Property observation is asynchronous. Pull the source dimensions at
     // the reconfiguration boundary as well, so the shell can size the window
     // before the first stable frame instead of waiting for an optional later
     // property-change notification.
     SyncVideoDimensions();
     TakeContent();
     break;
    case MPV_EVENT_SEEK:snapshot_.seeking=true;break;
    case MPV_EVENT_PLAYBACK_RESTART:snapshot_.seeking=false;break;
    case MPV_EVENT_END_FILE:{
     auto* end=(mpv_event_end_file*)event->data;
     if(end->reason==MPV_END_FILE_REASON_ERROR)
      snapshot_.error=Widen(api.error_string(end->error));
     if(end->reason==MPV_END_FILE_REASON_EOF)snapshot_.endReached=true;
     break;
    }
    case MPV_EVENT_LOG_MESSAGE:{
     auto* message=(mpv_event_log_message*)event->data;
     if(message&&message->text){
      std::wstring line=L"["+Widen(message->prefix?message->prefix:"")+L"] "+Widen(message->text);
      while(!line.empty()&&(line.back()==L'\n'||line.back()==L'\r'))line.pop_back();
      if(logSink)logSink(L"engine "+line);
      // A driver that refuses the video processor says so once, here. The film
      // goes on without it, and the viewer does not ask again for this film.
      if(line.find(L"d3d11vpp")!=std::wstring::npos&&!snapshot_.enhancement.empty()&&
         (line.find(L"ail")!=std::wstring::npos||line.find(L"rror")!=std::wstring::npos||
          line.find(L"not supported")!=std::wstring::npos||line.find(L"isabling")!=std::wstring::npos)){
       snapshot_.enhancementFailed=true;
       snapshot_.enhancement.clear();
       api.set_property_string(handle_,"vf","");
      }
      recentLog_.push_back(std::move(line));
      if(recentLog_.size()>16)recentLog_.erase(recentLog_.begin());
      logChanged_=true;
     }
     break;
    }
    case MPV_EVENT_SHUTDOWN:Destroy();return;
    default:break;
   }
  }
  if(logChanged_){
   logChanged_=false;
   snapshot_.failureLog.clear();
   for(const auto& line:recentLog_){snapshot_.failureLog+=line;snapshot_.failureLog+=L'\n';}
  }
  if(snapshot_.network&&snapshot_.opened)PollCache();
  // A network source is live when it has no length, or when its length keeps
  // growing under the playhead: a live HLS playlist is reported as long as its
  // newest segment, and that number moves with the edge. The resolver may have
  // said so already, before the engine could know.
  if(snapshot_.network&&snapshot_.opened&&snapshot_.duration>0){
   if(firstDuration_<0)firstDuration_=snapshot_.duration;
   else if(snapshot_.duration>firstDuration_+1.0)durationGrew_=true;
  }
  snapshot_.live=snapshot_.opened&&snapshot_.network&&
                 (liveHint_||snapshot_.duration<=0||durationGrew_);
 }
 PlaybackSnapshot Snapshot()const override{return snapshot_;}

 std::vector<TrackInfo> Tracks()const override{
  std::vector<TrackInfo> tracks;
  if(!handle_)return tracks;
  mpv_node node{};
  if(api.get_property(handle_,"track-list",MPV_FORMAT_NODE,&node)<0)return tracks;
  if(node.format==MPV_FORMAT_NODE_ARRAY&&node.u.list){
   for(int i=0;i<node.u.list->num;i++){
    const mpv_node& entry=node.u.list->values[i];
    if(entry.format!=MPV_FORMAT_NODE_MAP||!entry.u.list)continue;
    TrackInfo track;
    for(int f=0;f<entry.u.list->num;f++){
     const char* key=entry.u.list->keys[f];
     const mpv_node& value=entry.u.list->values[f];
     if(!strcmp(key,"id")&&value.format==MPV_FORMAT_INT64)track.id=value.u.int64;
     else if(!strcmp(key,"type")&&value.format==MPV_FORMAT_STRING)track.kind=Widen(value.u.string);
     else if(!strcmp(key,"title")&&value.format==MPV_FORMAT_STRING)track.title=Widen(value.u.string);
     else if(!strcmp(key,"lang")&&value.format==MPV_FORMAT_STRING)track.language=Widen(value.u.string);
     else if(!strcmp(key,"codec")&&value.format==MPV_FORMAT_STRING)track.codec=Widen(value.u.string);
     else if(!strcmp(key,"selected")&&value.format==MPV_FORMAT_FLAG)track.selected=value.u.flag!=0;
     else if(!strcmp(key,"external")&&value.format==MPV_FORMAT_FLAG)track.external=value.u.flag!=0;
    }
    tracks.push_back(std::move(track));
   }
  }
  api.free_node_contents(&node);
  return tracks;
 }
 std::vector<std::pair<std::wstring,std::wstring>> AudioDevices()const override{
  std::vector<std::pair<std::wstring,std::wstring>> devices;
  if(!handle_)return devices;
  mpv_node node{};
  if(api.get_property(handle_,"audio-device-list",MPV_FORMAT_NODE,&node)<0)return devices;
  if(node.format==MPV_FORMAT_NODE_ARRAY&&node.u.list){
   for(int i=0;i<node.u.list->num;i++){
    const mpv_node& entry=node.u.list->values[i];
    if(entry.format!=MPV_FORMAT_NODE_MAP||!entry.u.list)continue;
    std::wstring name,description;
    for(int f=0;f<entry.u.list->num;f++){
     const char* key=entry.u.list->keys[f];
     const mpv_node& value=entry.u.list->values[f];
     if(value.format!=MPV_FORMAT_STRING)continue;
     if(!strcmp(key,"name"))name=Widen(value.u.string);
     else if(!strcmp(key,"description"))description=Widen(value.u.string);
    }
    if(!name.empty())devices.emplace_back(name,description.empty()?name:description);
   }
  }
  api.free_node_contents(&node);
  return devices;
 }
 void SetAudioDevice(const std::wstring& id)override{
  if(!handle_)return;
  auto utf8=Narrow(id.empty()?L"auto":id);
  api.set_property_string(handle_,"audio-device",utf8.c_str());
 }
 void SelectTrack(const std::wstring& kind,int64_t id)override{
  if(!handle_)return;
  const char* property=kind==L"audio"?"aid":kind==L"sub"?"sid":"vid";
  if(id<0)api.set_property_string(handle_,property,"no");
  else{
   auto value=std::to_string(id);
   api.set_property_string(handle_,property,value.c_str());
  }
 }

 // ------------------------------------------------------------ quality ------
 // The pacing plan, in mpv's own words. `display-fps-override` is the important
 // one: in composition mode mpv has no window, so without being told it has no
 // idea what the display is doing and display-synced pacing cannot work at all.
 void SetSubtitleRendering(bool engineDraws)override{
  if(!handle_||engineDraws==engineDrawsSubtitles_)return;
  engineDrawsSubtitles_=engineDraws;
  api.set_property_string(handle_,"sub-visibility",engineDraws?"yes":"no");
 }
 void SetLanguagePreference(const std::wstring& subtitles,const std::wstring& audio)override{
  if(!handle_)return;
  if(!subtitles.empty())api.set_property_string(handle_,"slang",Narrow(subtitles).c_str());
  if(!audio.empty())api.set_property_string(handle_,"alang",Narrow(audio).c_str());
 }
 void SetAudioDelay(double seconds)override{
  if(handle_)api.set_property(handle_,"audio-delay",MPV_FORMAT_DOUBLE,&seconds);
 }
 void SetSubtitleDelay(double seconds)override{
  if(handle_)api.set_property(handle_,"sub-delay",MPV_FORMAT_DOUBLE,&seconds);
 }
 std::vector<ChapterInfo> Chapters()const override{
  std::vector<ChapterInfo> chapters;
  if(!handle_)return chapters;
  mpv_node node{};
  if(api.get_property(handle_,"chapter-list",MPV_FORMAT_NODE,&node)<0)return chapters;
  if(node.format==MPV_FORMAT_NODE_ARRAY&&node.u.list){
   for(int i=0;i<node.u.list->num;i++){
    const mpv_node& entry=node.u.list->values[i];
    if(entry.format!=MPV_FORMAT_NODE_MAP||!entry.u.list)continue;
    ChapterInfo chapter;
    for(int f=0;f<entry.u.list->num;f++){
     const char* key=entry.u.list->keys[f];
     const mpv_node& value=entry.u.list->values[f];
     if(!strcmp(key,"time")&&value.format==MPV_FORMAT_DOUBLE)chapter.start=value.u.double_;
     else if(!strcmp(key,"title")&&value.format==MPV_FORMAT_STRING)chapter.title=Widen(value.u.string);
    }
    chapters.push_back(std::move(chapter));
   }
  }
  api.free_node_contents(&node);
  return chapters;
 }
 int CurrentChapter()const override{
  if(!handle_)return -1;
  int64_t index=-1;
  if(api.get_property(handle_,"chapter",MPV_FORMAT_INT64,&index)<0)return -1;
  return int(index);
 }
 bool WriteScreenshot(const std::wstring& path,bool withSubtitles)override{
  if(!handle_)return false;
  auto utf8=Narrow(path);
  // "video" is the frame as decoded, without the viewer's own interface over
  // it; "subtitles" includes whatever the engine drew on top, which is the
  // right answer when the engine is the one drawing them.
  const char* command[]={"screenshot-to-file",utf8.c_str(),withSubtitles?"subtitles":"video",nullptr};
  return api.command(handle_,command)>=0;
 }
 void SetLoop(double from,double to)override{
  if(!handle_)return;
  auto set=[&](const char* name,double value){
   if(value<0){api.set_property_string(handle_,name,"no");return;}
   char text[32];snprintf(text,sizeof text,"%.3f",value);
   api.set_property_string(handle_,name,text);
  };
  set("ab-loop-a",from);
  set("ab-loop-b",from<0?-1:to);
 }
 void SetPresentationPlan(const PresentationPlan& plan)override{
  if(!handle_)return;
  bool first=!planSet_;
  if(!first&&plan.sync==plan_.sync&&plan.interpolate==plan_.interpolate&&
     plan.displayHz==plan_.displayHz&&plan.preferEfficiency==plan_.preferEfficiency)return;
  bool rateChanged=first||plan.displayHz!=plan_.displayHz;
  bool syncChanged=first||plan.sync!=plan_.sync;
  bool smoothChanged=first||plan.interpolate!=plan_.interpolate;
  bool costChanged=first||plan.preferEfficiency!=plan_.preferEfficiency;
  plan_=plan;planSet_=true;
  if(rateChanged&&plan.displayHz>0){
   char rate[32];snprintf(rate,sizeof rate,"%.4f",plan.displayHz);
   api.set_property_string(handle_,"display-fps-override",rate);
  }
  // Order matters on the way down: stop interpolating before leaving display
  // sync, or one frame is asked to blend against a cadence that no longer exists.
  if(smoothChanged&&!plan.interpolate)api.set_property_string(handle_,"interpolation","no");
  if(syncChanged)api.set_property_string(handle_,"video-sync",SyncOption(plan.sync));
  if(smoothChanged&&plan.interpolate)api.set_property_string(handle_,"interpolation","yes");
  if(costChanged){
   // Efficiency is paid for in scaling quality, which only costs anything when
   // the film is not being shown at its own size -- and is fully reversible,
   // which a profile would not be.
   const char* scale=plan.preferEfficiency?"bilinear":"spline36";
   const char* down=plan.preferEfficiency?"bilinear":"mitchell";
   api.set_property_string(handle_,"scale",scale);
   api.set_property_string(handle_,"cscale",scale);
   api.set_property_string(handle_,"dscale",down);
   api.set_property_string(handle_,"dither-depth",plan.preferEfficiency?"no":"auto");
  }
  snapshot_.syncMode=SyncName(plan.sync);
  if(plan.interpolate)snapshot_.syncMode+=L" + interpolation";
 }
 // RTX Video Super Resolution through the Direct3D 11 video processor filter: the
 // decoded texture is scaled on the GPU by the driver's own model before the
 // renderer sees it. No vendor SDK is linked; the driver decides whether it runs.
 void SetEnhancement(const EnhancementPlan& plan)override{
  if(!handle_)return;
  if(enhancementSet_&&plan==enhancement_)return;
  enhancementSet_=true;
  enhancement_=plan;
  if(plan.superResolution&&!snapshot_.enhancementFailed){
   char filter[96];
   snprintf(filter,sizeof filter,"d3d11vpp=scaling-mode=nvidia:scale=%.2f",plan.scale);
   int rc=api.set_property_string(handle_,"vf",filter);
   if(rc<0){
    snapshot_.enhancementFailed=true;
    snapshot_.enhancement.clear();
    api.set_property_string(handle_,"vf","");
    return;
   }
   wchar_t text[64];
   swprintf_s(text,L"RTX Video Super Resolution, x%.2f",plan.scale);
   snapshot_.enhancement=text;
  }else{
   api.set_property_string(handle_,"vf","");
   snapshot_.enhancement.clear();
  }
 }
 // Colour, not timing. The parts that can be retuned while the engine runs are
 // applied at once; a change of output colour space cannot be, because the
 // output was built for the one it has. That is reported rather than faked.
 void SetPresentationTarget(const PresentationTarget& target)override{
  if(!handle_||target==target_)return;
  bool pipeline=target.hdr!=created_.hdr||target.wideGamut!=created_.wideGamut;
  target_=target;
  if(target.hdr&&target.maxNits>0){
   char peak[32];snprintf(peak,sizeof peak,"%.0f",target.maxNits);
   api.set_property_string(handle_,"target-peak",peak);
  }
  if(!target.hdr&&!created_.hdr)
   api.set_property_string(handle_,"target-prim",target.wideGamut?"display-p3":"auto");
  snapshot_.targetPending=pipeline;
 }
 // §14.2: every queue bounded, and bounded by what this machine has rather than
 // by a number chosen once by somebody else.
 void SetStreamBudget(const StreamBudget& budget)override{
  if(!handle_)return;
  if(budget.forwardBytes==budget_.forwardBytes&&budget.backBytes==budget_.backBytes&&
     budget.seconds==budget_.seconds)return;
  budget_=budget;
  auto bytes=[&](const char* name,uint64_t value){
   if(!value)return;
   api.set_property_string(handle_,name,std::to_string(value).c_str());
  };
  bytes("demuxer-max-bytes",budget.forwardBytes);
  bytes("demuxer-max-back-bytes",budget.backBytes);
  if(budget.seconds>0){
   char seconds[32];snprintf(seconds,sizeof seconds,"%.1f",budget.seconds);
   api.set_property_string(handle_,"cache-secs",seconds);
  }
 }

private:
 void SyncVideoDimensions(){
  if(!handle_)return;
  int64_t width=0,height=0;
  if(api.get_property(handle_,"video-params/w",MPV_FORMAT_INT64,&width)>=0&&width>0)
   snapshot_.width=unsigned(width);
  if(api.get_property(handle_,"video-params/h",MPV_FORMAT_INT64,&height)>=0&&height>0)
   snapshot_.height=unsigned(height);
 }
 static void Wakeup(void* context){
  auto* self=(MpvEngine*)context;
  if(self->notify_)PostMessageW(self->notify_,self->message_,0,0);
 }
 void Observe(){
  struct Entry{uint64_t id;const char* name;mpv_format format;};
  const Entry entries[]={
   {ObsTimePos,"time-pos",MPV_FORMAT_DOUBLE},
   {ObsDuration,"duration",MPV_FORMAT_DOUBLE},
   {ObsPause,"pause",MPV_FORMAT_FLAG},
   {ObsCoreIdle,"core-idle",MPV_FORMAT_FLAG},
   {ObsSeeking,"seeking",MPV_FORMAT_FLAG},
   {ObsEof,"eof-reached",MPV_FORMAT_FLAG},
   {ObsCacheWait,"paused-for-cache",MPV_FORMAT_FLAG},
   {ObsWidth,"video-params/w",MPV_FORMAT_INT64},
   {ObsHeight,"video-params/h",MPV_FORMAT_INT64},
   {ObsFps,"container-fps",MPV_FORMAT_DOUBLE},
   {ObsVideoCodec,"video-codec",MPV_FORMAT_STRING},
   {ObsAudioCodec,"audio-codec-name",MPV_FORMAT_STRING},
   {ObsHwdec,"hwdec-current",MPV_FORMAT_STRING},
   {ObsVolume,"volume",MPV_FORMAT_DOUBLE},
   {ObsMute,"mute",MPV_FORMAT_FLAG},
   {ObsSpeed,"speed",MPV_FORMAT_DOUBLE},
   // Timing. Two kinds of loss, because they have two different answers: a
   // frame the decoder gave up on and a frame the output showed late.
   {ObsDropped,"frame-drop-count",MPV_FORMAT_INT64},
   {ObsDelayed,"vo-delayed-frame-count",MPV_FORMAT_INT64},
   {ObsAvSync,"avsync",MPV_FORMAT_DOUBLE},
   {ObsDisplayFps,"estimated-vf-fps",MPV_FORMAT_DOUBLE},
   {ObsDecoderDrops,"decoder-frame-drop-count",MPV_FORMAT_INT64},
   {ObsJitter,"vsync-jitter",MPV_FORMAT_DOUBLE},
   {ObsTransfer,"video-params/gamma",MPV_FORMAT_STRING},
   // Subtitles are observed like everything else. The cue on screen changes a
   // few times a minute; asking for it per frame would be absurd.
   {ObsSubText,"sub-text",MPV_FORMAT_STRING},
   {ObsSubAss,"sub-text/ass",MPV_FORMAT_STRING},
   {ObsSubStart,"sub-start",MPV_FORMAT_DOUBLE},
   {ObsSubEnd,"sub-end",MPV_FORMAT_DOUBLE},
   {ObsSubCodec,"current-tracks/sub/codec",MPV_FORMAT_STRING},
   {ObsSubId,"sid",MPV_FORMAT_INT64},
   {ObsNetwork,"demuxer-via-network",MPV_FORMAT_FLAG},
   {ObsSeekable,"seekable",MPV_FORMAT_FLAG},
  };
  for(const auto& entry:entries)api.observe_property(handle_,entry.id,entry.name,entry.format);
 }
 void Apply(mpv_event_property* property,uint64_t id){
  // A length that becomes unavailable is information, not noise: it is how a
  // live stream is told apart from a film.
  if(property&&id==ObsDuration&&property->format==MPV_FORMAT_NONE){snapshot_.duration=0;return;}
  if(!property||!property->data)return;
  auto number=[&]{return *(double*)property->data;};
  auto flag=[&]{return *(int*)property->data!=0;};
  auto integer=[&]{return *(int64_t*)property->data;};
  auto text=[&]{return Widen(*(char**)property->data);};
  switch(id){
   case ObsTimePos:  if(property->format==MPV_FORMAT_DOUBLE)snapshot_.position=number();break;
   case ObsDuration: if(property->format==MPV_FORMAT_DOUBLE)snapshot_.duration=number();break;
   case ObsPause:    if(property->format==MPV_FORMAT_FLAG)snapshot_.paused=flag();break;
   case ObsCoreIdle: break;
   case ObsSeeking:  if(property->format==MPV_FORMAT_FLAG)snapshot_.seeking=flag();break;
   case ObsEof:      if(property->format==MPV_FORMAT_FLAG)snapshot_.endReached=flag();break;
   case ObsCacheWait:if(property->format==MPV_FORMAT_FLAG)snapshot_.buffering=flag();break;
   case ObsWidth:    if(property->format==MPV_FORMAT_INT64)snapshot_.width=unsigned(integer());break;
   case ObsHeight:   if(property->format==MPV_FORMAT_INT64)snapshot_.height=unsigned(integer());break;
   case ObsFps:      if(property->format==MPV_FORMAT_DOUBLE)snapshot_.frameRate=number();break;
   case ObsVideoCodec:if(property->format==MPV_FORMAT_STRING)snapshot_.videoCodec=text();break;
   case ObsAudioCodec:if(property->format==MPV_FORMAT_STRING)snapshot_.audioCodec=text();break;
   case ObsHwdec:    if(property->format==MPV_FORMAT_STRING)snapshot_.hwdec=text();break;
   case ObsVolume:   if(property->format==MPV_FORMAT_DOUBLE)snapshot_.volume=number();break;
   case ObsMute:     if(property->format==MPV_FORMAT_FLAG)snapshot_.muted=flag();break;
   case ObsSpeed:    if(property->format==MPV_FORMAT_DOUBLE)snapshot_.speed=number();break;
   case ObsDropped:  if(property->format==MPV_FORMAT_INT64)snapshot_.droppedFrames=integer();break;
   case ObsDelayed:  if(property->format==MPV_FORMAT_INT64)snapshot_.delayedFrames=integer();break;
   case ObsAvSync:   if(property->format==MPV_FORMAT_DOUBLE)snapshot_.avSync=number();break;
   case ObsDisplayFps:if(property->format==MPV_FORMAT_DOUBLE)snapshot_.displayFps=number();break;
   case ObsDecoderDrops:if(property->format==MPV_FORMAT_INT64)snapshot_.decoderDrops=integer();break;
   case ObsJitter:  if(property->format==MPV_FORMAT_DOUBLE)snapshot_.vsyncJitter=number();break;
   case ObsTransfer:if(property->format==MPV_FORMAT_STRING)snapshot_.transfer=text();break;
   case ObsSubText: snapshot_.subtitleText=property->format==MPV_FORMAT_STRING?text():std::wstring();break;
   case ObsSubAss:  snapshot_.subtitleAss=property->format==MPV_FORMAT_STRING?text():std::wstring();break;
   case ObsSubStart:if(property->format==MPV_FORMAT_DOUBLE)snapshot_.subtitleStart=number();break;
   case ObsSubEnd:  if(property->format==MPV_FORMAT_DOUBLE)snapshot_.subtitleEnd=number();break;
   case ObsSubCodec:snapshot_.subtitleCodec=property->format==MPV_FORMAT_STRING?text():std::wstring();break;
   case ObsSubId:   snapshot_.subtitleTrack=property->format==MPV_FORMAT_INT64&&integer()>0;break;
   case ObsNetwork: if(property->format==MPV_FORMAT_FLAG)snapshot_.network=flag();break;
   case ObsSeekable:if(property->format==MPV_FORMAT_FLAG)snapshot_.seekable=flag();break;
   default:break;
  }
 }
 // The swapchain mpv rendered into, once it exists. It belongs to mpv: the
 // shell borrows it as composition content and must let go of it before this
 // engine is destroyed.
 void TakeContent(){
  if(content_||!handle_)return;
  int64_t value=0;
  if(api.get_property(handle_,"display-swapchain",MPV_FORMAT_INT64,&value)<0||!value)return;
  content_=(IUnknown*)(intptr_t)value;
 }
 // What the demuxer holds for a network source: how far ahead it has read, and
 // the seekable window -- for a live stream, the DVR range (Appendix G.1).
 // Twice a second is plenty for a number a person reads.
 void PollCache(){
  auto now=GetTickCount64();
  if(now-lastCachePoll_<500)return;
  lastCachePoll_=now;
  // Absent means nothing is held, not "whatever was held last time": after a
  // failed read the key can vanish, and a stale ten seconds would read as health.
  snapshot_.cacheAhead=0;
  mpv_node node{};
  if(api.get_property(handle_,"demuxer-cache-state",MPV_FORMAT_NODE,&node)<0)return;
  auto number=[](const mpv_node& value,double& into){
   if(value.format==MPV_FORMAT_DOUBLE)into=value.u.double_;
   else if(value.format==MPV_FORMAT_INT64)into=double(value.u.int64);
  };
  if(node.format==MPV_FORMAT_NODE_MAP&&node.u.list){
   for(int i=0;i<node.u.list->num;i++){
    const char* key=node.u.list->keys[i];
    const mpv_node& value=node.u.list->values[i];
    if(!strcmp(key,"cache-duration"))number(value,snapshot_.cacheAhead);
    if(!strcmp(key,"seekable-ranges")&&value.format==MPV_FORMAT_NODE_ARRAY&&value.u.list&&value.u.list->num>0){
     const mpv_node& last=value.u.list->values[value.u.list->num-1];
     if(last.format==MPV_FORMAT_NODE_MAP&&last.u.list){
      for(int j=0;j<last.u.list->num;j++){
       if(!strcmp(last.u.list->keys[j],"start"))number(last.u.list->values[j],snapshot_.liveStart);
       if(!strcmp(last.u.list->keys[j],"end"))number(last.u.list->values[j],snapshot_.liveEnd);
      }
     }
    }
   }
  }
  api.free_node_contents(&node);
 }
 void Destroy(){
  if(!handle_)return;
  api.set_wakeup_callback(handle_,nullptr,nullptr);
  content_=nullptr;
  api.terminate_destroy(handle_);
  handle_=nullptr;
 }

 HWND notify_=nullptr;UINT message_=0;
 mpv_handle* handle_=nullptr;
 IUnknown* content_=nullptr;
 unsigned surfaceWidth_=0,surfaceHeight_=0;
 // `created_` is the display this engine's output was built for; `target_` is
 // the display it is looking at now. They differ only when the window has moved
 // to a screen with different colour, which is what `targetPending` reports.
 PresentationTarget target_,created_;
 PresentationPlan plan_;bool planSet_=false;
 bool engineDrawsSubtitles_=false;
 StreamBudget budget_;
 PlaybackSnapshot snapshot_;
 std::vector<std::wstring> recentLog_;
 bool logChanged_=false;
 bool liveHint_=false;
 uint64_t lastCachePoll_=0;
 double firstDuration_=-1;
 bool durationGrew_=false;
 EnhancementPlan enhancement_;
 bool enhancementSet_=false;
};

}

std::unique_ptr<IPlaybackEngine> CreatePlaybackEngine(HWND notify,UINT message,
 const PresentationTarget& target,std::wstring& error){
 if(!Ready()){error=loadError;return nullptr;}
 auto engine=std::make_unique<MpvEngine>(notify,message,target);
 if(!engine->Start(error))return nullptr;
 return engine;
}
void PlaybackSetLogSink(void(*sink)(const std::wstring&)){logSink=sink;}
void PlaybackSetAudioExclusive(bool exclusive){audioExclusive=exclusive;}
bool PlaybackEngineLoaded(){return library!=nullptr;}
HMODULE PlaybackEngineModule(){return library;}
bool PlaybackEnsureLibrary(std::wstring& error){
 if(Ready())return true;
 error=loadError;
 return false;
}
std::wstring PlaybackEngineVersion(){return version;}
