// Vetro Look, GPL-3.0-or-later.
// See resolver.h.
#include "resolver.h"
#include <algorithm>
#include <filesystem>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace{

// Long enough for a slow site on a slow line; short enough that a hung helper
// is a message rather than a viewer that seems to have forgotten the request.
constexpr DWORD TimeoutMs=45000;
constexpr SIZE_T MemoryLimit=1024ull<<20;
constexpr size_t OutputLimit=1u<<20;

std::mutex mx;
std::thread worker;
std::atomic<bool> cancel{false};
HANDLE job=nullptr;                 // the running helper's job, under mx
ResolveOutcome finished;
bool hasFinished=false;
bool busy=false;

std::wstring Widen(const std::string& text){
 if(text.empty())return {};
 int length=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
 std::wstring out(size_t(length),L'\0');
 MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),out.data(),length);
 return out;
}

// Reads a pipe to its end, keeping at most OutputLimit bytes: a helper that
// prints forever does not get to fill our memory with it.
void Drain(HANDLE pipe,std::string& into){
 char buffer[4096];
 DWORD read=0;
 while(ReadFile(pipe,buffer,sizeof buffer,&read,nullptr)&&read){
  if(into.size()<OutputLimit)into.append(buffer,(std::min)(size_t(read),OutputLimit-into.size()));
 }
}

// The child's environment: ours, plus the two variables that make a Python
// helper write UTF-8 into a pipe instead of the console code page. A title in
// Cyrillic should arrive as Cyrillic.
std::wstring ChildEnvironment(){
 std::wstring block;
 if(wchar_t* strings=GetEnvironmentStringsW()){
  for(const wchar_t* p=strings;*p;p+=wcslen(p)+1){
   std::wstring entry=p;
   if(_wcsnicmp(p,L"PYTHONUTF8=",11)==0||_wcsnicmp(p,L"PYTHONIOENCODING=",17)==0)continue;
   block+=entry;block.push_back(L'\0');
  }
  FreeEnvironmentStringsW(strings);
 }
 block+=L"PYTHONUTF8=1";block.push_back(L'\0');
 block+=L"PYTHONIOENCODING=utf-8";block.push_back(L'\0');
 block.push_back(L'\0');
 return block;
}

