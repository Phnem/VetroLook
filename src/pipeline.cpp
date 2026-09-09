// Vetro Look, GPL-3.0-or-later.
#include "pipeline.h"
#include <libraw/libraw.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

std::shared_ptr<Image> OrientPixels(const std::shared_ptr<Image>& src,unsigned o){
 if(!src||o<2||o>8)return src;
 unsigned w=src->w,h=src->h;bool transpose=o>=5;
 auto out=std::make_shared<Image>();
 out->w=transpose?h:w;out->h=transpose?w:h;out->codec=src->codec;out->hasAlpha=src->hasAlpha;
 out->tier=src->tier;
 out->sourceW=transpose?src->SourceH():src->SourceW();
 out->sourceH=transpose?src->SourceW():src->SourceH();
 out->pixels.resize(size_t(out->w)*out->h*4);
 auto in=reinterpret_cast<const uint32_t*>(src->pixels.data());
 auto dst=reinterpret_cast<uint32_t*>(out->pixels.data());
 unsigned ow=out->w;
 if(!transpose){
  for(unsigned y=0;y<h;y++){
   const uint32_t* s=in+size_t(y)*w;
   uint32_t* d=dst+size_t(o==3||o==4?h-1-y:y)*ow;
   if(o==2||o==3)for(unsigned x=0;x<w;x++)d[w-1-x]=s[x];
   else memcpy(d,s,size_t(w)*4);
  }
 }else{
  constexpr unsigned Tile=32;
  for(unsigned y0=0;y0<h;y0+=Tile)for(unsigned x0=0;x0<w;x0+=Tile){
   unsigned ylimit=(std::min)(y0+Tile,h),xlimit=(std::min)(x0+Tile,w);
   for(unsigned y=y0;y<ylimit;y++){
    const uint32_t* s=in+size_t(y)*w;
    for(unsigned x=x0;x<xlimit;x++){
     unsigned dx=y,dy=x;
     switch(o){case 6:dx=h-1-y;break;case 7:dx=h-1-y;dy=w-1-x;break;case 8:dy=w-1-x;break;}
     dst[size_t(dy)*ow+dx]=s[x];
    }
   }
  }
 }
 return out;
}

std::shared_ptr<Image> Downsample(const std::shared_ptr<Image>& src,unsigned edge){
 if(!src||!src->w||!src->h||!edge)return {};
 unsigned longEdge=(std::max)(src->w,src->h);
 if(longEdge<=edge)return src;
 auto out=std::make_shared<Image>();
 out->w=(std::max)(1u,unsigned((uint64_t(src->w)*edge+longEdge/2)/longEdge));
 out->h=(std::max)(1u,unsigned((uint64_t(src->h)*edge+longEdge/2)/longEdge));
 out->codec=src->codec;out->hasAlpha=src->hasAlpha;
 // Reducing a frame does not change which asset it came from.
 out->sourceW=src->SourceW();out->sourceH=src->SourceH();
 out->tier=(out->w<out->sourceW||out->h<out->sourceH)?TierScreenRes:src->tier;
 out->pixels.resize(size_t(out->w)*out->h*4);
 for(unsigned y=0;y<out->h;y++)for(unsigned x=0;x<out->w;x++){
  uint64_t sum[4]={0,0,0,0};uint64_t n=0;
  unsigned x0=unsigned(uint64_t(x)*src->w/out->w),x1=unsigned(uint64_t(x+1)*src->w/out->w);
  unsigned y0=unsigned(uint64_t(y)*src->h/out->h),y1=unsigned(uint64_t(y+1)*src->h/out->h);
  if(x1<=x0)x1=x0+1;if(y1<=y0)y1=y0+1;
  for(unsigned sy=y0;sy<y1;sy++)for(unsigned sx=x0;sx<x1;sx++){
   const uint8_t* p=&src->pixels[((size_t)sy*src->w+sx)*4];
   for(int c=0;c<4;c++)sum[c]+=p[c];
   n++;
  }
  uint8_t* d=&out->pixels[((size_t)y*out->w+x)*4];
  for(int c=0;c<4;c++)d[c]=n?uint8_t(sum[c]/n):0;
 }
 return out;
}

