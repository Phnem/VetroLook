// Vetro Look, GPL-3.0-or-later.
// See ai.h. The only file in the application that knows whisper.cpp exists.
#include "ai.h"
#include "playback.h"
#include <mpv/client.h>
#include <whisper.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs=std::filesystem;

namespace{

// ------------------------------------------------------------ constants -----
// One model, named and checked exactly (Appendix U: populated from what ships,
// not from marketing text). A file that does not hash to this is not used.
constexpr const wchar_t* ModelFile=L"ggml-large-v3-turbo-q5_0.bin";
constexpr const wchar_t* ModelUrl=L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin";
constexpr const char* ModelSha256="394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2";
constexpr uint64_t ModelBytesExpected=574041195ull;
constexpr const wchar_t* VadFile=L"ggml-silero-v6.2.0.bin";
constexpr const wchar_t* VadUrl=L"https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin";
constexpr const char* VadSha256="2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987";
constexpr uint64_t VadBytesExpected=900000ull;
// The runtime: the official whisper.cpp release build with CUDA 12.4, checked
// against the SHA-256 GitHub publishes for it. Only the libraries the player
// loads are unpacked from it; the tools and samples in the archive are not.
constexpr const wchar_t* RuntimeUrl=L"https://github.com/ggml-org/whisper.cpp/releases/download/b5130/whisper-cublas-12.4.0-bin-x64.zip";
constexpr const wchar_t* RuntimeArchive=L"whisper-cublas-12.4.0-bin-x64.zip";
constexpr const char* RuntimeSha256="af520ddd034d985b55dfeea3e465ed93653ba2aee1a55e865033edc548c272a7";
constexpr uint64_t RuntimeBytesExpected=674539285ull;
constexpr uint64_t RuntimeBytesUnpacked=1128900000ull;
constexpr const wchar_t* RuntimeFiles[]={
 L"whisper.dll",L"ggml.dll",L"ggml-base.dll",L"ggml-cuda.dll",
 L"ggml-cpu-alderlake.dll",L"ggml-cpu-cannonlake.dll",L"ggml-cpu-cascadelake.dll",L"ggml-cpu-haswell.dll",
 L"ggml-cpu-icelake.dll",L"ggml-cpu-sandybridge.dll",L"ggml-cpu-skylakex.dll",L"ggml-cpu-sse42.dll",L"ggml-cpu-x64.dll",
 L"cublas64_12.dll",L"cublasLt64_12.dll",L"cudart64_12.dll",
};
// Bumped whenever a decoding option that changes the text changes, so an old
// transcript is not mistaken for one this build would have made (27.10).
constexpr int OptionsVersion=2;   // 2: the runtime's own VAD no longer rewrites timestamps
constexpr double LookaheadSeconds=300.0;   // 27.3: bounded
constexpr double ChunkSeconds=60.0;

std::string Narrow(const std::wstring& text){
 if(text.empty())return {};
 int length=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
 std::string out(size_t(length),'\0');
 WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),out.data(),length,nullptr,nullptr);
 return out;
}
std::wstring Widen(const char* text){
 if(!text||!*text)return {};
 int length=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
 std::wstring out(length?size_t(length-1):0,L'\0');
 if(length>1)MultiByteToWideChar(CP_UTF8,0,text,-1,out.data(),length);
 return out;
}
double Now(){return double(GetTickCount64())/1000.0;}

fs::path AppFolder(){
 wchar_t module[MAX_PATH]{};
 GetModuleFileNameW(nullptr,module,MAX_PATH);
 return fs::path(module).parent_path();
}
fs::path DataFolder(const wchar_t* child){
 wchar_t local[MAX_PATH]{};
 fs::path base=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH)?fs::path(local):fs::temp_directory_path();
 auto folder=base/L"VetroLook"/child;
 std::error_code ec;fs::create_directories(folder,ec);
 return folder;
}
// Where the runtime lives: beside the application when it was put there, and
// otherwise where the viewer's own download put it. An installed copy sits in
// Program Files, which it cannot write to, so a download always goes to the
// viewer's data folder.
fs::path RuntimeFolder(){
 std::error_code ec;
 auto beside=AppFolder()/L"whisper";
 if(fs::exists(beside/L"whisper.dll",ec))return beside;
 auto local=DataFolder(L"whisper");
 // A runtime removed while it was loaded is finished off at the next start,
 // before anything can load it again.
 static std::once_flag cleanup;
 std::call_once(cleanup,[&local]{
  std::error_code e;
  if(fs::exists(local/L".remove",e))
   for(const auto& entry:fs::directory_iterator(local,e))fs::remove(entry.path(),e);
 });
 return local;
}

// ------------------------------------------------------------- hashing ------
std::string Sha256File(const fs::path& path,std::atomic<bool>* cancel=nullptr){
 BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
 std::string hex;
 if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0){
  std::ifstream in(path,std::ios::binary);
  std::vector<char> buffer(1<<20);
  bool ok=bool(in);
  while(ok&&in){
   if(cancel&&cancel->load()){ok=false;break;}
   in.read(buffer.data(),std::streamsize(buffer.size()));
   auto got=in.gcount();
   if(got>0&&BCryptHashData(hash,(PUCHAR)buffer.data(),ULONG(got),0)<0)ok=false;
  }
  UCHAR digest[32];
  if(ok&&BCryptFinishHash(hash,digest,sizeof digest,0)>=0){
   static const char* digits="0123456789abcdef";
   for(UCHAR b:digest){hex.push_back(digits[b>>4]);hex.push_back(digits[b&15]);}
  }
  BCryptDestroyHash(hash);
 }
 BCryptCloseAlgorithmProvider(algorithm,0);
 return hex;
}

