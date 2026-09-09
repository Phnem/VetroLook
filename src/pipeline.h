#pragma once
// Vetro Look, GPL-3.0-or-later.
// Decode tiers and the pixel operations that sit between a decoder and the
// renderer. Kept apart from decoders.cpp so the viewer can ask for "the frame
// this window needs" instead of "every pixel the file happens to contain".
#include "image.h"

// THUMB  a filmstrip tile.
// SCREEN what the viewport actually shows: the smallest frame that still
//        covers the window's long edge with a little headroom.
// FULL   the original pixel grid; only zoom, edit and export need it.
enum class Tier{Thumb,Screen,Full};

// Long edge a SCREEN frame should carry for a window whose long edge is
// `viewportEdge` device pixels. Bucketed, so a resize by a few pixels does not
// invalidate every cached frame.
unsigned ScreenEdgeFor(unsigned viewportEdge);

// The smallest decode that still covers `edge` on the long side.
//
// For a RAW file this is a three-step ladder rather than one decision: the
// camera's own embedded JPEG if it is large enough, then a half-size demosaic
// if it is not, and only then the full-quality path. Browsing a folder of RAW
// must never pay for a full demosaic per frame.
std::shared_ptr<Image> DecodeScreen(const std::wstring& path,unsigned edge,
                                    const std::function<bool()>& cancelled);

// A reduced-resolution RAW develop: LibRaw's half_size with a fast
// interpolation. Around a fifth of the cost of the full path and entirely
// adequate for a fit-to-window view. Returns null for anything not RAW.
std::shared_ptr<Image> DecodeRawHalf(const std::wstring& path,
                                     const std::function<bool()>& cancelled);

// True when this file needs a demosaic to produce full-resolution pixels, and
// so must not be decoded in full while the user is moving through a folder.
bool IsRawPath(const std::wstring& path);

// Box-filtered reduction to `edge` pixels on the long side. Returns `src`
// itself when it is already small enough.
std::shared_ptr<Image> Downsample(const std::shared_ptr<Image>& src,unsigned edge);

// EXIF orientation 1..8 applied to pixels. Values outside that range, and the
// identity orientation, return `src` untouched.
std::shared_ptr<Image> OrientPixels(const std::shared_ptr<Image>& src,unsigned orientation);

// The average colour of an image, as the 1x1 reduction the backdrop wants.
// Reads at most a few thousand pixels regardless of source size.
bool AverageColour(const Image& src,uint8_t rgba[4]);

// Isolated RAW instrumentation. LibRaw exposes parse and entropy unpack as
// separate calls, but its production dcraw_process() fuses demosaic, colour
// conversion and tone work; keep that fused number honest instead of
// inventing theoretical percentages.
struct RawStageMetrics{
 double parseMs=0,unpackMs=0,demosaicAndColorMs=0,rgbCopyMs=0;
 uint64_t bayerBytes=0;
 unsigned rawWidth=0,rawHeight=0,outputWidth=0,outputHeight=0;
 bool success=false;
 std::wstring error;
};
RawStageMetrics MeasureRawStages(const std::wstring& path,bool halfSize=false);
