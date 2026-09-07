// Vetro Look, GPL-3.0-or-later.
// A small, standalone uninstaller: closes any running instance, removes
// every shell-registration trace via the same UnregisterViewer() the app's
// own --unregister flag uses, then deletes the installed copy. Links only
// shellreg.cpp, not the rest of the app, so it stays tiny and has nothing
// to break independently of the registration logic it shares with the app.
#include "../src/actions.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>

namespace{
std::wstring InstallDir(){
 wchar_t path[MAX_PATH]{};
 if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,path)))return L"";
 return std::wstring(path)+L"\\Programs\\VetroLook";
}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int){
 // Close a running instance first: Windows won't let us delete an .exe
 // that's still mapped into a live process.
 if(HWND existing=FindWindowW(L"VetroLook.Window",nullptr)){
  PostMessageW(existing,WM_CLOSE,0,0);
  for(int i=0;i<20&&FindWindowW(L"VetroLook.Window",nullptr);i++)Sleep(150);
 }

 UnregisterViewer();

 auto dir=InstallDir();
 if(!dir.empty()){
  // SHFileOperation needs a double-null-terminated path list.
  std::wstring path=dir;path.push_back(0);path.push_back(0);
  SHFILEOPSTRUCTW op{};
  op.wFunc=FO_DELETE;
  op.pFrom=path.c_str();
  op.fFlags=FOF_NO_UI;
 SHFileOperationW(&op);
 }
 RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VetroLook");

 MessageBoxW(nullptr,L"VetroLook has been removed from this account.",L"VetroLook",MB_OK|MB_ICONINFORMATION);
 return 0;
}
