// Vetro Look, GPL-3.0-or-later.
// A photo-aware file-system index. Raw MFT enumeration needs a volume handle
// most processes cannot open without elevation, so this instead does one
// fast walk per drive (FindFirstFileEx, no per-file metadata beyond what the
// directory entry already carries) and then never repeats it: the result is
// cached to disk, later launches revalidate only what might have changed,
// and a recursive ReadDirectoryChangesW watcher per drive is the non-admin
// equivalent of watching the USN journal — same effect, standard API.
#include "index.h"
#include "image.h"
#include "usn.h"
#include <shlobj.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
namespace fs=std::filesystem;

namespace{

uint64_t Fnv1a(const std::wstring& s){
 uint64_t h=1469598103934665603ull;
 for(wchar_t c:s){h^=uint64_t((wchar_t)towlower(c));h*=1099511628211ull;}
 return h;
}
uint64_t ToU64(const FILETIME& ft){return (uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;}

// True filesystem identity on NTFS is (volume serial, 128-bit file id): it
// survives rename and move, unlike a path. We fold it into the 64-bit id the
// rest of the app already keys everything on (hero-transition matching, grid
// cell lookup) via FNV-1a over the three raw numbers — collision odds at a
// library's realistic size (10^4-10^6 items) are astronomically low, and it
// avoids widening `uint64_t id` into a 192-bit type across the whole UI layer.
// Falls back to a path hash only when the real id can't be read (non-NTFS
// volume, or the item vanished between listing and querying).
bool TryGetFileId(const std::wstring& path,uint64_t& volSerial,uint64_t& idLo,uint64_t& idHi){
 HANDLE h=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
  nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(h==INVALID_HANDLE_VALUE)return false;
 FILE_ID_INFO info{};
 BOOL ok=GetFileInformationByHandleEx(h,FileIdInfo,&info,sizeof(info));
 CloseHandle(h);
 if(!ok)return false;
 volSerial=info.VolumeSerialNumber;
 memcpy(&idLo,&info.FileId.Identifier[0],8);
 memcpy(&idHi,&info.FileId.Identifier[8],8);
 return true;
}
uint64_t StableId(const std::wstring& path){
 uint64_t vol,lo,hi;
 if(TryGetFileId(path,vol,lo,hi)){
  uint64_t h=1469598103934665603ull;
  auto mix=[&](uint64_t v){h^=v;h*=1099511628211ull;};
  mix(vol);mix(lo);mix(hi);
  return h?h:1; // 0 is used elsewhere as "no id"
 }
 std::wstring lower=path;std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
 return Fnv1a(lower); // non-NTFS or the item vanished mid-scan: path is all we have left
}

std::wstring CacheDir(){
 wchar_t* base=nullptr;
 std::wstring dir;
 if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&base))){dir=base;CoTaskMemFree(base);}
 if(dir.empty())return L"";
 dir+=L"\\VetroLook";
 CreateDirectoryW(dir.c_str(),nullptr);
 return dir;
}
std::wstring CacheFile(){auto d=CacheDir();return d.empty()?L"":d+
#ifdef VETRO_REVIEW_BUILD
 L"\\index-review.bin";
#else
 L"\\index.bin";
#endif
}

// Paths that are never worth showing as photo folders: system trees, package
// caches and developer scratch directories that happen to carry small image
// assets. Kept short and conservative on purpose.
bool ExcludedFolder(const std::wstring& lower){
 static const wchar_t* substrings[]={
  L"\\appdata\\",L"\\.codex\\",L"\\cache\\",L"\\caches\\",L"\\.gradle\\",
  L"\\androidstudioprojects\\",L"\\android\\sdk\\",L"\\package cache\\",L"\\windows kits\\",
  L"\\windows\\",L"\\programdata\\",L"\\program files\\",L"\\program files (x86)\\",
  L"\\$recycle.bin\\",L"\\node_modules\\",L"\\.git\\",L"\\.svn\\",L"\\.hg\\",
  L"\\appdata\\local\\temp\\",L"\\appdata\\local\\packages\\",L"\\appdata\\local\\microsoft\\",
  L"\\appdata\\local\\google\\chrome\\",L"\\appdata\\local\\mozilla\\",L"\\appdata\\local\\vetrolook\\",
  L"\\system volume information\\",L"\\.vs\\",L"\\obj\\",L"\\bin\\debug\\",L"\\bin\\release\\",L"\\temp\\",L"\\tmp\\"};
 for(auto s:substrings)if(lower.find(s)!=std::wstring::npos)return true;
 return false;
}
bool InDevelopmentTree(const fs::path& input){
 static const wchar_t* markers[]={L".git",L"CMakeLists.txt",L"package.json",L"build.gradle",L"build.gradle.kts",L"settings.gradle",L"settings.gradle.kts",L"Cargo.toml",L"pyproject.toml"};
 for(auto path=input;!path.empty();){
  for(auto marker:markers)if(GetFileAttributesW((path/marker).c_str())!=INVALID_FILE_ATTRIBUTES)return true;
  auto parent=path.parent_path();if(parent==path)break;path=parent;
 }
 return false;
}
bool SmallTechnicalFolder(const FolderEntry& entry){
 if(entry.photoCount>4)return false;
 std::wstring name=entry.name;std::transform(name.begin(),name.end(),name.begin(),towlower);
 static const std::unordered_set<std::wstring> names={L"assets",L"asset",L"resources",L"resource",L"icons",L"icon",L"images",L"img",L"thumbs",L"thumbnails",L"docprops",L"media"};
 return names.count(name)!=0;
}

