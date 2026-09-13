// Vetro Look, GPL-3.0-or-later. See preview.h for the three rules this keeps.
#include "preview.h"
#include "playback.h"
#include <mpv/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace{

// The entry points this file needs, bound from the library the playback engine
// already loaded. Which library that is, and whether it is new enough, is
// decided once in mpvengine.cpp; nothing is loaded here.
struct Api{
 mpv_handle* (*create)(void);
 int (*initialize)(mpv_handle*);
 void (*terminate_destroy)(mpv_handle*);
 int (*set_option_string)(mpv_handle*,const char*,const char*);
 int (*set_property_string)(mpv_handle*,const char*,const char*);
 int (*command)(mpv_handle*,const char**);
 int (*command_node)(mpv_handle*,mpv_node*,mpv_node*);
 mpv_event* (*wait_event)(mpv_handle*,double);
 void (*free_node_contents)(mpv_node*);
};
Api api{};
bool apiReady=false;

template<class T> bool Bind(HMODULE module,T& slot,const char* name){
 slot=(T)GetProcAddress(module,name);
 return slot!=nullptr;
}
bool EnsureApi(){
 if(apiReady)return true;
 std::wstring error;
 PlaybackEnsureLibrary(error);
 HMODULE module=PlaybackEngineModule();
 if(!module)return false;
 apiReady=
  Bind(module,api.create,"mpv_create")&&
  Bind(module,api.initialize,"mpv_initialize")&&
  Bind(module,api.terminate_destroy,"mpv_terminate_destroy")&&
  Bind(module,api.set_option_string,"mpv_set_option_string")&&
  Bind(module,api.set_property_string,"mpv_set_property_string")&&
  Bind(module,api.command,"mpv_command")&&
  Bind(module,api.command_node,"mpv_command_node")&&
  Bind(module,api.wait_event,"mpv_wait_event")&&
  Bind(module,api.free_node_contents,"mpv_free_node_contents");
 return apiReady;
}

std::string Narrow(const std::wstring& text){
 if(text.empty())return {};
 int length=WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);
 std::string out(length?length-1:0,'\0');
 if(length>1)WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,out.data(),length,nullptr,nullptr);
 return out;
}

// ------------------------------------------------------------- the state ----
std::mutex mx;
std::condition_variable cv;
std::thread worker;
bool running=false,stopping=false;

std::wstring mediaPath,mediaSignature;
double mediaDuration=0,granularity=1;
uint64_t mediaGeneration=0;        // bumped by every open and close
HWND notifyWindow=nullptr;UINT notifyMessage=0;

struct Request{
 uint64_t media=0;
 int64_t bucket=0;
 double time=0;                    // the bucket's own position, in seconds
 unsigned maxEdge=240;
 std::wstring path;                // taken under the lock, used without it
};
Request pending;bool hasPending=false;
std::atomic<bool> busy{false};
std::atomic<bool> available{true};
std::atomic<bool> policyAllowed{true};
std::atomic<int> policyCoalesceMs{16};
std::atomic<int> policyScalePercent{100};

// The session cache. Keyed by bucket, because that is the only position the
// engine is ever asked for, and bounded by bytes rather than by a count: a
// frame from a 4K film is nine times the size of one from a 720p film and a
// count would budget them the same.
struct CacheEntry{std::shared_ptr<PreviewFrame> frame;uint64_t used=0;};
std::unordered_map<int64_t,CacheEntry> cache;
size_t cacheBytes=0,cacheBudget=24u<<20;
uint64_t cacheClock=0;

void CacheClearLocked(){cache.clear();cacheBytes=0;}
void CacheTrimLocked(){
 while(cacheBytes>cacheBudget&&cache.size()>1){
  auto oldest=cache.begin();
  for(auto it=cache.begin();it!=cache.end();++it)
   if(it->second.used<oldest->second.used)oldest=it;
  if(oldest->second.frame)cacheBytes-=oldest->second.frame->bgra.size();
  cache.erase(oldest);
 }
}
void CachePutLocked(int64_t bucket,std::shared_ptr<PreviewFrame> frame){
 if(!frame)return;
 auto existing=cache.find(bucket);
 if(existing!=cache.end()&&existing->second.frame)cacheBytes-=existing->second.frame->bgra.size();
 cacheBytes+=frame->bgra.size();
 cache[bucket]={std::move(frame),++cacheClock};
 CacheTrimLocked();
}

