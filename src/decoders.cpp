#include "image.h"
#include <wrl/client.h>
#include <wincodec.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <libraw/libraw.h>
#include <tinyexr.h>
#include <avif/avif.h>
#include "exif.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <chrono>
using Microsoft::WRL::ComPtr;
static void Check(bool ok){if(!ok) throw std::runtime_error("decode");}
static std::shared_ptr<Image> Allocate(unsigned w,unsigned h){Check(w&&h&&uint64_t(w)*h<=100000000);auto im=std::make_shared<Image>();im->w=w;im->h=h;im->pixels.resize(size_t(w)*h*4);return im;}
static void Premultiply(Image& im){for(size_t i=0;i<im.pixels.size();i+=4){unsigned a=im.pixels[i+3];for(int j=0;j<3;j++)im.pixels[i+j]=uint8_t((im.pixels[i+j]*a+127)/255);}}
// EXIF orientation, applied a row (or a cache-friendly tile) at a time. A
// 36 megapixel preview goes through here on every RAW open, so the naive
// per-pixel memcpy this replaced cost more than the JPEG decode itself.
static std::shared_ptr<Image> Orient(const std::shared_ptr<Image>& src,unsigned o){
 if(o<2||o>8)return src;
 unsigned w=src->w,h=src->h;bool transpose=o>=5;
 auto out=Allocate(transpose?h:w,transpose?w:h);out->codec=src->codec;
 out->tier=src->tier;
 out->sourceW=transpose?src->SourceH():src->SourceW();
 out->sourceH=transpose?src->SourceW():src->SourceH();
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
// maxEdge asks the JPEG decoder for the smallest DCT scaling that still covers
// that long edge; a filmstrip thumbnail never pays for a full-size decode.
static std::shared_ptr<Image> Jpeg(const uint8_t* bytes,size_t size,unsigned maxEdge=0){
 auto tj=tj3Init(TJINIT_DECOMPRESS);Check(tj!=nullptr);
 struct Guard{tjhandle p;~Guard(){tj3Destroy(p);}} guard{tj};
 Check(tj3DecompressHeader(tj,bytes,size)==0);
 int fullW=tj3Get(tj,TJPARAM_JPEGWIDTH),fullH=tj3Get(tj,TJPARAM_JPEGHEIGHT);
 int w=fullW,h=fullH;
 if(maxEdge&&unsigned((std::max)(fullW,fullH))>maxEdge){
  int count=0;auto factors=tj3GetScalingFactors(&count);
  for(int i=0;i<count;i++){
   int sw=TJSCALED(fullW,factors[i]),sh=TJSCALED(fullH,factors[i]);
   if(unsigned((std::max)(sw,sh))<maxEdge||int64_t(sw)*sh>=int64_t(w)*h)continue;
   if(tj3SetScalingFactor(tj,factors[i])==0){w=sw;h=sh;}
  }
 }
 auto im=Allocate(w,h);
 Check(tj3Decompress8(tj,bytes,size,im->pixels.data(),0,TJPF_BGRA)==0);im->codec=L"libjpeg-turbo";
 // The asset is the full JPEG, whatever DCT scaling this decode used.
 im->sourceW=unsigned(fullW);im->sourceH=unsigned(fullH);
 im->tier=(w<fullW||h<fullH)?TierScreenRes:TierFullRes;
 easyexif::EXIFInfo exif; exif.clear();
 if(size<=UINT_MAX&&exif.parseFrom(bytes,unsigned(size))==0)im=Orient(im,exif.Orientation);
 return im;
}
static std::wstring Extension(const std::wstring& path){auto e=std::filesystem::path(path).extension().wstring();std::transform(e.begin(),e.end(),e.begin(),towlower);return e;}
static constexpr const wchar_t* RawExtensions=L"|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|";
static bool IsRaw(const std::wstring& ext){return !ext.empty()&&std::wstring(RawExtensions).find(L"|"+ext+L"|")!=std::wstring::npos;}
bool Supported(const std::wstring& path){auto e=Extension(path);return !e.empty()&&std::wstring(L"|.jpg|.jpeg|.jfif|.png|.gif|.webp|.bmp|.tif|.tiff|.ico|.heic|.heif|.avif|.exr|.psd|.psb|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|").find(L"|"+e+L"|")!=std::wstring::npos;}
// A camera's own embedded preview, decoded straight from the file. The
// filmstrip must never demosaic a 36 megapixel frame to fill a 180 pixel tile,
// and must never read an 80 MB RAW into memory to do it.
static std::shared_ptr<Image> FromRawThumb(const std::wstring& path,unsigned maxEdge){
 LibRaw raw;
 if(raw.open_file(path.c_str())!=LIBRAW_SUCCESS)return {};
 // Modern RAW containers can advertise several previews. Pick the smallest
 // known preview that covers the requested edge, or the largest available
 // one when none does. LibRaw remains responsible for all container parsing.
 int selected=0;
 uint64_t selectedArea=0;bool selectedCovers=false;
 int count=(std::min)(raw.imgdata.thumbs_list.thumbcount,int(LIBRAW_THUMBNAIL_MAXCOUNT));
 for(int i=0;i<count;i++){
  const auto& item=raw.imgdata.thumbs_list.thumblist[i];
  unsigned longEdge=(std::max)(unsigned(item.twidth),unsigned(item.theight));
  uint64_t area=uint64_t(item.twidth)*item.theight;
  if(!area)continue;
  bool covers=!maxEdge||longEdge>=maxEdge;
  if((covers&&!selectedCovers)||(covers==selectedCovers&&
     ((covers&&(!selectedArea||area<selectedArea))||(!covers&&area>selectedArea)))){
   selected=i;selectedArea=area;selectedCovers=covers;
  }
 }
 if(raw.unpack_thumb_ex(selected)!=LIBRAW_SUCCESS)return {};
 int err=0;auto p=raw.dcraw_make_mem_thumb(&err);
 std::unique_ptr<libraw_processed_image_t,decltype(&LibRaw::dcraw_clear_mem)> mem(p,LibRaw::dcraw_clear_mem);
 if(!p||err)return {};
 // The embedded JPEG is a preview of the sensor frame, not an asset of its
 // own: the asset is what LibRaw would develop.
 unsigned assetW=raw.imgdata.sizes.iwidth?raw.imgdata.sizes.iwidth:raw.imgdata.sizes.width;
 unsigned assetH=raw.imgdata.sizes.iheight?raw.imgdata.sizes.iheight:raw.imgdata.sizes.height;
 int flip=raw.imgdata.sizes.flip;
 unsigned orientation=unsigned(flip==3?3:flip==5?8:flip==6?6:1);
 if(orientation>=5)std::swap(assetW,assetH);
 auto tag=[&](std::shared_ptr<Image> frame){
  if(frame&&assetW&&assetH){
   frame->sourceW=assetW;frame->sourceH=assetH;
   frame->tier=(frame->w<assetW||frame->h<assetH)?TierScreenRes:TierFullRes;
  }
  return frame;
 };
 if(p->type==LIBRAW_IMAGE_JPEG){
  auto frame=tag(Jpeg(p->data,p->data_size,maxEdge));
  if(frame)frame->codec=L"LibRaw · best embedded JPEG preview";
  return frame;
 }
 if(p->type!=LIBRAW_IMAGE_BITMAP||p->colors<3||p->bits!=8)return {};
 auto im=Allocate(p->width,p->height);
 for(size_t i=0;i<size_t(im->w)*im->h;i++){
  for(int j=0;j<3;j++)im->pixels[i*4+j]=p->data[i*p->colors+2-j];
  im->pixels[i*4+3]=255;
 }
 // LibRaw keeps dcraw's own flip codes, not EXIF orientation numbers.
 im->codec=L"LibRaw · best embedded bitmap preview";
 return tag(Orient(im,orientation));
}
static std::vector<uint8_t> ReadPrefix(const std::wstring& path,size_t limit){
 std::ifstream f(std::filesystem::path(path),std::ios::binary|std::ios::ate);
 if(!f)return {};
 auto length=f.tellg();
 if(length<=0)return {};
 std::vector<uint8_t> data((std::min)(size_t(length),limit));
 f.seekg(0);
 if(!f.read((char*)data.data(),std::streamsize(data.size())))return {};
 return data;
}
static void TargetDimensions(unsigned sourceW,unsigned sourceH,unsigned edge,
                             unsigned& targetW,unsigned& targetH){
 targetW=sourceW;targetH=sourceH;
 unsigned longEdge=(std::max)(sourceW,sourceH);
 if(!edge||longEdge<=edge)return;
 targetW=(std::max)(1u,unsigned((uint64_t(sourceW)*edge+longEdge/2)/longEdge));
 targetH=(std::max)(1u,unsigned((uint64_t(sourceH)*edge+longEdge/2)/longEdge));
}
static std::vector<uint8_t> ReadAll(const std::wstring& path){return ReadPrefix(path,512u*1024*1024);}

static std::shared_ptr<Image> WicRenderReady(const std::vector<uint8_t>& data,
                                             const std::wstring& ext,unsigned edge){
 ComPtr<IWICImagingFactory> factory;
 Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))));
 ComPtr<IWICStream> stream;Check(SUCCEEDED(factory->CreateStream(&stream)));
 Check(data.size()<=DWORD_MAX&&SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()),DWORD(data.size()))));
 ComPtr<IWICBitmapDecoder> decoder;
 Check(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder)));

 ComPtr<IWICBitmapFrameDecode> first;Check(SUCCEEDED(decoder->GetFrame(0,&first)));
 unsigned sourceW=0,sourceH=0;Check(SUCCEEDED(first->GetSize(&sourceW,&sourceH)));
 unsigned wantedW=0,wantedH=0;TargetDimensions(sourceW,sourceH,edge,wantedW,wantedH);
 if(wantedW==sourceW&&wantedH==sourceH)return {};

 // Multi-resolution TIFFs commonly store reduced IFDs as additional frames.
 // Start with the smallest level that still covers the viewport; WIC can then
 // do only the small residual scale. Other formats keep their first frame.
 ComPtr<IWICBitmapFrameDecode> frame=first;
 unsigned frameW=sourceW,frameH=sourceH;
 if(ext==L".tif"||ext==L".tiff"){
  UINT count=1;if(SUCCEEDED(decoder->GetFrameCount(&count))){
   uint64_t bestArea=uint64_t(frameW)*frameH;
   for(UINT index=1;index<count;index++){
    ComPtr<IWICBitmapFrameDecode> candidate;
    unsigned cw=0,ch=0;
    if(FAILED(decoder->GetFrame(index,&candidate))||FAILED(candidate->GetSize(&cw,&ch)))continue;
    if((std::max)(cw,ch)<edge)continue;
    uint64_t area=uint64_t(cw)*ch;
    if(area<bestArea){frame=candidate;frameW=cw;frameH=ch;bestArea=area;}
   }
  }
 }

 // Prefer the codec's own transform. JPEG/HEIF/TIFF codecs may select a
 // reduced representation here, avoiding full RGB conversion altogether.
 ComPtr<IWICBitmapSourceTransform> transform;
 if(SUCCEEDED(frame.As(&transform))){
  UINT tw=wantedW,th=wantedH;
  WICPixelFormatGUID format=GUID_WICPixelFormat32bppPBGRA;
  if(SUCCEEDED(transform->GetClosestSize(&tw,&th))&&tw&&th&&
     (std::max)(tw,th)>=edge&&(std::max)(tw,th)<=uint64_t(edge)*11/10&&
     SUCCEEDED(transform->GetClosestPixelFormat(&format))&&
     IsEqualGUID(format,GUID_WICPixelFormat32bppPBGRA)&&
     uint64_t(tw)*th<=100000000ull){
   auto out=Allocate(tw,th);
   if(SUCCEEDED(transform->CopyPixels(nullptr,tw,th,&format,WICBitmapTransformRotate0,
                                      tw*4,UINT(out->pixels.size()),out->pixels.data()))){
    out->sourceW=sourceW;out->sourceH=sourceH;out->tier=TierScreenRes;
    out->codec=L"Windows WIC source transform";
    return out;
   }
  }
 }

 // The fallback still writes only the viewport-sized BGRA destination. It
 // deliberately does not materialise a full Image followed by Downsample().
 ComPtr<IWICBitmapScaler> scaler;Check(SUCCEEDED(factory->CreateBitmapScaler(&scaler)));
 Check(SUCCEEDED(scaler->Initialize(frame.Get(),wantedW,wantedH,WICBitmapInterpolationModeFant)));
 ComPtr<IWICFormatConverter> converter;Check(SUCCEEDED(factory->CreateFormatConverter(&converter)));
 Check(SUCCEEDED(converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)));
 auto out=Allocate(wantedW,wantedH);
 Check(SUCCEEDED(converter->CopyPixels(nullptr,wantedW*4,UINT(out->pixels.size()),out->pixels.data())));
 out->sourceW=sourceW;out->sourceH=sourceH;out->tier=TierScreenRes;
 out->codec=(frameW!=sourceW||frameH!=sourceH)?L"Windows WIC reduced frame + scaler":L"Windows WIC scaler";
 return out;
}

