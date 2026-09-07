// Vetro Look, GPL-3.0-or-later.
// Filmstrip thumbnails. A separate lightweight cache: the gallery never holds a
// full-resolution bitmap alive just to show a 74 pixel strip.
#include "ui.h"
#include <wincodec.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
using Microsoft::WRL::ComPtr;

namespace{
constexpr unsigned ThumbEdge=180;
std::mutex mx;std::condition_variable cv;
std::deque<std::wstring> queue;
std::set<std::wstring> queued;
std::map<std::wstring,std::shared_ptr<Image>> cache;
bool stopping=false,started=false;
std::thread worker;HWND notifyWindow=nullptr;UINT notifyMessage=0;

std::shared_ptr<Image> ViaWic(const std::wstring& path){
 ComPtr<IWICImagingFactory> factory;
 if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return {};
 ComPtr<IWICBitmapDecoder> decoder;
 if(FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder)))return {};
 ComPtr<IWICBitmapFrameDecode> frame;
 if(FAILED(decoder->GetFrame(0,&frame)))return {};
 unsigned w=0,h=0;
 if(FAILED(frame->GetSize(&w,&h))||!w||!h)return {};
 unsigned short orientation=1;
 ComPtr<IWICMetadataQueryReader> reader;
 if(SUCCEEDED(frame->GetMetadataQueryReader(&reader))){
  PROPVARIANT value;PropVariantInit(&value);
  if(SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=274}",&value))&&value.vt==VT_UI2)orientation=value.uiVal;
  PropVariantClear(&value);
 }
 double scale=double(ThumbEdge)/double((std::max)(w,h));
 unsigned tw=(std::max)(1u,unsigned(w*(std::min)(1.,scale))),th=(std::max)(1u,unsigned(h*(std::min)(1.,scale)));
 ComPtr<IWICBitmapScaler> scaler;
 if(FAILED(factory->CreateBitmapScaler(&scaler))||FAILED(scaler->Initialize(frame.Get(),tw,th,WICBitmapInterpolationModeFant)))return {};
 ComPtr<IWICBitmapSource> source=scaler;
 if(orientation>=2&&orientation<=8){
  static const WICBitmapTransformOptions map[]={WICBitmapTransformRotate0,WICBitmapTransformRotate0,
   WICBitmapTransformFlipHorizontal,WICBitmapTransformRotate180,WICBitmapTransformFlipVertical,
   (WICBitmapTransformOptions)(WICBitmapTransformRotate90|WICBitmapTransformFlipHorizontal),
   WICBitmapTransformRotate90,
   (WICBitmapTransformOptions)(WICBitmapTransformRotate270|WICBitmapTransformFlipHorizontal),
   WICBitmapTransformRotate270};
  ComPtr<IWICBitmapFlipRotator> rotator;
  if(SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator))&&SUCCEEDED(rotator->Initialize(scaler.Get(),map[orientation])))source=rotator;
 }
 ComPtr<IWICFormatConverter> converter;
 if(FAILED(factory->CreateFormatConverter(&converter)))return {};
 if(FAILED(converter->Initialize(source.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
 unsigned fw=0,fh=0;
 if(FAILED(converter->GetSize(&fw,&fh))||!fw||!fh)return {};
 auto image=std::make_shared<Image>();
 image->w=fw;image->h=fh;image->pixels.resize(size_t(fw)*fh*4);
 if(FAILED(converter->CopyPixels(nullptr,fw*4,UINT(image->pixels.size()),image->pixels.data())))return {};
 return image;
}
std::shared_ptr<Image> Shrink(const std::shared_ptr<Image>& src){
 if(!src||!src->w||!src->h)return {};
 unsigned longEdge=(std::max)(src->w,src->h);
 if(longEdge<=ThumbEdge)return src;
 unsigned factor=(longEdge+ThumbEdge-1)/ThumbEdge;
 auto out=std::make_shared<Image>();
 out->w=(std::max)(1u,src->w/factor);out->h=(std::max)(1u,src->h/factor);
 out->pixels.resize(size_t(out->w)*out->h*4);
 for(unsigned y=0;y<out->h;y++)for(unsigned x=0;x<out->w;x++){
  unsigned sum[4]={0,0,0,0},n=0;
  for(unsigned dy=0;dy<factor;dy++)for(unsigned dx=0;dx<factor;dx++){
   unsigned sx=x*factor+dx,sy=y*factor+dy;
   if(sx>=src->w||sy>=src->h)continue;
   const uint8_t* p=&src->pixels[((size_t)sy*src->w+sx)*4];
   for(int c=0;c<4;c++)sum[c]+=p[c];
   n++;
  }
  uint8_t* d=&out->pixels[((size_t)y*out->w+x)*4];
  for(int c=0;c<4;c++)d[c]=n?uint8_t(sum[c]/n):0;
 }
 return out;
}
void Run(){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 while(true){
  std::wstring path;
  {
   std::unique_lock lock(mx);
   cv.wait(lock,[]{return stopping||!queue.empty();});
   if(stopping)break;
   path=std::move(queue.back());queue.pop_back();queued.erase(path);
  }
  std::shared_ptr<Image> thumb=ViaWic(path);
  if(!thumb){std::wstring error;thumb=Shrink(Decode(path,error));}
  if(!thumb)continue;
  {std::lock_guard lock(mx);cache[path]=thumb;}
  if(notifyWindow)PostMessageW(notifyWindow,notifyMessage,0,0);
 }
 CoUninitialize();
}
}

void ThumbStart(HWND notify,UINT message){
 notifyWindow=notify;notifyMessage=message;
 std::lock_guard lock(mx);
 if(!started){started=true;worker=std::thread(Run);}
}
void ThumbStop(){
 {std::lock_guard lock(mx);stopping=true;}
 cv.notify_all();
 if(worker.joinable())worker.join();
 cache.clear();
}
void ThumbRequest(const std::wstring& path){
 {
  std::lock_guard lock(mx);
  if(cache.count(path)||queued.count(path))return;
  queued.insert(path);queue.push_back(path);
  if(queue.size()>512){queued.erase(queue.front());queue.pop_front();}
 }
 cv.notify_one();
}
std::shared_ptr<Image> ThumbLookup(const std::wstring& path){
 std::lock_guard lock(mx);
 auto found=cache.find(path);
 return found==cache.end()?std::shared_ptr<Image>():found->second;
}
void ThumbTrim(const std::vector<std::wstring>& keep){
 std::set<std::wstring> wanted(keep.begin(),keep.end());
 std::lock_guard lock(mx);
 if(cache.size()<=wanted.size()+64)return;
 for(auto it=cache.begin();it!=cache.end();){
  if(wanted.count(it->first))++it;else it=cache.erase(it);
 }
}
