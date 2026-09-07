#pragma once
// Vetro Look, GPL-3.0-or-later.
// The real "Everything-style" fast path for NTFS: FSCTL_ENUM_USN_DATA walks
// the MFT directly instead of recursing FindFirstFile through every folder,
// and the USN journal gives an exact, ordered log of what changed since a
// given point — including while the app was closed, which ReadDirectoryChangesW
// cannot do at all (it only reports changes while a handle is open and
// listening, and can silently drop events if its buffer overflows).
//
// Opening a volume handle for this needs the caller to be elevated (or hold
// SeBackupPrivilege) on modern Windows — there is no non-admin way around
// that. Every function here fails cleanly (returns false) when the handle
// can't be opened, so the caller always has a working non-admin fallback:
// see index.cpp's FindFirstFileExW walk and ReadDirectoryChangesW watchers,
// which remain the path used whenever this one isn't available.
#include <windows.h>
#include <string>
#include <cstdint>
#include <functional>
#include <vector>

struct UsnJournalPos{
 DWORDLONG journalId=0;
 int64_t nextUsn=0;
 bool valid=false;
};

// One drive root, e.g. L"C:\\". Calls `onFolder` once per folder that holds
// at least one supported image, with the same shape ScanOneFolder builds:
// full path, photo count, total bytes, latest mtime, and up to four sample
// paths for the folder card — plus `onPhoto` for every individual photo, so
// the caller can populate photosByFolder the same way it does today. Ids
// passed to both are the file's real NTFS identity (volume serial + file
// reference number), matching StableId's fallback-path hash format exactly
// so the two paths never disagree with each other in the same session.
struct UsnFolderResult{
 std::wstring path,name;
 uint64_t id=0,totalBytes=0,modified=0;
 uint32_t photoCount=0;
 std::wstring samples[4];uint8_t sampleCount=0;
};
struct UsnPhotoResult{
 std::wstring path,name,ext;
 uint64_t id=0,size=0,modified=0;
};
using UsnFolderSink=std::function<void(const UsnFolderResult&,std::vector<UsnPhotoResult>&&)>;

// Returns false immediately (no partial work visible) if the volume can't be
// opened for raw access or has no USN journal and one can't be created.
// On success, every relevant folder has already been reported through `sink`
// and `outPos` holds the journal position to persist for UsnCatchUp later.
bool UsnFastEnumerate(wchar_t driveLetter,const UsnFolderSink& sink,UsnJournalPos& outPos);

// Reads the journal from `pos.nextUsn` forward and reports every folder path
// that may need a fresh look — same "just re-list it" contract the
// ReadDirectoryChangesW watcher already uses, so a create/delete/rename/edit
// anywhere under a folder marks that folder dirty. A record's *parent* can
// almost always still be resolved (parents outlive the specific child being
// renamed or deleted far more often than not); resolving a *removed* item's
// own former path is not reliable, and isn't needed — VerifyKnown's ordinary
// GetFileAttributes liveness check already catches a known folder that
// itself vanished, cheaply, on the same startup this runs on. Advances
// `pos.nextUsn` on success. Returns false if the journal referenced by
// `pos.journalId` is gone (e.g. the volume was reformatted) — the caller
// must then fall back to a full re-verify of that volume, exactly as if no
// journal position had ever been recorded.
using UsnChangeSink=std::function<void(const std::wstring& dirtyFolderPath)>;
bool UsnCatchUp(wchar_t driveLetter,UsnJournalPos& pos,const UsnChangeSink& sink);
