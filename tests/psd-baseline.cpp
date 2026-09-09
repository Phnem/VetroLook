// Vetro Look, GPL-3.0-or-later.
// The PSD composite decoder as it stood before the memory rework, kept only so
// the before/after figures in the performance report are measured rather than
// asserted. It is never linked into the application.
//
// Do not fix bugs here. If this file and src/psd.cpp disagree about anything
// other than allocation, src/psd.cpp is the one that is right.
#include "image.h"
#include <miniz.h>
#include <cstring>
#include <stdexcept>

namespace{
struct Reader{
 const uint8_t* p;size_t size,at=0;
 void Need(size_t n)const{if(at+n>size)throw std::runtime_error("psd");}
 uint8_t U8(){Need(1);return p[at++];}
 uint16_t U16(){Need(2);uint16_t v=uint16_t(p[at]<<8|p[at+1]);at+=2;return v;}
 uint32_t U32(){Need(4);uint32_t v=uint32_t(p[at])<<24|uint32_t(p[at+1])<<16|uint32_t(p[at+2])<<8|p[at+3];at+=4;return v;}
 uint64_t U64(){uint64_t hi=U32();return hi<<32|U32();}
 void Skip(uint64_t n){if(n>size-at)throw std::runtime_error("psd");at+=size_t(n);}
};
void Unpack(Reader& in,uint8_t* out,size_t count,size_t available){
 if(available>in.size-in.at)throw std::runtime_error("psd");
 size_t written=0,end=in.at+available;
 while(written<count&&in.at<end){
  int8_t code=int8_t(in.U8());
  if(code>=0){
   if(size_t(code)+1>end-in.at)throw std::runtime_error("psd");
   size_t run=size_t(code)+1;if(run>count-written)throw std::runtime_error("psd");
   memcpy(out+written,in.p+in.at,run);
   in.at+=size_t(code)+1;written+=run;
  }else if(code!=-128){
   if(in.at>=end)throw std::runtime_error("psd");
   size_t run=size_t(1-code);if(run>count-written)throw std::runtime_error("psd");
   uint8_t value=in.U8();
   memset(out+written,value,run);written+=run;
  }
 }
 if(written!=count)throw std::runtime_error("psd");
 in.at=end;
}
void UndoPrediction(uint8_t* data,unsigned width,size_t rows,unsigned depth){
 if(depth==8){
  for(size_t y=0;y<rows;y++){auto row=data+y*width;for(unsigned x=1;x<width;x++)row[x]=uint8_t(row[x]+row[x-1]);}
 }else{
  size_t stride=size_t(width)*2;
  for(size_t y=0;y<rows;y++)for(unsigned x=1;x<width;x++){
   auto row=data+y*stride;uint16_t a=uint16_t(row[(x-1)*2]<<8|row[(x-1)*2+1]);
   uint16_t b=uint16_t(row[x*2]<<8|row[x*2+1]);b=uint16_t(a+b);row[x*2]=uint8_t(b>>8);row[x*2+1]=uint8_t(b);
  }
 }
}
}