std::mutex mx;
std::condition_variable cv;
std::unordered_map<uint64_t,FolderEntry> folders;
std::unordered_map<std::wstring,std::vector<PhotoEntry>> photosByFolder; // key: lowercase path
std::unordered_map<std::wstring,uint64_t> folderIdByPath; // key: lowercase path -> folders[] key
std::unordered_map<wchar_t,UsnJournalPos> usnPositions; // per-drive fast-path catch-up state
std::atomic<bool> stopping{false},scanning{false};
std::atomic<uint64_t> knownPhotos{0};
std::thread scannerThread;
std::thread priorityThread;
std::deque<std::wstring> priorityFolders;
std::unordered_map<std::wstring,FolderState> folderStates;
std::atomic<bool> cacheReady{false};
std::vector<std::thread> watcherThreads;
std::atomic<int> watcherGeneration{0};
HWND notifyWindow=nullptr;UINT notifyFolderMsg=0,notifyPhotoMsg=0;
ULONGLONG lastNotify=0;
std::mutex workMx;std::condition_variable workCv;
std::deque<std::wstring> dirtyFolders;      // pending targeted re-scans
std::set<std::wstring> dirtyPending;
bool wantRescan=false,wantFullRebuild=false,wantDriveScan=false;

void NotifyFolders(){
 ULONGLONG now=GetTickCount64();
 if(now-lastNotify<120)return;
 lastNotify=now;
 if(notifyWindow)PostMessageW(notifyWindow,notifyFolderMsg,0,0);
}
void NotifyProgress(){if(notifyWindow)PostMessageW(notifyWindow,notifyPhotoMsg,0,0);}

