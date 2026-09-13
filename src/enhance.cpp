// Vetro Look, GPL-3.0-or-later.
// See enhance.h.
#include "enhance.h"
#include <algorithm>
#include <cmath>

double QuantiseEnhancementScale(double scale){
 if(!(scale>1))return 1;
 double q=std::floor(scale*4.0+0.5)/4.0;
 return (std::min)(4.0,(std::max)(1.0,q));
}

EnhancementDecision DecideEnhancement(EnhancementMode mode,const EnhancementInputs& in){
 EnhancementDecision d;
 if(mode==EnhancementMode::Off){d.reason=L"off";return d;}
 if(!in.rtxGpu){d.reason=L"no RTX GPU";return d;}
 if(!in.hardwareDecodeD3D11){d.reason=L"frames are not decoded on the GPU";return d;}
 if(in.hdrSource){d.reason=L"HDR film";return d;}
 if(!in.governorAllows){d.reason=L"held by the governor";return d;}
 if(!in.sourceWidth||!in.sourceHeight||!in.presentWidth||!in.presentHeight){d.reason=L"size not known yet";return d;}
 bool automatic=mode==EnhancementMode::Auto;
 if(automatic&&(in.onBattery||in.batterySaver)){d.reason=L"on battery";return d;}
 // The smaller of the two ratios: a letterboxed film is enlarged by whichever
 // side meets the window first.
 double scale=(std::min)(double(in.presentWidth)/in.sourceWidth,double(in.presentHeight)/in.sourceHeight);
 // 17.3: a 480x270 PiP of a 1080p film is a reduction, and nothing is gained by
 // enhancing what is about to be thrown away.
 if(automatic&&in.pictureInPicture){d.reason=L"picture in picture";return d;}
 double needed=automatic?1.15:1.02;
 if(scale<needed){d.reason=L"shown at or below its own size";return d;}
 d.superResolution=true;
 d.scale=QuantiseEnhancementScale(scale);
 if(d.scale<=1)d.scale=1.25;
 d.reason=automatic?L"auto":L"on";
 return d;
}