bool AverageColour(const Image& src,uint8_t rgba[4]){
 if(!src.w||!src.h||src.pixels.empty())return false;
 // At most ~64x64 samples: the backdrop is a blurred wash, and walking a
 // 36 megapixel buffer to compute it is work nobody can see.
 unsigned stepX=(std::max)(1u,src.w/64),stepY=(std::max)(1u,src.h/64);
 uint64_t sum[4]={0,0,0,0},n=0;
 for(unsigned y=0;y<src.h;y+=stepY)for(unsigned x=0;x<src.w;x+=stepX){
  const uint8_t* p=&src.pixels[((size_t)y*src.w+x)*4];
  for(int c=0;c<4;c++)sum[c]+=p[c];
  n++;
 }
 if(!n)return false;
 for(int c=0;c<4;c++)rgba[c]=uint8_t(sum[c]/n);
 return true;
}

RawStageMetrics MeasureRawStages(const std::wstring& path,bool halfSize){
 RawStageMetrics result;
 if(!IsRawPath(path)){result.error=L"not RAW";return result;}
 using Clock=std::chrono::steady_clock;
 auto elapsed=[](Clock::time_point at){
  return std::chrono::duration<double,std::milli>(Clock::now()-at).count();
 };
 try{
  LibRaw raw;
  auto started=Clock::now();int status=raw.open_file(path.c_str());result.parseMs=elapsed(started);
  if(status!=LIBRAW_SUCCESS){result.error=L"parse failed";return result;}
  result.rawWidth=raw.imgdata.sizes.raw_width;result.rawHeight=raw.imgdata.sizes.raw_height;
  raw.imgdata.params.output_bps=8;raw.imgdata.params.use_camera_wb=1;
  raw.imgdata.params.half_size=halfSize?1:0;raw.imgdata.params.user_qual=halfSize?0:3;
  started=Clock::now();status=raw.unpack();result.unpackMs=elapsed(started);
  if(status!=LIBRAW_SUCCESS){result.error=L"unpack failed";return result;}
  // LibRaw's unpacked Bayer storage is ushort for the ordinary mosaic path.
  // Non-Bayer/Foveon variants are reported as zero rather than guessed.
  if(raw.imgdata.rawdata.raw_image)
   result.bayerBytes=uint64_t(result.rawWidth)*result.rawHeight*sizeof(ushort);
  started=Clock::now();status=raw.dcraw_process();result.demosaicAndColorMs=elapsed(started);
  if(status!=LIBRAW_SUCCESS){result.error=L"dcraw_process failed";return result;}
  int error=0;started=Clock::now();
  auto processed=raw.dcraw_make_mem_image(&error);
  std::unique_ptr<libraw_processed_image_t,decltype(&LibRaw::dcraw_clear_mem)>
   owner(processed,LibRaw::dcraw_clear_mem);
  if(!processed||error||processed->type!=LIBRAW_IMAGE_BITMAP||processed->colors<3||processed->bits!=8){
   result.error=L"RGB image failed";return result;
  }
  result.outputWidth=processed->width;result.outputHeight=processed->height;
  std::vector<uint8_t> bgra(size_t(result.outputWidth)*result.outputHeight*4);
  for(size_t i=0;i<size_t(result.outputWidth)*result.outputHeight;i++){
   bgra[i*4]=processed->data[i*processed->colors+2];
   bgra[i*4+1]=processed->data[i*processed->colors+1];
   bgra[i*4+2]=processed->data[i*processed->colors];bgra[i*4+3]=255;
  }
  result.rgbCopyMs=elapsed(started);result.success=true;return result;
 }catch(...){result.error=L"exception";return result;}
}

unsigned ScreenEdgeFor(unsigned viewportEdge){
 // Buckets, not exact pixels: dragging a window edge must not throw away
 // every screen-ready frame the cache is holding.
 static const unsigned buckets[]={1280,1600,2048,2560,3200,4096,5120};
 unsigned want=unsigned(viewportEdge*1.2f);
 for(unsigned b:buckets)if(b>=want)return b;
 return buckets[std::size(buckets)-1];
}

bool IsRawPath(const std::wstring& path){
 auto extension=std::filesystem::path(path).extension().wstring();
 for(auto& c:extension)c=towlower(c);
 return !extension.empty()&&
  std::wstring(L"|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|").find(L"|"+extension+L"|")!=std::wstring::npos;
}

