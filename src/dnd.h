#pragma once
// Vetro Look, GPL-3.0-or-later.
#include <windows.h>
#include <vector>
#include <string>
#include <objidl.h>
IDataObject* CreateFileDataObject(const std::vector<std::wstring>& paths);
// Begins a native OLE drag session carrying the given file paths as
// CF_HDROP — the same thing Explorer hands out when you drag a file from
// it, so Explorer, browsers, Photoshop, chat apps and so on all accept it
// as real files rather than a bitmap. DoDragDrop pumps its own message
// loop and only returns once the drag ends (drop or cancel).
HRESULT BeginFileDrag(const std::vector<std::wstring>& paths,DWORD* performedEffect=nullptr);
