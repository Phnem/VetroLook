#include "browser.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>
namespace fs=std::filesystem;
namespace{
std::mutex mx;std::condition_variable cv;std::thread worker;
HWND window=nullptr;UINT message=0;bool stopping=false;
uint64_t generation=0;std::optional<std::pair<uint64_t,std::wstring>> request;
std::optional<BrowserResult> result;
std::vector<BrowserFolder> roots,pins;
const wchar_t* PinKey=L"Software\\VetroLook\\Settings";
std::wstring Key(std::wstring s){for(auto& c:s)c=towlower(c);while(s.size()>3&&(s.back()==L'\\'||s.back()==L'/'))s.pop_back();return s;}
BrowserFolder Entry(const std::wstring& path){auto name=fs::path(path).filename().wstring();if(name.empty())name=path;return {path,name,FolderIcon::Folder};}
void SavePins(){std::vector<wchar_t> text;for(auto& p:pins){text.insert(text.end(),p.path.begin(),p.path.end());text.push_back(0);}text.push_back(0);if(text.size()==1)text.push_back(0);RegSetKeyValueW(HKEY_CURRENT_USER,PinKey,L"PinnedFolders",REG_MULTI_SZ,text.data(),DWORD(text.size()*2));}
}
bool BrowserWithin(const std::wstring& path,const std::wstring& root){if(path.empty()||root.empty())return false;auto p=Key(path),r=Key(root);return p==r||(p.size()>r.size()&&p.compare(0,r.size(),r)==0&&(r.back()==L'\\'||p[r.size()]==L'\\'));}
void BrowserStart(HWND notify,UINT msg){
 window=notify;message=msg;
 struct Known{const KNOWNFOLDERID* id;FolderIcon icon;};
 const Known known[]={{&FOLDERID_Pictures,FolderIcon::Pictures},{&FOLDERID_Downloads,FolderIcon::Downloads},{&FOLDERID_Desktop,FolderIcon::Desktop},{&FOLDERID_Documents,FolderIcon::Documents},{&FOLDERID_Videos,FolderIcon::Videos},{&FOLDERID_Music,FolderIcon::Music}};
 for(auto& k:known){PWSTR path=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(*k.id,KF_FLAG_DONT_VERIFY,nullptr,&path))){auto f=Entry(path);f.icon=k.icon;roots.push_back(std::move(f));CoTaskMemFree(path);}}
 DWORD drives=GetLogicalDrives();for(int i=0;i<26;i++)if(drives&(1u<<i)){auto path=std::wstring(1,wchar_t(L'A'+i))+L":\\";roots.push_back({path,path,FolderIcon::Drive});}
 DWORD bytes=0;if(RegGetValueW(HKEY_CURRENT_USER,PinKey,L"PinnedFolders",RRF_RT_REG_MULTI_SZ,nullptr,nullptr,&bytes)==ERROR_SUCCESS&&bytes<1<<20){std::vector<wchar_t> data(bytes/2+2,0);if(RegGetValueW(HKEY_CURRENT_USER,PinKey,L"PinnedFolders",RRF_RT_REG_MULTI_SZ,nullptr,data.data(),&bytes)==ERROR_SUCCESS)for(auto p=data.data();*p;p+=wcslen(p)+1)pins.push_back(Entry(p));}
 stopping=false;worker=std::thread([]{
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  for(;;){std::pair<uint64_t,std::wstring> job;{std::unique_lock lock(mx);cv.wait(lock,[]{return stopping||request.has_value();});if(stopping)break;job=std::move(*request);request.reset();}
   BrowserResult r;r.generation=job.first;r.path=job.second;
   WIN32_FIND_DATAW data{};auto pattern=(fs::path(r.path)/L"*").wstring();HANDLE search=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
   if(search!=INVALID_HANDLE_VALUE){do{if((data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(data.dwFileAttributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM))&&wcscmp(data.cFileName,L".")&&wcscmp(data.cFileName,L".."))r.children.push_back(Entry((fs::path(r.path)/data.cFileName).wstring()));{std::lock_guard lock(mx);if(stopping||generation!=job.first)break;}}while(FindNextFileW(search,&data));FindClose(search);
    std::sort(r.children.begin(),r.children.end(),[](auto& a,auto& b){return StrCmpLogicalW(a.name.c_str(),b.name.c_str())<0;});
   }else if(GetLastError()!=ERROR_FILE_NOT_FOUND)r.error=L"Folder unavailable";
   {std::lock_guard lock(mx);if(stopping)break;if(generation!=job.first)continue;result=std::move(r);}PostMessageW(window,message,0,0);
  }
 });
}
void BrowserStop(){{std::lock_guard lock(mx);stopping=true;request.reset();}cv.notify_one();if(worker.joinable())worker.join();}
uint64_t BrowserRequest(const std::wstring& path){uint64_t id;{std::lock_guard lock(mx);id=++generation;request=std::make_pair(id,path);}cv.notify_one();return id;}
std::optional<BrowserResult> BrowserTakeResult(){std::lock_guard lock(mx);auto r=std::move(result);result.reset();return r;}
std::vector<BrowserFolder> BrowserRoots(){return roots;}
std::vector<BrowserFolder> BrowserPins(){return pins;}
bool BrowserIsPinned(const std::wstring& path){return std::any_of(pins.begin(),pins.end(),[&](auto& p){return Key(p.path)==Key(path);});}
void BrowserTogglePin(const std::wstring& path){auto it=std::find_if(pins.begin(),pins.end(),[&](auto& p){return Key(p.path)==Key(path);});if(it==pins.end())pins.push_back(Entry(path));else pins.erase(it);SavePins();}
std::vector<BrowserFolder> BrowserAncestry(const std::wstring& path){
 if(path.empty())return {};auto target=Key(path);BrowserFolder base=Entry(fs::path(path).root_path().wstring());
 for(auto& root:roots)if(BrowserWithin(target,root.path)&&root.path.size()>base.path.size())base=root;
 std::vector<BrowserFolder> out{base};auto suffix=path.substr((std::min)(path.size(),base.path.size()));while(!suffix.empty()&&(suffix.front()==L'\\'||suffix.front()==L'/'))suffix.erase(suffix.begin());auto relative=fs::path(suffix);auto running=fs::path(base.path);
 for(auto& part:relative){if(part==L"."||part.empty())continue;running/=part;out.push_back(Entry(running.wstring()));}return out;
}
bool BrowserCanDelete(const std::wstring& path){if(path.empty()||fs::path(path).parent_path()==fs::path(path)||Key(fs::path(path).root_path().wstring())==Key(path))return false;for(auto& r:roots)if(Key(r.path)==Key(path))return false;return true;}
bool BrowserRecycle(HWND owner,const std::wstring& path){
 if(!BrowserCanDelete(path))return false;
 Microsoft::WRL::ComPtr<IFileOperation> op;Microsoft::WRL::ComPtr<IShellItem> item;
 if(FAILED(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&op)))||FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item))))return false;
 op->SetOwnerWindow(owner);op->SetOperationFlags(FOF_ALLOWUNDO|FOFX_RECYCLEONDELETE);if(FAILED(op->DeleteItem(item.Get(),nullptr))||FAILED(op->PerformOperations()))return false;BOOL aborted=TRUE;return SUCCEEDED(op->GetAnyOperationsAborted(&aborted))&&!aborted;
}
