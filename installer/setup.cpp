// Vetro Look, GPL-3.0-or-later.
// A self-contained installer: the real VetroLook.exe travels inside this
// executable as a resource, so `release/` ships only this file plus the
// uninstaller and a readme — nothing else to lose track of. Running it
// writes the payload to a stable per-user location, registers it as an
// image viewer from *that* location (so every later launch — file
// association, Start Menu, autostart — always resolves to the same path
// regardless of where this installer itself was run from), then starts
// the app normally.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <string>
#include <vector>

namespace{
std::wstring InstallDir(){
 wchar_t path[MAX_PATH]{};
 if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,path)))return L"";
 return std::wstring(path)+L"\\Programs\\VetroLook";
}
bool WriteResource(WORD id,const std::wstring& target,std::wstring& error){
 HMODULE self=GetModuleHandleW(nullptr);
 HRSRC res=FindResourceW(self,MAKEINTRESOURCEW(id),RT_RCDATA);
 if(!res){error=L"missing embedded payload";return false;}
 HGLOBAL mem=LoadResource(self,res);
 if(!mem){error=L"could not load payload";return false;}
 void* data=LockResource(mem);
 DWORD size=SizeofResource(self,res);
 if(!data||!size){error=L"empty payload";return false;}
 HANDLE file=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE){error=L"could not open "+target;return false;}
 DWORD written=0;
 BOOL ok=WriteFile(file,data,size,&written,nullptr);
 CloseHandle(file);
 if(!ok||written!=size){error=L"write failed";return false;}
 return true;
}
bool WritePayload(const std::wstring& targetExe,std::wstring& error){return WriteResource(1,targetExe,error);}
bool WriteUninstaller(const std::wstring& target,std::wstring& error){return WriteResource(2,target,error);}
bool WriteReadme(const std::wstring& target,std::wstring& error){return WriteResource(3,target,error);}
bool WriteNotices(const std::wstring& target,std::wstring& error){return WriteResource(4,target,error);}
void RegisterUninstaller(const std::wstring& dir){
 const std::wstring key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VetroLook";
 const std::wstring uninstaller=L"\""+dir+L"\\Uninstall.exe\"";
 RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"DisplayName",REG_SZ,L"VetroLook",sizeof(L"VetroLook"));
 RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"DisplayVersion",REG_SZ,L"1.0.0",sizeof(L"1.0.0"));
 RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"Publisher",REG_SZ,L"Phnem",sizeof(L"Phnem"));
 RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"InstallLocation",REG_SZ,dir.c_str(),DWORD((dir.size()+1)*sizeof(wchar_t)));
 RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"UninstallString",REG_SZ,uninstaller.c_str(),DWORD((uninstaller.size()+1)*sizeof(wchar_t)));
 RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"URLInfoAbout",REG_SZ,L"https://github.com/Phnem/VetroLook",sizeof(L"https://github.com/Phnem/VetroLook"));
 DWORD noModify=1;RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"NoModify",REG_DWORD,&noModify,sizeof(noModify));
}
bool CreateShortcut(const std::wstring& location,const std::wstring& target){
 IShellLinkW* link=nullptr;
 if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link))))return false;
 link->SetPath(target.c_str());link->SetDescription(L"VetroLook");link->SetIconLocation(target.c_str(),0);
 IPersistFile* file=nullptr;HRESULT hr=link->QueryInterface(IID_PPV_ARGS(&file));
 if(SUCCEEDED(hr)){hr=file->Save(location.c_str(),TRUE);file->Release();}link->Release();return SUCCEEDED(hr);
}
void OfferShortcuts(const std::wstring& dir){
 if(MessageBoxW(nullptr,L"Create Desktop and Start Menu shortcuts?",L"VetroLook Setup",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
 wchar_t path[MAX_PATH]{};
 if(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_DESKTOPDIRECTORY,nullptr,0,path)))CreateShortcut(std::wstring(path)+L"\\VetroLook.lnk",dir+L"\\VetroLook.exe");
 if(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_PROGRAMS,nullptr,0,path))){std::wstring menu=std::wstring(path)+L"\\VetroLook";SHCreateDirectoryExW(nullptr,menu.c_str(),nullptr);CreateShortcut(menu+L"\\VetroLook.lnk",dir+L"\\VetroLook.exe");}
}
bool RunAndWait(const std::wstring& exe,const std::wstring& args,DWORD timeoutMs){
 std::wstring cmd=L"\""+exe+L"\""+(args.empty()?L"":L" "+args);
 std::vector<wchar_t> buf(cmd.begin(),cmd.end());buf.push_back(0);
 STARTUPINFOW si{};si.cb=sizeof(si);
 PROCESS_INFORMATION pi{};
 if(!CreateProcessW(exe.c_str(),buf.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi))return false;
 WaitForSingleObject(pi.hProcess,timeoutMs);
 CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
 return true;
}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int){
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 auto dir=InstallDir();
 if(dir.empty()){MessageBoxW(nullptr,L"Could not resolve the local install folder.",L"VetroLook Setup",MB_OK|MB_ICONERROR);return 1;}
 SHCreateDirectoryExW(nullptr,(std::wstring(dir)).c_str(),nullptr); // creates the full "\Programs\VetroLook" chain
 std::wstring targetExe=dir+L"\\VetroLook.exe";

 // A running instance keeps its .exe file mapped, so it must close before
 // this can overwrite it — relevant for re-running the installer to update.
 if(HWND existing=FindWindowW(L"VetroLook.Window",nullptr)){
  PostMessageW(existing,WM_CLOSE,0,0);
  for(int i=0;i<20&&FindWindowW(L"VetroLook.Window",nullptr);i++)Sleep(150);
 }

 std::wstring error;
 if(!WritePayload(targetExe,error)){
  MessageBoxW(nullptr,error.c_str(),L"VetroLook Setup",MB_OK|MB_ICONERROR);
  return 1;
 }
 if(!WriteUninstaller(dir+L"\\Uninstall.exe",error)||!WriteReadme(dir+L"\\README.txt",error)||!WriteNotices(dir+L"\\THIRD_PARTY_NOTICES.txt",error)){
  MessageBoxW(nullptr,error.c_str(),L"VetroLook Setup",MB_OK|MB_ICONERROR);
  return 1;
 }
 RegisterUninstaller(dir);
 OfferShortcuts(dir);

 // Registration reads its own module path, so it must run from the
 // installed copy to register *that* path rather than this installer's.
 RunAndWait(targetExe,L"--register",15000);

 // Carry through anything the user dropped onto or passed to the
 // installer itself (e.g. double-clicking an image with Setup.exe still
 // set as a stale handler during testing).
 int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 std::wstring forward;
 for(int i=1;i<argc;i++){forward+=L"\"";forward+=argv[i];forward+=L"\" ";}
 if(argv)LocalFree(argv);

 std::wstring cmd=L"\""+targetExe+L"\" "+forward;
 std::vector<wchar_t> buf(cmd.begin(),cmd.end());buf.push_back(0);
 STARTUPINFOW si{};si.cb=sizeof(si);
 PROCESS_INFORMATION pi{};
 if(CreateProcessW(targetExe.c_str(),buf.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){
  CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
 }
 return 0;
}
