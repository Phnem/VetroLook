#include "image.h"
#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

static uint64_t PixelHash(const std::shared_ptr<Image>& image){
 uint64_t hash=1469598103934665603ull;if(!image)return 0;
 for(uint8_t value:image->pixels){hash^=value;hash*=1099511628211ull;}return hash;
}

int wmain(int argc,wchar_t** argv){
 if(argc<2)return 2;
 if(argc>2&&!wcscmp(argv[1],L"--memory-truncate")){
  std::ifstream in(std::filesystem::path(argv[2]),std::ios::binary|std::ios::ate);if(!in)return 2;
  auto n=in.tellg();std::vector<uint8_t> bytes(static_cast<size_t>(n));in.seekg(0);in.read((char*)bytes.data(),n);
  const uint8_t* preview=nullptr;size_t previewSize=0;std::wstring error;
  auto image=DecodePsd(bytes.data(),bytes.size()/2,&preview,&previewSize,&error);
  std::wcout<<L"memory_truncate\tfull="<<(image?L"unexpected":L"none")<<L"\terror="<<error<<L"\n";
  return !image&&!error.empty()?0:1;
 }
 bool expectFailure=argc>2&&!wcscmp(argv[1],L"--expect-fail");
 const wchar_t* path=argv[expectFailure?2:1];
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 auto start=std::chrono::steady_clock::now();
 auto preview=DecodeThumb(path,2048);
 double previewMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 std::wstring error;start=std::chrono::steady_clock::now();auto full=Decode(path,error);
 double fullMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters));
 std::error_code ec;auto bytes=std::filesystem::file_size(path,ec);
 std::wcout<<L"file="<<std::filesystem::path(path).filename().wstring()
  <<L"\tbytes="<<(ec?0:bytes)<<L"\tpreview_ms="<<std::fixed<<std::setprecision(2)<<previewMs
  <<L"\tpreview="<<(preview?std::to_wstring(preview->w)+L"x"+std::to_wstring(preview->h):L"none")
  <<L"\tfull_ms="<<fullMs<<L"\tfull="<<(full?std::to_wstring(full->w)+L"x"+std::to_wstring(full->h):L"none")
  <<L"\tcodec="<<(full?full->codec:L"-")<<L"\tpeak_working_set="<<counters.PeakWorkingSetSize
  <<L"\tpixel_hash="<<PixelHash(full)
  <<L"\terror="<<error<<L"\n";
 CoUninitialize();
 return expectFailure?!full?0:1:full?0:1;
}
