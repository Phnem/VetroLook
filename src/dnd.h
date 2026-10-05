#pragma once
// Vetro Look, GPL-3.0-or-later.
#include <windows.h>
#include <vector>
#include <string>
#include <objidl.h>
#include <functional>
IDataObject* CreateFileDataObject(const std::vector<std::wstring>& paths);
// Begins a native OLE drag session carrying the given file paths as
// CF_HDROP — the same thing Explorer hands out when you drag a file from
// it, so Explorer, browsers, Photoshop, chat apps and so on all accept it
// as real files rather than a bitmap. DoDragDrop pumps its own message
// loop and only returns once the drag ends (drop or cancel).
HRESULT BeginFileDrag(const std::vector<std::wstring>& paths,DWORD* performedEffect=nullptr);
// Coordinates are client pixels. Hover is also called by the host's dwell timer.
struct FileDropHandlers{
 std::function<bool(POINT)> hover;
 std::function<void()> leave;
 std::function<bool(const std::vector<std::wstring>&,POINT)> drop;
};
HRESULT RegisterFileDrop(HWND window,FileDropHandlers handlers);
void RevokeFileDrop(HWND window);
bool CopyDroppedFiles(HWND owner,const std::vector<std::wstring>& paths,const std::wstring& folder);
