#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
// Which rung of the decode ladder a frame came from.
enum ImageTier{TierFullRes=0,TierScreenRes=1,TierThumbRes=2};

// A decoded frame, and — separately — the asset it was decoded from.
//
// `w`/`h` are the pixels this frame actually has, which is what the renderer
// and the GPU need. `sourceW`/`sourceH` are what the file contains, which is
// what the header reports and what Save must write. Conflating the two is how
// a viewer ends up either uploading 138 MB to draw a 1400 px window, or
// silently saving somebody's photograph at a third of its resolution.
struct Image {
 unsigned w=0,h=0;                 // this frame
 std::vector<uint8_t> pixels;
 std::wstring codec; double ms=0; bool hasAlpha=false;
 unsigned sourceW=0,sourceH=0;     // the asset, after EXIF orientation
 int tier=TierFullRes;
 // Scale of this frame against the asset. 1.0 for a full-resolution decode.
 float Scale()const{return sourceW&&w?float(w)/float(sourceW):1.f;}
 unsigned SourceW()const{return sourceW?sourceW:w;}
 unsigned SourceH()const{return sourceH?sourceH:h;}
};
// `cancelled` is polled during long RAW work: switching photographs must not
// leave a demosaic of the previous one running to completion.
std::shared_ptr<Image> Decode(const std::wstring& path, std::wstring& error,
                              const std::function<bool()>& cancelled = {});
// Cheapest path to a small image: embedded RAW preview or a scaled JPEG decode.
// Returns null for formats that have no such shortcut; decode those in full.
std::shared_ptr<Image> DecodeThumb(const std::wstring& path, unsigned maxEdge);
// Decoder-native (or streaming) screen decode for formats where going through
// a full BGRA Image would be avoidable work. Returns null when the format has
// no specialised path, so pipeline.cpp can retain its measured fallback.
std::shared_ptr<Image> DecodeRenderReady(const std::wstring& path, unsigned maxEdge,
                                         const std::function<bool()>& cancelled = {});
std::shared_ptr<Image> DecodePsd(const uint8_t* bytes, size_t size,
                                 const uint8_t** jpegPreview, size_t* jpegSize,
                                 std::wstring* variantError = nullptr);
std::shared_ptr<Image> DecodePsdScreen(const uint8_t* bytes, size_t size,
                                       unsigned maxEdge,
                                       std::wstring* variantError = nullptr);
std::shared_ptr<Image> DecodeWuffs(const std::vector<uint8_t>& data);
bool Supported(const std::wstring& path);
std::wstring ExplorerSelection(HWND window);
bool ExplorerCanPreview(HWND window);