// ------------------------------------------------------------- decoding ----
// The preview decoder. Deliberately the smallest thing that can answer "what
// frame is at this second": no audio, no subtitles, no hardware decoder to
// contend with the film's, and a scaler in the filter chain so a 4K frame is
// never copied at 4K just to be shown two centimetres wide.
mpv_handle* decoder=nullptr;
std::wstring decoderPath;
unsigned decoderWidth=0;
double lastUse=0;

double Now(){return double(GetTickCount64())/1000.0;}

void DestroyDecoder(){
 if(!decoder)return;
 api.terminate_destroy(decoder);
 decoder=nullptr;decoderPath.clear();decoderWidth=0;
}

bool ConfigureDecoder(mpv_handle* handle,unsigned width,const char* renderer){
 const char* options[][2]={
  {"config","no"},
  {"terminal","no"},
  {"osc","no"},
  {"osd-level","0"},
  {"input-default-bindings","no"},
  {"input-vo-keyboard","no"},
  {"audio","no"},
  {"sub","no"},
  {"sub-auto","no"},
  {"ytdl","no"},
  {"idle","yes"},
  {"keep-open","yes"},
  {"pause","yes"},
  {"force-window","no"},
  // Software decode on purpose: the hardware decoder belongs to the film, and
  // a preview that takes it away is a preview that costs frames (12.3).
  {"hwdec","no"},
  {"vd-lavc-threads","2"},
  {"vd-lavc-skiploopfilter","all"},
  {"vd-lavc-fast","yes"},
  {"demuxer-max-bytes","16MiB"},
  {"demuxer-max-back-bytes","2MiB"},
  {"cache","no"},
  {"hr-seek","no"},
  {"video-sync","audio"},
 };
 for(const auto& option:options)api.set_option_string(handle,option[0],option[1]);
 api.set_option_string(handle,"vo",renderer);
 char filter[64];
 snprintf(filter,sizeof filter,"scale=%u:-2",width);
 api.set_option_string(handle,"vf",filter);
 return api.initialize(handle)>=0;
}

// Drains the decoder's events until `wanted` arrives or the wait runs out. The
// preview decoder has no other reader, so nothing is lost by discarding them.
bool WaitFor(mpv_event_id wanted,double seconds){
 double deadline=Now()+seconds;
 while(Now()<deadline){
  mpv_event* event=api.wait_event(decoder,0.05);
  if(!event)break;
  if(event->event_id==wanted)return true;
  if(event->event_id==MPV_EVENT_SHUTDOWN)return false;
  if(event->event_id==MPV_EVENT_END_FILE){
   auto* end=(mpv_event_end_file*)event->data;
   if(end&&end->reason==MPV_END_FILE_REASON_ERROR)return false;
  }
 }
 return false;
}

bool EnsureDecoder(const std::wstring& path,unsigned width){
 if(decoder&&decoderPath==path&&decoderWidth==width)return true;
 if(decoder&&(decoderPath!=path||decoderWidth!=width))DestroyDecoder();
 if(!EnsureApi())return false;
 // Two rungs, for the same reason the film has two: an output that refuses to
 // hand back a frame is not a reason to have no previews.
 const char* renderers[]={"null","gpu"};
 for(const char* renderer:renderers){
  mpv_handle* handle=api.create();
  if(!handle)return false;
  if(!ConfigureDecoder(handle,width,renderer)){api.terminate_destroy(handle);continue;}
  decoder=handle;decoderPath=path;decoderWidth=width;
  auto utf8=Narrow(path);
  const char* command[]={"loadfile",utf8.c_str(),nullptr};
  if(api.command(decoder,command)<0||!WaitFor(MPV_EVENT_FILE_LOADED,6.0)){
   DestroyDecoder();
   continue;
  }
  return true;
 }
 return false;
}