// ------------------------------------------------------------- runtime ------
// Bound by name from the libraries in `whisper\`. The declarations come from the
// v1.9.4 header the binaries were published with; only their types are used.
struct WhisperApi{
 decltype(&whisper_version) version;
 decltype(&whisper_print_system_info) systemInfo;
 decltype(&whisper_log_set) logSet;
 decltype(&whisper_context_default_params_by_ref) contextDefaults;
 decltype(&whisper_free_context_params) freeContextParams;
 decltype(&whisper_init_from_file_with_params) initFromFile;
 decltype(&whisper_free) free;
 decltype(&whisper_init_state) initState;
 decltype(&whisper_free_state) freeState;
 decltype(&whisper_full_default_params_by_ref) fullDefaults;
 decltype(&whisper_free_params) freeParams;
 decltype(&whisper_full_with_state) fullWithState;
 decltype(&whisper_full_n_segments_from_state) segments;
 decltype(&whisper_full_get_segment_t0_from_state) segmentT0;
 decltype(&whisper_full_get_segment_t1_from_state) segmentT1;
 decltype(&whisper_full_get_segment_text_from_state) segmentText;
 decltype(&whisper_full_get_segment_no_speech_prob_from_state) segmentNoSpeech;
 decltype(&whisper_full_n_tokens_from_state) tokens;
 decltype(&whisper_full_get_token_data_from_state) tokenData;
 decltype(&whisper_full_get_token_text_from_state) tokenText;
 decltype(&whisper_full_lang_id_from_state) languageId;
 decltype(&whisper_lang_str) languageString;
 decltype(&whisper_token_eot) tokenEot;
 decltype(&whisper_vad_default_params) vadDefaults;
 decltype(&whisper_vad_default_context_params) vadContextDefaults;
 decltype(&whisper_vad_init_from_file_with_params) vadInit;
 decltype(&whisper_vad_segments_from_samples) vadSegments;
 decltype(&whisper_vad_segments_n_segments) vadCount;
 decltype(&whisper_vad_segments_get_segment_t0) vadT0;
 decltype(&whisper_vad_segments_get_segment_t1) vadT1;
 decltype(&whisper_vad_free_segments) vadFreeSegments;
 decltype(&whisper_vad_free) vadFree;
};
WhisperApi wapi{};
HMODULE whisperModule=nullptr;
// Set when there may be a runtime to load that was not there before: at start,
// and again when a download finishes. A runtime that failed to load is not
// retried on every pass of the worker.
std::atomic<bool> runtimeRetry{true};
bool runtimeReady=false;
std::wstring runtimeError,runtimeVersion,runtimeDevice;
std::mutex logMx;

template<class T> bool BindFrom(HMODULE module,T& slot,const char* name){
 slot=(T)GetProcAddress(module,name);
 return slot!=nullptr;
}

// ggml's own log: kept only for the one fact the status line wants -- which
// device the model actually landed on.
void WhisperLog(ggml_log_level,const char* text,void*){
 if(!text)return;
 std::string line=text;
 std::lock_guard lock(logMx);
 // "Device 0: NVIDIA GeForce RTX 3060 Ti, compute capability 8.6" names the GPU;
 // a CPU backend line only counts while no GPU has been named.
 auto device=line.find("Device 0:");
 if(device!=std::string::npos){
  auto name=line.substr(device+10);
  auto comma=name.find(',');
  if(comma!=std::string::npos)name=name.substr(0,comma);
  runtimeDevice=L"CUDA, "+Widen(name.c_str());
 }else if(line.find("loaded CPU backend")!=std::string::npos&&runtimeDevice.empty()){
  runtimeDevice=L"CPU";
 }
}

void LoadRuntime(){
 auto folder=RuntimeFolder();
 auto library=folder/L"whisper.dll";
 std::error_code ec;
 if(!fs::exists(library,ec)){runtimeError=L"whisper.dll was not found in the whisper folder";return;}
 // The runtime's dependencies -- ggml, and the CUDA libraries -- sit beside it
 // and are found there, not wherever PATH happens to point. The folder is also
 // put on the search path for later loads: ggml loads its GPU backend itself,
 // with an ordinary LoadLibrary, and that backend's own dependency on cuBLAS is
 // otherwise looked for everywhere except beside it -- which silently leaves the
 // model on the CPU.
 SetDllDirectoryW(folder.c_str());
 whisperModule=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
 if(!whisperModule){runtimeError=L"whisper.dll could not be loaded (error "+std::to_wstring(GetLastError())+L")";return;}
 HMODULE m=whisperModule;
 bool ok=
  BindFrom(m,wapi.version,"whisper_version")&&BindFrom(m,wapi.systemInfo,"whisper_print_system_info")&&
  BindFrom(m,wapi.logSet,"whisper_log_set")&&
  BindFrom(m,wapi.contextDefaults,"whisper_context_default_params_by_ref")&&
  BindFrom(m,wapi.freeContextParams,"whisper_free_context_params")&&
  BindFrom(m,wapi.initFromFile,"whisper_init_from_file_with_params")&&BindFrom(m,wapi.free,"whisper_free")&&
  BindFrom(m,wapi.initState,"whisper_init_state")&&BindFrom(m,wapi.freeState,"whisper_free_state")&&
  BindFrom(m,wapi.fullDefaults,"whisper_full_default_params_by_ref")&&BindFrom(m,wapi.freeParams,"whisper_free_params")&&
  BindFrom(m,wapi.fullWithState,"whisper_full_with_state")&&
  BindFrom(m,wapi.segments,"whisper_full_n_segments_from_state")&&
  BindFrom(m,wapi.segmentT0,"whisper_full_get_segment_t0_from_state")&&
  BindFrom(m,wapi.segmentT1,"whisper_full_get_segment_t1_from_state")&&
  BindFrom(m,wapi.segmentText,"whisper_full_get_segment_text_from_state")&&
  BindFrom(m,wapi.segmentNoSpeech,"whisper_full_get_segment_no_speech_prob_from_state")&&
  BindFrom(m,wapi.tokens,"whisper_full_n_tokens_from_state")&&
  BindFrom(m,wapi.tokenData,"whisper_full_get_token_data_from_state")&&
  BindFrom(m,wapi.tokenText,"whisper_full_get_token_text_from_state")&&
  BindFrom(m,wapi.languageId,"whisper_full_lang_id_from_state")&&BindFrom(m,wapi.languageString,"whisper_lang_str")&&
  BindFrom(m,wapi.tokenEot,"whisper_token_eot")&&
  BindFrom(m,wapi.vadDefaults,"whisper_vad_default_params")&&
  BindFrom(m,wapi.vadContextDefaults,"whisper_vad_default_context_params")&&
  BindFrom(m,wapi.vadInit,"whisper_vad_init_from_file_with_params")&&
  BindFrom(m,wapi.vadSegments,"whisper_vad_segments_from_samples")&&
  BindFrom(m,wapi.vadCount,"whisper_vad_segments_n_segments")&&
  BindFrom(m,wapi.vadT0,"whisper_vad_segments_get_segment_t0")&&
  BindFrom(m,wapi.vadT1,"whisper_vad_segments_get_segment_t1")&&
  BindFrom(m,wapi.vadFreeSegments,"whisper_vad_free_segments")&&BindFrom(m,wapi.vadFree,"whisper_vad_free");
 if(!ok){
  runtimeError=L"whisper.dll does not export the interface this build expects";
  FreeLibrary(whisperModule);whisperModule=nullptr;
  return;
 }
 // A ggml built with loadable backends looks for them beside the *executable*.
 // Ours live in whisper\, so they are pointed at explicitly when the loader
 // exists; a monolithic build simply does not export it.
 for(const wchar_t* name:{L"ggml.dll",L"ggml-base.dll"}){
  if(HMODULE ggml=GetModuleHandleW(name)){
   using LoadAll=void(*)(const char*);
   if(auto loadAll=(LoadAll)GetProcAddress(ggml,"ggml_backend_load_all_from_path")){
    auto utf8=Narrow(folder.wstring());
    loadAll(utf8.c_str());
    break;
   }
  }
 }
 wapi.logSet(&WhisperLog,nullptr);
 runtimeVersion=Widen(wapi.version());
 runtimeReady=true;
}
bool RuntimeReady(){
 if(runtimeReady)return true;
 if(!runtimeRetry.exchange(false))return false;
 runtimeError.clear();
 LoadRuntime();
 return runtimeReady;
}

