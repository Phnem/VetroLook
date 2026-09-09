// Vetro Look, GPL-3.0-or-later.
// Per-user shell registration. Everything lands under HKEY_CURRENT_USER, so no
// elevation is needed and nothing another application owns is overwritten.
// Windows reserves the actual default-handler choice for the user, so we only
// publish the capability and then open the page where they confirm it.
#include "actions.h"
#include <shlobj.h>
#include <shellapi.h>
#include <aclapi.h>
#include <algorithm>
#include <string>
#include <vector>

namespace{
const wchar_t* ProgId=L"VetroLook.Image";
const wchar_t* AppKey=L"Software\\Classes\\Applications\\VetroLook.exe";
const wchar_t* Capabilities=L"Software\\VetroLook\\Capabilities";
const wchar_t* AppName=L"Vetro Look";
const wchar_t* RunKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* Extensions[]={L".jpg",L".jpeg",L".jfif",L".png",L".gif",L".webp",L".avif",L".exr",
 L".bmp",L".tif",L".tiff",L".ico",L".heic",L".heif",L".psd",L".psb",L".cr2",L".cr3",L".nef",L".arw",L".dng",
 L".raf",L".rw2",L".orf",L".pef"};

std::wstring ExecutablePath(){
 wchar_t path[MAX_PATH]{};
 DWORD length=GetModuleFileNameW(nullptr,path,MAX_PATH);
 return length&&length<MAX_PATH?std::wstring(path,length):std::wstring();
}
bool Text(const std::wstring& key,const wchar_t* name,const std::wstring& value){
 return RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),name,REG_SZ,value.c_str(),
  DWORD((value.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
}
bool Empty(const std::wstring& key,const wchar_t* name){
 return RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),name,REG_NONE,nullptr,0)==ERROR_SUCCESS;
}
bool Dword(const std::wstring& key,const wchar_t* name,DWORD value){
 return RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),name,REG_DWORD,&value,sizeof(value))==ERROR_SUCCESS;
}
// Explorer's "Open with" surface runs inside an AppContainer, which can only
// launch an executable that grants read and execute to ALL APPLICATION
// PACKAGES. Program Files carries that grant already; a build living in an
// ordinary folder does not, so the picker answers a pick with access denied
// and simply comes back — a loop the user cannot escape. Granting it on our
// own file is exactly what an installer would have done, needs no elevation
// because the user owns the file, and only ever widens read access.
void AllowAppContainerLaunch(const std::wstring& exe){
 BYTE sid[SECURITY_MAX_SID_SIZE];DWORD sidSize=sizeof(sid);
 if(!CreateWellKnownSid(WinBuiltinAnyPackageSid,nullptr,sid,&sidSize))return;
 PACL existing=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
 if(GetNamedSecurityInfoW(exe.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,
   nullptr,nullptr,&existing,nullptr,&descriptor)!=ERROR_SUCCESS)return;
 EXPLICIT_ACCESSW access{};
 access.grfAccessPermissions=GENERIC_READ|GENERIC_EXECUTE;
 access.grfAccessMode=GRANT_ACCESS;
 access.grfInheritance=NO_INHERITANCE;
 access.Trustee.TrusteeForm=TRUSTEE_IS_SID;
 access.Trustee.TrusteeType=TRUSTEE_IS_WELL_KNOWN_GROUP;
 access.Trustee.ptstrName=reinterpret_cast<LPWSTR>(sid);
 PACL updated=nullptr;
 if(SetEntriesInAclW(1,&access,existing,&updated)==ERROR_SUCCESS&&updated){
  SetNamedSecurityInfoW(const_cast<LPWSTR>(exe.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,
   nullptr,nullptr,updated,nullptr);
  LocalFree(updated);
 }
 if(descriptor)LocalFree(descriptor);
}
}

bool ViewerRegistered(){
 wchar_t buffer[32768]{};DWORD size=sizeof(buffer);
 auto key=std::wstring(L"Software\\Classes\\")+ProgId+L"\\shell\\open\\command";
 if(RegGetValueW(HKEY_CURRENT_USER,key.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,buffer,&size)!=ERROR_SUCCESS)return false;
 auto exe=ExecutablePath();return !exe.empty()&&std::wstring(buffer).find(exe)!=std::wstring::npos;
}