std::shared_ptr<Image> DecodeRenderReady(const std::wstring& path,unsigned maxEdge,
                                         const std::function<bool()>& cancelled){
 try{
  auto ext=Extension(path);
  if(!maxEdge||IsRaw(ext)||ext==L".jpg"||ext==L".jpeg"||ext==L".jfif")return {};
  auto data=ReadAll(path);if(data.empty()||(cancelled&&cancelled()))return {};
  if(ext==L".psd"||ext==L".psb"){
   std::wstring error;
   auto out=DecodePsdScreen(data.data(),data.size(),maxEdge,&error);
   if(cancelled&&cancelled())return {};
   return out;
  }
  if(ext==L".webp"){
   WebPDecoderConfig config{};Check(WebPInitDecoderConfig(&config)!=0);
   Check(WebPGetFeatures(data.data(),data.size(),&config.input)==VP8_STATUS_OK);
   if(config.input.has_animation)return {};
   unsigned targetW=0,targetH=0;
   TargetDimensions(config.input.width,config.input.height,maxEdge,targetW,targetH);
   if(targetW==unsigned(config.input.width)&&targetH==unsigned(config.input.height))return {};
   config.output.colorspace=MODE_BGRA;
   config.options.use_threads=1;config.options.use_scaling=1;
   config.options.scaled_width=int(targetW);config.options.scaled_height=int(targetH);
   struct Guard{WebPDecBuffer* p;~Guard(){WebPFreeDecBuffer(p);}}guard{&config.output};
   Check(WebPDecode(data.data(),data.size(),&config)==VP8_STATUS_OK);
   auto out=Allocate(targetW,targetH);
   for(unsigned y=0;y<targetH;y++)memcpy(out->pixels.data()+size_t(y)*targetW*4,
                                         config.output.u.RGBA.rgba+size_t(y)*config.output.u.RGBA.stride,
                                         size_t(targetW)*4);
   if(config.input.has_alpha){Premultiply(*out);out->hasAlpha=true;}
   out->sourceW=config.input.width;out->sourceH=config.input.height;out->tier=TierScreenRes;
   out->codec=L"libwebp scaled decode";return out;
  }
  if(ext==L".avif"){
   // This build's optional libavif scaler path is not production-safe here,
   // including after copying the transient dav1d planes. The system WIC path
   // was therefore measured as the other available decoder-side option.
   // The system WIC decoder is much faster on the local fixture, but differs
   // from the trusted libavif output by 5.25 mean code values (45 max). Keep
   // the measured libavif full-YUV/RGB + worker reduction fallback until the
   // bundled scaler is available and passes fidelity; speed alone is not a
   // sufficient reason to change colour output.
   return {};
  }
  if(ext==L".png"||ext==L".gif"||ext==L".bmp"||ext==L".ico"||
     ext==L".tif"||ext==L".tiff"||ext==L".heic"||ext==L".heif")
   return WicRenderReady(data,ext,maxEdge);
  return {};
 }catch(...){return {};}
}
std::shared_ptr<Image> DecodeThumb(const std::wstring& path,unsigned maxEdge){
 try{
  auto ext=Extension(path);
  if(IsRaw(ext))return FromRawThumb(path,maxEdge);
  if(ext==L".jpg"||ext==L".jpeg"||ext==L".jfif"){
   auto data=ReadPrefix(path,512u*1024*1024);
   return data.empty()?std::shared_ptr<Image>():Jpeg(data.data(),data.size(),maxEdge);
  }
  if(ext==L".psd"||ext==L".psb"){
   // Photoshop's own thumbnail sits in the resource block near the head of the
   // file, so a filmstrip tile never has to read, let alone flatten, the rest.
   auto data=ReadPrefix(path,8u*1024*1024);
   if(data.empty())return {};
   const uint8_t* preview=nullptr;size_t previewSize=0;
   DecodePsd(data.data(),data.size(),&preview,&previewSize);
   auto frame=preview?Jpeg(preview,previewSize,maxEdge):std::shared_ptr<Image>();
   // The embedded JPEG's header describes the preview, not the Photoshop
   // asset. Preserve the source dimensions from the cheap fixed PSD header.
   if(frame&&data.size()>=26&&data[0]=='8'&&data[1]=='B'&&data[2]=='P'&&data[3]=='S'){
    auto be32=[&](size_t at){return uint32_t(data[at])<<24|uint32_t(data[at+1])<<16|
                                  uint32_t(data[at+2])<<8|data[at+3];};
    unsigned sourceH=be32(14),sourceW=be32(18);
    if(sourceW&&sourceH){frame->sourceW=sourceW;frame->sourceH=sourceH;frame->tier=TierThumbRes;}
   }
   return frame;
  }
  return {};
 }catch(...){return {};}
}
std::shared_ptr<Image> Decode(const std::wstring& path,std::wstring& error,const std::function<bool()>& cancelled){
 auto start=std::chrono::steady_clock::now();
 try{
 auto ext=Extension(path);std::shared_ptr<Image> im;
 // Decode() is the second, full-quality stage for RAW.  The first stage is
 // DecodeThumb(); returning the embedded thumbnail here as well would only
 // pretend that a full-resolution replacement had happened.
 if(IsRaw(ext)){
  LibRaw raw;
  if(cancelled)raw.set_progress_handler([](void* state,enum LibRaw_progress,int,int){
   return (*static_cast<const std::function<bool()>*>(state))()?1:0;
  },(void*)&cancelled);
  Check(raw.open_file(path.c_str())==LIBRAW_SUCCESS);
  raw.imgdata.params.output_bps=8;raw.imgdata.params.use_camera_wb=1;raw.imgdata.params.half_size=0;
  Check(raw.unpack()==LIBRAW_SUCCESS);
  if(cancelled&&cancelled())throw std::runtime_error("cancelled");
  Check(raw.dcraw_process()==LIBRAW_SUCCESS);
  if(cancelled&&cancelled())throw std::runtime_error("cancelled");
  int err=0;auto p=raw.dcraw_make_mem_image(&err);
  std::unique_ptr<libraw_processed_image_t,decltype(&LibRaw::dcraw_clear_mem)> mem(p,LibRaw::dcraw_clear_mem);
  Check(p&&err==LIBRAW_SUCCESS&&p->type==LIBRAW_IMAGE_BITMAP&&p->colors>=3&&p->bits==8);
  if(cancelled&&cancelled())throw std::runtime_error("cancelled");
  im=Allocate(p->width,p->height);
  for(unsigned y=0;y<im->h;y++){
   if((y&31)==0&&cancelled&&cancelled())throw std::runtime_error("cancelled");
   for(unsigned x=0;x<im->w;x++){
    size_t i=size_t(y)*im->w+x;
    for(int j=0;j<3;j++)im->pixels[i*4+j]=p->data[i*p->colors+2-j];
    im->pixels[i*4+3]=255;
   }
  }
  im->codec=L"LibRaw · full-resolution demosaic";
 }else{
 std::ifstream f(std::filesystem::path(path),std::ios::binary|std::ios::ate);Check(bool(f));auto length=f.tellg();Check(length>0&&length<=512LL*1024*1024);std::vector<uint8_t> data((size_t)length);f.seekg(0);Check(bool(f.read((char*)data.data(),length)));
 // Photoshop documents are recognised by signature: they are routinely handed
 // around with the wrong extension, and Windows has no PSD codec to fall back on.
 if(data.size()>4&&!memcmp(data.data(),"8BPS",4)){
  const uint8_t* preview=nullptr;size_t previewSize=0;
  std::wstring psdError;im=DecodePsd(data.data(),data.size(),&preview,&previewSize,&psdError);
  if(!im&&preview){im=Jpeg(preview,previewSize);im->codec=L"PSD embedded preview";}
  if(!im){error=psdError.empty()?L"This Photoshop file carries no flattened image. Re-save it with \"Maximize compatibility\" turned on.":psdError;return {};}
 }
 else if(data.size()>2&&data[0]==255&&data[1]==216)im=Jpeg(data.data(),data.size());
 else if(ext==L".png"||ext==L".gif")im=DecodeWuffs(data);
 else if(ext==L".avif"){
 auto dec=avifDecoderCreate();Check(dec!=nullptr);struct G{avifDecoder*p;~G(){avifDecoderDestroy(p);}}g{dec};dec->codecChoice=AVIF_CODEC_CHOICE_DAV1D;dec->maxThreads=4;dec->imageSizeLimit=100000000;
 Check(avifDecoderSetIOMemory(dec,data.data(),data.size())==AVIF_RESULT_OK&&avifDecoderParse(dec)==AVIF_RESULT_OK&&avifDecoderNextImage(dec)==AVIF_RESULT_OK);
 im=Allocate(dec->image->width,dec->image->height);avifRGBImage rgb;avifRGBImageSetDefaults(&rgb,dec->image);rgb.depth=8;rgb.format=AVIF_RGB_FORMAT_BGRA;rgb.pixels=im->pixels.data();rgb.rowBytes=im->w*4;rgb.alphaPremultiplied=AVIF_TRUE;Check(avifImageYUVToRGB(dec->image,&rgb)==AVIF_RESULT_OK);im->codec=L"libavif + dav1d";
 }else if(ext==L".webp"){
  WebPData wd{data.data(),data.size()};WebPAnimDecoderOptions opt;WebPAnimDecoderOptionsInit(&opt);opt.color_mode=MODE_BGRA;opt.use_threads=1;
  auto dec=WebPAnimDecoderNew(&wd,&opt);Check(dec!=nullptr);struct G{WebPAnimDecoder*p;~G(){WebPAnimDecoderDelete(p);}}g{dec};WebPAnimInfo info{};Check(WebPAnimDecoderGetInfo(dec,&info));im=Allocate(info.canvas_width,info.canvas_height);uint8_t* frame;int time;Check(WebPAnimDecoderGetNext(dec,&frame,&time));memcpy(im->pixels.data(),frame,im->pixels.size());Premultiply(*im);im->codec=L"libwebp";
 }else if(ext==L".exr"){
  float* rgba=nullptr;int w=0,h=0;const char* err=nullptr;int ret=LoadEXRFromMemory(&rgba,&w,&h,data.data(),data.size(),&err);if(err)FreeEXRErrorMessage(err);std::unique_ptr<float,decltype(&free)> owner(rgba,free);Check(ret==0&&rgba);im=Allocate(w,h);
  for(size_t i=0;i<size_t(w)*h;i++){for(int j=0;j<3;j++){float v=rgba[i*4+2-j];v=std::isfinite(v)?std::max(0.f,v):0;v=v/(1+v);im->pixels[i*4+j]=uint8_t(std::clamp(powf(v,1/2.2f)*255,0.f,255.f));}float a=rgba[i*4+3];im->pixels[i*4+3]=uint8_t(std::isfinite(a)?std::clamp(a*255,0.f,255.f):255);}Premultiply(*im);im->codec=L"TinyEXR · SDR tone map";
 }
 if(!im){ComPtr<IWICImagingFactory> factory;Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))));ComPtr<IWICStream> stream;Check(SUCCEEDED(factory->CreateStream(&stream)));Check(SUCCEEDED(stream->InitializeFromMemory(data.data(),DWORD(data.size()))));ComPtr<IWICBitmapDecoder> dec;Check(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&dec)));ComPtr<IWICBitmapFrameDecode> frame;Check(SUCCEEDED(dec->GetFrame(0,&frame)));unsigned w,h;Check(SUCCEEDED(frame->GetSize(&w,&h)));im=Allocate(w,h);ComPtr<IWICFormatConverter> cv;Check(SUCCEEDED(factory->CreateFormatConverter(&cv)));Check(SUCCEEDED(cv->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)));Check(SUCCEEDED(cv->CopyPixels(nullptr,w*4,UINT(im->pixels.size()),im->pixels.data())));im->codec=L"Windows WIC";}
 }
 for(size_t i=3;i<im->pixels.size();i+=4)if(im->pixels[i]<255){im->hasAlpha=true;break;}
 im->ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return im;
 }catch(...){error=L"This image could not be opened. The file may be damaged or its codec unavailable.";return {};}
}