// Lists the direct children of one folder and folds the supported images
// into a FolderEntry; returns the subdirectories to descend into next. Never
// throws: an inaccessible folder just yields nothing.
// `previous` (keyed by lowercase filename) lets a rescan reuse a photo's
// already-known stable id without touching disk again: only a genuinely new
// or changed file pays for a CreateFile/GetFileInformationByHandleEx round
// trip. A rename or cross-folder move never matches by filename here, so it
// always recomputes — but since the id is the file's real NTFS identity, the
// recomputed value comes out identical to what it was before the move.
std::vector<std::wstring> ScanOneFolder(const std::wstring& path,FolderEntry& out,std::vector<PhotoEntry>& photos,
 const std::unordered_map<std::wstring,PhotoEntry>* previous=nullptr,bool applyDevelopmentFilter=true){
 std::vector<std::wstring> subdirs;
 out=FolderEntry{};
 out.path=path;out.id=StableId(path);out.name=fs::path(path).filename().wstring();
 photos.clear();
 if(GetFileAttributesW((path+L"\\.nomedia").c_str())!=INVALID_FILE_ATTRIBUTES||
    (applyDevelopmentFilter&&InDevelopmentTree(fs::path(path))))return subdirs;
 std::wstring pattern=path+L"\\*";
 WIN32_FIND_DATAW data{};
 HANDLE find=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
 if(find==INVALID_HANDLE_VALUE)return subdirs;
 do{
  if(!wcscmp(data.cFileName,L".")||!wcscmp(data.cFileName,L".."))continue;
  // A drive root already ends in a separator, so appending another one used
  // to spell every path under it "C:\\Users\..." — internally harmless, since
  // every index key runs through NormalisePath, but the raw string leaks into
  // FolderEntry/PhotoEntry and then fails a plain comparison against the same
  // file spelled the ordinary way.
  bool rooted=!path.empty()&&(path.back()==L'\\'||path.back()==L'/');
  std::wstring child=path+(rooted?L"":L"\\")+data.cFileName;
  if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
   // Junctions/symlinks are not descended into, so a cycle can never form.
   if(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)continue;
   std::wstring lower=child;std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
   if(ExcludedFolder(L"\\"+lower+L"\\"))continue;
   subdirs.push_back(child);
  }else{
   if(!Supported(child))continue;
   PhotoEntry photo;
   photo.path=child;photo.name=data.cFileName;
   auto dot=photo.name.find_last_of(L'.');
   photo.ext=dot==std::wstring::npos?L"":photo.name.substr(dot+1);
   photo.size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
   photo.modified=ToU64(data.ftLastWriteTime);
   photo.id=0;
   if(previous){
    std::wstring lowerName=photo.name;std::transform(lowerName.begin(),lowerName.end(),lowerName.begin(),towlower);
    auto it=previous->find(lowerName);
    if(it!=previous->end()&&it->second.size==photo.size&&it->second.modified==photo.modified)photo.id=it->second.id;
   }
   photo.id=StableId(child); // size/mtime equality does not prove physical identity
   std::transform(photo.ext.begin(),photo.ext.end(),photo.ext.begin(),towlower);
   photos.push_back(photo);
   out.totalBytes+=photo.size;
   out.modified=(std::max)(out.modified,photo.modified);
  }
 }while(FindNextFileW(find,&data));
 FindClose(find);
 if(!photos.empty()){
  out.path=path;
  auto slash=path.find_last_of(L'\\');
  out.name=slash==std::wstring::npos?path:path.substr(slash+1);
  out.photoCount=uint32_t(photos.size());
  out.id=StableId(path);
  out.sampleCount=uint8_t(std::min<size_t>(4,photos.size()));
  for(uint8_t i=0;i<out.sampleCount;i++)out.samples[i]=photos[i].path;
 }
 return subdirs;
}
std::unordered_map<std::wstring,PhotoEntry> SnapshotPhotosByName(const std::wstring& folderPath){
 std::wstring lower=folderPath;std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
 std::unordered_map<std::wstring,PhotoEntry> out;
 std::lock_guard lock(mx);
 auto found=photosByFolder.find(lower);
 if(found!=photosByFolder.end())for(auto& p:found->second){
  std::wstring name=p.name;std::transform(name.begin(),name.end(),name.begin(),towlower);
  out[name]=p;
 }
 return out;
}

void StoreFolder(const FolderEntry& entry,std::vector<PhotoEntry>&& photos){
 std::wstring lower=entry.path;std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
 std::lock_guard lock(mx);
 auto pathIdentity=folderIdByPath.find(lower);
 if(pathIdentity!=folderIdByPath.end()&&pathIdentity->second!=entry.id)folders.erase(pathIdentity->second);
 auto old=folders.find(entry.id);
 if(old!=folders.end()&&NormalisePath(old->second.path)!=lower){
  auto oldKey=NormalisePath(old->second.path);
  auto oldPhotos=photosByFolder.find(oldKey);
  if(oldPhotos!=photosByFolder.end()){knownPhotos-=oldPhotos->second.size();photosByFolder.erase(oldPhotos);}
  folderIdByPath.erase(oldKey);
 }
 auto previous=photosByFolder.find(lower);
 if(previous!=photosByFolder.end())knownPhotos-=previous->second.size();
 if(entry.photoCount&&!SmallTechnicalFolder(entry)){
  // A rename/move keeps the same id (real NTFS identity), but the old
  // folderIdByPath entry for a *different* previous path would otherwise
  // leak; a rescan always calls this with the folder's current path so the
  // stale entry only exists if it was actually removed first (RemoveFolder).
  auto display=entry;
  std::vector<const PhotoEntry*> candidates;for(auto& photo:photos)candidates.push_back(&photo);
  std::sort(candidates.begin(),candidates.end(),[](auto a,auto b){return a->modified!=b->modified?a->modified>b->modified:a->name<b->name;});
  display.sampleCount=uint8_t((std::min)(size_t(4),candidates.size()));
  for(uint8_t i=0;i<display.sampleCount;++i)display.samples[i]=candidates[i]->path;
  folders[entry.id]=std::move(display);
  photosByFolder[lower]=std::move(photos);
  folderIdByPath[lower]=entry.id;
  knownPhotos+=entry.photoCount;
 }else{
  auto it=folderIdByPath.find(lower);
  if(it!=folderIdByPath.end()){folders.erase(it->second);folderIdByPath.erase(it);}
  photosByFolder.erase(lower);
 }
}
void RemoveFolder(const std::wstring& path){
 std::wstring lower=path;std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
 std::lock_guard lock(mx);
 auto previous=photosByFolder.find(lower);
 if(previous!=photosByFolder.end()){knownPhotos-=previous->second.size();photosByFolder.erase(previous);}
 auto it=folderIdByPath.find(lower);
 if(it!=folderIdByPath.end()){folders.erase(it->second);folderIdByPath.erase(it);}
}