bool RegisterAsViewer(std::wstring& error){
 auto exe=ExecutablePath();
 if(exe.empty()){error=L"executable path";return false;}
 AllowAppContainerLaunch(exe);
 std::wstring command=L"\""+exe+L"\" \"%1\"";
 std::wstring icon=L"\""+exe+L"\",0";
 std::wstring progIdKey=std::wstring(L"Software\\Classes\\")+ProgId;
 bool ok=true;
 ok&=Text(AppKey,L"FriendlyAppName",AppName);
 ok&=Text(std::wstring(AppKey)+L"\\shell\\open\\command",nullptr,command);
 ok&=Text(std::wstring(AppKey)+L"\\DefaultIcon",nullptr,icon);
 ok&=Text(progIdKey,nullptr,L"Vetro Look Image");
 ok&=Text(progIdKey,L"FriendlyTypeName",L"Vetro Look Image");
 ok&=Text(progIdKey+L"\\shell\\open\\command",nullptr,command);
 ok&=Text(progIdKey+L"\\DefaultIcon",nullptr,icon);
 ok&=Text(progIdKey+L"\\Application",L"ApplicationName",AppName);
 ok&=Text(progIdKey+L"\\Application",L"ApplicationDescription",L"Native image viewer");
 ok&=Text(progIdKey+L"\\Application",L"ApplicationIcon",icon);
 ok&=Text(Capabilities,L"ApplicationName",AppName);
 ok&=Text(Capabilities,L"ApplicationDescription",L"Native image viewer for JPEG, PNG, WebP, AVIF and RAW");
 ok&=Text(Capabilities,L"ApplicationIcon",icon);
 ok&=Text(L"Software\\RegisteredApplications",AppName,Capabilities);
 ok&=Text(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\VetroLook.exe",nullptr,exe);
 for(auto extension:Extensions){
  ok&=Text(std::wstring(AppKey)+L"\\SupportedTypes",extension,L"");
  ok&=Text(std::wstring(Capabilities)+L"\\FileAssociations",extension,ProgId);
  ok&=Empty(std::wstring(L"Software\\Classes\\")+extension+L"\\OpenWithProgids",ProgId);
  std::wstring fileExt=std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\")+extension;
  // UserChoice is deliberately left alone. Since Windows 8 it is hash-protected
  // and belongs to the user. Publishing capabilities and OpenWith entries is all an app
  // may do; choosing the default stays with the user, via the picker's
  // "Always" button or Settings > Default apps.
  Empty(fileExt+L"\\OpenWithProgids",ProgId);
  // Append to the per-extension Open with list without disturbing its order.
  HKEY list=nullptr;
  if(RegCreateKeyExW(HKEY_CURRENT_USER,(fileExt+L"\\OpenWithList").c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&list,nullptr)==ERROR_SUCCESS){
   std::wstring slot;std::wstring taken;
   for(wchar_t letter=L'a';letter<=L'z';letter++){
    wchar_t name[2]={letter,0};wchar_t value[64]{};DWORD size=sizeof(value);
    if(RegGetValueW(list,nullptr,name,RRF_RT_REG_SZ,nullptr,value,&size)!=ERROR_SUCCESS)continue;
    taken+=letter;
    if(!_wcsicmp(value,L"VetroLook.exe"))slot=name;
   }
   if(slot.empty())for(wchar_t letter=L'a';letter<=L'z';letter++)
    if(taken.find(letter)==std::wstring::npos){slot=std::wstring(1,letter);break;}
   if(!slot.empty()){
    RegSetValueExW(list,slot.c_str(),0,REG_SZ,(const BYTE*)L"VetroLook.exe",DWORD(sizeof(L"VetroLook.exe")));
    wchar_t order[64]{};DWORD size=sizeof(order);
    if(RegGetValueW(list,nullptr,L"MRUList",RRF_RT_REG_SZ,nullptr,order,&size)!=ERROR_SUCCESS)order[0]=0;
    std::wstring mru=order;
    if(mru.find(slot[0])==std::wstring::npos)mru+=slot;
    RegSetValueExW(list,L"MRUList",0,REG_SZ,(const BYTE*)mru.c_str(),DWORD((mru.size()+1)*sizeof(wchar_t)));
   }
   RegCloseKey(list);
  }
 }
 ok&=Dword(L"Software\\VetroLook",L"AssociationSchema",3);
 wchar_t runValue[MAX_PATH*2]{};DWORD runBytes=sizeof(runValue);
 if(RegGetValueW(HKEY_CURRENT_USER,RunKey,AppName,RRF_RT_REG_SZ,nullptr,runValue,&runBytes)==ERROR_SUCCESS)
  Text(RunKey,AppName,L"\""+exe+L"\" --background");
 SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST|SHCNF_FLUSH,nullptr,nullptr);
 if(!ok)error=L"registry";
 return ok;
}

