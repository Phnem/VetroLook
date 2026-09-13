// Vetro Look, GPL-3.0-or-later.
// When the viewer spends the GPU on enhancement, and how much (enhance.h).
#include "../src/enhance.h"
#include <iostream>

namespace{
int failures=0;
void Check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;}
bool Near(double a,double b){return a>b-1e-9&&a<b+1e-9;}
// A 1080p SDR film, hardware decoded on an RTX card, on mains, shown on a
// maximised 1440p window.
EnhancementInputs Typical(){
 EnhancementInputs in;
 in.rtxGpu=true;in.hardwareDecodeD3D11=true;
 in.sourceWidth=1920;in.sourceHeight=1080;
 in.presentWidth=2560;in.presentHeight=1440;
 return in;
}
}

int main(){
 auto in=Typical();
 auto d=DecideEnhancement(EnhancementMode::Auto,in);
 Check(d.superResolution&&Near(d.scale,1.25),"auto: 1080p on 1440p is enlarged, at a quarter step");
 Check(!DecideEnhancement(EnhancementMode::Off,in).superResolution,"off is off");

 in=Typical();in.rtxGpu=false;
 Check(!DecideEnhancement(EnhancementMode::On,in).superResolution,"no RTX part, nothing to ask for, even when on");
 in=Typical();in.hardwareDecodeD3D11=false;
 Check(!DecideEnhancement(EnhancementMode::On,in).superResolution,"software decode: the frames are not on the GPU");
 in=Typical();in.hdrSource=true;
 Check(!DecideEnhancement(EnhancementMode::On,in).superResolution,"HDR films are left to the HDR pipeline");
 in=Typical();in.governorAllows=false;
 Check(!DecideEnhancement(EnhancementMode::On,in).superResolution,"the governor wins over the viewer's choice too");

 in=Typical();in.onBattery=true;
 Check(!DecideEnhancement(EnhancementMode::Auto,in).superResolution,"auto: not on battery");
 Check(DecideEnhancement(EnhancementMode::On,in).superResolution,"on: battery is the viewer's call");
 in=Typical();in.batterySaver=true;
 Check(!DecideEnhancement(EnhancementMode::Auto,in).superResolution,"auto: not under battery saver");

 in=Typical();in.pictureInPicture=true;in.presentWidth=420;in.presentHeight=236;
 Check(!DecideEnhancement(EnhancementMode::Auto,in).superResolution,"a small PiP is a reduction, not a canvas");
 Check(!DecideEnhancement(EnhancementMode::On,in).superResolution,"even when on, a reduction is not enhanced");

 in=Typical();in.presentWidth=2099;in.presentHeight=1181;
 Check(!DecideEnhancement(EnhancementMode::Auto,in).superResolution,"auto: nine percent larger is not worth it");
 Check(DecideEnhancement(EnhancementMode::On,in).superResolution,"on: any real enlargement");

 in=Typical();in.sourceWidth=1280;in.sourceHeight=720;in.presentWidth=3840;in.presentHeight=2160;
 d=DecideEnhancement(EnhancementMode::Auto,in);
 Check(d.superResolution&&Near(d.scale,3.0),"720p on 4K is three times");
 in=Typical();in.sourceWidth=640;in.sourceHeight=360;in.presentWidth=3840;in.presentHeight=2160;
 Check(Near(DecideEnhancement(EnhancementMode::Auto,in).scale,4.0),"the scale never exceeds four");
 in=Typical();in.presentWidth=2560;in.presentHeight=1080;
 Check(!DecideEnhancement(EnhancementMode::Auto,in).superResolution,"ultrawide: the smaller ratio decides");
 in=Typical();in.sourceWidth=0;
 Check(!DecideEnhancement(EnhancementMode::On,in).superResolution,"unknown size: wait");

 Check(Near(QuantiseEnhancementScale(1.33),1.25)&&Near(QuantiseEnhancementScale(1.38),1.5)&&
       Near(QuantiseEnhancementScale(0.7),1.0)&&Near(QuantiseEnhancementScale(9),4.0),"quantising");
 Check(Near(QuantiseEnhancementScale(1.34),QuantiseEnhancementScale(1.36)),"a drag across a small range keeps one filter");

 std::cout<<(failures?"FAILURES: ":"all passed: ")<<failures<<"\n";
 return failures?1:0;
}
