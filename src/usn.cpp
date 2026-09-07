// Vetro Look, GPL-3.0-or-later.
#include "usn.h"
#include "image.h"
#include <winioctl.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace{

uint64_t Fnv1aId(uint64_t volSerial,uint64_t idLo,uint64_t idHi){
 uint64_t h=1469598103934665603ull;
 auto mix=[&](uint64_t v){h^=v;h*=1099511628211ull;};
 mix(volSerial);mix(idLo);mix(idHi);
 return h?h:1;
}
// For NTFS (not ReFS) a classic 64-bit file reference number is exactly the
// low 64 bits of the 128-bit FILE_ID_INFO identity index.cpp's StableId uses
// for the ordinary fallback path, so both paths land on the same id for the
// same file — this is what lets the two co-exist without ever disagreeing.
uint64_t IdFromFrn(uint64_t volSerial,DWORDLONG frn){return Fnv1aId(volSerial,frn,0);}

bool GetFrn(const std::wstring& path,DWORDLONG& frn){
 HANDLE h=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
  nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(h==INVALID_HANDLE_VALUE)return false;
 BY_HANDLE_FILE_INFORMATION info{};
 BOOL ok=GetFileInformationByHandle(h,&info);
 CloseHandle(h);
 if(!ok)return false;
 frn=(DWORDLONG(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
 return true;
}

// Paths that are never worth surfacing as photo folders — kept in sync with
// index.cpp's ExcludedFolder so the fast path and the fallback path agree on
// what counts as a folder worth showing.
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
bool InDevelopmentTree(const std::filesystem::path& input){
 static const wchar_t* markers[]={L".git",L"CMakeLists.txt",L"package.json",L"build.gradle",L"build.gradle.kts",L"settings.gradle",L"settings.gradle.kts",L"Cargo.toml",L"pyproject.toml"};
 for(auto path=input;!path.empty();){for(auto marker:markers)if(GetFileAttributesW((path/marker).c_str())!=INVALID_FILE_ATTRIBUTES)return true;
  auto parent=path.parent_path();if(parent==path)break;path=parent;}
 return false;
}

HANDLE OpenVolume(wchar_t drive){
 std::wstring vol=L"\\\\.\\";vol+=drive;vol+=L":";
 return CreateFileW(vol.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
}

bool QueryJournal(HANDLE vol,USN_JOURNAL_DATA_V0& out){
 DWORD br=0;
 return DeviceIoControl(vol,FSCTL_QUERY_USN_JOURNAL,nullptr,0,&out,sizeof(out),&br,nullptr)&&br>=sizeof(out);
}

bool ValidRecord(const uint8_t* p,const uint8_t* end){
 if(size_t(end-p)<offsetof(USN_RECORD_V2,FileName))return false;
 const auto* r=reinterpret_cast<const USN_RECORD_V2*>(p);
 return r->MajorVersion==2&&r->RecordLength>=offsetof(USN_RECORD_V2,FileName)&&
  r->RecordLength<=size_t(end-p)&&r->FileNameOffset>=offsetof(USN_RECORD_V2,FileName)&&
  !(r->FileNameLength%sizeof(wchar_t))&&!(r->FileNameOffset%sizeof(wchar_t))&&
  size_t(r->FileNameOffset)+r->FileNameLength<=r->RecordLength;
}

struct DirNode{DWORDLONG parent;std::wstring name;};

std::wstring ResolvePath(DWORDLONG frn,DWORDLONG rootFrn,const std::wstring& driveRoot,
 std::unordered_map<DWORDLONG,DirNode>& dirs,std::unordered_map<DWORDLONG,std::wstring>& cache,int depth=0){
 if(frn==rootFrn)return driveRoot;
 if(depth>256)return L""; // pathological/cyclic MFT data — bail rather than recurse forever
 auto cached=cache.find(frn);
 if(cached!=cache.end())return cached->second;
 auto it=dirs.find(frn);
 if(it==dirs.end())return L""; // parent missing from this enumeration — orphaned, skip it
 std::wstring parentPath=ResolvePath(it->second.parent,rootFrn,driveRoot,dirs,cache,depth+1);
 if(parentPath.empty())return L"";
 std::wstring full=parentPath+L"\\"+it->second.name;
 cache.emplace(frn,full);
 return full;
}

}

bool UsnFastEnumerate(wchar_t driveLetter,const UsnFolderSink& sink,UsnJournalPos& outPos){
 HANDLE vol=OpenVolume(driveLetter);
 if(vol==INVALID_HANDLE_VALUE)return false;
 struct VolGuard{HANDLE h;~VolGuard(){CloseHandle(h);}}guard{vol};

 USN_JOURNAL_DATA_V0 jd{};
 if(!QueryJournal(vol,jd))return false;

 std::wstring driveRoot=std::wstring(1,driveLetter)+L":";
 HANDLE root=CreateFileW((driveRoot+L"\\").c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(root==INVALID_HANDLE_VALUE)return false;
 FILE_ID_INFO identity{};BOOL haveIdentity=GetFileInformationByHandleEx(root,FileIdInfo,&identity,sizeof(identity));CloseHandle(root);
 if(!haveIdentity)return false;
 uint64_t volSerial=identity.VolumeSerialNumber;

 DWORDLONG rootFrn=0;
 if(!GetFrn(driveRoot+L"\\",rootFrn))return false;

 std::unordered_map<DWORDLONG,DirNode> dirs;dirs.reserve(1u<<16);
 struct PendingPhoto{std::wstring name;DWORDLONG frn,parent;uint64_t size,modified;};
 std::unordered_map<DWORDLONG,std::vector<PendingPhoto>> byParent;byParent.reserve(1u<<14);

 MFT_ENUM_DATA_V0 med{};med.StartFileReferenceNumber=0;med.LowUsn=0;med.HighUsn=MAXLONGLONG;
 std::vector<uint8_t> buf(1u<<16);
 for(;;){
  DWORD br=0;
  BOOL ok=DeviceIoControl(vol,FSCTL_ENUM_USN_DATA,&med,sizeof(med),buf.data(),(DWORD)buf.size(),&br,nullptr);
  if(!ok){
   if(GetLastError()==ERROR_HANDLE_EOF)break;
   return false; // something else went wrong mid-walk — no partial index left behind
  }
  if(br<sizeof(USN))break;
  DWORDLONG next=*(USN*)buf.data();
  uint8_t* p=buf.data()+sizeof(USN);
  uint8_t* end=buf.data()+br;
  while(p<end){
   auto* rec=(USN_RECORD_V2*)p;
   if(!ValidRecord(p,end))return false;
   std::wstring name((wchar_t*)(p+rec->FileNameOffset),rec->FileNameLength/sizeof(wchar_t));
   bool isDir=(rec->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
   bool isReparse=(rec->FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
   if(isDir&&!isReparse){
    dirs.emplace(rec->FileReferenceNumber,DirNode{rec->ParentFileReferenceNumber,name});
   }else if(!isDir&&Supported(name)){
    PendingPhoto pp;pp.name=name;pp.frn=rec->FileReferenceNumber;pp.parent=rec->ParentFileReferenceNumber;
    pp.size=0;pp.modified=(uint64_t(rec->TimeStamp.HighPart)<<32)|rec->TimeStamp.LowPart;
    byParent[rec->ParentFileReferenceNumber].push_back(std::move(pp));
   }
   p+=rec->RecordLength;
  }
  med.StartFileReferenceNumber=next;
 }

 // File size isn't in a USN_RECORD, so the qualifying photos get one cheap
 // metadata-only query each — still per-*photo*, not per-*directory-listing*,
 // which is the difference that makes this faster than the FindFirstFile
 // walk on a large tree (one syscall per relevant file instead of one per
 // directory plus one stat per file already included in that listing).
 // We stay honest about the trade-off: this is not free, just far fewer
 // round trips than re-walking the whole tree with FindFirstFile/Next.
 std::unordered_map<DWORDLONG,std::wstring> pathCache;
 for(auto& [parentFrn,photos]:byParent){
  std::wstring folderPath=ResolvePath(parentFrn,rootFrn,driveRoot,dirs,pathCache);
  if(folderPath.empty())continue;
  std::wstring lowerCheck=folderPath;std::transform(lowerCheck.begin(),lowerCheck.end(),lowerCheck.begin(),towlower);
  if(ExcludedFolder(L"\\"+lowerCheck+L"\\"))continue;
  if(InDevelopmentTree(std::filesystem::path(folderPath)))continue;
  bool ignored=false;
  for(auto ancestor=std::filesystem::path(folderPath);!ancestor.empty();){
   if(GetFileAttributesW((ancestor/L".nomedia").c_str())!=INVALID_FILE_ATTRIBUTES){ignored=true;break;}
   auto parent=ancestor.parent_path();if(parent==ancestor)break;ancestor=parent;
  }
  if(ignored)continue;

  UsnFolderResult entry;
  entry.path=folderPath;
  auto slash=folderPath.find_last_of(L'\\');
  entry.name=slash==std::wstring::npos?folderPath:folderPath.substr(slash+1);
  entry.id=IdFromFrn(volSerial,parentFrn);

  std::vector<UsnPhotoResult> out;out.reserve(photos.size());
  for(auto& pp:photos){
   std::wstring full=folderPath+L"\\"+pp.name;
   WIN32_FILE_ATTRIBUTE_DATA fad{};
   if(!GetFileAttributesExW(full.c_str(),GetFileExInfoStandard,&fad))continue; // vanished mid-walk
   UsnPhotoResult photo;
   photo.path=full;photo.name=pp.name;
   auto dot=pp.name.find_last_of(L'.');
   photo.ext=dot==std::wstring::npos?L"":pp.name.substr(dot+1);
   photo.size=(uint64_t(fad.nFileSizeHigh)<<32)|fad.nFileSizeLow;
   photo.modified=(uint64_t(fad.ftLastWriteTime.dwHighDateTime)<<32)|fad.ftLastWriteTime.dwLowDateTime;
   photo.id=IdFromFrn(volSerial,pp.frn);
   entry.totalBytes+=photo.size;
   entry.modified=(std::max)(entry.modified,photo.modified);
   out.push_back(std::move(photo));
  }
  if(out.empty())continue;
  entry.photoCount=uint32_t(out.size());
  entry.sampleCount=uint8_t((std::min)(size_t(4),out.size()));
  for(uint8_t i=0;i<entry.sampleCount;i++)entry.samples[i]=out[i].path;
  sink(entry,std::move(out));
 }

 outPos.journalId=jd.UsnJournalID;
 outPos.nextUsn=jd.NextUsn;
 outPos.valid=true;
 return true;
}

// A record's own current path only resolves for something that still
// exists; its *parent* almost always still exists even when the record
// itself is a deletion, so that's what every case below resolves against.
std::wstring ResolveByFrn(HANDLE vol,DWORDLONG frn){
 FILE_ID_DESCRIPTOR fid{};fid.dwSize=sizeof(fid);fid.Type=FileIdType;fid.FileId.QuadPart=(LONGLONG)frn;
 HANDLE fh=OpenFileById(vol,&fid,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,FILE_FLAG_BACKUP_SEMANTICS);
 if(fh==INVALID_HANDLE_VALUE)return L"";
 wchar_t pathBuf[32768];
 DWORD n=GetFinalPathNameByHandleW(fh,pathBuf,32767,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
 CloseHandle(fh);
 if(n==0||n>=32767)return L"";
 std::wstring full=pathBuf;
 if(full.rfind(L"\\\\?\\",0)==0)full=full.substr(4);
 return full;
}

bool UsnCatchUp(wchar_t driveLetter,UsnJournalPos& pos,const UsnChangeSink& sink){
 if(!pos.valid)return false;
 HANDLE vol=OpenVolume(driveLetter);
 if(vol==INVALID_HANDLE_VALUE)return false;
 struct VolGuard{HANDLE h;~VolGuard(){CloseHandle(h);}}guard{vol};

 USN_JOURNAL_DATA_V0 jd{};
 if(!QueryJournal(vol,jd))return false;
 if(jd.UsnJournalID!=pos.journalId||pos.nextUsn<(std::max)(jd.LowestValidUsn,jd.FirstUsn)||pos.nextUsn>jd.NextUsn){
  // The journal was recreated (reformat, or it rolled past what we last
  // read) — our position is meaningless now. Caller must fall back to a
  // full re-verify of this volume instead of trusting a partial catch-up.
  pos.valid=false;return false;
 }

 READ_USN_JOURNAL_DATA_V0 rjd{};
 rjd.StartUsn=pos.nextUsn;rjd.ReasonMask=0xFFFFFFFF;rjd.UsnJournalID=jd.UsnJournalID;
 std::vector<uint8_t> buf(1u<<16);
 std::unordered_set<DWORDLONG> reportedParents;
 for(;;){
  DWORD br=0;
  if(!DeviceIoControl(vol,FSCTL_READ_USN_JOURNAL,&rjd,sizeof(rjd),buf.data(),(DWORD)buf.size(),&br,nullptr))return false;
  if(br<sizeof(USN))break;
  DWORDLONG next=*(USN*)buf.data();
  uint8_t* p=buf.data()+sizeof(USN);
  uint8_t* end=buf.data()+br;
  if(p>=end){pos.nextUsn=next;break;} // caught up: nothing new since last time
  while(p<end){
   auto* rec=(USN_RECORD_V2*)p;
   if(!ValidRecord(p,end)){pos.valid=false;return false;}
   std::wstring name((wchar_t*)(p+rec->FileNameOffset),rec->FileNameLength/sizeof(wchar_t));
   bool isDir=(rec->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
   if(isDir||Supported(name)){
    if(reportedParents.insert(rec->ParentFileReferenceNumber).second){
     auto parentPath=ResolveByFrn(vol,rec->ParentFileReferenceNumber);
     if(!parentPath.empty())sink(parentPath);
    }
    // A directory that changed (renamed, or newly created) might itself now
    // qualify or disqualify as a photo folder, same as the parent does.
    if(isDir&&reportedParents.insert(rec->FileReferenceNumber).second){
     auto ownPath=ResolveByFrn(vol,rec->FileReferenceNumber);
     if(!ownPath.empty())sink(ownPath);
    }
   }
   p+=rec->RecordLength;
  }
  rjd.StartUsn=next;
  pos.nextUsn=next;
  if(int64_t(next)>=jd.NextUsn)break;
 }
 return true;
}