// Walks a whole subtree iteratively (no recursion depth limit) and reports
// every folder as it finishes, so the gallery fills in while this still runs.
void WalkTree(const std::wstring& root){
 std::vector<std::wstring> stack{root};
 int sinceNotify=0;
 while(!stack.empty()&&!stopping){
  auto current=std::move(stack.back());stack.pop_back();
  FolderEntry entry;std::vector<PhotoEntry> photos;
  auto subdirs=ScanOneFolder(current,entry,photos);
  if(entry.photoCount)StoreFolder(entry,std::move(photos));
  for(auto& d:subdirs)stack.push_back(std::move(d));
  if(++sinceNotify>=24){sinceNotify=0;NotifyFolders();NotifyProgress();}
 }
 NotifyFolders();NotifyProgress();
}

// The real fast path: MFT enumeration via FSCTL_ENUM_USN_DATA instead of a
// FindFirstFile walk. Only usable when the volume handle can be opened,
// which needs elevation on modern Windows — returns false immediately (no
// partial state stored) when it can't, so the caller falls back to WalkTree
// for that one drive without disturbing anything else.
bool TryUsnFullScan(wchar_t driveLetter){
 UsnJournalPos pos;
 bool ok=UsnFastEnumerate(driveLetter,[&](const UsnFolderResult& f,std::vector<UsnPhotoResult>&& usnPhotos){
  FolderEntry entry;
  entry.path=f.path;entry.name=f.name;entry.id=f.id;
  entry.photoCount=f.photoCount;entry.totalBytes=f.totalBytes;entry.modified=f.modified;
  entry.sampleCount=f.sampleCount;
  for(uint8_t i=0;i<f.sampleCount;i++)entry.samples[i]=f.samples[i];
  std::vector<PhotoEntry> photos;photos.reserve(usnPhotos.size());
  for(auto& p:usnPhotos){
   PhotoEntry pe;pe.path=p.path;pe.name=p.name;pe.ext=p.ext;pe.size=p.size;pe.modified=p.modified;pe.id=p.id;
   photos.push_back(std::move(pe));
  }
  StoreFolder(entry,std::move(photos));
  NotifyFolders();NotifyProgress();
 },pos);
 if(ok){std::lock_guard lock(mx);usnPositions[driveLetter]=pos;}
 return ok;
}
// The cheap startup path once a fast-path journal position is already known:
// reads only what changed since last time (including everything that
// happened while the app was closed — the one thing ReadDirectoryChangesW
// categorically cannot do) instead of re-listing every known folder.
bool TryUsnCatchUp(wchar_t driveLetter){
 UsnJournalPos pos;
 {std::lock_guard lock(mx);auto it=usnPositions.find(driveLetter);if(it==usnPositions.end())return false;pos=it->second;}
 std::vector<std::wstring> dirty;
 bool ok=UsnCatchUp(driveLetter,pos,[&](const std::wstring& path){dirty.push_back(path);});
 {std::lock_guard lock(mx);usnPositions[driveLetter]=pos;}
 if(!ok)return false;
 for(auto& path:dirty){
  if(stopping)return true;
  FolderEntry entry;std::vector<PhotoEntry> photos;
  if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){RemoveFolder(path);continue;}
  auto previous=SnapshotPhotosByName(path);
  ScanOneFolder(path,entry,photos,&previous);
  StoreFolder(entry,std::move(photos));
 }
 if(!dirty.empty())NotifyFolders();
 return true;
}

std::vector<std::wstring> LocalDrives(){
 std::vector<std::wstring> out;
 DWORD mask=GetLogicalDrives();
 for(int i=0;i<26;i++){
  if(!(mask&(1u<<i)))continue;
  std::wstring root=std::wstring(1,wchar_t(L'A'+i))+L":\\";
  UINT type=GetDriveTypeW(root.c_str());
  if(type==DRIVE_FIXED||type==DRIVE_REMOVABLE||type==DRIVE_REMOTE)out.push_back(root);
 }
 return out;
}

