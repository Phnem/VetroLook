// Vetro Look, GPL-3.0-or-later.
// PSD composite: identical pixels, measured peak memory, before and after.
//
// Each decoder runs in its own process invocation so the peak working set is
// attributable to one of them and not to whichever ran first.
#include "image.h"
#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

std::shared_ptr<Image> DecodePsdBaseline(const uint8_t* bytes,size_t size,std::wstring* variantError);

namespace{
uint64_t Hash(const std::shared_ptr<Image>& image){
 uint64_t h=1469598103934665603ull;
 if(!image)return 0;
 for(uint8_t v:image->pixels){h^=v;h*=1099511628211ull;}
 return h;
}
size_t PeakMb(){
 PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);
 GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters));
 return counters.PeakWorkingSetSize/(1024*1024);
}
}

int wmain(int argc,wchar_t** argv){
 if(argc<3){std::wcerr<<L"usage: psd-memory-test <before|after|screen> <file> [edge]\n";return 2;}
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 bool baseline=!wcscmp(argv[1],L"before");
 bool screen=!wcscmp(argv[1],L"screen");
 std::ifstream file(std::filesystem::path(argv[2]),std::ios::binary|std::ios::ate);
 if(!file)return 2;
 auto length=file.tellg();
 // Braces, not parentheses: `bytes(size_t(length))` declares a function.
 std::vector<uint8_t> bytes(static_cast<size_t>(length));
 file.seekg(0);
 if(!file.read((char*)bytes.data(),length))return 2;

 auto start=std::chrono::steady_clock::now();
 std::wstring error;
 std::shared_ptr<Image> image;
 if(baseline)image=DecodePsdBaseline(bytes.data(),bytes.size(),&error);
 else if(screen)image=DecodePsdScreen(bytes.data(),bytes.size(),argc>3?unsigned(_wtoi(argv[3])):2560u,&error);
 else{
  const uint8_t* preview=nullptr;size_t previewSize=0;
  image=DecodePsd(bytes.data(),bytes.size(),&preview,&previewSize,&error);
 }
 double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 // The file buffer is not part of what changed; report the peak with it
 // excluded so the two runs compare the decoder and nothing else.
 size_t fileMb=bytes.size()/(1024*1024);
 std::wcout<<argv[1]<<L"\t"<<std::filesystem::path(argv[2]).filename().wstring()
  <<L"\tms="<<std::fixed<<std::setprecision(2)<<ms
  <<L"\tsize="<<(image?std::to_wstring(image->w)+L"x"+std::to_wstring(image->h):L"none")
  <<L"\thash="<<Hash(image)
  <<L"\tpeak_mb="<<PeakMb()
  <<L"\tpeak_minus_file_mb="<<(PeakMb()>fileMb?PeakMb()-fileMb:0)
  <<L"\terror="<<error<<L"\n";
 CoUninitialize();
 return image?0:1;
}
