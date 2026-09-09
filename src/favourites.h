#pragma once
// Vetro Look, GPL-3.0-or-later.
// Favourites: one boolean per photograph, and nothing else.
//
// Deliberately not a rating. A five-star scale is a cataloguing tool; this is
// the heart on the toolbar. External XMP ratings are read elsewhere
// (metaread.h) and are never mixed with this value in either direction.
//
// Identity, not path. On NTFS a favourite is stored against
// (volume serial, FILE_ID_128), so renaming or moving a file inside the same
// volume keeps it. Where that identity is unavailable — FAT, a network share,
// a file that vanished — it falls back to (path, size, mtime).
#include <cstdint>
#include <string>
#include <vector>

// Loads the store from disk. Cheap and idempotent; safe to call before the
// first query, and called automatically by the queries below.
void FavouritesLoad();
// Flushes pending changes. Called on a timer and at shutdown; a crash loses
// at most the last few seconds.
void FavouritesFlush();

bool FavouriteGet(const std::wstring& path);
void FavouriteSet(const std::wstring& path,bool on);

// Every favourite whose file still exists, most recently marked first.
// Entries whose file has disappeared are kept in the store (a disconnected
// drive is not a reason to forget) but left out of this list.
std::vector<std::wstring> FavouritePaths();
size_t FavouriteCount();

// True when anything under `folder` is a favourite. Used for the small heart
// badge on folder cards, so it answers from the in-memory index rather than
// touching the disk.
bool FolderHasFavourite(const std::wstring& folder);
uint32_t FolderFavouriteCount(const std::wstring& folder);

// Bumped on every change, so the UI can tell whether a cached derived view
// (the Favourites collection, folder badges) is still current.
uint64_t FavouritesRevision();
