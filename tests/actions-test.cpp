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
 auto recycle=(dir/L"recycle-fixture.png").wstring();check(WriteImage(original,0,recycle,error),"recycle fixture created");check(RecycleImage(nullptr,recycle,error)&&!std::filesystem::exists(recycle),"own fixture moved to recycle bin");
 for(auto c:error)std::cerr<<(c<128?char(c):'?');std::cerr<<std::endl;CoUninitialize();return failures?1:0;
}




