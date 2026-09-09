// Vetro Look, GPL-3.0-or-later.
// Filmstrip thumbnails. A separate lightweight cache: the gallery never holds a
// full-resolution bitmap alive just to show a 74 pixel strip.
#include "ui.h"
#include "pipeline.h"
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
// A failed tile is remembered, not retried on every repaint, but the memory
// fades: a file locked by another program should appear once it is released.
constexpr ULONGLONG RetryDelay=4000,Revalidate=1500;
constexpr unsigned MaxAttempts=3;
constexpr size_t QueueCap=512;
enum State{Queued,Loading,Ready,Failed};
struct Entry{
 State state=Queued;
 std::shared_ptr<Image> image;
 unsigned attempts=0;
 ULONGLONG retryAt=0,checked=0;
 uint64_t size=0,written=0;
};
std::mutex mx;std::condition_variable cv;
std::deque<std::wstring> queue;
std::map<std::wstring,Entry> entries;
bool stopping=false,started=false;
std::vector<std::thread> workers;HWND notifyWindow=nullptr;UINT notifyMessage=0;

bool Stamp(const std::wstring& path,uint64_t& size,uint64_t& written){
 WIN32_FILE_ATTRIBUTE_DATA info{};
 if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info))return false;
 size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
 written=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
 return true;
}

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
 ComPtr<IWICBitmapSource> source;
 // Ask the decoder to produce the small frame itself where it can. A JPEG or
 // JPEG-XR codec that supports this never expands the full image at all,
 // which is the whole cost of a thumbnail; a scaler bolted on afterwards has
 // to decode 36 megapixels first and then throw them away.
 {
  ComPtr<IWICBitmapSourceTransform> transform;
  if(SUCCEEDED(frame.As(&transform))){
   UINT nativeW=tw,nativeH=th;
   if(SUCCEEDED(transform->GetClosestSize(&nativeW,&nativeH))&&nativeW&&nativeH&&
      (nativeW<w||nativeH<h)){
    ComPtr<IWICBitmapScaler> native;
    if(SUCCEEDED(factory->CreateBitmapScaler(&native))&&
       SUCCEEDED(native->Initialize(frame.Get(),nativeW,nativeH,WICBitmapInterpolationModeFant)))
     source=native;
   }
  }
 }
 if(!source){
  ComPtr<IWICBitmapScaler> scaler;
  if(FAILED(factory->CreateBitmapScaler(&scaler))||
     FAILED(scaler->Initialize(frame.Get(),tw,th,WICBitmapInterpolationModeFant)))return {};
  source=scaler;
 }
 if(orientation>=2&&orientation<=8){
  static const WICBitmapTransformOptions map[]={WICBitmapTransformRotate0,WICBitmapTransformRotate0,
   WICBitmapTransformFlipHorizontal,WICBitmapTransformRotate180,WICBitmapTransformFlipVertical,
   (WICBitmapTransformOptions)(WICBitmapTransformRotate90|WICBitmapTransformFlipHorizontal),
   WICBitmapTransformRotate90,
   (WICBitmapTransformOptions)(WICBitmapTransformRotate270|WICBitmapTransformFlipHorizontal),
   WICBitmapTransformRotate270};
  ComPtr<IWICBitmapFlipRotator> rotator;
  if(SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator))&&SUCCEEDED(rotator->Initialize(source.Get(),map[orientation])))source=rotator;
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
 // Downsample() returns `src` itself when it is already small enough, so a
 // decoder that produced the right size is not resized a second time.
 return Downsample(src,ThumbEdge);
}
// A camera preview or a scaled JPEG first; WIC for the formats it handles well;
// only then a full decode. A folder of RAW files used to reach that last line
// for every tile, which is what left the filmstrip black.
std::shared_ptr<Image> Load(const std::wstring& path){
 if(auto quick=DecodeThumb(path,ThumbEdge))return Shrink(quick);
 if(auto wic=ViaWic(path))return wic;
 std::wstring error;return Shrink(Decode(path,error));
}
void Run(){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 while(true){
  std::wstring path;
  {
   std::unique_lock lock(mx);
   cv.wait(lock,[]{return stopping||!queue.empty();});
   if(stopping)break;
   path=std::move(queue.back());queue.pop_back();
   auto it=entries.find(path);
   if(it==entries.end()||it->second.state!=Queued)continue;
   it->second.state=Loading;
  }
  auto thumb=Load(path);
  uint64_t size=0,written=0;Stamp(path,size,written);
  {
   std::lock_guard lock(mx);
   auto it=entries.find(path);
   if(it==entries.end())continue;
   auto& entry=it->second;
   entry.checked=GetTickCount64();
   if(thumb){entry.state=Ready;entry.image=thumb;entry.attempts=0;entry.size=size;entry.written=written;}
   else{entry.state=Failed;entry.attempts++;entry.retryAt=entry.checked+RetryDelay*entry.attempts;}
  }
  if(thumb&&notifyWindow)PostMessageW(notifyWindow,notifyMessage,0,0);
 }
 CoUninitialize();
}
}

