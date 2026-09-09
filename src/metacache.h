#pragma once
// Vetro Look, GPL-3.0-or-later.
// A disk-backed cache of the metadata fields the library and the viewer header
// need before a photograph is decoded: the rating badge, the camera and lens
// text, the capture date the timeline groups by.
//
// Reading EXIF and XMP out of a 70 MB RAW costs tens of milliseconds. Doing it
// again every time a thumbnail scrolls past is what makes a gallery feel slow,
// so the answer is remembered and re-read only when the file — or its XMP
// sidecar — actually changes.
#include <cstdint>
#include <string>

struct CachedMeta{
 bool valid=false;
 // identity the entry was built from
 uint64_t size=0,written=0,sidecarWritten=0,sidecarSize=0;
 // cached fields
 unsigned width=0,height=0,orientation=1;
 int rating=0;bool hasRating=false;
 std::wstring label,xmpSource;
 std::wstring camera,lens,dateTaken;
 double aperture=0,shutter=0,focalLength=0;
 long iso=0;
};

// Returns a still-valid entry, or valid=false when the file has changed, its
// sidecar has changed, or it was never read.
CachedMeta MetaCacheGet(const std::wstring& path);
void MetaCachePut(const std::wstring& path,const CachedMeta& entry);

// Reads the record through the cache: a hit costs a stat of the file and its
// sidecar, a miss costs a full Exiv2 read and is then remembered.
CachedMeta MetaCacheLookup(const std::wstring& path);

void MetaCacheLoad();
void MetaCacheFlush();
size_t MetaCacheSize();