// Removes the registration published by this application without touching
// the user's UserChoice, settings, or favourites.
void UnregisterViewer(){
 RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\")+ProgId).c_str());
 RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\Classes\\Applications\\VetroLook.exe");
 RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Capabilities");
 RegDeleteKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook",L"AssociationSchema");
 RegDeleteKeyValueW(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",AppName);
 RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\VetroLook.exe");
 HKEY runKey=nullptr;
 if(RegOpenKeyExW(HKEY_CURRENT_USER,RunKey,0,KEY_SET_VALUE,&runKey)==ERROR_SUCCESS){
  RegDeleteValueW(runKey,AppName);RegCloseKey(runKey);
 }
 for(auto extension:Extensions){
  std::wstring classesKey=std::wstring(L"Software\\Classes\\")+extension;
  RegDeleteKeyValueW(HKEY_CURRENT_USER,(classesKey+L"\\OpenWithProgids").c_str(),ProgId);
  wchar_t currentDefault[256]{};DWORD size=sizeof(currentDefault);
  if(RegGetValueW(HKEY_CURRENT_USER,classesKey.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,currentDefault,&size)==ERROR_SUCCESS&&
    !_wcsicmp(currentDefault,ProgId))
   RegDeleteKeyValueW(HKEY_CURRENT_USER,classesKey.c_str(),nullptr);
  std::wstring fileExt=std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\")+extension;
  RegDeleteKeyValueW(HKEY_CURRENT_USER,(fileExt+L"\\OpenWithProgids").c_str(),ProgId);
  HKEY list=nullptr;
  if(RegOpenKeyExW(HKEY_CURRENT_USER,(fileExt+L"\\OpenWithList").c_str(),0,KEY_READ|KEY_WRITE,&list)==ERROR_SUCCESS){
   for(wchar_t letter=L'a';letter<=L'z';letter++){
    wchar_t name[2]={letter,0};wchar_t value[64]{};DWORD vsize=sizeof(value);
    if(RegGetValueW(list,nullptr,name,RRF_RT_REG_SZ,nullptr,value,&vsize)!=ERROR_SUCCESS)continue;
    if(!_wcsicmp(value,L"VetroLook.exe")){
     RegDeleteValueW(list,name);
     wchar_t order[64]{};DWORD osize=sizeof(order);
     if(RegGetValueW(list,nullptr,L"MRUList",RRF_RT_REG_SZ,nullptr,order,&osize)==ERROR_SUCCESS){
      std::wstring mru=order;mru.erase(std::remove(mru.begin(),mru.end(),letter),mru.end());
      RegSetValueExW(list,L"MRUList",0,REG_SZ,(const BYTE*)mru.c_str(),DWORD((mru.size()+1)*sizeof(wchar_t)));
     }
    }
   }
   RegCloseKey(list);
  }
 }
 SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST|SHCNF_FLUSH,nullptr,nullptr);
}

// Renaming or moving the executable leaves every recorded command pointing at
// a file that is no longer there. Explorer answers a double click by opening
// its application picker, and picking this app runs that same dead command, so
// the picker comes straight back — an unbreakable loop from the user's side.
// Rewriting the paths at startup makes that self-healing, but only when a
// registration already exists: an app that was never registered must not start
// claiming file associations on its own.
void RepairRegistrationIfStale(){
 auto exe=ExecutablePath();
 if(exe.empty())return;
 bool registered=false,stale=false;
 wchar_t buffer[32768]{};DWORD size=sizeof(buffer);
 auto key=std::wstring(L"Software\\Classes\\")+ProgId+L"\\shell\\open\\command";
 if(RegGetValueW(HKEY_CURRENT_USER,key.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,buffer,&size)==ERROR_SUCCESS){registered=true;stale=std::wstring(buffer).find(exe)==std::wstring::npos;}
 if(registered&&stale){std::wstring ignored;RegisterAsViewer(ignored);}
}

void OpenDefaultAppsPage(){
 // Windows 10 1803 and later only allow the user to pick the default; deep-link
 // to the page with this application preselected and let them confirm.
 std::wstring page=std::wstring(L"ms-settings:defaultapps?registeredAppUser=")+AppName;
 auto result=(INT_PTR)ShellExecuteW(nullptr,L"open",page.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
 if(result<=32)ShellExecuteW(nullptr,L"open",L"ms-settings:defaultapps",nullptr,nullptr,SW_SHOWNORMAL);
}

bool AutostartEnabled(){
 wchar_t buffer[MAX_PATH*2]{};DWORD size=sizeof(buffer);
 return RegGetValueW(HKEY_CURRENT_USER,RunKey,AppName,RRF_RT_REG_SZ,nullptr,buffer,&size)==ERROR_SUCCESS;
}
bool SetAutostart(bool on){
 if(!on){
  HKEY key=nullptr;
  if(RegOpenKeyExW(HKEY_CURRENT_USER,RunKey,0,KEY_SET_VALUE,&key)!=ERROR_SUCCESS)return true;
  RegDeleteValueW(key,AppName);RegCloseKey(key);
  return true;
 }
 auto exe=ExecutablePath();
 if(exe.empty())return false;
 std::wstring command=L"\""+exe+L"\" --background";
 return RegSetKeyValueW(HKEY_CURRENT_USER,RunKey,AppName,REG_SZ,command.c_str(),
  DWORD((command.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
}
