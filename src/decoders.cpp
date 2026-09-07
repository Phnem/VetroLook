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
static std::shared_ptr<Image> Jpeg(const uint8_t* bytes,size_t size){
 auto tj=tj3Init(TJINIT_DECOMPRESS);Check(tj!=nullptr);
 struct Guard{tjhandle p;~Guard(){tj3Destroy(p);}} guard{tj};
 Check(tj3DecompressHeader(tj,bytes,size)==0);
 auto im=Allocate(tj3Get(tj,TJPARAM_JPEGWIDTH),tj3Get(tj,TJPARAM_JPEGHEIGHT));
 Check(tj3Decompress8(tj,bytes,size,im->pixels.data(),0,TJPF_BGRA)==0);im->codec=L"libjpeg-turbo";
 easyexif::EXIFInfo exif; exif.clear();
 if(size<=UINT_MAX&&exif.parseFrom(bytes,unsigned(size))==0&&exif.Orientation>=2&&exif.Orientation<=8){
  unsigned o=exif.Orientation;auto out=Allocate(o>=5?im->h:im->w,o>=5?im->w:im->h);out->codec=im->codec;
  for(unsigned y=0;y<im->h;y++)for(unsigned x=0;x<im->w;x++){
   unsigned dx=x,dy=y;
   switch(o){case 2:dx=im->w-1-x;break;case 3:dx=im->w-1-x;dy=im->h-1-y;break;case 4:dy=im->h-1-y;break;case 5:dx=y;dy=x;break;case 6:dx=im->h-1-y;dy=x;break;case 7:dx=im->h-1-y;dy=im->w-1-x;break;case 8:dx=y;dy=im->w-1-x;break;}
   memcpy(out->pixels.data()+(size_t(dy)*out->w+dx)*4,im->pixels.data()+(size_t(y)*im->w+x)*4,4);
  }im=out;
 }return im;
}
static std::wstring Extension(const std::wstring& path){auto e=std::filesystem::path(path).extension().wstring();std::transform(e.begin(),e.end(),e.begin(),towlower);return e;}
bool Supported(const std::wstring& path){auto e=Extension(path);return !e.empty()&&std::wstring(L"|.jpg|.jpeg|.jfif|.png|.gif|.webp|.bmp|.tif|.tiff|.ico|.heic|.heif|.avif|.exr|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|").find(L"|"+e+L"|")!=std::wstring::npos;}
std::shared_ptr<Image> Decode(const std::wstring& path,std::wstring& error){
 auto start=std::chrono::steady_clock::now();
 try{
 std::ifstream f(std::filesystem::path(path),std::ios::binary|std::ios::ate);Check(bool(f));auto length=f.tellg();Check(length>0&&length<=512LL*1024*1024);std::vector<uint8_t> data((size_t)length);f.seekg(0);Check(bool(f.read((char*)data.data(),length)));
 auto ext=Extension(path);std::shared_ptr<Image> im;
 if(data.size()>2&&data[0]==255&&data[1]==216)im=Jpeg(data.data(),data.size());
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
 }else if(std::wstring(L"|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|").find(L"|"+ext+L"|")!=std::wstring::npos){
  LibRaw raw;Check(raw.open_buffer(data.data(),data.size())==LIBRAW_SUCCESS);int code=raw.unpack_thumb();
  if(code==LIBRAW_SUCCESS){int err=0;auto p=raw.dcraw_make_mem_thumb(&err);std::unique_ptr<libraw_processed_image_t,decltype(&LibRaw::dcraw_clear_mem)> mem(p,LibRaw::dcraw_clear_mem);Check(p&&err==0);if(p->type==LIBRAW_IMAGE_JPEG) im=Jpeg(p->data,p->data_size);else if(p->type==LIBRAW_IMAGE_BITMAP&&p->colors>=3&&p->bits==8){im=Allocate(p->width,p->height);for(size_t i=0;i<size_t(im->w)*im->h;i++){for(int j=0;j<3;j++)im->pixels[i*4+j]=p->data[i*p->colors+2-j];im->pixels[i*4+3]=255;}}}
  if(!im){raw.imgdata.params.half_size=1;raw.imgdata.params.output_bps=8;raw.imgdata.params.use_camera_wb=1;Check(raw.unpack()==0&&raw.dcraw_process()==0);int err=0;auto p=raw.dcraw_make_mem_image(&err);std::unique_ptr<libraw_processed_image_t,decltype(&LibRaw::dcraw_clear_mem)>mem(p,LibRaw::dcraw_clear_mem);Check(p&&err==0&&p->colors>=3);im=Allocate(p->width,p->height);for(size_t i=0;i<size_t(im->w)*im->h;i++){for(int j=0;j<3;j++)im->pixels[i*4+j]=p->data[i*p->colors+2-j];im->pixels[i*4+3]=255;}im->codec=L"LibRaw · half-size decode";}else im->codec=L"LibRaw · embedded preview";
 }
 if(!im){ComPtr<IWICImagingFactory> factory;Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))));ComPtr<IWICStream> stream;Check(SUCCEEDED(factory->CreateStream(&stream)));Check(SUCCEEDED(stream->InitializeFromMemory(data.data(),DWORD(data.size()))));ComPtr<IWICBitmapDecoder> dec;Check(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&dec)));ComPtr<IWICBitmapFrameDecode> frame;Check(SUCCEEDED(dec->GetFrame(0,&frame)));unsigned w,h;Check(SUCCEEDED(frame->GetSize(&w,&h)));im=Allocate(w,h);ComPtr<IWICFormatConverter> cv;Check(SUCCEEDED(factory->CreateFormatConverter(&cv)));Check(SUCCEEDED(cv->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)));Check(SUCCEEDED(cv->CopyPixels(nullptr,w*4,UINT(im->pixels.size()),im->pixels.data())));im->codec=L"Windows WIC";}
 for(size_t i=3;i<im->pixels.size();i+=4)if(im->pixels[i]<255){im->hasAlpha=true;break;}
 im->ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return im;
 }catch(...){error=L"This image could not be opened. The file may be damaged or its codec unavailable.";return {};}
}