// ------------------------------------------------------------- persistence
// VLK2: unlike VLK1, ids are the file's real NTFS identity (see StableId),
// not a path hash, and are persisted directly so a warm load never has to
// re-open every file just to read its own cache back. Bumping the magic
// means one full rescan the first time a VLK1-cache build launches after
// this change, which is the correct one-time migration cost for a format
// change — it is not paid again after that.
constexpr uint32_t CacheMagic=0x564C4B36; // "VLK6": rebuild after targeted-folder correctness fixes
void SaveCache(){
 auto path=CacheFile();if(path.empty())return;
 std::ofstream file(fs::path(path),std::ios::binary|std::ios::trunc);
 if(!file)return;
 auto writeStr=[&](const std::wstring& s){uint32_t n=uint32_t(s.size());file.write((char*)&n,4);file.write((char*)s.data(),n*sizeof(wchar_t));};
 std::lock_guard lock(mx);
 uint32_t magic=CacheMagic;file.write((char*)&magic,4);
 uint32_t count=uint32_t(folders.size());file.write((char*)&count,4);
 for(auto& [id,entry]:folders){
  writeStr(entry.path);file.write((char*)&entry.modified,8);file.write((char*)&entry.id,8);
  auto found=photosByFolder.find([&]{std::wstring l=entry.path;std::transform(l.begin(),l.end(),l.begin(),towlower);return l;}());
  uint32_t photoCount=found!=photosByFolder.end()?uint32_t(found->second.size()):0;
  file.write((char*)&photoCount,4);
  if(found!=photosByFolder.end())for(auto& photo:found->second){
   writeStr(photo.name);writeStr(photo.ext);
   file.write((char*)&photo.size,8);file.write((char*)&photo.modified,8);file.write((char*)&photo.id,8);
  }
 }
 uint32_t driveCount=uint32_t(usnPositions.size());file.write((char*)&driveCount,4);
 for(auto& [drive,pos]:usnPositions){
  file.write((char*)&drive,sizeof(wchar_t));
  file.write((char*)&pos.journalId,8);file.write((char*)&pos.nextUsn,8);
 }
}
bool LoadCache(){
 auto path=CacheFile();if(path.empty())return false;
 std::ifstream file(fs::path(path),std::ios::binary);
 if(!file)return false;
 auto readStr=[&](std::wstring& s)->bool{
  uint32_t n=0;if(!file.read((char*)&n,4)||n>1u<<20)return false;
  s.resize(n);if(n)file.read((char*)s.data(),n*sizeof(wchar_t));return bool(file);
 };
 uint32_t magic=0;if(!file.read((char*)&magic,4)||magic!=CacheMagic)return false;
 uint32_t count=0;if(!file.read((char*)&count,4)||count>1u<<22)return false;
 std::unordered_map<uint64_t,FolderEntry> loadedFolders;
 std::unordered_map<std::wstring,std::vector<PhotoEntry>> loadedPhotos;
 std::unordered_map<std::wstring,uint64_t> loadedFolderIds;
 uint64_t total=0;
 for(uint32_t i=0;i<count&&file;i++){
  std::wstring folderPath;if(!readStr(folderPath))return false;
  uint64_t modified=0;if(!file.read((char*)&modified,8))return false;
  uint64_t folderId=0;if(!file.read((char*)&folderId,8))return false;
  uint32_t photoCount=0;if(!file.read((char*)&photoCount,4)||photoCount>1u<<20)return false;
  FolderEntry entry;entry.path=folderPath;
  auto slash=folderPath.find_last_of(L'\\');
  entry.name=slash==std::wstring::npos?folderPath:folderPath.substr(slash+1);
  entry.modified=modified;entry.id=folderId;
  std::vector<PhotoEntry> photos;photos.reserve(photoCount);
  for(uint32_t j=0;j<photoCount&&file;j++){
   PhotoEntry photo;
   if(!readStr(photo.name)||!readStr(photo.ext))return false;
   if(!file.read((char*)&photo.size,8)||!file.read((char*)&photo.modified,8))return false;
   if(!file.read((char*)&photo.id,8))return false;
   photo.path=folderPath+L"\\"+photo.name;
   entry.totalBytes+=photo.size;
   photos.push_back(std::move(photo));
  }
  entry.photoCount=uint32_t(photos.size());
  entry.sampleCount=uint8_t(std::min<size_t>(4,photos.size()));
  for(uint8_t k=0;k<entry.sampleCount;k++)entry.samples[k]=photos[k].path;
  if(entry.photoCount){
   std::wstring lower=folderPath;std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
   total+=entry.photoCount;
   loadedPhotos[lower]=std::move(photos);
   loadedFolderIds[lower]=entry.id;
   loadedFolders[entry.id]=std::move(entry);
  }
 }
 std::unordered_map<wchar_t,UsnJournalPos> loadedUsn;
 uint32_t driveCount=0;
 if(file.read((char*)&driveCount,4)&&driveCount<=64){
  for(uint32_t i=0;i<driveCount&&file;i++){
   wchar_t drive=0;uint64_t journalId=0;int64_t nextUsn=0;
   if(!file.read((char*)&drive,sizeof(wchar_t)))break;
   if(!file.read((char*)&journalId,8)||!file.read((char*)&nextUsn,8))break;
   loadedUsn[drive]=UsnJournalPos{journalId,nextUsn,true};
  }
 } // an older VLK3 file written before this section existed just leaves it empty — fine, not fatal
 std::lock_guard lock(mx);
 folders=std::move(loadedFolders);
 photosByFolder=std::move(loadedPhotos);
 folderIdByPath=std::move(loadedFolderIds);
 usnPositions=std::move(loadedUsn);
 knownPhotos=total;
 return true;
}

