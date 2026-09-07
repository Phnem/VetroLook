// Explorer Shell selection flow adapted from QuickLook, copyright 2017-2026
// QL-Win Contributors, GPL-3.0-or-later. Uses active shell view and CF_HDROP.
#include "image.h"
#include <wrl/client.h>
#include <shlobj.h>
#include <shellapi.h>
#include <exdisp.h>
#include <servprov.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;
static void ShellTrace(const wchar_t* label,HWND window){
 if(!GetEnvironmentVariableW(L"VETRO_DEBUG",nullptr,0))return;
 wchar_t path[MAX_PATH]{},name[128]{};GetTempPathW(MAX_PATH,path);wcscat_s(path,L"vetro-shell.log");
 GetClassNameW(window,name,128);FILE* f=nullptr;_wfopen_s(&f,path,L"a");
 if(f){fwprintf(f,L"%s hwnd=%p class=%s\n",label,window,name);fclose(f);}
}
bool ExplorerCanPreview(HWND window){
 ShellTrace(L"foreground",window);
 wchar_t name[100]{};GetClassNameW(window,name,100);
 if(wcscmp(name,L"CabinetWClass")&&wcscmp(name,L"ExploreWClass"))return false;
 GUITHREADINFO gi{sizeof(gi)};if(!GetGUIThreadInfo(GetWindowThreadProcessId(window,nullptr),&gi))return false;
 ShellTrace(L"focus",gi.hwndFocus);ShellTrace(L"caret",gi.hwndCaret);
 if(gi.flags&GUI_CARETBLINKING)return false;
 // Explorer can retain focus on its top-level host while the active tab owns
 // the selection (notably with the Windows 11 XAML host).
 if(gi.hwndFocus==window)return true;
 // Only intercept in the file list; do not consume spaces in search, rename or address fields.
 for(HWND focus=gi.hwndFocus;focus&&focus!=window;focus=GetParent(focus)){
  GetClassNameW(focus,name,100);
  if(wcsstr(name,L"Edit")||wcsstr(name,L"RichEdit"))return false;
  if(!wcscmp(name,L"DirectUIHWND")||!wcscmp(name,L"SysListView32"))return true;
 }
 return false;
}
std::wstring ExplorerSelection(HWND window){
 ShellTrace(L"selection-request",window);
 ComPtr<IShellWindows> windows;if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&windows))))return {};
 long count=0;windows->get_Count(&count);
 HWND activeTab=FindWindowExW(window,nullptr,L"ShellTabWindowClass",nullptr);
 ShellTrace(L"active-tab",activeTab);
 for(long i=0;i<count;i++){
  VARIANT index{};index.vt=VT_I4;index.lVal=i;ComPtr<IDispatch> dispatch;if(FAILED(windows->Item(index,&dispatch))||!dispatch)continue;
  ComPtr<IServiceProvider> service;if(FAILED(dispatch.As(&service)))continue;ComPtr<IShellBrowser> browser;if(FAILED(service->QueryService(IID_IShellBrowser,IID_PPV_ARGS(&browser))))continue;
  HWND handle=nullptr;browser->GetWindow(&handle);
  ShellTrace(L"browser",handle);
  if(activeTab&&handle!=activeTab&&handle!=window)continue;
  if(handle!=window&&(!IsChild(window,handle)||!IsWindowVisible(handle)))continue;
  ComPtr<IShellView> view;if(FAILED(browser->QueryActiveShellView(&view)))continue;
  HWND viewWindow=nullptr;view->GetWindow(&viewWindow);
  ShellTrace(L"view",viewWindow);
  if(viewWindow&&!IsWindowVisible(viewWindow)){ShellTrace(L"hidden-view",viewWindow);continue;}
  // Shell item arrays avoid CF_HDROP marshalling and preserve long paths.
  ComPtr<IFolderView2> folder;
  if(SUCCEEDED(view.As(&folder))){
   ComPtr<IShellItemArray> items;
   if(SUCCEEDED(folder->Items(SVGIO_SELECTION,IID_PPV_ARGS(&items)))){
    ComPtr<IShellItem> item;PWSTR name=nullptr;
    if(SUCCEEDED(items->GetItemAt(0,&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&name))){
     std::wstring result(name);CoTaskMemFree(name);ShellTrace(Supported(result)?L"supported-selection":L"unsupported-selection",viewWindow);return result;
    }
   }
  }
  ComPtr<IDataObject> data;if(FAILED(view->GetItemObject(SVGIO_SELECTION,IID_PPV_ARGS(&data))))continue;
  FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM medium{};
  if(SUCCEEDED(data->GetData(&format,&medium))){auto drop=(HDROP)medium.hGlobal;UINT len=DragQueryFileW(drop,0,nullptr,0);std::wstring path(len+1,L'\0');if(len)DragQueryFileW(drop,0,path.data(),len+1);ReleaseStgMedium(&medium);path.resize(len);return path;}
 }return {};
}