// --------------------------------------------------------- audio source -----
// The second engine: no video, no subtitles, no output device, audio resampled
// to what the model wants and written as fast as it decodes. Bound from the
// library the player already loaded; nothing about which libmpv is decided here.
struct MpvApi{
 mpv_handle* (*create)(void);
 int (*initialize)(mpv_handle*);
 void (*terminate_destroy)(mpv_handle*);
 int (*set_option_string)(mpv_handle*,const char*,const char*);
 int (*command)(mpv_handle*,const char**);
 mpv_event* (*wait_event)(mpv_handle*,double);
};
MpvApi mapi{};
bool mpvReady=false;
bool EnsureMpv(){
 if(mpvReady)return true;
 std::wstring error;
 if(!PlaybackEnsureLibrary(error))return false;
 HMODULE m=PlaybackEngineModule();
 if(!m)return false;
 mpvReady=BindFrom(m,mapi.create,"mpv_create")&&BindFrom(m,mapi.initialize,"mpv_initialize")&&
          BindFrom(m,mapi.terminate_destroy,"mpv_terminate_destroy")&&
          BindFrom(m,mapi.set_option_string,"mpv_set_option_string")&&
          BindFrom(m,mapi.command,"mpv_command")&&BindFrom(m,mapi.wait_event,"mpv_wait_event");
 return mpvReady;
}

// Reads `length` seconds from `start` as 16 kHz mono float samples. Returns
// false when the source could not be read or `cancel` said to stop.
bool ExtractAudio(const std::wstring& path,double start,double length,long long audioTrack,
                  const std::function<bool()>& cancel,std::vector<float>& samples){
 samples.clear();
 if(!EnsureMpv())return false;
 auto wav=fs::temp_directory_path()/(L"vetro-ai-"+std::to_wstring(GetCurrentProcessId())+L".wav");
 std::error_code ec;fs::remove(wav,ec);
 mpv_handle* handle=mapi.create();
 if(!handle)return false;
 char startText[32],lengthText[32],track[32];
 snprintf(startText,sizeof startText,"%.3f",start);
 snprintf(lengthText,sizeof lengthText,"%.3f",length);
 if(audioTrack>0)snprintf(track,sizeof track,"%lld",audioTrack);else strcpy_s(track,"auto");
 auto wavUtf8=Narrow(wav.wstring());
 const char* options[][2]={
  {"config","no"},{"terminal","no"},{"idle","no"},{"vid","no"},{"sid","no"},{"vo","null"},
  {"ao","pcm"},{"ao-pcm-waveheader","no"},{"audio-samplerate","16000"},{"audio-channels","mono"},
  {"audio-format","s16"},{"untimed","yes"},{"ytdl","no"},{"hwdec","no"},{"sub-auto","no"},
  {"audio-file-auto","no"},{"cache","no"},
 };
 for(const auto& option:options)mapi.set_option_string(handle,option[0],option[1]);
 mapi.set_option_string(handle,"ao-pcm-file",wavUtf8.c_str());
 mapi.set_option_string(handle,"start",startText);
 mapi.set_option_string(handle,"length",lengthText);
 mapi.set_option_string(handle,"aid",track);
 bool ok=mapi.initialize(handle)>=0;
 auto pathUtf8=Narrow(path);
 if(ok){
  const char* command[]={"loadfile",pathUtf8.c_str(),nullptr};
  ok=mapi.command(handle,command)>=0;
 }
 bool failed=false,cancelled=false;
 while(ok){
  mpv_event* event=mapi.wait_event(handle,0.2);
  if(cancel()){cancelled=true;break;}
  if(!event||event->event_id==MPV_EVENT_NONE)continue;
  if(event->event_id==MPV_EVENT_END_FILE){
   auto* end=(mpv_event_end_file*)event->data;
   if(end&&end->reason==MPV_END_FILE_REASON_ERROR)failed=true;
   break;
  }
  if(event->event_id==MPV_EVENT_SHUTDOWN)break;
 }
 mapi.terminate_destroy(handle);
 if(!ok||failed||cancelled){fs::remove(wav,ec);return false;}
 std::ifstream in(wav,std::ios::binary);
 in.seekg(0,std::ios::end);
 auto bytes=size_t(in.tellg());
 in.seekg(0);
 std::vector<int16_t> pcm(bytes/2);
 in.read((char*)pcm.data(),std::streamsize(pcm.size()*2));
 in.close();
 fs::remove(wav,ec);
 samples.resize(pcm.size());
 for(size_t i=0;i<pcm.size();i++)samples[i]=float(pcm[i])/32768.0f;
 return true;
}