// A cheap pass: re-list only the folders the cache already knows about (one
// FindFirstFile per folder, not a walk) and drop anything that vanished; new
// subtrees are only discovered by watchers or a full rebuild. This is what
// "rescan" runs — no drive is walked end to end.
void VerifyKnown(const std::unordered_set<wchar_t>& skip={}){
 std::vector<std::wstring> paths;
 {std::lock_guard lock(mx);paths.reserve(folders.size());for(auto& [id,entry]:folders)paths.push_back(entry.path);}
 for(auto& path:paths){
  if(stopping)return;
  if(!path.empty()&&skip.count(wchar_t(towupper(path[0]))))continue;
  if(ExcludedFolder(L"\\"+NormalisePath(path)+L"\\")){RemoveFolder(path);continue;}
  if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){RemoveFolder(path);continue;}
  FolderEntry entry;std::vector<PhotoEntry> photos;
  auto previous=SnapshotPhotosByName(path);
  ScanOneFolder(path,entry,photos,&previous);
  StoreFolder(entry,std::move(photos));
 }
 NotifyFolders();
}
void ScanNewGround(){
 // A full rebuild, or the first run with no cache at all. NTFS drives try
 // the real MFT/USN fast path first; anything that fails it (no NTFS, or no
 // elevation) gets the ordinary recursive walk instead — same end result,
 // just slower to reach it.
 for(auto& drive:LocalDrives()){
  if(stopping)return;
  wchar_t letter=drive[0];
  if(TryUsnFullScan(letter))continue;
  WalkTree(drive);
 }
}