void ThumbStart(HWND notify,UINT message){
 notifyWindow=notify;notifyMessage=message;
 std::lock_guard lock(mx);
 if(started)return;
 started=true;
 unsigned count=(std::max)(2u,(std::min)(4u,std::thread::hardware_concurrency()/2));
 for(unsigned i=0;i<count;i++)workers.emplace_back(Run);
}
void ThumbStop(){
 {std::lock_guard lock(mx);stopping=true;}
 cv.notify_all();
 for(auto& worker:workers)if(worker.joinable())worker.join();
 workers.clear();entries.clear();queue.clear();
}
void ThumbRequest(const std::wstring& path){
 {
  std::lock_guard lock(mx);
  if(stopping)return;
  ULONGLONG now=GetTickCount64();
  auto it=entries.find(path);
  if(it!=entries.end()){
   auto& entry=it->second;
   if(entry.state==Queued||entry.state==Loading)return;
   if(entry.state==Failed){
    if(entry.attempts>=MaxAttempts||now<entry.retryAt)return;
   }else{
    // The viewer can write over a photograph it is showing, so a finished tile
    // is re-read when the file itself changed -- but at most twice a second,
    // since the filmstrip asks for every visible tile on every frame.
    if(now-entry.checked<Revalidate)return;
    entry.checked=now;
    uint64_t size=0,written=0;
    if(!Stamp(path,size,written)||(size==entry.size&&written==entry.written))return;
   }
   entry.state=Queued;
  }else entries.emplace(path,Entry{});
  queue.push_back(path);
  if(queue.size()>QueueCap){
   auto stale=entries.find(queue.front());
   if(stale!=entries.end()&&stale->second.state==Queued)entries.erase(stale);
   queue.pop_front();
  }
 }
 cv.notify_one();
}
void ThumbPrioritize(const std::wstring& path){
 {
  std::lock_guard lock(mx);
  auto entry=entries.find(path);
  // A very large repaint burst can hit QueueCap and evict the first request
  // before PaintGallery reaches its final active-item prioritisation. Restore
  // that request here; the selected tile must not disappear behind the cap.
  if(entry==entries.end()){
   entries.emplace(path,Entry{});queue.push_back(path);
   if(queue.size()>QueueCap){
    auto stale=entries.find(queue.front());
    if(stale!=entries.end()&&stale->second.state==Queued)entries.erase(stale);
    queue.pop_front();
   }
   cv.notify_one();return;
  }
  if(entry->second.state!=Queued)return;
  auto queued=std::find(queue.begin(),queue.end(),path);
  if(queued==queue.end())return;
  queue.erase(queued);
  queue.push_back(path);
 }
 cv.notify_one();
}
std::shared_ptr<Image> ThumbLookup(const std::wstring& path){
 std::lock_guard lock(mx);
 auto found=entries.find(path);
 return found==entries.end()?std::shared_ptr<Image>():found->second.image;
}

#ifdef VETRO_THUMB_TESTS
int ThumbDebugState(const std::wstring& path){
 std::lock_guard lock(mx);auto it=entries.find(path);
 return it==entries.end()?-1:int(it->second.state);
}
unsigned ThumbDebugAttempts(const std::wstring& path){
 std::lock_guard lock(mx);auto it=entries.find(path);
 return it==entries.end()?0:it->second.attempts;
}
size_t ThumbDebugQueueSize(){std::lock_guard lock(mx);return queue.size();}
std::wstring ThumbDebugNext(){std::lock_guard lock(mx);return queue.empty()?std::wstring():queue.back();}
size_t ThumbDebugReadyCount(){
 std::lock_guard lock(mx);size_t count=0;
 for(auto& [path,entry]:entries)if(entry.state==Ready&&entry.image)count++;
 return count;
}
size_t ThumbDebugQueuedOutside(const std::vector<std::wstring>& keep){
 std::set<std::wstring> wanted(keep.begin(),keep.end());std::lock_guard lock(mx);size_t count=0;
 for(auto& path:queue)if(!wanted.count(path))count++;
 return count;
}
#endif
void ThumbTrim(const std::vector<std::wstring>& keep){
 if(keep.empty())return;
 std::set<std::wstring> wanted(keep.begin(),keep.end());
 std::lock_guard lock(mx);
 // Leaving another folder's backlog in the queue makes the new filmstrip wait
 // behind work whose result nobody will look at.
 for(auto it=queue.begin();it!=queue.end();){
  if(wanted.count(*it)){++it;continue;}
  auto stale=entries.find(*it);
  if(stale!=entries.end()&&stale->second.state==Queued)entries.erase(stale);
  it=queue.erase(it);
 }
 if(entries.size()<=wanted.size()+64)return;
 for(auto it=entries.begin();it!=entries.end();){
  if(wanted.count(it->first)||it->second.state==Loading)++it;else it=entries.erase(it);
 }
}
