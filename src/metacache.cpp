// Vetro Look, GPL-3.0-or-later.
#include "metacache.h"
#include "metaread.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace{

std::mutex mx;
std::unordered_map<std::wstring,CachedMeta> entries;   // key: lowercased path
bool loaded=false,dirty=false;
constexpr uint32_t Magic=0x564B4D32;                   // "VKM2"
constexpr size_t MaxEntries=40000;                     // ~8 MB on disk at that size

std::wstring Lower(std::wstring s){for(auto& c:s)c=towlower(c);return s;}

bool Stamp(const std::wstring& path,uint64_t& size,uint64_t& written){
 WIN32_FILE_ATTRIBUTE_DATA info{};
 if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info))return false;
 size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
 written=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
 return true;
}

std::wstring StorePath(){
 wchar_t* base=nullptr;std::wstring dir;
 if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&base))){dir=base;CoTaskMemFree(base);}
 if(dir.empty())return {};
 dir+=L"\\VetroLook";
 CreateDirectoryW(dir.c_str(),nullptr);
#ifdef VETRO_REVIEW_BUILD
 return dir+L"\\metacache-review.bin";
#else
 return dir+L"\\metacache.bin";
#endif
}

struct Writer{
 std::vector<uint8_t> bytes;
 void U32(uint32_t v){for(int i=0;i<4;i++)bytes.push_back(uint8_t(v>>(i*8)));}
 void U64(uint64_t v){for(int i=0;i<8;i++)bytes.push_back(uint8_t(v>>(i*8)));}
 void F64(double v){uint64_t raw;memcpy(&raw,&v,8);U64(raw);}
 void Str(const std::wstring& s){
  uint32_t chars=uint32_t((std::min)(s.size(),size_t(4096)));
  U32(chars);
  for(uint32_t i=0;i<chars;i++){uint16_t u=uint16_t(s[i]);bytes.push_back(uint8_t(u));bytes.push_back(uint8_t(u>>8));}
 }
};
struct Reader{
 const uint8_t* p;const uint8_t* end;bool ok=true;
 uint32_t U32(){if(p+4>end){ok=false;return 0;}uint32_t v=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);p+=4;return v;}
 uint64_t U64(){if(p+8>end){ok=false;return 0;}uint64_t v=0;for(int i=0;i<8;i++)v|=uint64_t(p[i])<<(i*8);p+=8;return v;}
 double F64(){uint64_t raw=U64();double v=0;memcpy(&v,&raw,8);return v;}
 std::wstring Str(){
  uint32_t chars=U32();
  if(!ok||chars>4096||p+size_t(chars)*2>end){ok=false;return {};}
  std::wstring s(chars,L'\0');
  for(uint32_t i=0;i<chars;i++){s[i]=wchar_t(uint16_t(p[0])|(uint16_t(p[1])<<8));p+=2;}
  return s;
 }
};

void LoadLocked(){
 if(loaded)return;
 loaded=true;
 auto path=StorePath();
 if(path.empty())return;
 HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return;
 LARGE_INTEGER size{};std::vector<uint8_t> bytes;
 if(GetFileSizeEx(file,&size)&&size.QuadPart>8&&size.QuadPart<256ll*1024*1024){
  bytes.resize(size_t(size.QuadPart));
  DWORD got=0;
  if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&got,nullptr)||got!=bytes.size())bytes.clear();
 }
 CloseHandle(file);
 if(bytes.size()<8)return;
 Reader r{bytes.data(),bytes.data()+bytes.size()};
 if(r.U32()!=Magic)return;
 uint32_t count=r.U32();
 if(count>MaxEntries)count=MaxEntries;
 for(uint32_t i=0;i<count&&r.ok;i++){
  auto key=r.Str();
  CachedMeta e;
  e.size=r.U64();e.written=r.U64();e.sidecarSize=r.U64();e.sidecarWritten=r.U64();
  e.width=r.U32();e.height=r.U32();e.orientation=r.U32();
  e.rating=int(int32_t(r.U32()));e.hasRating=r.U32()!=0;
  e.label=r.Str();e.xmpSource=r.Str();e.camera=r.Str();e.lens=r.Str();e.dateTaken=r.Str();
  e.aperture=r.F64();e.shutter=r.F64();e.focalLength=r.F64();e.iso=long(int32_t(r.U32()));
  if(!r.ok||key.empty())break;
  e.valid=true;
  entries.emplace(std::move(key),std::move(e));
 }
}