// ------------------------------------------------------------ the session ---
std::mutex mx;
std::condition_variable cv;
std::thread worker;
bool workerRunning=false;
std::atomic<bool> stopping{false};

struct Session{
 bool open=false;
 std::wstring path,signature;
 double duration=0;
 long long audioTrack=0;
 bool wantSubtitles=false,wantSpeech=false;
 std::wstring language;          // chosen; empty for automatic
 HWND notify=nullptr;UINT message=0;
};
Session session;                   // under mx
std::atomic<uint64_t> sessionGeneration{0};
std::atomic<double> playhead{0};
std::atomic<bool> allowed{true};
Transcript transcript;             // under mx
AiStatus status;                   // under mx

whisper_context* context=nullptr;   // worker thread only
whisper_vad_context* vadContext=nullptr;

fs::path ModelPath(){return DataFolder(L"models")/ModelFile;}
fs::path VadPath(){return DataFolder(L"models")/VadFile;}

std::wstring KeyFor(const Session& s,const std::wstring& language){
 std::error_code ec;
 auto bytes=fs::file_size(ModelPath(),ec);
 return AiCacheKey(s.signature,L"large-v3-turbo-q5_0",std::to_wstring(ec?0:bytes),language,s.audioTrack,OptionsVersion);
}
fs::path CachePathFor(const std::wstring& key){return DataFolder(L"ai")/AiCacheFileName(key);}

