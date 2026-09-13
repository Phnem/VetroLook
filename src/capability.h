#pragma once
// Vetro Look, GPL-3.0-or-later.
// What this machine can do, and what it is doing right now.
//
// Two different questions live here, and they are asked at very different
// rates. What the machine *is* -- which adapter, which decode profiles, which
// display -- is probed once and changes only when hardware does. What the
// machine is *doing* -- memory left, battery, clock ceiling -- is sampled
// cheaply and often, because the Resource Governor is only as good as the
// numbers it reads.
//
// Nothing here decides anything. The probe reports, the pure functions at the
// bottom turn a report into a choice, and the Governor and the pacing plan are
// the ones that act. That split is what makes this testable on a machine that
// is not the one the user has.
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

struct ID3D11Device;

// ------------------------------------------------------------- the machine ---
struct AdapterFacts{
 std::wstring description,vendor;     // vendor as a name: NVIDIA, AMD, Intel
 unsigned vendorId=0,deviceId=0;
 uint64_t dedicatedVideoMemory=0,sharedSystemMemory=0;
 // An integrated part has no memory of its own worth the name, which changes
 // what a VRAM budget means: its pressure is the system's pressure.
 bool integrated=false;
};

// The decode profiles the adapter admits to, asked of Direct3D rather than of
// the playback engine: the answer has to exist before an engine is created, and
// it must not depend on which engine that turns out to be.
struct DecodeFacts{
 bool h264=false,hevc8=false,hevc10=false,vp9=false,av1=false,mpeg2=false,vc1=false;
 bool Any()const{return h264||hevc8||hevc10||vp9||av1||mpeg2||vc1;}
};

// The output the window is on. In composition mode the playback engine has no
// window and therefore cannot ask any of this for itself -- which is exactly
// why the viewer has to tell it. See docs/STAGES.md, stage 2's gate result.
struct DisplayFacts{
 std::wstring name;                   // the adapter's own name for the output
 double refreshHz=0;                  // exact, from the display configuration
 unsigned width=0,height=0;
 bool hdr=false;                      // Windows HDR is on for this output
 bool wideGamut=false;         // Windows is managing a wider gamut than sRGB
 // What the panel's own EDID claims, which is not the same thing: an
 // unmanaged display that says it is wide still receives sRGB numbers, and
 // acting on the claim alone would mis-grade every film on it.
 bool panelWideGamut=false;
 double maxNits=0,minNits=0;          // the panel's own claim, when it makes one
 double sdrWhiteNits=0;               // where Windows puts paper white
 bool variableRefresh=false;
};

struct Capabilities{
 AdapterFacts adapter;
 DecodeFacts decode;
 DisplayFacts display;
 uint64_t totalRam=0;
 unsigned cores=0;
 std::wstring os;
 bool probed=false;
};

// ---------------------------------------------------------- what it is doing --
struct PowerFacts{
 bool onBattery=false,batterySaver=false;
 int batteryPercent=-1;               // -1 when there is no battery
 // The clock ceiling the firmware is currently willing to allow, over what the
 // part can do. Windows exposes no thermal reading a normal process may read,
 // and this is the honest proxy: a sustained ceiling below the maximum is the
 // machine telling us it is hot or power-limited.
 double clockHeadroom=1;
 bool throttled=false;
};
struct MemoryFacts{
 uint64_t totalRam=0,availableRam=0,workingSet=0;
 uint64_t vramBudget=0,vramUsage=0;   // the process's own share, as DXGI sees it
 bool vramKnown=false;
 double ramPressure=0,vramPressure=0; // 0 comfortable .. 1 out of room
};

// Probes the machine once and keeps the answer. `device` may be null, in which
// case the adapter and decode halves stay empty and the rest is still filled.
const Capabilities& CapabilityProbe(ID3D11Device* device,HWND window);
const Capabilities& CapabilitiesNow();
// Re-reads the output the window is on. Cheap enough for a monitor change or a
// window dragged between screens, which is when it is needed.
DisplayFacts ProbeDisplay(HWND window);
// Replaces the remembered display. Called when the window has moved to another
// screen or the screen itself has changed, so everything downstream reads one
// agreed answer rather than probing for itself.
void CapabilityDisplayChanged(const DisplayFacts& display);
PowerFacts ReadPower();
MemoryFacts ReadMemory(ID3D11Device* device);

// ------------------------------------------------------- pipeline selection ---
// §13.2: a machine and a film add up to one pipeline, and the answer is
// arithmetic on the two reports rather than a probe of its own.
struct MediaProfile{
 std::wstring codec;                  // as the container names it
 unsigned width=0,height=0,bitDepth=8;
 bool hdr=false;
 double fps=0;
};
struct PipelineChoice{
 bool hardwareDecode=false;
 bool hdrOutput=false;                // the film's own colour reaches the panel
 bool toneMap=false;                  // HDR film, SDR panel
 bool highQualityScaler=true;
 bool enhancementCandidate=false;     // stage 9 may offer RTX here
 const wchar_t* note=L"";
};
PipelineChoice ChoosePipeline(const Capabilities& machine,const MediaProfile& media,const PowerFacts& power);
// The one-phrase description of a choice. Separate from the function that makes
// it, because the engine's own answer about decode arrives later than the
// prediction and the phrase has to be able to follow it.
const wchar_t* PipelineNote(const PipelineChoice& choice);
// True when the adapter can decode this codec at this depth in hardware.
bool DecodeSupported(const DecodeFacts& decode,const std::wstring& codec,unsigned bitDepth);

// One sanitised block for Diagnostics and the log: no paths, no user names.
std::wstring CapabilityReport(const Capabilities& machine,const PowerFacts& power,const MemoryFacts& memory);
