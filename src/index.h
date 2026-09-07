#pragma once
// Vetro Look, GPL-3.0-or-later.
// The gallery's file-system index: which folders hold images, how many and
// how large, kept current without ever re-walking the whole disk.
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

struct FolderEntry{
 std::wstring path;                 // normalised absolute path, no trailing slash
 std::wstring name;                 // last path component, for display
 uint32_t photoCount=0;             // supported images directly inside (not recursive)
 uint64_t totalBytes=0;             // sum of those images' sizes
 uint64_t modified=0;               // latest mtime among them, FILETIME as uint64
 uint64_t id=0;                     // stable id: hash of the normalised path
 std::wstring samples[4];           // up to four paths, for the folder-card preview
 uint8_t sampleCount=0;
 std::vector<std::wstring> members; // virtual family; never changes physical directories
};
struct PhotoEntry{
 std::wstring path,name,ext;
 uint64_t size=0,modified=0,id=0;
 uint32_t variantCount=1;
};

// Starts the background scanner and per-drive watchers. Safe to call once;
// posts `folderMsg` (throttled) whenever the folder set changes and the
// gallery should re-read a snapshot, and `photoMsg` whenever indexing
// progress (photo count so far) advances enough to be worth a status update.
void IndexStart(HWND notify,UINT folderMsg,UINT photoMsg);
void IndexStop();

// A cheap re-validation: re-checks known folders' mtimes and looks for newly
// arrived drives, but never re-walks a subtree that hasn't changed. This is
// what the "rescan" control calls.
void IndexRescan();
// A full rebuild from scratch — only for a damaged/missing cache or an
// explicit user request, never triggered automatically.
void IndexFullRebuild();
// Re-enumerates drives; call on WM_DEVICECHANGE so a freshly attached USB
// drive gets scanned without waiting for the next full rescan.
void IndexDrivesChanged();
// Scans one folder right away, ahead of the background pass — used when a
// file is opened directly (Explorer, drag-drop) so its siblings are ready
// the moment the user backs out into the album grid, rather than showing
// an empty grid until the wider scan happens to reach that folder.
void IndexTouchFolder(const std::wstring& folder);

bool IndexIsScanning();
uint64_t IndexKnownPhotoCount();

// Snapshots for the UI thread. Both copy under a lock, so call them only
// when notified (or on a user action), not once per frame.
std::vector<FolderEntry> IndexSnapshotFolders();
std::vector<PhotoEntry> IndexPhotosIn(const std::wstring& folder);

std::wstring NormalisePath(const std::wstring& path);
uint64_t PathId(const std::wstring& normalised);
enum class FolderState{Unknown,Indexing,Ready,Error};
FolderState IndexFolderState(const std::wstring& folder);
uint64_t PhysicalId(const std::wstring& path);
std::vector<PhotoEntry> IndexInspectDirectory(const std::wstring& path,FolderEntry& folder);