void SaveTranscript(const Transcript& t){
 auto path=CachePathFor(t.key);
 auto temporary=path;temporary+=L".tmp";
 {
  std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
  auto text=SerializeTranscript(t);
  out.write(text.data(),std::streamsize(text.size()));
  if(!out)return;
 }
 MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
}
bool LoadTranscript(const std::wstring& key,Transcript& t){
 std::ifstream in(CachePathFor(key),std::ios::binary);
 if(!in)return false;
 std::string text((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
 return ParseTranscript(text,t)&&t.key==key;
}

void Notify(){
 HWND window;UINT message;
 {std::lock_guard lock(mx);window=session.notify;message=session.message;}
 if(window)PostMessageW(window,message,0,0);
}
void SetState(AiState state){
 std::lock_guard lock(mx);
 status.state=state;
}

bool EnsureVad(){
 if(vadContext)return true;
 std::error_code ec;
 if(!fs::exists(VadPath(),ec))return false;
 auto params=wapi.vadContextDefaults();
 params.use_gpu=false;          // a sub-megabyte model: the CPU is quicker than the round trip
 params.n_threads=2;
 auto utf8=Narrow(VadPath().wstring());
 vadContext=wapi.vadInit(utf8.c_str(),params);
 return vadContext!=nullptr;
}
bool EnsureContext(){
 if(context)return true;
 SetState(AiState::Loading);
 Notify();
 auto params=wapi.contextDefaults();
 params->use_gpu=true;
 params->flash_attn=true;
 auto utf8=Narrow(ModelPath().wstring());
 context=wapi.initFromFile(utf8.c_str(),*params);
 wapi.freeContextParams(params);
 std::lock_guard lock(mx);
 if(!context){
  status.error=L"the speech model could not be loaded";
  status.state=AiState::Failed;
 }else{
  status.state=AiState::Working;
  std::lock_guard logLock(logMx);
  status.device=runtimeDevice;
 }
 return context!=nullptr;
}

// Speech spans in a chunk, in the film's own time. The VAD reports centiseconds
// in some builds and seconds in others; a span longer than the chunk it came
// from says which.
std::vector<Span> DetectSpeech(const std::vector<float>& samples,double start,double length){
 std::vector<Span> spans;
 if(!EnsureVad()||samples.empty())return spans;
 auto params=wapi.vadDefaults();
 params.threshold=0.5f;
 params.min_speech_duration_ms=250;
 params.min_silence_duration_ms=300;
 params.speech_pad_ms=120;
 auto* segments=wapi.vadSegments(vadContext,params,samples.data(),int(samples.size()));
 if(!segments)return spans;
 int count=wapi.vadCount(segments);
 double scale=1.0;
 for(int i=0;i<count;i++)if(wapi.vadT1(segments,i)>length*1.5+1){scale=0.01;break;}
 for(int i=0;i<count;i++){
  double a=start+wapi.vadT0(segments,i)*scale,b=start+wapi.vadT1(segments,i)*scale;
  if(b>a)spans.push_back({a,b});
 }
 wapi.vadFreeSegments(segments);
 return spans;
}

struct AbortContext{uint64_t generation;double chunkStart,chunkLength;};
bool ShouldAbort(void* data){
 auto* c=(AbortContext*)data;
 return stopping.load()||sessionGeneration.load()!=c->generation||!allowed.load()||
        ChunkAbandoned(c->chunkStart,c->chunkLength,playhead.load(),LookaheadSeconds);
}

std::vector<AiCue> Transcribe(const std::vector<float>& samples,double start,const std::wstring& language,
                             AbortContext& abort,std::wstring& detected,bool& aborted){
 std::vector<AiCue> cues;
 aborted=false;
 whisper_state* state=wapi.initState(context);
 if(!state)return cues;
 auto* params=wapi.fullDefaults(WHISPER_SAMPLING_GREEDY);
 unsigned cores=std::thread::hardware_concurrency();
 params->n_threads=int((std::min)(8u,(std::max)(2u,cores/2)));
 params->translate=false;
 params->no_context=true;          // chunks stand alone: no loop can carry over
 params->no_timestamps=false;
 params->token_timestamps=true;
 params->print_special=false;params->print_progress=false;
 params->print_realtime=false;params->print_timestamps=false;
 params->suppress_blank=true;
 params->suppress_nst=true;
 auto languageUtf8=Narrow(language.empty()?std::wstring(L"auto"):language);
 params->language=languageUtf8.c_str();
 params->detect_language=false;
 // The runtime's own VAD is off on purpose. It transcribes silence-trimmed audio
 // and maps the times back, and that mapping was measured to smear them: one
 // two-second line came back as a twenty-second cue, words ending before they
 // began. Our own speech detection already keeps silent chunks away from the
 // model (27.5, 27.6), so the model sees the real timeline and times it truly.
 params->vad=false;
 params->abort_callback=&ShouldAbort;
 params->abort_callback_user_data=&abort;
 int rc=wapi.fullWithState(context,state,*params,samples.data(),int(samples.size()));
 wapi.freeParams(params);
 if(rc!=0||ShouldAbort(&abort)){
  aborted=true;
  wapi.freeState(state);
  return cues;
 }
 if(const char* code=wapi.languageString(wapi.languageId(state)))detected=Widen(code);
 auto eot=wapi.tokenEot(context);
 int count=wapi.segments(state);
 for(int i=0;i<count;i++){
  AiCue cue;
  cue.start=start+double(wapi.segmentT0(state,i))/100.0;
  cue.end=start+double(wapi.segmentT1(state,i))/100.0;
  cue.text=Widen(wapi.segmentText(state,i));
  cue.noSpeech=wapi.segmentNoSpeech(state,i);
  int tokens=wapi.tokens(state,i);
  double sum=0;int counted=0;
  for(int k=0;k<tokens;k++){
   auto data=wapi.tokenData(state,i,k);
   if(data.id>=eot)continue;      // special tokens are not words
   sum+=data.p;counted++;
   auto piece=Widen(wapi.tokenText(context,state,i,k));
   if(piece.empty())continue;
   if(cue.words.empty()||piece[0]==L' '){
    AiWord word;
    word.start=start+double(data.t0)/100.0;word.end=start+double(data.t1)/100.0;
    word.p=data.p;
    size_t a=0;while(a<piece.size()&&piece[a]==L' ')a++;
    word.text=piece.substr(a);
    cue.words.push_back(std::move(word));
   }else{
    cue.words.back().text+=piece;
    cue.words.back().end=start+double(data.t1)/100.0;
    cue.words.back().p=(std::min)(cue.words.back().p,data.p);
   }
  }
  cue.confidence=counted?float(sum/counted):0.f;
  // Token timing is approximate; a word may never end before it begins, nor
  // run outside its own line.
  for(auto& word:cue.words){
   word.start=(std::max)(cue.start,(std::min)(word.start,cue.end));
   word.end=(std::max)(word.start,(std::min)(word.end,cue.end));
  }
  if(cue.end>cue.start)cues.push_back(std::move(cue));
 }
 wapi.freeState(state);
 return cues;
}

void Work(){
 // Secondary work by construction (Prototype G): the film's threads come first.
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
 // Stretches whose audio could not be read, for this film only. They are
 // skipped by the planner so they are not retried forever, and deliberately
 // kept out of the analysed map: audio we could not hear is not silence, and
 // silence skip must never jump over it.
 std::vector<Span> unreadable;
 uint64_t unreadableFor=0;
 while(!stopping.load()){
  Session s;
  uint64_t generation;
  {
   std::unique_lock lock(mx);
   cv.wait_for(lock,std::chrono::milliseconds(300));
   if(stopping.load())break;
   s=session;
   generation=sessionGeneration.load();
  }
  if(!s.open||!(s.wantSubtitles||s.wantSpeech)||s.duration<=0){SetState(AiState::Idle);continue;}
  if(!RuntimeReady()){
   std::lock_guard lock(mx);
   status.state=AiState::Unavailable;status.error=runtimeError;
   continue;
  }
  std::error_code ec;
  bool haveModel=fs::exists(ModelPath(),ec);
  bool transcribe=s.wantSubtitles&&haveModel;
  if(s.wantSubtitles&&!haveModel&&!s.wantSpeech){SetState(AiState::NeedsModel);continue;}
  if(!allowed.load()){SetState(AiState::Held);continue;}

  // What is still to do, around where the viewer is.
  std::vector<Span> done;
  std::wstring language;
  {
   std::lock_guard lock(mx);
   done=transcribe?transcript.covered:transcript.analysed;
   language=!s.language.empty()?s.language:transcript.language;
  }
  if(unreadableFor!=generation){unreadable.clear();unreadableFor=generation;}
  for(const auto& span:unreadable)CoverageAdd(done,span);
  auto plan=PlanChunk(done,playhead.load(),s.duration,LookaheadSeconds,ChunkSeconds);
  if(!plan.work){SetState(AiState::Idle);continue;}
  SetState(AiState::Working);

  double started=Now();
  auto cancelled=[&]{
   return stopping.load()||sessionGeneration.load()!=generation||
          ChunkAbandoned(plan.start,plan.length,playhead.load(),LookaheadSeconds);
  };
  std::vector<float> samples;
  if(!ExtractAudio(s.path,plan.start,plan.length,s.audioTrack,cancelled,samples)){
   if(cancelled())continue;
   CoverageAdd(unreadable,{plan.start,plan.start+plan.length});
   std::lock_guard lock(mx);
   status.state=AiState::Failed;status.error=L"the film's audio could not be read";
   continue;
  }
  double audioSeconds=double(samples.size())/16000.0;
  double end=plan.start+(std::max)(audioSeconds,0.5);
  bool finalChunk=end>=s.duration-0.5;
  auto speech=DetectSpeech(samples,plan.start,plan.length);

  std::vector<AiCue> fresh;
  double coveredUntil=end;
  std::wstring detected;
  if(transcribe){
   if(speech.empty()&&vadContext){
    // Nothing said: nothing to transcribe, and nothing for a model to invent.
   }else if(EnsureContext()){
    AbortContext abort{generation,plan.start,plan.length};
    bool aborted=false;
    auto cues=Transcribe(samples,plan.start,language,abort,detected,aborted);
    if(aborted)continue;
    auto settled=SettleChunk(std::move(cues),plan.start,end,finalChunk);
    std::vector<AiCue> kept;
    {
     std::lock_guard lock(mx);
     kept=transcript.cues;
    }
    for(auto& cue:settled.kept){
     if(CueSuppressed(cue,kept))continue;
     kept.push_back(cue);
     fresh.push_back(std::move(cue));
    }
    coveredUntil=settled.coveredUntil;
   }else continue;
  }
  if(sessionGeneration.load()!=generation)continue;
  {
   std::lock_guard lock(mx);
   CoverageAdd(transcript.analysed,{plan.start,end});
   for(const auto& span:speech)CoverageAdd(transcript.speech,span);
   if(transcribe){
    CoverageAdd(transcript.covered,{plan.start,coveredUntil});
    InsertCues(transcript.cues,fresh);
    if(transcript.language.empty()&&!detected.empty()&&!speech.empty())transcript.language=detected;
   }
   status.covered=CoverageSeconds(transcript.covered);
   status.analysed=CoverageSeconds(transcript.analysed);
   status.cues=int(transcript.cues.size());
   status.language=transcript.language;
   status.lastChunkSeconds=audioSeconds;
   status.lastChunkWall=Now()-started;
   status.error.clear();
   if(!runtimeDevice.empty()){std::lock_guard logLock(logMx);status.device=runtimeDevice;}
   if(!transcript.key.empty())SaveTranscript(transcript);
  }
  Notify();
  // Well ahead of the viewer, there is no hurry. A pause between chunks turns a
  // run of back-to-back GPU bursts into occasional ones, which is the difference
  // between the film's frames arriving late and not (measured: 17 late frames in
  // a minute of uninterrupted chunks). A seek wakes the worker at once.
  double ahead=0;
  {
   std::lock_guard lock(mx);
   const auto& spans=transcribe?transcript.covered:transcript.analysed;
   double from=playhead.load();
   double gap=CoverageFirstGap(spans,from,s.duration);
   ahead=(gap<0?s.duration:gap)-from;
  }
  // Two steps of breathing room: a short one as soon as the line on screen is
  // safe, a longer one once there are minutes in hand.
  if(ahead>=30.0){
   std::unique_lock lock(mx);
   cv.wait_for(lock,std::chrono::milliseconds(ahead>=120.0?3000:600));
  }
 }
 if(context){wapi.free(context);context=nullptr;}
 if(vadContext){wapi.vadFree(vadContext);vadContext=nullptr;}
}

void EnsureWorker(){
 if(workerRunning)return;
 workerRunning=true;
 stopping.store(false);
 worker=std::thread(Work);
}

// ------------------------------------------------------------ download ------
std::atomic<bool> downloading{false},downloadCancel{false};
std::atomic<double> downloadFraction{0};
std::thread downloader;
HWND downloadNotify=nullptr;UINT downloadMessage=0;

class Progress final:public IBindStatusCallback{
public:
 ULONG STDMETHODCALLTYPE AddRef()override{return 2;}
 ULONG STDMETHODCALLTYPE Release()override{return 1;}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out)override{
  if(riid==IID_IUnknown||riid==IID_IBindStatusCallback){*out=this;return S_OK;}
  *out=nullptr;return E_NOINTERFACE;
 }
 HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD,IBinding*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE GetPriority(LONG*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE OnLowResource(DWORD)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress,ULONG progressMax,ULONG,LPCWSTR)override{
  if(downloadCancel.load())return E_ABORT;
  // Past 4 GB the counters wrap; the known size is the better denominator.
  // This file is one stretch of the whole download; the bar shows the whole.
  double total=progressMax?double(progressMax):double(expected);
  double part=total>0?(std::min)(1.0,double(progress)/total):0;
  downloadFraction.store(base+span*part);
  double now=Now();
  if(now-lastPost>0.5&&downloadNotify){lastPost=now;PostMessageW(downloadNotify,downloadMessage,1,0);}
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT,LPCWSTR)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* flags,BINDINFO*)override{
  // Synchronous on this worker thread, straight from the server, and not kept
  // a second time in the browser cache: the model is half a gigabyte.
  *flags=BINDF_GETNEWESTVERSION|BINDF_NOWRITECACHE;return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD,DWORD,FORMATETC*,STGMEDIUM*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID,IUnknown*)override{return S_OK;}
 double lastPost=0;
 double base=0,span=1;
 uint64_t expected=0;
};

