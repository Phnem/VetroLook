// Vetro Look, GPL-3.0-or-later.
#include "favourites.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
namespace fs=std::filesystem;

namespace{

// One record per favourite. Both keys are kept: the identity is what survives
// a rename, the path is what lets the Favourites collection be listed without
// walking every volume looking for file ids.
struct Entry{
 uint64_t volumeSerial=0,idLow=0,idHigh=0;   // NTFS identity, 0 when unavailable
 uint64_t size=0,written=0;                  // fallback identity
 std::wstring path;                          // last known location
 uint64_t marked=0;                          // FILETIME when it became a favourite
};

std::mutex mx;
std::vector<Entry> entries;
// identity hash -> index into `entries`; path (lowercased) -> index.
std::unordered_map<uint64_t,size_t> byIdentity;
std::unordered_map<std::wstring,size_t> byPath;
// lowercased parent folder -> how many favourites sit directly inside it.
std::unordered_map<std::wstring,uint32_t> byFolder;
bool loaded=false,dirty=false;
uint64_t revision=0;

std::wstring Lower(std::wstring s){for(auto& c:s)c=towlower(c);return s;}

uint64_t Mix(uint64_t a,uint64_t b,uint64_t c){
 uint64_t h=1469598103934665603ull;
 for(uint64_t v:{a,b,c}){h^=v;h*=1099511628211ull;}
 return h?h:1;
}

struct Identity{
 bool real=false;                            // came from the filesystem, not the path
 uint64_t volumeSerial=0,idLow=0,idHigh=0;
 uint64_t size=0,written=0;
};
bool ReadIdentity(const std::wstring& path,Identity& out){
 HANDLE file=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
  nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(file!=INVALID_HANDLE_VALUE){
  FILE_ID_INFO info{};
  if(GetFileInformationByHandleEx(file,FileIdInfo,&info,sizeof(info))){
   out.volumeSerial=info.VolumeSerialNumber;
   memcpy(&out.idLow,&info.FileId.Identifier[0],8);
   memcpy(&out.idHigh,&info.FileId.Identifier[8],8);
   // An all-zero id is what some network redirectors return; it identifies
   // nothing, so it must not be treated as identity.
   out.real=out.idLow||out.idHigh;
  }
  BY_HANDLE_FILE_INFORMATION basic{};
  if(GetFileInformationByHandle(file,&basic)){
   out.size=(uint64_t(basic.nFileSizeHigh)<<32)|basic.nFileSizeLow;
   out.written=(uint64_t(basic.ftLastWriteTime.dwHighDateTime)<<32)|basic.ftLastWriteTime.dwLowDateTime;
  }
  CloseHandle(file);
  return true;
 }
 WIN32_FILE_ATTRIBUTE_DATA attributes{};
 if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes))return false;
 out.size=(uint64_t(attributes.nFileSizeHigh)<<32)|attributes.nFileSizeLow;
 out.written=(uint64_t(attributes.ftLastWriteTime.dwHighDateTime)<<32)|attributes.ftLastWriteTime.dwLowDateTime;
 return true;
}

std::wstring Parent(const std::wstring& path){
 std::error_code ec;
 auto parent=fs::path(path).parent_path().wstring();
 (void)ec;
 while(!parent.empty()&&(parent.back()==L'\\'||parent.back()==L'/'))parent.pop_back();
 return Lower(parent);
}

std::wstring StorePath(){
 wchar_t* base=nullptr;std::wstring dir;
 if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&base))){dir=base;CoTaskMemFree(base);}
 if(dir.empty())return {};
 dir+=L"\\VetroLook";
 CreateDirectoryW(dir.c_str(),nullptr);
#ifdef VETRO_REVIEW_BUILD
 return dir+L"\\favourites-review.bin";
#else
 return dir+L"\\favourites.bin";
#endif
}

constexpr uint32_t Magic=0x564B4631;   // "VKF1"

void IndexLocked(size_t at){
 auto& e=entries[at];
 if(e.volumeSerial||e.idLow||e.idHigh)byIdentity[Mix(e.volumeSerial,e.idLow,e.idHigh)]=at;
 if(!e.path.empty()){
  byPath[Lower(e.path)]=at;
  byFolder[Parent(e.path)]++;
 }
}
void ReindexLocked(){
 byIdentity.clear();byPath.clear();byFolder.clear();
 for(size_t i=0;i<entries.size();i++)IndexLocked(i);
}

