#include "ui.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int ThumbDebugState(const std::wstring& path);
unsigned ThumbDebugAttempts(const std::wstring& path);
size_t ThumbDebugQueueSize();
std::wstring ThumbDebugNext();
size_t ThumbDebugReadyCount();
size_t ThumbDebugQueuedOutside(const std::vector<std::wstring>& keep);

int wmain(int argc,wchar_t** argv){
 if(argc<22){std::wcerr<<L"usage: VetroFilmstripStressTests <corrupt-output> <20+ image paths>\n";return 2;}
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);int failures=0;
 auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;};
 std::wstring corrupt=argv[1];std::vector<std::wstring> files;for(int i=2;i<argc;i++)files.emplace_back(argv[i]);
 // Deterministic queue check before workers begin consuming it.
 for(auto& path:files)ThumbRequest(path);
 ThumbPrioritize(files.front());
 check(ThumbDebugNext()==files.front(),"active thumbnail is next in queue");
 ThumbStart(nullptr,0);
 auto deadline=GetTickCount64()+60000;
 while(GetTickCount64()<deadline&&ThumbDebugReadyCount()<(std::min)(size_t(20),files.size()))Sleep(20);
 check(ThumbLookup(files.front())!=nullptr,"active thumbnail became ready");
 check(ThumbDebugReadyCount()>0,"filmstrip is not completely black");
 check(ThumbDebugReadyCount()>=(std::min)(size_t(20),files.size()),"twenty real thumbnails loaded");
 {std::ofstream out(std::filesystem::path(corrupt),std::ios::binary);out<<"not an image";}
 ThumbRequest(corrupt);deadline=GetTickCount64()+10000;
 while(GetTickCount64()<deadline&&ThumbDebugState(corrupt)!=3)Sleep(20);
 auto attempts=ThumbDebugAttempts(corrupt);for(int i=0;i<100;i++)ThumbRequest(corrupt);
 check(ThumbDebugState(corrupt)==3&&attempts==1,"failed thumbnail is cached");
 check(ThumbDebugAttempts(corrupt)==1,"failed thumbnail retry has backoff");
 // Simulate a folder change while the previous 500-item backlog is active.
 std::vector<std::wstring> next;
 for(size_t i=0;i<(std::min)(size_t(20),files.size());i++)next.push_back(files[files.size()-1-i]);
 ThumbTrim(next);for(auto& path:next)ThumbRequest(path);ThumbPrioritize(next.front());
 check(ThumbDebugQueuedOutside(next)==0,"old queued tasks removed on folder change");
 deadline=GetTickCount64()+30000;while(GetTickCount64()<deadline&&!ThumbLookup(next.front()))Sleep(20);
 check(ThumbLookup(next.front())!=nullptr,"new active thumbnail loads after folder change");
 ThumbStop();DeleteFileW(corrupt.c_str());CoUninitialize();return failures?1:0;
}