// Downloads `url` to `target` through a temporary file, and keeps it only if it
// hashes to `sha256`. A half-finished or tampered file never reaches the model
// folder under the model's name.
bool DownloadVerified(const wchar_t* url,const fs::path& target,const char* sha256,std::wstring& error,
                      double base,double span,uint64_t expected,bool checkExisting){
 std::error_code ec;
 if(fs::exists(target,ec)){
  // A file kept from an earlier attempt is used only if it is exactly the one
  // that was published; anything else is fetched again.
  if(!checkExisting||Sha256File(target,&downloadCancel)==sha256)return true;
  fs::remove(target,ec);
 }
 auto temporary=target;temporary+=L".part";
 fs::remove(temporary,ec);
 Progress progress;
 progress.base=base;progress.span=span;progress.expected=expected;
 HRESULT hr=URLDownloadToFileW(nullptr,url,temporary.c_str(),0,&progress);
 if(FAILED(hr)){
  fs::remove(temporary,ec);
  error=downloadCancel.load()?L"cancelled":L"download failed (0x"+[&]{wchar_t b[16];swprintf_s(b,L"%08X",unsigned(hr));return std::wstring(b);}()+L")";
  return false;
 }
 if(Sha256File(temporary,&downloadCancel)!=sha256){
  fs::remove(temporary,ec);
  error=L"the downloaded file did not match its published checksum";
  return false;
 }
 fs::rename(temporary,target,ec);
 if(ec){error=L"the download could not be moved into place";return false;}
 return true;
}