void WriteU64(std::vector<uint8_t>& out,uint64_t v){
 for(int i=0;i<8;i++)out.push_back(uint8_t(v>>(i*8)));
}
uint64_t ReadU64(const uint8_t* p){
 uint64_t v=0;for(int i=0;i<8;i++)v|=uint64_t(p[i])<<(i*8);return v;
}

void LoadLocked(){
 if(loaded)return;
 loaded=true;
 auto path=StorePath();
 if(path.empty())return;
 HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
  FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE){
  // Nothing stored yet. A build that predates this file kept favourites in the
  // registry, keyed by path; carry those across once so nobody loses theirs.
  HKEY key=nullptr;
  if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Favourites",0,KEY_READ,&key)==ERROR_SUCCESS){
   for(DWORD index=0;;index++){
    wchar_t name[32768];DWORD nameLength=32768,type=0,value=0,valueSize=sizeof(value);
    if(RegEnumValueW(key,index,name,&nameLength,nullptr,&type,(LPBYTE)&value,&valueSize)!=ERROR_SUCCESS)break;
    if(type!=REG_DWORD||!value)continue;
    Identity id{};
    if(!ReadIdentity(name,id))continue;
    Entry e;
    if(id.real){e.volumeSerial=id.volumeSerial;e.idLow=id.idLow;e.idHigh=id.idHigh;}
    e.size=id.size;e.written=id.written;e.path=name;e.marked=0;
    entries.push_back(std::move(e));
   }
   RegCloseKey(key);
   if(!entries.empty()){ReindexLocked();dirty=true;}
  }
  return;
 }
 LARGE_INTEGER size{};
 std::vector<uint8_t> bytes;
 if(GetFileSizeEx(file,&size)&&size.QuadPart>8&&size.QuadPart<64ll*1024*1024){
  bytes.resize(size_t(size.QuadPart));
  DWORD got=0;
  if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&got,nullptr)||got!=bytes.size())bytes.clear();
 }
 CloseHandle(file);
 if(bytes.size()<8)return;
 const uint8_t* p=bytes.data();const uint8_t* end=p+bytes.size();
 uint32_t magic=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
 uint32_t count=uint32_t(p[4])|(uint32_t(p[5])<<8)|(uint32_t(p[6])<<16)|(uint32_t(p[7])<<24);
 if(magic!=Magic)return;
 p+=8;
 entries.reserve(count);
 for(uint32_t i=0;i<count&&p+48<=end;i++){
  Entry e;
  e.volumeSerial=ReadU64(p);p+=8;
  e.idLow=ReadU64(p);p+=8;
  e.idHigh=ReadU64(p);p+=8;
  e.size=ReadU64(p);p+=8;
  e.written=ReadU64(p);p+=8;
  e.marked=ReadU64(p);p+=8;
  if(p+4>end)break;
  uint32_t chars=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  p+=4;
  if(chars>32768||p+size_t(chars)*2>end)break;
  e.path.assign(chars,L'\0');
  for(uint32_t c=0;c<chars;c++){e.path[c]=wchar_t(uint16_t(p[0])|(uint16_t(p[1])<<8));p+=2;}
  entries.push_back(std::move(e));
 }
 ReindexLocked();
}

void SaveLocked(){
 if(!dirty)return;
 auto path=StorePath();
 if(path.empty())return;
 std::vector<uint8_t> bytes;
 bytes.reserve(entries.size()*96+8);
 uint32_t count=uint32_t(entries.size());
 for(uint32_t v:{Magic,count})for(int i=0;i<4;i++)bytes.push_back(uint8_t(v>>(i*8)));
 for(auto& e:entries){
  WriteU64(bytes,e.volumeSerial);WriteU64(bytes,e.idLow);WriteU64(bytes,e.idHigh);
  WriteU64(bytes,e.size);WriteU64(bytes,e.written);WriteU64(bytes,e.marked);
  uint32_t chars=uint32_t((std::min)(e.path.size(),size_t(32768)));
  for(int i=0;i<4;i++)bytes.push_back(uint8_t(chars>>(i*8)));
  for(uint32_t c=0;c<chars;c++){
   uint16_t unit=uint16_t(e.path[c]);
   bytes.push_back(uint8_t(unit));bytes.push_back(uint8_t(unit>>8));
  }
 }
 // Write beside the real file and swap, so a crash mid-write cannot leave a
 // truncated store where a complete one used to be.
 auto temporary=path+L".tmp";
 HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return;
 DWORD written=0;
 bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size();
 CloseHandle(file);
 if(!ok){DeleteFileW(temporary.c_str());return;}
 if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
  DeleteFileW(temporary.c_str());
 else dirty=false;
}