std::shared_ptr<Image> DecodePsdBaseline(const uint8_t* bytes,size_t size,std::wstring* variantError){
 if(variantError)variantError->clear();
 try{
  Reader in{bytes,size};
  if(in.U32()!=0x38425053)return {};
  unsigned version=in.U16();
  if(version!=1&&version!=2)return {};
  in.Skip(6);
  unsigned channels=in.U16();
  uint32_t height=in.U32(),width=in.U32();
  unsigned depth=in.U16(),mode=in.U16();
  if(!width||!height||!channels||uint64_t(width)*height>100000000ull)return {};
  if(channels>56)return {};
  uint32_t paletteSize=in.U32();
  size_t paletteAt=in.at;
  in.Skip(paletteSize);
  uint32_t resourceBytes=in.U32();
  size_t resourceEnd=in.at+resourceBytes;
  if(resourceEnd>size)return {};
  in.at=resourceEnd;
  in.Skip(version==2?in.U64():in.U32());
  unsigned compression=in.U16();
  if(depth!=8&&depth!=16)return {};
  if(compression>3)return {};
  unsigned used=mode==3&&channels>=3?(std::min)(channels,4u):mode==4&&channels>=4?(std::min)(channels,5u):
   (mode==1||mode==8||mode==2)?(std::min)(channels,2u):0u;
  if(!used)return {};
  size_t pixels=size_t(width)*height,sample=depth/8;
  if(pixels>SIZE_MAX/used||pixels*used>SIZE_MAX/sample||pixels*used*sample>512u*1024u*1024u)return {};
  std::vector<uint8_t> plane(pixels*used);
  if(compression==0){
   for(unsigned c=0;c<used;c++){
    in.Need(pixels*sample);
    if(sample==1)memcpy(plane.data()+c*pixels,in.p+in.at,pixels);
    else for(size_t i=0;i<pixels;i++)plane[c*pixels+i]=in.p[in.at+i*2];
    in.at+=pixels*sample;
   }
  }else if(compression==1){
   std::vector<uint64_t> counts(size_t(height)*channels);
   for(auto& count:counts)count=version==2?in.U32():in.U16();
   std::vector<uint8_t> row(size_t(width)*sample);
   for(unsigned c=0;c<channels;c++)for(uint32_t y=0;y<height;y++){
    uint64_t available=counts[size_t(c)*height+y];if(available>SIZE_MAX)throw std::runtime_error("psd");
    if(c>=used){in.Skip(available);continue;}
    in.Need(size_t(available));
    Unpack(in,row.data(),row.size(),size_t(available));
    uint8_t* out=plane.data()+size_t(c)*pixels+size_t(y)*width;
    if(sample==1)memcpy(out,row.data(),width);
    else for(uint32_t x=0;x<width;x++)out[x]=row[size_t(x)*2];
   }
  }else{
   size_t inflatedSize=pixels*channels*sample;
   if(inflatedSize>512u*1024u*1024u)return {};
   std::vector<uint8_t> inflated(inflatedSize);mz_ulong actual=mz_ulong(inflated.size());
   int code=mz_uncompress(inflated.data(),&actual,in.p+in.at,mz_ulong(in.size-in.at));
   if(code!=MZ_OK||actual!=inflated.size())throw std::runtime_error("psd");
   if(compression==3)UndoPrediction(inflated.data(),width,height*channels,depth);
   for(unsigned c=0;c<used;c++){
    auto source=inflated.data()+size_t(c)*pixels*sample;auto out=plane.data()+size_t(c)*pixels;
    if(sample==1)memcpy(out,source,pixels);else for(size_t i=0;i<pixels;i++)out[i]=source[i*2];
   }
  }
  auto image=std::make_shared<Image>();
  image->w=width;image->h=height;image->pixels.resize(pixels*4);
  image->codec=L"PSD composite";
  const uint8_t* palette=bytes+paletteAt;
  for(size_t i=0;i<pixels;i++){
   uint8_t r,g,b,a=255;
   if(mode==3){
    r=plane[i];g=plane[pixels+i];b=plane[2*pixels+i];
    if(used>3)a=plane[3*pixels+i];
   }else if(mode==4){
    unsigned k=plane[3*pixels+i];
    r=uint8_t(plane[i]*k/255);g=uint8_t(plane[pixels+i]*k/255);b=uint8_t(plane[2*pixels+i]*k/255);
   }else if(mode==2){
    unsigned index=plane[i];
    r=palette[index];g=palette[256+index];b=palette[512+index];
   }else{
    r=g=b=plane[i];
    if(used>1)a=plane[pixels+i];
   }
   uint8_t* out=&image->pixels[i*4];
   out[0]=uint8_t((b*a+127)/255);out[1]=uint8_t((g*a+127)/255);out[2]=uint8_t((r*a+127)/255);out[3]=a;
  }
  return image;
 }catch(...){if(variantError)*variantError=L"The Photoshop document is damaged or truncated.";return {};}
}