// One frame, at the nearest useful keyframe to `seconds`. Keyframe accuracy is
// the right trade here: a preview is a question about where you are going, and
// a tenth of a second of imprecision in the answer is invisible while the wait
// for an exact decode is not (20.1).
std::shared_ptr<PreviewFrame> Decode(double seconds){
 if(!decoder)return nullptr;
 char target[32];snprintf(target,sizeof target,"%.3f",seconds<0?0:seconds);
 const char* seek[]={"seek",target,"absolute+keyframes",nullptr};
 if(api.command(decoder,seek)<0)return nullptr;
 if(!WaitFor(MPV_EVENT_PLAYBACK_RESTART,4.0))return nullptr;

 mpv_node values[2]{};
 values[0].format=MPV_FORMAT_STRING;values[0].u.string=(char*)"screenshot-raw";
 values[1].format=MPV_FORMAT_STRING;values[1].u.string=(char*)"video";
 mpv_node_list list{};list.num=2;list.values=values;
 mpv_node args{};args.format=MPV_FORMAT_NODE_ARRAY;args.u.list=&list;
 mpv_node result{};
 if(api.command_node(decoder,&args,&result)<0)return nullptr;

 std::shared_ptr<PreviewFrame> frame;
 if(result.format==MPV_FORMAT_NODE_MAP&&result.u.list){
  int64_t w=0,h=0,stride=0;
  const char* format=nullptr;
  const mpv_byte_array* data=nullptr;
  for(int i=0;i<result.u.list->num;i++){
   const char* key=result.u.list->keys[i];
   const mpv_node& value=result.u.list->values[i];
   if(!strcmp(key,"w")&&value.format==MPV_FORMAT_INT64)w=value.u.int64;
   else if(!strcmp(key,"h")&&value.format==MPV_FORMAT_INT64)h=value.u.int64;
   else if(!strcmp(key,"stride")&&value.format==MPV_FORMAT_INT64)stride=value.u.int64;
   else if(!strcmp(key,"format")&&value.format==MPV_FORMAT_STRING)format=value.u.string;
   else if(!strcmp(key,"data")&&value.format==MPV_FORMAT_BYTE_ARRAY)data=value.u.ba;
  }
  // The only format worth handling is the one the viewer draws in. Anything
  // else means a build of the library that does not agree with this one, and a
  // wrongly-coloured preview is worse than none.
  bool usable=w>0&&h>0&&data&&data->data&&stride>=w*4&&
              format&&(!strcmp(format,"bgr0")||!strcmp(format,"bgra"));
  if(usable){
   frame=std::make_shared<PreviewFrame>();
   frame->width=unsigned(w);frame->height=unsigned(h);frame->time=seconds;frame->exact=true;
   frame->bgra.resize(size_t(w)*size_t(h)*4);
   const uint8_t* source=(const uint8_t*)data->data;
   for(int64_t row=0;row<h;row++)
    memcpy(frame->bgra.data()+size_t(row)*size_t(w)*4,source+size_t(row)*size_t(stride),size_t(w)*4);
   // A screenshot in bgr0 has no alpha to speak of; the viewer draws it as an
   // opaque image, so the fourth byte is made opaque rather than left as junk.
   for(size_t i=3;i<frame->bgra.size();i+=4)frame->bgra[i]=255;
  }
 }
 api.free_node_contents(&result);
 return frame;
}

void Run(){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 while(true){
  Request request;
  uint64_t generation=0;
  {
   std::unique_lock lock(mx);
   // Nothing to do: let the decoder go after a while. It is the second largest
   // thing this application can hold open, and a timeline nobody is touching
   // has no use for it (12.3, P2).
   cv.wait_for(lock,std::chrono::milliseconds(2000),[]{return stopping||hasPending;});
   if(stopping)break;
   if(!hasPending){
    if(decoder&&Now()-lastUse>20.0)DestroyDecoder();
    continue;
   }
   request=pending;hasPending=false;generation=mediaGeneration;
   lastUse=Now();
  }
  // Coalescing happens here rather than at the call site: a pointer crossing a
  // timeline produces requests far faster than any decoder answers them, and
  // the only one worth decoding is the one the pointer stopped on (19.4).
  int settle=policyCoalesceMs.load();
  if(settle>0){
   Sleep(DWORD(settle));
   std::lock_guard lock(mx);
   if(hasPending)continue;          // the pointer moved on; take the newer one
  }
  busy.store(true);
  bool ok=EnsureDecoder(request.path,request.maxEdge);
  std::shared_ptr<PreviewFrame> frame;
  if(ok)frame=Decode(request.time);
  busy.store(false);
  {
   std::lock_guard lock(mx);
   available.store(ok);
   // Latest wins, and the film may even have changed underneath: a frame whose
   // media generation has moved on is not drawn, it is dropped here (19.3).
   if(generation==mediaGeneration&&frame){
    frame->time=request.time;
    CachePutLocked(request.bucket,frame);
   }
  }
  if(frame&&notifyWindow)PostMessageW(notifyWindow,notifyMessage,0,0);
 }
 DestroyDecoder();
 CoUninitialize();
}
}