ResolveOutcome Resolve(const std::wstring& url){
 ResolveOutcome outcome;
 auto started=GetTickCount64();
 auto done=[&](StreamFailure failure,const std::wstring& detail){
  outcome.failure=failure;outcome.detail=detail;
  outcome.seconds=double(GetTickCount64()-started)/1000.0;
  return outcome;
 };
 auto exe=ResolverPath();
 if(exe.empty())return done(StreamFailure::ResolverMissing,L"yt-dlp.exe was not found");
 if(!ResolverAddressSafe(url))return done(StreamFailure::NoPublicStream,L"address refused before resolving");

 // One address, as one argument, after "--": nothing in it can become an option.
 // No configuration file, no cookies, no playlist, no post-processing command.
 // The format asks for the best picture up to 2160 lines with its best sound,
 // or the best single file when the site does not split them.
 std::wstring command=L"\""+exe+L"\" --ignore-config --no-playlist --no-warnings --no-progress"
  L" --no-cache-dir --socket-timeout 15 -f \"bv*[height<=?2160]+ba/b\""
  L" --print \"VETRO-TITLE:%(title)s\" --print \"VETRO-LIVE:%(is_live)s\""
  L" --print \"VETRO-DURATION:%(duration)s\" --print \"VETRO-UA:%(http_headers.User-Agent)s\""
  L" --print \"VETRO-REFERER:%(http_headers.Referer)s\" --print \"VETRO-URLS:%(urls)s\""
  L" -- \""+url+L"\"";

 SECURITY_ATTRIBUTES inherit{sizeof inherit,nullptr,TRUE};
 HANDLE outRead=nullptr,outWrite=nullptr,errRead=nullptr,errWrite=nullptr;
 if(!CreatePipe(&outRead,&outWrite,&inherit,0)||!CreatePipe(&errRead,&errWrite,&inherit,0)){
  if(outRead){CloseHandle(outRead);CloseHandle(outWrite);}
  return done(StreamFailure::ResolverCrashed,L"pipes could not be created");
 }
 SetHandleInformation(outRead,HANDLE_FLAG_INHERIT,0);
 SetHandleInformation(errRead,HANDLE_FLAG_INHERIT,0);

 HANDLE localJob=CreateJobObjectW(nullptr,nullptr);
 if(localJob){
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|
   JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  limits.BasicLimitInformation.ActiveProcessLimit=6;
  limits.JobMemoryLimit=MemoryLimit;
  SetInformationJobObject(localJob,JobObjectExtendedLimitInformation,&limits,sizeof limits);
 }

 STARTUPINFOW startup{};startup.cb=sizeof startup;
 startup.dwFlags=STARTF_USESTDHANDLES;
 startup.hStdInput=nullptr;startup.hStdOutput=outWrite;startup.hStdError=errWrite;
 PROCESS_INFORMATION process{};
 auto environment=ChildEnvironment();
 auto folder=std::filesystem::path(exe).parent_path().wstring();
 BOOL created=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,
  CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.data(),
  folder.c_str(),&startup,&process);
 CloseHandle(outWrite);CloseHandle(errWrite);
 if(!created){
  CloseHandle(outRead);CloseHandle(errRead);
  if(localJob)CloseHandle(localJob);
  return done(StreamFailure::ResolverCrashed,L"the resolver could not be started (error "+
              std::to_wstring(GetLastError())+L")");
 }
 if(localJob)AssignProcessToJobObject(localJob,process.hProcess);
 {
  std::lock_guard lock(mx);
  job=localJob;
 }
 ResumeThread(process.hThread);
 CloseHandle(process.hThread);

 std::string out,err;
 std::thread outReader([&]{Drain(outRead,out);});
 std::thread errReader([&]{Drain(errRead,err);});

 bool timedOut=false,cancelled=false;
 auto deadline=GetTickCount64()+TimeoutMs;
 while(WaitForSingleObject(process.hProcess,100)==WAIT_TIMEOUT){
  if(cancel.load()){cancelled=true;break;}
  if(GetTickCount64()>=deadline){timedOut=true;break;}
 }
 // A helper that is abandoned is killed, whole tree: the job holds every
 // process it started, and closing the pipe ends are not enough to stop one.
 if(timedOut||cancelled){
  if(localJob)TerminateJobObject(localJob,1);
  else TerminateProcess(process.hProcess,1);
  WaitForSingleObject(process.hProcess,2000);
 }
 DWORD exitCode=0;
 GetExitCodeProcess(process.hProcess,&exitCode);
 // Grandchildren may still hold the write ends; the job's end closes them.
 if(localJob)TerminateJobObject(localJob,exitCode);
 outReader.join();errReader.join();
 CloseHandle(outRead);CloseHandle(errRead);
 CloseHandle(process.hProcess);
 {
  std::lock_guard lock(mx);
  job=nullptr;
 }
 if(localJob)CloseHandle(localJob);

 auto errors=Widen(err);
 while(!errors.empty()&&(errors.back()==L'\n'||errors.back()==L'\r'))errors.pop_back();
 if(cancelled)return done(StreamFailure::None,L"cancelled");
 if(timedOut)return done(StreamFailure::ResolverTimeout,L"no answer within 45 s");
 if(exitCode==0&&ParseResolverOutput(out,outcome.stream)){
  outcome.ok=true;
  return done(StreamFailure::None,errors);
 }
 // A crash is not the site's fault; it is recoverable and says so (Appendix G).
 if(exitCode>=0xC0000000ul)return done(StreamFailure::ResolverCrashed,L"exit code "+std::to_wstring(exitCode));
 return done(ClassifyResolverFailure(errors,exitCode),errors);
}

}

std::wstring ResolverPath(){
 std::vector<std::filesystem::path> candidates;
 wchar_t module[MAX_PATH]{};
 if(GetModuleFileNameW(nullptr,module,MAX_PATH))
  candidates.push_back(std::filesystem::path(module).parent_path()/L"resolver"/L"yt-dlp.exe");
 wchar_t local[MAX_PATH]{};
 if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH))
  candidates.push_back(std::filesystem::path(local)/L"VetroLook"/L"resolver"/L"yt-dlp.exe");
 std::error_code ec;
 for(const auto& candidate:candidates)if(std::filesystem::exists(candidate,ec))return candidate.wstring();
 wchar_t found[MAX_PATH]{};
 if(SearchPathW(nullptr,L"yt-dlp.exe",nullptr,MAX_PATH,found,nullptr))return found;
 return {};
}

void ResolverCancel(){
 cancel.store(true);
 {
  std::lock_guard lock(mx);
  if(job)TerminateJobObject(job,1);
 }
 if(worker.joinable())worker.join();
 std::lock_guard lock(mx);
 busy=false;
}

void ResolverStart(const std::wstring& url,uint64_t generation,HWND notify,UINT message){
 ResolverCancel();
 cancel.store(false);
 {
  std::lock_guard lock(mx);
  hasFinished=false;busy=true;
 }
 worker=std::thread([url,generation,notify,message]{
  auto outcome=Resolve(url);
  outcome.generation=generation;
  if(cancel.load())return;
  {
   std::lock_guard lock(mx);
   finished=std::move(outcome);hasFinished=true;busy=false;
  }
  if(notify)PostMessageW(notify,message,0,0);
 });
}

bool ResolverCollect(ResolveOutcome& outcome){
 std::lock_guard lock(mx);
 if(!hasFinished)return false;
 outcome=std::move(finished);hasFinished=false;
 return true;
}

bool ResolverBusy(){
 std::lock_guard lock(mx);
 return busy;
}