// ------------------------------------------------------------ live watch --
void RunWatcher(std::wstring root,int generation){
 HANDLE dir=CreateFileW(root.c_str(),FILE_LIST_DIRECTORY,
  FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
  FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr);
 if(dir==INVALID_HANDLE_VALUE)return;
 HANDLE changed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 if(!changed){CloseHandle(dir);return;}
 std::vector<uint8_t> buffer(64*1024);
 while(!stopping&&generation==watcherGeneration.load()){
  OVERLAPPED operation{};operation.hEvent=changed;ResetEvent(changed);
  DWORD got=0;
  BOOL ok=ReadDirectoryChangesW(dir,buffer.data(),DWORD(buffer.size()),TRUE,
   FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE,
   nullptr,&operation,nullptr);
  if(ok){
   while(WaitForSingleObject(changed,200)==WAIT_TIMEOUT&&!stopping&&generation==watcherGeneration.load()){}
   if(stopping||generation!=watcherGeneration.load())CancelIoEx(dir,&operation);
   ok=GetOverlappedResult(dir,&operation,&got,TRUE);
  }
  if(!ok||stopping)break;
  if(!got){std::lock_guard lock(workMx);wantFullRebuild=true;workCv.notify_all();continue;}
  size_t offset=0;
  while(offset<got){
   auto* info=(FILE_NOTIFY_INFORMATION*)(buffer.data()+offset);
   std::wstring name(info->FileName,info->FileNameLength/sizeof(wchar_t));
   std::wstring full=root.substr(0,root.size()-1)+L"\\"+name; // root already ends in '\\'
   auto slash=full.find_last_of(L'\\');
   std::wstring parent=slash==std::wstring::npos?root:full.substr(0,slash);
   bool goingAway=info->Action==FILE_ACTION_REMOVED||info->Action==FILE_ACTION_RENAMED_OLD_NAME;
   bool relevant=Supported(full);
   DWORD attrs=goingAway?INVALID_FILE_ATTRIBUTES:GetFileAttributesW(full.c_str());
   bool isFolder=attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY);
   {
    std::lock_guard lock(workMx);
    // The parent's own photo count may have changed either way, so it
    // always needs a fresh look — cheap, since that is one directory listing.
    if((relevant||isFolder||goingAway)&&dirtyPending.insert(parent).second)dirtyFolders.push_back(parent);
    // A folder that just appeared (create/rename-in) may itself already
    // hold photos; one that is disappearing must have its own entry, if
    // any, dropped rather than left pointing at a path that no longer exists.
    if(isFolder&&dirtyPending.insert(full).second)dirtyFolders.push_back(full);
   }
   if(goingAway)RemoveFolder(full);
   workCv.notify_all();
   if(!info->NextEntryOffset)break;
   offset+=info->NextEntryOffset;
  }
 }
 CloseHandle(changed);CloseHandle(dir);
}
void SpawnWatchers(){
 int generation=++watcherGeneration;
 for(auto& t:watcherThreads)if(t.joinable())CancelSynchronousIo(t.native_handle());
 for(auto& t:watcherThreads)if(t.joinable())t.join();
 watcherThreads.clear();
 for(auto& drive:LocalDrives())watcherThreads.emplace_back(RunWatcher,drive,generation);
}

void ScannerLoop(){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 scanning=true;
 bool hadCache=LoadCache();
 cacheReady=true;workCv.notify_all();
 NotifyFolders();NotifyProgress();
 SpawnWatchers();
 if(!hadCache)ScanNewGround();
 else{
  // Catch up on whatever happened while the app was closed — the gap
  // ReadDirectoryChangesW can't cover — for every drive we have a fast-path
  // journal position for, then the ordinary cheap liveness check for
  // everything else (including drives that never got a journal position:
  // non-NTFS, or no elevation available this run).
  std::unordered_set<wchar_t> caughtUp;
  for(auto& drive:LocalDrives()){
   if(TryUsnCatchUp(drive[0])){caughtUp.insert(drive[0]);continue;}
   bool invalid=false;
   {std::lock_guard lock(mx);auto position=usnPositions.find(drive[0]);invalid=position!=usnPositions.end()&&!position->second.valid;}
   if(invalid){
    auto snapshot=IndexSnapshotFolders();for(auto& folder:snapshot)if(!folder.path.empty()&&towupper(folder.path[0])==drive[0])RemoveFolder(folder.path);
    if(!TryUsnFullScan(drive[0]))WalkTree(drive);
    caughtUp.insert(drive[0]);
   }
  }
  VerifyKnown(caughtUp);
 }
 SaveCache();
 scanning=false;NotifyFolders();NotifyProgress();
 while(!stopping){
  std::wstring dirty;bool doRescan=false,doFull=false,doDrives=false;
  {
   std::unique_lock lock(workMx);
   workCv.wait_for(lock,std::chrono::milliseconds(400),[]{
    return stopping||!dirtyFolders.empty()||wantRescan||wantFullRebuild||wantDriveScan;
   });
   if(stopping)break;
   if(!dirtyFolders.empty()){dirty=dirtyFolders.front();dirtyFolders.pop_front();dirtyPending.erase(dirty);}
   doRescan=wantRescan;wantRescan=false;
   doFull=wantFullRebuild;wantFullRebuild=false;
   doDrives=wantDriveScan;wantDriveScan=false;
  }
  if(!dirty.empty()){
   FolderEntry entry;std::vector<PhotoEntry> photos;
   if(GetFileAttributesW(dirty.c_str())==INVALID_FILE_ATTRIBUTES)RemoveFolder(dirty);
   else{auto previous=SnapshotPhotosByName(dirty);ScanOneFolder(dirty,entry,photos,&previous);StoreFolder(entry,std::move(photos));}
   NotifyFolders();
  }
  if(doFull){
   scanning=true;NotifyFolders();
   {std::lock_guard lock(mx);folders.clear();photosByFolder.clear();knownPhotos=0;}
   ScanNewGround();SaveCache();
   scanning=false;NotifyFolders();NotifyProgress();
  }else if(doRescan){
   scanning=true;NotifyFolders();
   VerifyKnown();
   SaveCache();
   scanning=false;NotifyFolders();NotifyProgress();
  }
  if(doDrives)SpawnWatchers();
 }
 CoUninitialize();
}
}

