#pragma once
// Vetro Look, GPL-3.0-or-later.
// Video enhancement (17): when to spend the GPU on making a film look better
// than its own pixels, and how much. Pure, like pacing and the governor: the
// decision is made from facts, and the facts are tested as literals.
//
// The one enhancement a shipping driver offers today is NVIDIA RTX Video Super
// Resolution, reached through the engine's own Direct3D video processor filter
// rather than a vendor SDK -- so the vendor stays behind this header (17.4) and
// no licence decision is taken inside the player's core.

enum class EnhancementMode{Off,Auto,On};

struct EnhancementInputs{
 bool rtxGpu=false;             // an NVIDIA RTX part with its own memory
 bool hardwareDecodeD3D11=false;// the frames are already on the GPU as D3D11 textures
 bool hdrSource=false;          // PQ or HLG: the SDR upscaler would flatten it
 bool onBattery=false,batterySaver=false;
 bool governorAllows=true;      // GovernorPolicy::enhancement
 bool pictureInPicture=false;
 unsigned sourceWidth=0,sourceHeight=0;
 unsigned presentWidth=0,presentHeight=0;   // the film's on-screen size, device pixels
};

struct EnhancementDecision{
 bool superResolution=false;
 double scale=1;                // output size over source size, quantised
 const wchar_t* reason=L"";     // for Diagnostics
};

// 17.2, 17.3: Off is off. Auto upscales only where it shows -- a real enlargement,
// on mains, with the governor's blessing, not in a small PiP. On is the viewer's
// explicit choice: power and a mild enlargement no longer hold it back, but the
// hardware, the decode path, HDR and the governor still do.
EnhancementDecision DecideEnhancement(EnhancementMode mode,const EnhancementInputs& in);

// Quarter steps from 1 to 4. A window being dragged changes the wanted scale on
// every frame; the filter is rebuilt only when this quantised value moves.
double QuantiseEnhancementScale(double scale);
