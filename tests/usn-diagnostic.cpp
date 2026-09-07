#include "../src/usn.h"
#include <filesystem>
#include <iostream>
#include <chrono>
namespace fs=std::filesystem;
bool Supported(const std::wstring& path){
 static const wchar_t* formats[]={L".jpg",L".jpeg",L".png",L".webp",L".gif",L".bmp",L".avif",L".exr",L".dng",L".cr2",L".cr3",L".nef",L".arw",L".orf",L".rw2"};
 auto ext=fs::path(path).extension().wstring();for(auto& c:ext)c=towlower(c);for(auto f:formats)if(ext==f)return true;return false;
}
int wmain(int argc,wchar_t** argv){
 wchar_t drive=argc>1?wchar_t(towupper(argv[1][0])):L'C';uint64_t folders=0,photos=0;UsnJournalPos pos;
 auto started=std::chrono::steady_clock::now();
 bool ok=UsnFastEnumerate(drive,[&](const UsnFolderResult&,std::vector<UsnPhotoResult>&& found){++folders;photos+=found.size();},pos);
 auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
 std::wcout<<L"volume="<<drive<<L": mode="<<(ok?L"mft":L"unavailable")<<L" folders="<<folders<<L" photos="<<photos<<L" elapsed_ms="<<ms<<L" journal_valid="<<pos.valid<<L"\n";
 return ok?0:3;
}