std::shared_ptr<Image> DecodeRawHalf(const std::wstring& path,const std::function<bool()>& cancelled){
 if(!IsRawPath(path))return {};
 try{
  LibRaw raw;
  if(cancelled)raw.set_progress_handler([](void* state,enum LibRaw_progress,int,int){
   return (*static_cast<const std::function<bool()>*>(state))()?1:0;
  },(void*)&cancelled);
  if(raw.open_file(path.c_str())!=LIBRAW_SUCCESS)return {};
  // half_size drops the demosaic entirely: each output pixel comes from one
  // Bayer quad. At fit-to-window that is indistinguishable from the full
  // develop, and it is around five times faster.
  raw.imgdata.params.output_bps=8;
  raw.imgdata.params.use_camera_wb=1;
  raw.imgdata.params.half_size=1;
  raw.imgdata.params.user_qual=0;
  raw.imgdata.params.no_auto_bright=0;
  if(raw.unpack()!=LIBRAW_SUCCESS)return {};
  if(cancelled&&cancelled())return {};
  if(raw.dcraw_process()!=LIBRAW_SUCCESS)return {};
  if(cancelled&&cancelled())return {};
  int error=0;
  auto processed=raw.dcraw_make_mem_image(&error);
  std::unique_ptr<libraw_processed_image_t,decltype(&LibRaw::dcraw_clear_mem)>
   owner(processed,LibRaw::dcraw_clear_mem);
  if(!processed||error||processed->type!=LIBRAW_IMAGE_BITMAP||processed->colors<3||processed->bits!=8)return {};
  auto out=std::make_shared<Image>();
  out->w=processed->width;out->h=processed->height;
  if(!out->w||!out->h||uint64_t(out->w)*out->h>100000000ull)return {};
  out->pixels.resize(size_t(out->w)*out->h*4);
  for(size_t i=0;i<size_t(out->w)*out->h;i++){
   for(int c=0;c<3;c++)out->pixels[i*4+c]=processed->data[i*processed->colors+2-c];
   out->pixels[i*4+3]=255;
  }
  out->codec=L"LibRaw · half-size develop";
  // Half-size means exactly that: the asset is twice this in each direction.
  out->sourceW=out->w*2;out->sourceH=out->h*2;out->tier=TierScreenRes;
  // LibRaw reports dcraw flip codes, not EXIF orientation numbers.
  int flip=raw.imgdata.sizes.flip;
  return OrientPixels(out,unsigned(flip==3?3:flip==5?8:flip==6?6:1));
 }catch(...){return {};}
}

std::shared_ptr<Image> DecodeScreen(const std::wstring& path,unsigned edge,
                                    const std::function<bool()>& cancelled){
 // Step one for every format: whatever the file hands over cheaply. For RAW
 // that is the camera's embedded JPEG, for JPEG a DCT-scaled decode.
 auto quick=DecodeThumb(path,edge);
 if(quick&&(std::max)(quick->w,quick->h)>=edge){
  // DCT and embedded-preview dimensions are discrete. Keep a close decoder
  // frame, but do one worker-side box reduction when it carries >10% extra
  // edge (and therefore >21% extra upload bytes). The cache/GPU then hold the
  // render bucket, not whichever native size happened to be available.
  if((std::max)(quick->w,quick->h)>uint64_t(edge)*11/10)return Downsample(quick,edge);
  return quick;
 }
 if(cancelled&&cancelled())return quick;
 if(IsRawPath(path)){
  // The embedded preview is too small for this window — some bodies store a
  // 1600 px JPEG. A half-size develop is the next rung, still far short of a
  // full demosaic.
  if(auto half=DecodeRawHalf(path,cancelled)){
   if((std::max)(half->w,half->h)>edge)return Downsample(half,edge);
   return half;
  }
  return quick;
 }
 // A PSD thumbnail can be only 160 px wide. It is an excellent immediate
 // preview, but not a render-ready frame for a desktop viewport. Give native
 // screen decoders a chance to produce the next rung before accepting it.
 if(auto native=DecodeRenderReady(path,edge,cancelled))return native;
 if(quick)return quick;
 std::wstring error;
 auto full=Decode(path,error,cancelled);
 if(!full)return {};
 return Downsample(full,edge);
}