// ------------------------------------------------------------ the public ----
double PreviewGranularity(double duration){
 // Roughly two hundred buckets across any film: enough that a dragged pointer
 // always lands on something, few enough that a six-hour recording does not ask
 // for twenty thousand decodes (19.6).
 if(duration<=0)return 1;
 double step=duration/200.0;
 const double steps[]={0.5,1,2,5,10,15,30,60,120};
 for(double candidate:steps)if(step<=candidate)return candidate;
 return 300;
}
int64_t PreviewBucket(double seconds,double granularity){
 if(granularity<=0)return 0;
 if(seconds<0)seconds=0;
 return int64_t(seconds/granularity+0.5);
}
double PreviewBucketTime(int64_t bucket,double granularity){
 return double(bucket)*granularity;
}

void PreviewOpen(const std::wstring& path,const std::wstring& signature,double duration,
                 HWND notify,UINT message){
 std::lock_guard lock(mx);
 bool sameMedia=!signature.empty()&&signature==mediaSignature;
 mediaPath=path;mediaSignature=signature;mediaDuration=duration;
 granularity=PreviewGranularity(duration);
 notifyWindow=notify;notifyMessage=message;
 mediaGeneration++;
 hasPending=false;
 available.store(true);
 // Reopening the same file -- a rebuilt engine, a return to the same film --
 // keeps what was already decoded. A different film does not.
 if(!sameMedia)CacheClearLocked();
 if(!running&&!stopping){running=true;worker=std::thread(Run);}
}

void PreviewClose(){
 std::lock_guard lock(mx);
 mediaGeneration++;
 hasPending=false;
 mediaPath.clear();mediaSignature.clear();mediaDuration=0;
 CacheClearLocked();
}

void PreviewStop(){
 {
  std::lock_guard lock(mx);
  if(!running)return;
  stopping=true;hasPending=false;
 }
 cv.notify_all();
 if(worker.joinable())worker.join();
 std::lock_guard lock(mx);
 running=false;stopping=false;
 CacheClearLocked();
}

int64_t PreviewRequest(double seconds,unsigned maxEdge){
 std::lock_guard lock(mx);
 if(mediaPath.empty())return 0;
 int64_t bucket=PreviewBucket(seconds,granularity);
 if(!policyAllowed.load())return bucket;        // cached frames still show
 if(cache.find(bucket)!=cache.end()){
  cache[bucket].used=++cacheClock;
  return bucket;
 }
 unsigned scaled=unsigned(maxEdge*policyScalePercent.load()/100);
 pending={mediaGeneration,bucket,PreviewBucketTime(bucket,granularity),(std::max)(96u,scaled),mediaPath};
 hasPending=true;
 cv.notify_one();
 return bucket;
}

std::shared_ptr<PreviewFrame> PreviewBest(double seconds){
 std::lock_guard lock(mx);
 if(cache.empty())return nullptr;
 int64_t bucket=PreviewBucket(seconds,granularity);
 auto exact=cache.find(bucket);
 if(exact!=cache.end()){exact->second.used=++cacheClock;return exact->second.frame;}
 // Progressive display (19.5): a neighbour within a couple of buckets is a
 // truthful enough answer to show immediately, and it is replaced in place the
 // moment the exact one lands. Further away than that and a still from the
 // wrong scene would be a lie, so nothing is shown.
 for(int64_t distance=1;distance<=2;distance++){
  for(int64_t side:{-distance,distance}){
   auto neighbour=cache.find(bucket+side);
   if(neighbour!=cache.end()&&neighbour->second.frame){
    auto frame=std::make_shared<PreviewFrame>(*neighbour->second.frame);
    frame->exact=false;
    return frame;
   }
  }
 }
 return nullptr;
}

bool PreviewAvailable(){return available.load();}
bool PreviewBusy(){return busy.load();}

void PreviewSetPolicy(bool allowed,float scale,int coalesceMs){
 policyAllowed.store(allowed);
 policyScalePercent.store((std::max)(25,(std::min)(100,int(scale*100.f+0.5f))));
 policyCoalesceMs.store((std::max)(0,(std::min)(400,coalesceMs)));
}
void PreviewSetMemoryBudget(size_t bytes){
 std::lock_guard lock(mx);
 cacheBudget=(std::max)(size_t(4u<<20),bytes);
 CacheTrimLocked();
}
size_t PreviewCacheBytes(){
 std::lock_guard lock(mx);
 return cacheBytes;
}