// Unpacks the runtime's libraries from the verified archive with the tar that
// ships with Windows, into a staging folder first: a half-unpacked runtime is
// never what the loader finds.
bool ExtractRuntime(const fs::path& archive,std::wstring& error){
 std::error_code ec;
 auto staging=DataFolder(L"whisper-staging");
 fs::remove_all(staging,ec);
 fs::create_directories(staging,ec);
 wchar_t system[MAX_PATH]{};
 GetSystemDirectoryW(system,MAX_PATH);
 std::wstring tar=std::wstring(system)+L"\\tar.exe";
 std::wstring command=L"\""+tar+L"\" -xf \""+archive.wstring()+L"\" -C \""+staging.wstring()+L"\"";
 for(auto name:RuntimeFiles)command+=L" \"Release/"+std::wstring(name)+L"\"";
 STARTUPINFOW startup{};startup.cb=sizeof startup;
 PROCESS_INFORMATION process{};
 if(!CreateProcessW(tar.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)){
  error=L"the runtime archive could not be unpacked";
  return false;
 }
 CloseHandle(process.hThread);
 while(WaitForSingleObject(process.hProcess,200)==WAIT_TIMEOUT){
  if(downloadCancel.load())TerminateProcess(process.hProcess,1);
 }
 DWORD code=1;
 GetExitCodeProcess(process.hProcess,&code);
 CloseHandle(process.hProcess);
 if(code!=0||downloadCancel.load()){
  fs::remove_all(staging,ec);
  error=downloadCancel.load()?L"cancelled":L"the runtime archive could not be unpacked";
  return false;
 }
 for(auto name:RuntimeFiles){
  if(!fs::exists(staging/L"Release"/name,ec)){
   fs::remove_all(staging,ec);
   error=std::wstring(L"the runtime archive does not contain ")+name;
   return false;
  }
 }
 auto destination=DataFolder(L"whisper");
 for(auto name:RuntimeFiles){
  fs::remove(destination/name,ec);
  fs::rename(staging/L"Release"/name,destination/name,ec);
  if(ec){
   ec.clear();
   fs::copy_file(staging/L"Release"/name,destination/name,fs::copy_options::overwrite_existing,ec);
   if(ec){fs::remove_all(staging,ec);error=L"the runtime could not be moved into place";return false;}
  }
 }
 fs::remove_all(staging,ec);
 fs::remove(destination/L".remove",ec);
 return fs::exists(destination/L"whisper.dll",ec);
}

}

// ======================================================= public interface ===
bool AiRuntimePresent(){
 std::error_code ec;
 auto folder=RuntimeFolder();
 return fs::exists(folder/L"whisper.dll",ec)&&!fs::exists(folder/L".remove",ec);
}
bool AiInstalled(){return AiRuntimePresent()&&AiModelPresent();}
uint64_t AiDownloadBytes(){
 std::error_code ec;
 uint64_t bytes=0;
 if(!AiRuntimePresent())bytes+=RuntimeBytesExpected;
 if(!fs::exists(VadPath(),ec))bytes+=VadBytesExpected;
 if(!fs::exists(ModelPath(),ec))bytes+=ModelBytesExpected;
 return bytes;
}
uint64_t AiInstalledBytes(){
 // Read from disk once and again only after a download or a removal: the menu
 // asks on every frame it is painted.
 static std::atomic<uint64_t> cached{0};
 static std::atomic<bool> known{false};
 if(known.load()&&!downloading.load())return cached.load();
 std::error_code ec;
 uint64_t bytes=0;
 for(auto name:RuntimeFiles){auto size=fs::file_size(RuntimeFolder()/name,ec);if(!ec)bytes+=size;ec.clear();}
 for(const auto& file:{ModelPath(),VadPath()}){auto size=fs::file_size(file,ec);if(!ec)bytes+=size;ec.clear();}
 cached.store(bytes);
 known.store(!downloading.load());
 return bytes;
}
std::wstring AiModelName(){return L"large-v3-turbo (q5)";}
uint64_t AiModelBytes(){
 std::error_code ec;
 auto bytes=fs::file_size(ModelPath(),ec);
 return ec?ModelBytesExpected:bytes;
}
bool AiModelPresent(){
 std::error_code ec;
 return fs::exists(ModelPath(),ec)&&fs::exists(VadPath(),ec);
}
void AiModelDownload(HWND notify,UINT message){
 if(downloading.load())return;
 if(downloader.joinable())downloader.join();
 downloadNotify=notify;downloadMessage=message;
 downloadCancel.store(false);downloadFraction.store(0);downloading.store(true);
 {std::lock_guard lock(mx);status.error.clear();}
 downloader=std::thread([]{
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  std::wstring error;
  std::error_code ec;
  // Everything AI subtitles need, in one go, with one progress bar measured in
  // bytes across all of it: the runtime, the speech detector and the model.
  bool needRuntime=!AiRuntimePresent();
  bool needVad=!fs::exists(VadPath(),ec);
  bool needModel=!fs::exists(ModelPath(),ec);
  double total=(needRuntime?double(RuntimeBytesExpected):0)+(needVad?double(VadBytesExpected):0)+
               (needModel?double(ModelBytesExpected):0);
  if(total<=0)total=1;
  double at=0;
  bool ok=true;
  if(needRuntime){
   double span=double(RuntimeBytesExpected)/total;
   auto archive=DataFolder(L"downloads")/RuntimeArchive;
   // The archive takes most of this stretch; unpacking it takes the rest.
   ok=DownloadVerified(RuntimeUrl,archive,RuntimeSha256,error,at,span*.94,RuntimeBytesExpected,true);
   if(ok){
    downloadFraction.store(at+span*.95);
    if(downloadNotify)PostMessageW(downloadNotify,downloadMessage,1,0);
    ok=ExtractRuntime(archive,error);
    if(ok){fs::remove(archive,ec);runtimeRetry.store(true);}
   }
   at+=span;
   if(ok)downloadFraction.store(at);
  }else{
   // Downloading again after a removal that is still waiting for a restart
   // keeps the runtime that is already here.
   fs::remove(RuntimeFolder()/L".remove",ec);
  }
  if(ok&&needVad){
   double span=double(VadBytesExpected)/total;
   ok=DownloadVerified(VadUrl,VadPath(),VadSha256,error,at,span,VadBytesExpected,false);
   at+=span;
  }
  if(ok&&needModel){
   double span=double(ModelBytesExpected)/total;
   ok=DownloadVerified(ModelUrl,ModelPath(),ModelSha256,error,at,span,ModelBytesExpected,false);
   at+=span;
  }
  if(ok)downloadFraction.store(1);
  if(!ok){std::lock_guard lock(mx);status.error=error;}
  downloading.store(false);
  CoUninitialize();
  if(downloadNotify)PostMessageW(downloadNotify,downloadMessage,2,ok?1:0);
  cv.notify_all();
 });
}
void AiModelDownloadCancel(){downloadCancel.store(true);}
bool AiModelRemove(){
 std::error_code ec;
 // The model is in use while a context holds it; the worker lets go first.
 {
  std::lock_guard lock(mx);
  session.wantSubtitles=false;
 }
 sessionGeneration++;
 cv.notify_all();
 for(int i=0;i<50&&context;i++)Sleep(20);
 bool removed=!fs::exists(ModelPath(),ec)||fs::remove(ModelPath(),ec);
 fs::remove(VadPath(),ec);
 // The runtime this viewer downloaded goes too. One placed beside the
 // application belongs to whoever put it there, and is left alone. A runtime
 // already loaded cannot be deleted from under the process, so it is marked and
 // removed at the next start.
 auto local=DataFolder(L"whisper");
 if(RuntimeFolder()==local){
  if(whisperModule){std::ofstream marker(local/L".remove");marker<<"1";}
  else for(const auto& entry:fs::directory_iterator(local,ec))fs::remove(entry.path(),ec);
 }
 return removed;
}

