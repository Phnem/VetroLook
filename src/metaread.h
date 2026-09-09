#pragma once
// Vetro Look, GPL-3.0-or-later.
// One metadata record per file, read once, from one place.
//
// The Info panel, the viewer header and the library index all used to reach
// for metadata through different parsers. They now all read this, so a value
// shown in one of them cannot disagree with the same value in another.
#include <cstdint>
#include <string>
#include <vector>

// External rating as Adobe writes it. Deliberately NOT the same concept as a
// Vetro Look favourite: this one is read-only, comes from somebody else's
// software, and has five states plus a rejection.
enum class XmpRating:int{Rejected=-1,Unrated=0};

struct MetadataRecord{
 bool ready=false;

 // ---- FILE
 unsigned width=0,height=0;
 unsigned orientation=1;              // EXIF 1..8
 std::wstring format,mime,bitDepth;

 // ---- CAMERA
 std::wstring cameraMake,cameraModel,lens,lensMake,serial;
 double focalLength=0,focalLength35=0,aperture=0,shutter=0,exposureBias=0;
 long iso=0;
 std::wstring flash,metering,whiteBalance,exposureProgram,dateTaken;

 // ---- COLOR
 std::wstring iccProfile,colourSpace;
 bool hasIcc=false;

 // ---- LOCATION
 bool hasGps=false;double lat=0,lon=0,altitude=0;

 // ---- AUTHOR
 std::wstring author,copyright,title,description,creatorTool;
 std::vector<std::wstring> keywords;

 // ---- ADOBE / XMP.  Read-only: Vetro Look never writes any of this.
 bool hasRating=false;
 int rating=0;                        // -1 rejected, 0 unrated, 1..5 stars
 std::wstring label;                  // xmp:Label, free text ("Red", "Select", ...)
 std::wstring xmpSource;              // L"sidecar", L"embedded", or empty

 // ---- diagnostics
 std::wstring reader;                 // which path produced this record
 std::wstring warning;                // non-fatal problems, e.g. a broken sidecar
};

// The XMP sidecar path a file would use, whether or not it exists. Adobe
// writes both "DSC_1248.xmp" and "DSC_1248.NEF.xmp" depending on version and
// setting; this returns the one that exists, preferring the bare stem.
std::wstring SidecarFor(const std::wstring& path);
bool SidecarStamp(const std::wstring& path,uint64_t& size,uint64_t& written);

// Reads everything above. Never throws; a file it cannot parse comes back
// with ready=false and a warning. Safe to call from a worker thread.
MetadataRecord ReadMetadata(const std::wstring& path);

// True when Exiv2 was compiled in and initialised. Kept so the Info panel can
// say which reader produced a record during a regression hunt.
bool MetadataReaderAvailable();