// Finds the stored entry for a file, and repairs its remembered path when the
// file has been renamed or moved within the same volume.
size_t FindLocked(const std::wstring& path,bool repair){
 Identity id{};
 bool known=ReadIdentity(path,id);
 if(known&&id.real){
  auto found=byIdentity.find(Mix(id.volumeSerial,id.idLow,id.idHigh));
  if(found!=byIdentity.end()){
   auto& e=entries[found->second];
   if(repair&&!e.path.empty()&&Lower(e.path)!=Lower(path)){
    byPath.erase(Lower(e.path));
    auto oldFolder=Parent(e.path);
    auto count=byFolder.find(oldFolder);
    if(count!=byFolder.end()&&count->second)
     {if(--count->second==0)byFolder.erase(count);}
    e.path=path;e.size=id.size;e.written=id.written;
    byPath[Lower(path)]=found->second;
    byFolder[Parent(path)]++;
    dirty=true;
   }
   return found->second;
  }
 }
 auto byName=byPath.find(Lower(path));
 if(byName==byPath.end())return SIZE_MAX;
 auto& e=entries[byName->second];
 // A path-keyed entry only counts when the file still looks like the one that
 // was marked; otherwise a deleted-and-replaced file would inherit a heart.
 if(!e.volumeSerial&&!e.idLow&&!e.idHigh&&known&&e.size&&(e.size!=id.size||e.written!=id.written))
  return SIZE_MAX;
 return byName->second;
}

}   // namespace

void FavouritesLoad(){std::lock_guard lock(mx);LoadLocked();}
void FavouritesFlush(){std::lock_guard lock(mx);SaveLocked();}
uint64_t FavouritesRevision(){std::lock_guard lock(mx);return revision;}

bool FavouriteGet(const std::wstring& path){
 std::lock_guard lock(mx);LoadLocked();
 return FindLocked(path,true)!=SIZE_MAX;
}

void FavouriteSet(const std::wstring& path,bool on){
 std::lock_guard lock(mx);LoadLocked();
 size_t at=FindLocked(path,true);
 if(on){
  if(at!=SIZE_MAX)return;
  Identity id{};
  ReadIdentity(path,id);
  Entry e;
  if(id.real){e.volumeSerial=id.volumeSerial;e.idLow=id.idLow;e.idHigh=id.idHigh;}
  e.size=id.size;e.written=id.written;e.path=path;
  FILETIME now{};GetSystemTimeAsFileTime(&now);
  e.marked=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
  entries.push_back(std::move(e));
  IndexLocked(entries.size()-1);
 }else{
  if(at==SIZE_MAX)return;
  entries.erase(entries.begin()+ptrdiff_t(at));
  ReindexLocked();
 }
 dirty=true;revision++;
 SaveLocked();
}

std::vector<std::wstring> FavouritePaths(){
 std::lock_guard lock(mx);LoadLocked();
 std::vector<const Entry*> live;
 live.reserve(entries.size());
 for(auto& e:entries){
  if(e.path.empty())continue;
  if(GetFileAttributesW(e.path.c_str())==INVALID_FILE_ATTRIBUTES)continue;
  live.push_back(&e);
 }
 std::sort(live.begin(),live.end(),[](const Entry* a,const Entry* b){return a->marked>b->marked;});
 std::vector<std::wstring> out;
 out.reserve(live.size());
 for(auto* e:live)out.push_back(e->path);
 return out;
}

size_t FavouriteCount(){std::lock_guard lock(mx);LoadLocked();return entries.size();}

uint32_t FolderFavouriteCount(const std::wstring& folder){
 std::lock_guard lock(mx);LoadLocked();
 auto key=Lower(folder);
 while(!key.empty()&&(key.back()==L'\\'||key.back()==L'/'))key.pop_back();
 auto found=byFolder.find(key);
 return found==byFolder.end()?0u:found->second;
}
bool FolderHasFavourite(const std::wstring& folder){return FolderFavouriteCount(folder)>0;}
