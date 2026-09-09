#include "actions.h"
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 2;CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);std::filesystem::path dir(argv[1]);std::filesystem::create_directories(dir);
 Image original;original.w=4;original.h=3;original.pixels.resize(48);
 for(unsigned y=0;y<3;y++)for(unsigned x=0;x<4;x++){auto p=&original.pixels[(y*4+x)*4];p[0]=uint8_t(30*x);p[1]=uint8_t(60*y);p[2]=180;p[3]=255;}
 int failures=0;auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;};
 std::wstring error;
 for(int angle:{0,90,180,270}){auto name=(dir/(L"rotation-"+std::to_wstring(angle)+L".png")).wstring();check(WriteImage(original,angle,name,error),"PNG encoding");auto loaded=Decode(name,error);auto expected=RotatedPixels(original,angle);check(loaded&&loaded->w==expected.w&&loaded->h==expected.h&&loaded->pixels==expected.pixels,"lossless rotation pixels and dimensions");}
 for(auto ext:{L".jpg",L".bmp",L".tiff"}){auto name=(dir/(std::wstring(L"export")+ext)).wstring();check(WriteImage(original,90,name,error),"format export");auto loaded=Decode(name,error);check(loaded&&loaded->w==3&&loaded->h==4,"export decoding/dimensions");}
 auto replace=(dir/L"rotation-0.png").wstring();check(WriteImage(original,90,replace,error),"atomic overwrite");auto loaded=Decode(replace,error);check(loaded&&loaded->w==3&&loaded->h==4,"overwritten file reflects rotation");
 auto unsupported=(dir/L"unsupported.xyz");{std::ofstream f(unsupported);f<<"preserve me";}check(!WriteImage(original,0,unsupported.wstring(),error),"unsupported encoder rejected");{std::ifstream f(unsupported);std::string text;std::getline(f,text);check(text=="preserve me","failed save preserves destination");}
 // Photoshop composites, written by hand: 4x3 RGB, stored raw and RLE packed.
 auto psd=[&](bool packed){
  std::vector<uint8_t> out;
  auto u8=[&](unsigned v){out.push_back(uint8_t(v));};
  auto u16=[&](unsigned v){u8(v>>8);u8(v&255);};
  auto u32=[&](unsigned v){u16(v>>16);u16(v&0xffff);};
  for(char c:std::string("8BPS"))u8(unsigned(c));
  u16(1);for(int i=0;i<6;i++)u8(0);
  u16(3);u32(3);u32(4);u16(8);u16(3);
  u32(0);u32(0);u32(0);
  u16(packed?1:0);
  const uint8_t plane[3][12]={{0,30,60,90,0,30,60,90,0,30,60,90},
                              {0,0,0,0,60,60,60,60,120,120,120,120},
                              {180,180,180,180,180,180,180,180,180,180,180,180}};
  if(packed)for(int c=0;c<3;c++)for(int y=0;y<3;y++)u16(5);   // one literal run of four per scanline
  for(int c=0;c<3;c++)for(int y=0;y<3;y++){
   if(packed)u8(3);
   for(int x=0;x<4;x++)u8(plane[c][y*4+x]);
  }
  auto name=(dir/(packed?L"composite-rle.psd":L"composite-raw.psd")).wstring();
  std::ofstream f(name,std::ios::binary);f.write((const char*)out.data(),std::streamsize(out.size()));f.close();
  auto image=Decode(name,error);
  bool ok=image&&image->w==4&&image->h==3;
  for(unsigned y=0;ok&&y<3;y++)for(unsigned x=0;ok&&x<4;x++){
   auto p=&image->pixels[(y*4+x)*4];
   ok=p[2]==plane[0][y*4+x]&&p[1]==plane[1][y*4+x]&&p[0]==plane[2][y*4+x]&&p[3]==255;
  }
  check(ok,packed?"PSD composite, RLE packed":"PSD composite, raw");
 };
 psd(false);psd(true);
 // Every EXIF orientation, checked against the obvious per-pixel transform. The
 // decoder's own version works a row or a tile at a time and is easy to get
 // subtly wrong; both read the same compressed bytes, so this is exact.
 Image pattern;pattern.w=37;pattern.h=23;pattern.pixels.resize(size_t(37)*23*4);
 for(unsigned y=0;y<23;y++)for(unsigned x=0;x<37;x++){
  auto p=&pattern.pixels[(y*37+x)*4];p[0]=uint8_t(x*7);p[1]=uint8_t(y*11);p[2]=uint8_t(x+y);p[3]=255;
 }
 auto plain=(dir/L"orient-base.jpg").wstring();
 check(WriteImage(pattern,0,plain,error),"orientation fixture");
 std::vector<uint8_t> jpegBytes;
 {std::ifstream f(plain,std::ios::binary);jpegBytes.assign(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());}
 auto upright=Decode(plain,error);
 for(unsigned o=2;o<=8;o++){
  std::vector<uint8_t> app1{0xFF,0xE1,0,34,'E','x','i','f',0,0,'I','I',42,0,8,0,0,0,
   1,0,0x12,0x01,3,0,1,0,0,0,uint8_t(o),0,0,0,0,0,0,0};
  std::vector<uint8_t> tagged(jpegBytes.begin(),jpegBytes.begin()+2);
  tagged.insert(tagged.end(),app1.begin(),app1.end());
  tagged.insert(tagged.end(),jpegBytes.begin()+2,jpegBytes.end());
  auto name=(dir/(L"orient-"+std::to_wstring(o)+L".jpg")).wstring();
  {std::ofstream f(name,std::ios::binary);f.write((const char*)tagged.data(),std::streamsize(tagged.size()));}
  auto turned=Decode(name,error);
  bool ok=upright&&turned&&turned->w==(o>=5?upright->h:upright->w)&&turned->h==(o>=5?upright->w:upright->h);
  for(unsigned y=0;ok&&y<upright->h;y++)for(unsigned x=0;ok&&x<upright->w;x++){
   unsigned dx=x,dy=y;
   switch(o){case 2:dx=upright->w-1-x;break;case 3:dx=upright->w-1-x;dy=upright->h-1-y;break;
    case 4:dy=upright->h-1-y;break;case 5:dx=y;dy=x;break;case 6:dx=upright->h-1-y;dy=x;break;
    case 7:dx=upright->h-1-y;dy=upright->w-1-x;break;case 8:dx=y;dy=upright->w-1-x;break;}
   ok=!memcmp(&turned->pixels[(size_t(dy)*turned->w+dx)*4],&upright->pixels[(size_t(y)*upright->w+x)*4],4);
  }
  check(ok,"EXIF orientation matches the reference transform");
 }
 // A thumbnail request must come back smaller than the file it came from.
 Image wide;wide.w=600;wide.h=400;wide.pixels.resize(size_t(600)*400*4,200);
 auto scaled=(dir/L"scaled.jpg").wstring();
 check(WriteImage(wide,0,scaled,error),"large JPEG fixture");
 auto thumb=DecodeThumb(scaled,64);
 check(thumb&&thumb->w<600&&thumb->w>=64,"JPEG thumbnail decodes scaled down");
 auto recycle=(dir/L"recycle-fixture.png").wstring();check(WriteImage(original,0,recycle,error),"recycle fixture created");check(RecycleImage(nullptr,recycle,error)&&!std::filesystem::exists(recycle),"own fixture moved to recycle bin");
 for(auto c:error)std::cerr<<(c<128?char(c):'?');std::cerr<<std::endl;CoUninitialize();return failures?1:0;
}




