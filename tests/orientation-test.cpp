// Vetro Look, GPL-3.0-or-later.
// EXIF orientation, all eight of them, through every tier of the decoder.
//
// The mirrored orientations (2, 4, 5, 7) are the ones that matter: an
// implementation that only rotates passes a symmetric test and quietly flips
// a quarter of everybody's phone photographs. The fixture is asymmetric in
// both axes so a mirror cannot be mistaken for a rotation.
#include "image.h"
#include "pipeline.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <iostream>
namespace fs=std::filesystem;

namespace{
int failures=0,checks=0;
void Check(bool ok,const std::wstring& name,const std::wstring& detail={}){
 checks++;if(!ok)failures++;
 std::wcout<<(ok?L"PASS ":L"FAIL ")<<name;
 if(!detail.empty())std::wcout<<L"   ["<<detail<<L"]";
 std::wcout<<L"\n";
}
// Reference implementation, written straight from the EXIF specification's
// table rather than from the decoder, so the two can actually disagree.
void Expected(unsigned w,unsigned h,unsigned orientation,unsigned x,unsigned y,
              unsigned& sx,unsigned& sy){
 switch(orientation){
  case 1: sx=x;         sy=y;         break;   // as stored
  case 2: sx=w-1-x;     sy=y;         break;   // mirrored horizontally
  case 3: sx=w-1-x;     sy=h-1-y;     break;   // rotated 180
  case 4: sx=x;         sy=h-1-y;     break;   // mirrored vertically
  // The transposed cases land in an h-by-w destination, so the column index
  // is bounded by `h` and the row index by `w`, not the other way round.
  case 5: sx=y;         sy=x;         break;   // transposed
  case 6: sx=h-1-y;     sy=x;         break;   // rotated 90 clockwise
  case 7: sx=h-1-y;     sy=w-1-x;     break;   // transverse
  case 8: sx=y;         sy=w-1-x;     break;   // rotated 90 counter-clockwise
  default:sx=x;sy=y;break;
 }
}
bool Near(const uint8_t* a,const uint8_t* b,int tolerance){
 for(int c=0;c<3;c++)if(abs(int(a[c])-int(b[c]))>tolerance)return false;
 return true;
}
// Compares an oriented frame against the stored frame transformed by hand.
// `stored` is orientation 1, `shown` is what the decoder produced.
bool Matches(const Image& stored,const Image& shown,unsigned orientation,std::wstring& why){
 bool transposed=orientation>=5;
 unsigned wantW=transposed?stored.h:stored.w,wantH=transposed?stored.w:stored.h;
 if(shown.w!=wantW||shown.h!=wantH){
  why=L"size "+std::to_wstring(shown.w)+L"x"+std::to_wstring(shown.h)+
      L" wanted "+std::to_wstring(wantW)+L"x"+std::to_wstring(wantH);
  return false;
 }
 // Sample a grid rather than every pixel: JPEG is lossy and the corners are
 // what carry the orientation information.
 for(unsigned gy=1;gy<8;gy++)for(unsigned gx=1;gx<8;gx++){
  unsigned x=stored.w*gx/8,y=stored.h*gy/8;
  if(x>=stored.w||y>=stored.h)continue;
  unsigned sx=0,sy=0;
  Expected(stored.w,stored.h,orientation,x,y,sx,sy);
  if(sx>=shown.w||sy>=shown.h){why=L"mapped out of range";return false;}
  const uint8_t* want=&stored.pixels[(size_t(y)*stored.w+x)*4];
  const uint8_t* got=&shown.pixels[(size_t(sy)*shown.w+sx)*4];
  if(!Near(want,got,26)){
   why=L"pixel ("+std::to_wstring(x)+L","+std::to_wstring(y)+L") -> ("+
       std::to_wstring(sx)+L","+std::to_wstring(sy)+L")";
   return false;
  }
 }
 return true;
}
}

int wmain(int argc,wchar_t** argv){
 if(argc<2){std::wcerr<<L"usage: orientation-test <fixture directory>\n";return 2;}
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 fs::path dir(argv[1]);

 std::wstring error;
 auto stored=Decode((dir/L"orient-1.jpg").wstring(),error);
 if(!stored){std::wcerr<<L"orient-1.jpg did not decode: "<<error<<L"\n";return 2;}

 for(unsigned orientation=1;orientation<=8;orientation++){
  auto path=(dir/(L"orient-"+std::to_wstring(orientation)+L".jpg")).wstring();
  std::wstring why;

  auto full=Decode(path,error);
  Check(full&&Matches(*stored,*full,orientation,why),
        L"orientation "+std::to_wstring(orientation)+L": full decode",why);

  auto screen=DecodeScreen(path,4096,{});
  Check(screen&&Matches(*stored,*screen,orientation,why),
        L"orientation "+std::to_wstring(orientation)+L": screen tier",why);

  // The thumbnail tier only has to agree about the shape of the frame; it is
  // a scaled decode and cannot be compared pixel for pixel.
  auto thumb=DecodeThumb(path,64);
  bool transposed=orientation>=5;
  bool portrait=thumb&&thumb->h>thumb->w;
  bool wantPortrait=transposed?(stored->w>stored->h):(stored->h>stored->w);
  Check(thumb&&portrait==wantPortrait,
        L"orientation "+std::to_wstring(orientation)+L": thumbnail aspect",
        thumb?std::to_wstring(thumb->w)+L"x"+std::to_wstring(thumb->h):L"none");

  // OrientPixels is the shared primitive; applying it to an already-oriented
  // frame with the identity must be a no-op that does not copy.
  auto identity=OrientPixels(full,1);
  Check(identity==full,L"orientation "+std::to_wstring(orientation)+L": identity is free",L"");
 }

 // Round trip: every orientation composed with its inverse is the original.
 for(unsigned orientation=2;orientation<=8;orientation++){
  auto once=OrientPixels(stored,orientation);
  static const unsigned inverse[9]={0,1,2,3,4,5,8,7,6};
  auto back=OrientPixels(once,inverse[orientation]);
  bool same=back&&back->w==stored->w&&back->h==stored->h&&back->pixels==stored->pixels;
  Check(same,L"orientation "+std::to_wstring(orientation)+L": round trip through its inverse",
        back?std::to_wstring(back->w)+L"x"+std::to_wstring(back->h):L"none");
 }

 std::wcout<<L"\n"<<(failures?L"FAILED ":L"OK ")<<failures<<L" of "<<checks<<L" checks failed\n";
 CoUninitialize();
 return failures?1:0;
}
