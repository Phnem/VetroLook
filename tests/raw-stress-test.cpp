#include "image.h"
#include "ui.h"
#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

size_t ThumbDebugReadyCount();

int wmain(int argc,wchar_t** argv){
 if(argc<21){std::wcerr<<L"usage: VetroRawStressTests <20+ RAW paths>\n";return 2;}
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 std::vector<std::wstring> paths;for(int i=1;i<argc;i++)paths.emplace_back(argv[i]);
 int failures=0;auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;};
 std::mutex mx;std::condition_variable cv;std::wstring requested;uint64_t generation=0;
 std::atomic<uint64_t> latest{0};std::atomic<unsigned> started{0},completed{0},cancelled{0},staleCompleted{0};
 std::atomic<bool> stopping{false},finalDone{false};
 ThumbStart(nullptr,0);for(auto& path:paths)ThumbRequest(path);ThumbPrioritize(paths.front());
 size_t thumbnailsBefore=ThumbDebugReadyCount();
 std::thread worker([&]{
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  while(true){
   std::wstring path;uint64_t id=0;
   {std::unique_lock lock(mx);cv.wait(lock,[&]{return stopping||!requested.empty();});if(stopping)break;path=std::move(requested);requested.clear();id=generation;}
   if(latest!=id)continue;
   DecodeThumb(path,2048);
   if(latest!=id){cancelled++;continue;}
   started++;bool sawCancel=false;std::wstring error;
   auto image=Decode(path,error,[&]{bool stale=latest!=id;if(stale)sawCancel=true;return stale;});
   if(sawCancel)cancelled++;
   if(latest!=id){if(image)staleCompleted++;continue;}
   if(image)completed++;finalDone=true;
  }
  CoUninitialize();
 });
 auto request=[&](const std::wstring& path){
  auto before=std::chrono::steady_clock::now();
  {std::lock_guard lock(mx);requested=path;generation++;latest=generation;}
  cv.notify_one();
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
 };
 double maxRequestMs=request(paths.front());
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(!started&&std::chrono::steady_clock::now()<deadline)Sleep(1);
 check(started>0,"first full demosaic started");
 for(size_t i=1;i<paths.size();i++){maxRequestMs=(std::max)(maxRequestMs,request(paths[i]));Sleep(5);}
 deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
 while(!finalDone&&std::chrono::steady_clock::now()<deadline)Sleep(10);
 {std::lock_guard lock(mx);stopping=true;latest=++generation;}cv.notify_one();worker.join();
 PROCESS_MEMORY_COUNTERS memory{sizeof(memory)};GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
 check(finalDone&&completed==1,"only the final generation completed");
 check(cancelled>0,"stale LibRaw decode observed cancellation");
 check(staleCompleted==0,"no stale full frame completed after cancellation");
 check(requested.empty(),"no old full-demosaic backlog remains");
 check(maxRequestMs<20.0,"request path stayed non-blocking");
 size_t thumbnailsAfter=ThumbDebugReadyCount();
 check(thumbnailsAfter>thumbnailsBefore,"thumbnail workers progressed during rapid full-decode switching");
 std::cout<<"METRIC requests="<<paths.size()<<" full_started="<<started<<" cancelled="<<cancelled
          <<" completed="<<completed<<" max_request_ms="<<maxRequestMs
          <<" peak_working_set="<<memory.PeakWorkingSetSize<<"\n";
 ThumbStop();CoUninitialize();return failures?1:0;
}