void IndexStart(HWND notify,UINT folderMsg,UINT photoMsg){
 notifyWindow=notify;notifyFolderMsg=folderMsg;notifyPhotoMsg=photoMsg;
 stopping=false;
 scannerThread=std::thread(ScannerLoop);
 priorityThread=std::thread([]{
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  while(!stopping){
   std::wstring path;
   {std::unique_lock lock(workMx);workCv.wait(lock,[]{return stopping||(cacheReady&&!priorityFolders.empty());});
    if(stopping)break;path=priorityFolders.front();priorityFolders.pop_front();}
   FolderEntry folder;std::vector<PhotoEntry> photos;
   // An explicitly opened folder is authoritative user intent.  It must be
   // indexed even when it lives inside a project tree that the background
   // library scan deliberately suppresses.
   ScanOneFolder(path,folder,photos,nullptr,false);StoreFolder(folder,std::move(photos));
   {std::lock_guard lock(workMx);folderStates[path]=GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES?FolderState::Error:FolderState::Ready;}
   if(notifyWindow)PostMessageW(notifyWindow,notifyFolderMsg,0,0);
  }
  CoUninitialize();
 });
}
void IndexStop(){
 {std::lock_guard lock(workMx);stopping=true;}
 workCv.notify_all();
 if(priorityThread.joinable())priorityThread.join();
 if(scannerThread.joinable())scannerThread.join();
 ++watcherGeneration;
 for(auto& t:watcherThreads)if(t.joinable())CancelSynchronousIo(t.native_handle());
 for(auto& t:watcherThreads)if(t.joinable())t.join();
 watcherThreads.clear();SaveCache();
}
void IndexRescan(){{std::lock_guard lock(workMx);wantRescan=true;}workCv.notify_all();}
void IndexFullRebuild(){{std::lock_guard lock(workMx);wantFullRebuild=true;}workCv.notify_all();}
void IndexDrivesChanged(){{std::lock_guard lock(workMx);wantRescan=true;wantDriveScan=true;}workCv.notify_all();}
void IndexTouchFolder(const std::wstring& folder){
 auto path=NormalisePath(folder);
 {std::lock_guard lock(workMx);if(folderStates[path]==FolderState::Indexing)return;folderStates[path]=FolderState::Indexing;priorityFolders.push_back(path);}
 workCv.notify_all();
}
FolderState IndexFolderState(const std::wstring& folder){std::lock_guard lock(workMx);auto it=folderStates.find(NormalisePath(folder));return it==folderStates.end()?FolderState::Unknown:it->second;}
uint64_t PhysicalId(const std::wstring& path){return StableId(NormalisePath(path));}
std::vector<PhotoEntry> IndexInspectDirectory(const std::wstring& path,FolderEntry& folder){std::vector<PhotoEntry> photos;ScanOneFolder(NormalisePath(path),folder,photos,nullptr,false);return photos;}
bool IndexIsScanning(){return scanning;}
uint64_t IndexKnownPhotoCount(){return knownPhotos;}
std::vector<FolderEntry> IndexSnapshotFolders(){
 std::lock_guard lock(mx);
 std::vector<FolderEntry> out;out.reserve(folders.size());
 for(auto& [id,entry]:folders)out.push_back(entry);
 return out;
}
std::vector<PhotoEntry> IndexPhotosIn(const std::wstring& folder){
 std::wstring lower=NormalisePath(folder);
 std::lock_guard lock(mx);
 auto found=photosByFolder.find(lower);
 return found==photosByFolder.end()?std::vector<PhotoEntry>():found->second;
}
std::wstring NormalisePath(const std::wstring& path){
 std::error_code ec;auto absolute=fs::absolute(fs::path(path),ec);
 std::wstring lower=(ec?fs::path(path):absolute).lexically_normal().make_preferred().wstring();
 while(lower.size()>3&&(lower.back()==L'\\'||lower.back()==L'/'))lower.pop_back();
 std::transform(lower.begin(),lower.end(),lower.begin(),towlower);return lower;
}
uint64_t PathId(const std::wstring& normalised){return Fnv1a(normalised);}