void AiOpen(const std::wstring& path,const std::wstring& signature,double duration,
            long long audioTrack,HWND notify,UINT message){
 std::lock_guard lock(mx);
 session=Session{};
 session.open=true;session.path=path;session.signature=signature;session.duration=duration;
 session.audioTrack=audioTrack;session.notify=notify;session.message=message;
 transcript=Transcript{};
 status=AiStatus{};
 status.duration=duration;
 sessionGeneration++;
 EnsureWorker();
 cv.notify_all();
}
void AiClose(){
 std::lock_guard lock(mx);
 session=Session{};
 transcript=Transcript{};
 status=AiStatus{};
 sessionGeneration++;
 cv.notify_all();
}
void AiSetWanted(bool subtitles,bool speech){
 std::lock_guard lock(mx);
 if(session.wantSubtitles==subtitles&&session.wantSpeech==speech)return;
 session.wantSubtitles=subtitles;session.wantSpeech=speech;
 // A transcript is found by its key, and the key needs the language; load what
 // this film already has as soon as anything is wanted from it.
 if((subtitles||speech)&&session.open&&transcript.key.empty()){
  auto key=KeyFor(session,session.language);
  Transcript loaded;
  if(LoadTranscript(key,loaded)){transcript=std::move(loaded);status.fromCache=true;}
  else transcript.key=key;
  status.covered=CoverageSeconds(transcript.covered);
  status.analysed=CoverageSeconds(transcript.analysed);
  status.cues=int(transcript.cues.size());
  status.language=transcript.language;
 }
 cv.notify_all();
}
void AiSetLanguage(const std::wstring& language){
 std::lock_guard lock(mx);
 if(session.language==language)return;
 session.language=language;
 // Another language is another transcript (27.10).
 if(session.open){
  auto key=KeyFor(session,language);
  Transcript loaded;
  transcript=LoadTranscript(key,loaded)?std::move(loaded):Transcript{};
  transcript.key=key;
  sessionGeneration++;
 }
 cv.notify_all();
}
void AiSetPlayhead(double seconds){
 double before=playhead.exchange(seconds);
 if(fabs(before-seconds)>5.0)cv.notify_all();
}
void AiSetAllowed(bool value){
 if(allowed.exchange(value)!=value)cv.notify_all();
}
bool AiCueAt(double seconds,AiCue& cue){
 std::lock_guard lock(mx);
 const AiCue* found=CueAt(transcript.cues,seconds);
 if(!found)return false;
 cue=*found;
 return true;
}
bool AiHasCues(){
 std::lock_guard lock(mx);
 return !transcript.cues.empty();
}
double AiSilenceTarget(double position,SilenceSkip mode){
 std::lock_guard lock(mx);
 return SilenceSkipTarget(transcript.speech,transcript.analysed,position,mode);
}
AiStatus AiStatusNow(){
 AiStatus copy;
 {
  std::lock_guard lock(mx);
  copy=status;
 }
 if(downloading.load()){copy.state=AiState::Downloading;copy.downloadFraction=downloadFraction.load();}
 else if(!AiInstalled())copy.state=AiState::NeedsModel;
 else if(copy.state==AiState::Unavailable||copy.state==AiState::NeedsModel)copy.state=AiState::Idle;
 copy.runtimeVersion=runtimeVersion;
 copy.modelName=AiModelName();
 return copy;
}
bool AiExport(const std::wstring& path,bool vtt){
 std::vector<AiCue> cues;
 {
  std::lock_guard lock(mx);
  cues=transcript.cues;
 }
 if(cues.empty())return false;
 auto text=vtt?ExportVtt(cues):ExportSrt(cues);
 std::ofstream out(fs::path(path),std::ios::binary|std::ios::trunc);
 if(!vtt)out.write("\xEF\xBB\xBF",3);       // SRT readers on Windows want the BOM to see UTF-8
 out.write(text.data(),std::streamsize(text.size()));
 return bool(out);
}
bool AiClearCache(){
 std::error_code ec;
 auto folder=DataFolder(L"ai");
 for(const auto& entry:fs::directory_iterator(folder,ec))fs::remove(entry.path(),ec);
 std::lock_guard lock(mx);
 auto key=transcript.key;
 transcript=Transcript{};
 transcript.key=key;
 sessionGeneration++;
 return true;
}
void AiStop(){
 downloadCancel.store(true);
 stopping.store(true);
 cv.notify_all();
 if(worker.joinable())worker.join();
 workerRunning=false;
 if(downloader.joinable())downloader.join();
}
