// Vetro Look, GPL-3.0-or-later.
// Photoshop documents, read the way a viewer needs them: the flattened
// composite Photoshop stores for compatibility, never the layer stack.
#include "image.h"
#include <miniz.h>
#include <algorithm>
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
// PackBits, one scanline at a time, with the destination length as the only
// stopping condition a damaged file cannot talk us out of.
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

// jpegPreview/jpegSize receive Photoshop's own embedded JPEG thumbnail when the
// composite cannot be used -- a document saved without "Maximize compatibility"
// carries no usable merged image, and that thumbnail is all there is.
static std::shared_ptr<Image> DecodePsdImpl(const uint8_t* bytes,size_t size,
                                            const uint8_t** jpegPreview,size_t* jpegSize,
                                            std::wstring* variantError,unsigned maxEdge){
 *jpegPreview=nullptr;*jpegSize=0;
 if(variantError)variantError->clear();
 try{
  Reader in{bytes,size};
  if(in.U32()!=0x38425053)return {};        // "8BPS"
  unsigned version=in.U16();
  if(version!=1&&version!=2){if(variantError)*variantError=L"Unsupported Photoshop document version.";return {};}
  in.Skip(6);
  unsigned channels=in.U16();
  uint32_t height=in.U32(),width=in.U32();
  unsigned depth=in.U16(),mode=in.U16();
  if(!width||!height||!channels||uint64_t(width)*height>100000000ull){if(variantError)*variantError=L"Unsupported Photoshop document dimensions.";return {};}
  if(channels>56){if(variantError)*variantError=L"Unsupported Photoshop channel count.";return {};}

  uint32_t paletteSize=in.U32();
  size_t paletteAt=in.at;
  in.Skip(paletteSize);

  uint32_t resourceBytes=in.U32();
  size_t resourceEnd=in.at+resourceBytes;
  // A caller after the thumbnail alone reads only the head of the file, so scan
  // whatever resources arrived rather than refusing a short buffer outright.
  bool truncated=resourceEnd>size;
  if(truncated)resourceEnd=size;
  while(in.at+12<=resourceEnd){
   if(in.U32()!=0x3842494D)break;           // "8BIM"
   unsigned id=in.U16();
   unsigned nameLength=in.U8();
   in.Skip(nameLength+((nameLength+1)&1));   // the Pascal name is padded to an even total
   uint32_t length=in.U32();
   size_t blockAt=in.at;
   if(length>resourceEnd-in.at)break;
   // 1036 is the modern RGB thumbnail, 1033 the Photoshop 4 BGR one. Both wrap
   // a JPEG behind the same 28 byte descriptor.
   if((id==1036||id==1033)&&length>28){
    Reader thumb{bytes+blockAt,length};
    if(thumb.U32()==1){*jpegPreview=bytes+blockAt+28;*jpegSize=length-28;}
   }
   in.at=blockAt;in.Skip(length+(length&1));
  }
  if(truncated){if(variantError)*variantError=L"The Photoshop document is damaged or truncated.";return {};}
  in.at=resourceEnd;

  in.Skip(version==2?in.U64():in.U32());     // layer and mask information

  unsigned compression=in.U16();
  if(depth!=8&&depth!=16){if(variantError)*variantError=L"Unsupported Photoshop composite depth (only 8-bit and 16-bit are supported).";return {};}
  if(compression>3){if(variantError)*variantError=L"Unsupported Photoshop composite compression.";return {};}
  unsigned used=mode==3&&channels>=3?(std::min)(channels,4u):mode==4&&channels>=4?(std::min)(channels,5u):
   (mode==1||mode==8||mode==2)?(std::min)(channels,2u):0u;
  if(!used){if(variantError)*variantError=L"Unsupported Photoshop composite color mode.";return {};}
  if(mode==2&&(depth!=8||paletteSize<768)){if(variantError)*variantError=L"Unsupported indexed Photoshop palette/depth.";return {};}

  size_t sourcePixels=size_t(width)*height,sample=depth/8;
  if(mode==4)used=(std::min)(used,4u);
  if(sourcePixels>SIZE_MAX/4||sourcePixels*4>1024u*1024u*1024u){
   if(variantError)*variantError=L"Photoshop composite is too large to decode safely.";return {};
  }

  uint32_t outWidth=width,outHeight=height;
  unsigned longEdge=(std::max)(width,height);
  if(maxEdge&&longEdge>maxEdge){
   outWidth=(std::max)(1u,uint32_t((uint64_t(width)*maxEdge+longEdge/2)/longEdge));
   outHeight=(std::max)(1u,uint32_t((uint64_t(height)*maxEdge+longEdge/2)/longEdge));
  }
  bool screen=outWidth!=width||outHeight!=height;
  size_t pixels=size_t(outWidth)*outHeight;

  // The composite is written straight into the destination image rather than
  // into a full planar copy first. A 100 megapixel CMYK document used to hold
  // a 400 MB plane buffer, a 400 MB inflate buffer and the 400 MB result all
  // at once; only the result is unavoidable.
  //
  // Every colour mode fits the four bytes of a BGRA pixel: RGB and CMYK land
  // one channel per byte, grey and indexed occupy one byte and are expanded
  // in the finishing pass below.
  auto image=std::make_shared<Image>();
  image->w=outWidth;image->h=outHeight;image->pixels.assign(pixels*4,0);
  image->sourceW=width;image->sourceH=height;
  image->tier=screen?TierScreenRes:TierFullRes;
  image->codec=screen?L"PSD streaming screen composite":L"PSD composite";
  // Destination byte for each source channel: B G R A is the memory order.
  static const unsigned rgbSlot[4]={2,1,0,3};
  auto slotFor=[&](unsigned channel)->unsigned{
   if(mode==3||mode==4)return rgbSlot[channel<4?channel:3];
   return channel==0?2u:3u;                       // grey/indexed: value, then alpha
  };
  // Screen decoding never constructs a source-sized plane or BGRA frame.
  // Photoshop stores channels sequentially, which lets us box-filter one
  // channel using one source row, one target row and a tiny vertical
  // accumulator, then write that channel directly into the final BGRA image.
  std::vector<uint16_t> horizontal(screen?outWidth:0);
  std::vector<uint32_t> vertical(screen?outWidth:0);
  uint32_t currentTargetY=0,verticalRows=0;
  auto flushScreenRow=[&](unsigned channel){
   if(!verticalRows)return;
   unsigned slot=slotFor(channel);
   uint8_t* out=image->pixels.data()+(size_t(currentTargetY)*outWidth)*4+slot;
   for(uint32_t x=0;x<outWidth;x++)out[size_t(x)*4]=uint8_t(vertical[x]/verticalRows);
   std::fill(vertical.begin(),vertical.end(),0u);verticalRows=0;
  };
  auto deposit=[&](unsigned channel,uint32_t y,const uint8_t* row){
   unsigned slot=slotFor(channel);
   if(!screen){
    uint8_t* out=image->pixels.data()+(size_t(y)*width)*4+slot;
    if(sample==1)for(uint32_t x=0;x<width;x++)out[size_t(x)*4]=row[x];
    else for(uint32_t x=0;x<width;x++)out[size_t(x)*4]=row[size_t(x)*2];
    return;
   }
   if(y==0){currentTargetY=0;verticalRows=0;std::fill(vertical.begin(),vertical.end(),0u);}
   // Use the same half-open source intervals as a conventional box filter:
   // [floor(ty*H/th), floor((ty+1)*H/th)). Mapping source y with
   // floor(y*th/H) instead shifts boundaries by one row for non-integer
   // ratios and produces visible horizontal seams.
   while(currentTargetY+1<outHeight&&
         y>=uint32_t(uint64_t(currentTargetY+1)*height/outHeight)){
    flushScreenRow(channel);currentTargetY++;
   }
   for(uint32_t x=0;x<outWidth;x++){
    uint32_t begin=uint32_t(uint64_t(x)*width/outWidth);
    uint32_t end=uint32_t(uint64_t(x+1)*width/outWidth);
    if(end<=begin)end=begin+1;
    uint32_t sum=0;
    if(sample==1)for(uint32_t sx=begin;sx<end;sx++)sum+=row[sx];
    else for(uint32_t sx=begin;sx<end;sx++)sum+=row[size_t(sx)*2];
    horizontal[x]=uint16_t(sum/(end-begin));
    vertical[x]+=horizontal[x];
   }
   verticalRows++;
   if(y+1==height)flushScreenRow(channel);
  };

  std::vector<uint8_t> row(size_t(width)*sample);
  if(compression==0){
   for(unsigned c=0;c<used;c++){
    in.Need(sourcePixels*sample);
    for(uint32_t y=0;y<height;y++)deposit(c,y,in.p+in.at+size_t(y)*width*sample);
    in.at+=sourcePixels*sample;
   }
  }else if(compression==1){
   // Every scanline of every channel is length-prefixed up front, including the
   // channels past the ones we keep, so the table has to be read in full.
   std::vector<uint64_t> counts(size_t(height)*channels);
   for(auto& count:counts)count=version==2?in.U32():in.U16();
   for(unsigned c=0;c<channels;c++)for(uint32_t y=0;y<height;y++){
    uint64_t available=counts[size_t(c)*height+y];if(available>SIZE_MAX)throw std::runtime_error("psd");
    if(c>=used){in.Skip(available);continue;}
    in.Need(size_t(available));
    Unpack(in,row.data(),row.size(),size_t(available));
    deposit(c,y,row.data());
   }
  }else{
   // ZIP, streamed a scanline at a time. mz_uncompress would need the whole
   // decompressed composite — every channel, at full bit depth — resident
   // before a single pixel could be placed.
   mz_stream stream{};
   if(mz_inflateInit(&stream)!=MZ_OK)throw std::runtime_error("psd");
   struct Guard{mz_stream* s;~Guard(){mz_inflateEnd(s);}} guard{&stream};
   stream.next_in=in.p+in.at;
   stream.avail_in=mz_uint32((std::min)(in.size-in.at,size_t(0xFFFFFFFFu)));
   bool ended=false;
   auto pull=[&](uint8_t* into,size_t want){
    stream.next_out=into;stream.avail_out=mz_uint32(want);
    while(stream.avail_out){
     // Exhausted input does not mean exhausted output: miniz keeps up to a
     // 32 KB dictionary of decompressed bytes it has not handed over yet, so
     // the only honest stopping conditions are an error, the end of the
     // stream, or a call that made no progress.
     if(ended)throw std::runtime_error("psd");
     mz_uint before=stream.avail_out;
     int code=mz_inflate(&stream,MZ_NO_FLUSH);
     if(code==MZ_STREAM_END){ended=true;if(stream.avail_out)throw std::runtime_error("psd");break;}
     if(code!=MZ_OK)throw std::runtime_error("psd");
     if(stream.avail_out==before)throw std::runtime_error("psd");
    }
   };
   for(unsigned c=0;c<channels;c++)for(uint32_t y=0;y<height;y++){
    pull(row.data(),row.size());
    if(c>=used)continue;
    if(compression==3)UndoPrediction(row.data(),width,1,depth);
    deposit(c,y,row.data());
   }
  }

  // The finishing pass: expand or convert in place, then premultiply. No
  // second full-size buffer, and one linear walk over the result.
  const uint8_t* palette=bytes+paletteAt;
  for(size_t i=0;i<pixels;i++){
   uint8_t* out=&image->pixels[i*4];
   uint8_t r,g,b,a=255;
   if(mode==3){
    b=out[0];g=out[1];r=out[2];
    if(used>3)a=out[3];
   }else if(mode==4){
    // Photoshop stores CMYK inverted, which turns the conversion back to RGB
    // into a plain multiply against the stored key channel.
    unsigned k=out[3];
    r=uint8_t(out[2]*k/255);g=uint8_t(out[1]*k/255);b=uint8_t(out[0]*k/255);
   }else if(mode==2){
    unsigned index=out[2];
    r=palette[index];g=palette[256+index];b=palette[512+index];
   }else{
    r=g=b=out[2];
    if(used>1)a=out[3];
   }
   out[0]=uint8_t((b*a+127)/255);out[1]=uint8_t((g*a+127)/255);out[2]=uint8_t((r*a+127)/255);out[3]=a;
  }
  return image;
 }catch(...){if(variantError&&variantError->empty())*variantError=L"The Photoshop document is damaged or truncated.";return {};}
}

std::shared_ptr<Image> DecodePsd(const uint8_t* bytes,size_t size,const uint8_t** jpegPreview,
                                 size_t* jpegSize,std::wstring* variantError){
 return DecodePsdImpl(bytes,size,jpegPreview,jpegSize,variantError,0);
}

std::shared_ptr<Image> DecodePsdScreen(const uint8_t* bytes,size_t size,unsigned maxEdge,
                                       std::wstring* variantError){
 const uint8_t* preview=nullptr;size_t previewSize=0;
 return DecodePsdImpl(bytes,size,&preview,&previewSize,variantError,maxEdge);
}
