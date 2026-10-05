#pragma once
// Vetro Look, GPL-3.0-or-later. One-level navigation, independent of media indexing.
#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
enum class FolderIcon{Folder,Pictures,Downloads,Desktop,Documents,Videos,Music,Drive};
struct BrowserFolder{std::wstring path,name;FolderIcon icon=FolderIcon::Folder;};
struct BrowserResult{uint64_t generation=0;std::wstring path,error;std::vector<BrowserFolder> children;};
void BrowserStart(HWND notify,UINT message);
void BrowserStop();
uint64_t BrowserRequest(const std::wstring& path);
std::optional<BrowserResult> BrowserTakeResult();
std::vector<BrowserFolder> BrowserRoots();
std::vector<BrowserFolder> BrowserPins();
bool BrowserIsPinned(const std::wstring& path);
void BrowserTogglePin(const std::wstring& path);
std::vector<BrowserFolder> BrowserAncestry(const std::wstring& path);
bool BrowserWithin(const std::wstring& path,const std::wstring& root);
bool BrowserCanDelete(const std::wstring& path);
bool BrowserRecycle(HWND owner,const std::wstring& path);