void SaveLocked(){
 if(!dirty)return;
 auto path=StorePath();
 if(path.empty())return;
 Writer w;
 w.U32(Magic);
 uint32_t count=uint32_t((std::min)(entries.size(),MaxEntries));
 w.U32(count);
 uint32_t written=0;
 for(auto& [key,e]:entries){
  if(written++>=count)break;
  w.Str(key);
  w.U64(e.size);w.U64(e.written);w.U64(e.sidecarSize);w.U64(e.sidecarWritten);
  w.U32(e.width);w.U32(e.height);w.U32(e.orientation);
  w.U32(uint32_t(int32_t(e.rating)));w.U32(e.hasRating?1u:0u);
  w.Str(e.label);w.Str(e.xmpSource);w.Str(e.camera);w.Str(e.lens);w.Str(e.dateTaken);
  w.F64(e.aperture);w.F64(e.shutter);w.F64(e.focalLength);w.U32(uint32_t(int32_t(e.iso)));
 }
 auto temporary=path+L".tmp";
 HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return;
 DWORD done=0;
 bool ok=WriteFile(file,w.bytes.data(),DWORD(w.bytes.size()),&done,nullptr)&&done==w.bytes.size();
 CloseHandle(file);
 if(!ok){DeleteFileW(temporary.c_str());return;}
 if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))dirty=false;
 else DeleteFileW(temporary.c_str());
}

}   // namespace

void MetaCacheLoad(){std::lock_guard lock(mx);LoadLocked();}
void MetaCacheFlush(){std::lock_guard lock(mx);SaveLocked();}
size_t MetaCacheSize(){std::lock_guard lock(mx);return entries.size();}

CachedMeta MetaCacheGet(const std::wstring& path){
 uint64_t size=0,written=0;
 if(!Stamp(path,size,written))return {};
 uint64_t sidecarSize=0,sidecarWritten=0;
 SidecarStamp(path,sidecarSize,sidecarWritten);
 std::lock_guard lock(mx);
 LoadLocked();
 auto found=entries.find(Lower(path));
 if(found==entries.end())return {};
 auto& e=found->second;
 // The image itself, and the sidecar beside it, are both part of the answer.
 // Lightroom changes a rating by rewriting only the sidecar.
 if(e.size!=size||e.written!=written||e.sidecarSize!=sidecarSize||e.sidecarWritten!=sidecarWritten)return {};
 return e;
}

void MetaCachePut(const std::wstring& path,const CachedMeta& entry){
 std::lock_guard lock(mx);
 LoadLocked();
 if(entries.size()>=MaxEntries&&!entries.count(Lower(path))){
  // No access ordering is kept: this is a derived cache, and dropping an
  // arbitrary tenth of it costs one re-read each, not correctness.
  size_t drop=entries.size()/10;
  for(auto it=entries.begin();it!=entries.end()&&drop;)
   {it=entries.erase(it);drop--;}
 }
 entries[Lower(path)]=entry;
 dirty=true;
}

CachedMeta MetaCacheLookup(const std::wstring& path){
 auto hit=MetaCacheGet(path);
 if(hit.valid)return hit;
 auto record=ReadMetadata(path);
 CachedMeta e;
 e.valid=true;
 Stamp(path,e.size,e.written);
 SidecarStamp(path,e.sidecarSize,e.sidecarWritten);
 e.width=record.width;e.height=record.height;e.orientation=record.orientation;
 e.rating=record.rating;e.hasRating=record.hasRating;
 e.label=record.label;e.xmpSource=record.xmpSource;
 e.camera=record.cameraModel.empty()?record.cameraMake
          :(record.cameraMake.empty()||record.cameraModel.rfind(record.cameraMake,0)==0
            ?record.cameraModel:record.cameraMake+L" "+record.cameraModel);
 e.lens=record.lens;e.dateTaken=record.dateTaken;
 e.aperture=record.aperture;e.shutter=record.shutter;e.focalLength=record.focalLength;e.iso=record.iso;
 MetaCachePut(path,e);
 return e;
}
