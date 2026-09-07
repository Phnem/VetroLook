// Vetro Look, GPL-3.0-or-later.
// Metadata and histogram collection. Everything here runs on its own thread so
// opening an image never waits for EXIF parsing or pixel analysis.
#include "ui.h"
#include "exif.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <libraw/libraw.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <turbojpeg.h>
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;

namespace{
struct Job{std::wstring path;uint64_t id=0;std::shared_ptr<Image> pixels;};
std::mutex mx;std::condition_variable cv;
Job pending;bool hasPending=false,stopping=false,started=false;
std::unique_ptr<Meta> finished;uint64_t finishedId=0;
std::atomic<uint64_t> wanted{0};
std::thread worker;HWND notifyWindow=nullptr;UINT notifyMessage=0;

std::wstring Widen(const std::string& s){
 if(s.empty())return {};
 int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),nullptr,0);
 std::wstring out(size_t(n),L'\0');
 MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),out.data(),n);
 while(!out.empty()&&(out.back()==L' '||out.back()==L'\0'))out.pop_back();
 return out;
}
std::wstring Round(double v,int places){
 std::wostringstream out;out<<std::fixed<<std::setprecision(places)<<v;
 auto s=out.str();
 if(s.find(L'.')!=std::wstring::npos){while(!s.empty()&&s.back()==L'0')s.pop_back();if(!s.empty()&&s.back()==L'.')s.pop_back();}
 return s;
}
std::wstring Grouped(unsigned long long v){
 auto s=std::to_wstring(v);std::wstring out;
 for(size_t i=0;i<s.size();i++){if(i&&(s.size()-i)%3==0)out+=L' ';out+=s[i];}
 return out;
}
std::wstring Bytes(unsigned long long size){
 const wchar_t* units[]={L"B",L"KB",L"MB",L"GB"};
 double v=double(size);int unit=0;
 while(v>=1024.&&unit<3){v/=1024.;unit++;}
 return Round(v,unit?(v<10?2:1):0)+L" "+units[unit];
}
std::wstring Stamp(const FILETIME& ft){
 SYSTEMTIME utc{},local{};
 if(!FileTimeToSystemTime(&ft,&utc))return {};
 SystemTimeToTzSpecificLocalTime(nullptr,&utc,&local);
 wchar_t date[80]{},time[40]{};
 GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_LONGDATE,&local,nullptr,date,80,nullptr);
 GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,&local,nullptr,time,40);
 return std::wstring(date)+L", "+time;
}
std::wstring ShutterText(double seconds){
 if(seconds<=0)return {};
 if(seconds>=1.)return Round(seconds,1)+L" s";
 return L"1/"+std::to_wstring((long long)(1./seconds+.5))+L" s";
}
void Add(std::vector<Field>& into,Str label,const std::wstring& value){
 if(!value.empty())into.push_back({T(label),value});
}
std::wstring FormatName(const std::wstring& extension){
 struct Pair{const wchar_t* ext;const wchar_t* name;};
 static const Pair table[]={{L".jpg",L"JPEG"},{L".jpeg",L"JPEG"},{L".jfif",L"JPEG"},{L".png",L"PNG"},
  {L".gif",L"GIF"},{L".webp",L"WebP"},{L".bmp",L"BMP"},{L".tif",L"TIFF"},{L".tiff",L"TIFF"},{L".ico",L"ICO"},
  {L".heic",L"HEIF"},{L".heif",L"HEIF"},{L".avif",L"AVIF"},{L".exr",L"OpenEXR"},{L".cr2",L"Canon RAW"},
  {L".cr3",L"Canon RAW"},{L".nef",L"Nikon RAW"},{L".arw",L"Sony RAW"},{L".dng",L"Adobe DNG"},
  {L".raf",L"Fujifilm RAW"},{L".rw2",L"Panasonic RAW"},{L".orf",L"Olympus RAW"},{L".pef",L"Pentax RAW"}};
 for(auto& p:table)if(extension==p.ext)return p.name;
 auto bare=extension.empty()?std::wstring():extension.substr(1);
 for(auto& c:bare)c=towupper(c);
 return bare;
}
bool RawExtension(const std::wstring& e){
 return std::wstring(L"|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|").find(L"|"+e+L"|")!=std::wstring::npos;
}

// Standard formats: dimensions, bit depth and colour context straight from WIC.
void ReadWic(const std::wstring& path,Meta& meta,unsigned& w,unsigned& h,std::wstring& depth,std::wstring& profile){
 ComPtr<IWICImagingFactory> factory;
 if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return;
 ComPtr<IWICBitmapDecoder> decoder;
 if(FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder)))return;
 ComPtr<IWICBitmapFrameDecode> frame;
 if(FAILED(decoder->GetFrame(0,&frame)))return;
 frame->GetSize(&w,&h);
 frame->GetResolution(&meta.dpiX,&meta.dpiY);
 WICPixelFormatGUID format{};
 if(SUCCEEDED(frame->GetPixelFormat(&format))){
  ComPtr<IWICComponentInfo> info;ComPtr<IWICPixelFormatInfo> pixelInfo;
  if(SUCCEEDED(factory->CreateComponentInfo(format,&info))&&SUCCEEDED(info.As(&pixelInfo))){
   UINT bpp=0,channels=0;
   pixelInfo->GetBitsPerPixel(&bpp);pixelInfo->GetChannelCount(&channels);
   if(bpp&&channels)depth=std::to_wstring(bpp/channels)+L"-bit";
  }
 }
 UINT contexts=0;
 if(SUCCEEDED(frame->GetColorContexts(0,nullptr,&contexts))&&contexts){
  std::vector<ComPtr<IWICColorContext>> owned(contexts);
  std::vector<IWICColorContext*> raw(contexts);
  bool ok=true;
  for(UINT i=0;i<contexts;i++){if(FAILED(factory->CreateColorContext(&owned[i]))){ok=false;break;}raw[i]=owned[i].Get();}
  if(ok&&SUCCEEDED(frame->GetColorContexts(contexts,raw.data(),&contexts))&&contexts){
   WICColorContextType type=WICColorContextUninitialized;
   raw[0]->GetType(&type);
   if(type==WICColorContextExifColorSpace){UINT value=0;raw[0]->GetExifColorSpace(&value);profile=value==1?L"sRGB":L"Uncalibrated";}
   else if(type==WICColorContextProfile)profile=language?L"Embedded ICC profile":L"Встроенный ICC-профиль";
  }
 }
 (void)meta;
}

void ReadExif(const std::wstring& path,Meta& meta,std::wstring& profile){
 std::ifstream file(fs::path(path),std::ios::binary|std::ios::ate);
 if(!file)return;
 auto length=file.tellg();
 if(length<=0)return;
 size_t take=(size_t)(std::min)((long long)length,4LL*1024*1024);
 std::vector<unsigned char> head(take);
 file.seekg(0);
 if(!file.read((char*)head.data(),(std::streamsize)take))return;
 easyexif::EXIFInfo exif;exif.clear();
 if(exif.parseFrom(head.data(),unsigned(take))!=0)return;
 meta.orientation=exif.Orientation;
 auto make=Widen(exif.Make),model=Widen(exif.Model);
 std::wstring body=make;
 if(!model.empty()){
  if(!body.empty()&&model.rfind(body,0)==0)body.clear();
  body=body.empty()?model:body+L" "+model;
 }
 Add(meta.camera,S_Camera,body);
 Add(meta.camera,S_Lens,Widen(exif.LensInfo.Model));
 if(exif.FocalLength>0)Add(meta.camera,S_FocalLength,Round(exif.FocalLength,1)+L" mm");
 if(exif.FNumber>0)Add(meta.camera,S_Aperture,L"ƒ/"+Round(exif.FNumber,1));
 Add(meta.camera,S_Shutter,ShutterText(exif.ExposureTime));
 if(exif.ISOSpeedRatings)Add(meta.camera,S_ISO,std::to_wstring(exif.ISOSpeedRatings));
 if(exif.ExposureBiasValue!=0)Add(meta.camera,S_ExposureComp,(exif.ExposureBiasValue>0?L"+":L"")+Round(exif.ExposureBiasValue,1)+L" EV");
 if(!meta.camera.empty())Add(meta.camera,S_Flash,exif.Flash?(language?L"Fired":L"Сработала"):(language?L"Off":L"Выключена"));
 if(exif.MeteringMode){
  const wchar_t* ru[]={L"Неизвестно",L"Средний",L"Центровзвешенный",L"Точечный",L"Мультиточечный",L"Матричный",L"Частичный"};
  const wchar_t* en[]={L"Unknown",L"Average",L"Center-weighted",L"Spot",L"Multi-spot",L"Evaluative",L"Partial"};
  unsigned index=exif.MeteringMode<7?exif.MeteringMode:0;
  Add(meta.camera,S_Metering,language?en[index]:ru[index]);
 }
 if(profile.empty()&&exif.ColorSpace)profile=exif.ColorSpace==1?L"sRGB":(exif.ColorSpace==2?L"Adobe RGB":L"Uncalibrated");
 if((exif.GeoLocation.LatComponents.direction=='N'||exif.GeoLocation.LatComponents.direction=='S')&&
    (exif.GeoLocation.LonComponents.direction=='E'||exif.GeoLocation.LonComponents.direction=='W')){
  Add(meta.location,S_Latitude,Round(fabs(exif.GeoLocation.Latitude),6)+L"° "+wchar_t(exif.GeoLocation.LatComponents.direction));
  Add(meta.location,S_Longitude,Round(fabs(exif.GeoLocation.Longitude),6)+L"° "+wchar_t(exif.GeoLocation.LonComponents.direction));
  meta.lat=exif.GeoLocation.LatComponents.direction=='S'?-fabs(exif.GeoLocation.Latitude):fabs(exif.GeoLocation.Latitude);
  meta.lon=exif.GeoLocation.LonComponents.direction=='W'?-fabs(exif.GeoLocation.Longitude):fabs(exif.GeoLocation.Longitude);
  meta.hasGps=true;
 }
}

void ReadRaw(const std::wstring& path,Meta& meta,unsigned& w,unsigned& h,std::wstring& depth){
 LibRaw raw;
 if(raw.open_file(path.c_str())!=LIBRAW_SUCCESS)return;
 auto& id=raw.imgdata.idata;auto& other=raw.imgdata.other;auto& sizes=raw.imgdata.sizes;
 if(sizes.width&&sizes.height){w=sizes.width;h=sizes.height;}
 std::wstring make=Widen(id.make),model=Widen(id.model),body=make;
 if(!model.empty()){if(!body.empty()&&model.rfind(body,0)==0)body.clear();body=body.empty()?model:body+L" "+model;}
 Add(meta.camera,S_Camera,body);
 Add(meta.camera,S_Lens,Widen(raw.imgdata.lens.Lens));
 if(other.focal_len>0)Add(meta.camera,S_FocalLength,Round(other.focal_len,1)+L" mm");
 if(other.aperture>0)Add(meta.camera,S_Aperture,L"ƒ/"+Round(other.aperture,1));
 Add(meta.camera,S_Shutter,ShutterText(other.shutter));
 if(other.iso_speed>0)Add(meta.camera,S_ISO,std::to_wstring((long long)(other.iso_speed+.5f)));
 depth=L"14-bit";
 if(other.gpsdata[0]||other.gpsdata[4]){
  // LibRaw exposes GPS as packed rationals; degrees/minutes/seconds with a hemisphere byte.
  auto convert=[&](const unsigned* p,unsigned char ref)->double{
   if(!p[1]||!p[3]||!p[5])return 0;
   double v=double(p[0])/p[1]+double(p[2])/p[3]/60.+double(p[4])/p[5]/3600.;
   return (ref=='S'||ref=='W')?-v:v;
  };
  double lat=convert(other.gpsdata,(unsigned char)other.gpsdata[8]);
  double lon=convert(other.gpsdata+4,(unsigned char)other.gpsdata[9]);
  if(lat||lon){
   Add(meta.location,S_Latitude,Round(lat,6)+L"°");
   Add(meta.location,S_Longitude,Round(lon,6)+L"°");
   meta.lat=lat;meta.lon=lon;meta.hasGps=true;
  }
 }
 raw.recycle();
}

void Histogram(const std::shared_ptr<Image>& image,Meta& meta){
 if(!image||!image->w||!image->h||image->pixels.empty())return;
 // Analysis runs over a reduced copy: at most about a megapixel regardless of source size.
 unsigned long long total=(unsigned long long)image->w*image->h;
 unsigned step=1;
 while(total/((unsigned long long)step*step)>1000000ull)step++;
 unsigned long long counted=0,high=0,low=0;
 for(unsigned y=0;y<image->h;y+=step)for(unsigned x=0;x<image->w;x+=step){
  const uint8_t* px=&image->pixels[((size_t)y*image->w+x)*4];
  unsigned a=px[3];
  if(!a)continue;
  unsigned b=px[0],g=px[1],r=px[2];
  if(a&&a<255){r=(std::min)(255u,r*255/a);g=(std::min)(255u,g*255/a);b=(std::min)(255u,b*255/a);}
  meta.hist[0][r]++;meta.hist[1][g]++;meta.hist[2][b]++;
  unsigned luma=(unsigned)((r*2126u+g*7152u+b*722u)/10000u);
  if(luma>255)luma=255;
  meta.hist[3][luma]++;
  if(luma>=250)high++;
  if(luma<=5)low++;
  counted++;
 }
 if(!counted)return;
 for(int c=0;c<4;c++){
  uint32_t peak=0;
  // The extreme bins are usually a flat background; ignore them when scaling.
  for(int i=2;i<254;i++)peak=(std::max)(peak,meta.hist[c][i]);
  meta.histPeak[c]=peak?peak:1;
 }
 meta.clippedHigh=float(double(high)*100./double(counted));
 meta.clippedLow=float(double(low)*100./double(counted));
 meta.histReady=true;
}

void Build(const Job& job,Meta& meta){
 meta.path=job.path;
 auto extension=fs::path(job.path).extension().wstring();
 for(auto& c:extension)c=towlower(c);
 unsigned w=job.pixels?job.pixels->w:0,h=job.pixels?job.pixels->h:0;
 std::wstring depth=L"8-bit",profile;
 if(RawExtension(extension))ReadRaw(job.path,meta,w,h,depth);
 else ReadWic(job.path,meta,w,h,depth,profile);
 if(extension!=L".exr")ReadExif(job.path,meta,profile);
 else depth=L"32-bit float";
 if(profile.empty())profile=L"sRGB";

 WIN32_FILE_ATTRIBUTE_DATA attributes{};
 unsigned long long size=0;
 std::wstring created,modified;
 if(GetFileAttributesExW(job.path.c_str(),GetFileExInfoStandard,&attributes)){
  size=((unsigned long long)attributes.nFileSizeHigh<<32)|attributes.nFileSizeLow;
  created=Stamp(attributes.ftCreationTime);
  modified=Stamp(attributes.ftLastWriteTime);
 }
 Add(meta.file,S_Filename,fs::path(job.path).filename().wstring());
 Add(meta.file,S_Format,FormatName(extension));
 if(w&&h){
  Add(meta.file,S_Dimensions,Grouped(w)+L" × "+Grouped(h));
  double mp=double(w)*double(h)/1e6;
  Add(meta.file,S_Megapixels,Round(mp,mp<1?2:(mp<10?1:0))+L" MP");
  unsigned dw=w,dh=h;if(meta.orientation>=5&&meta.orientation<=8)std::swap(dw,dh);
  unsigned divisor=std::gcd(dw,dh);
  std::wstring aspect=std::to_wstring(dw/divisor)+L":"+std::to_wstring(dh/divisor);
  if(dw<divisor*100&&dh<divisor*100){}else{
   double a=double(dw)/dh;
   if(fabs(a-9./19.5)<.001)aspect=L"9:19.5";
   else if(fabs(a-19.5/9.)<.001)aspect=L"19.5:9";
  }
  if(dw/divisor==6&&dh/divisor==13)aspect=L"9:19.5";
  if(dw/divisor==13&&dh/divisor==6)aspect=L"19.5:9";
  meta.file.push_back({language?L"Aspect ratio":L"Соотношение",aspect});
  std::wstring orientation=dw==dh?L"Square":(dw<dh?L"Portrait":L"Landscape");
  if(meta.orientation)orientation+=L" · EXIF "+std::to_wstring(meta.orientation);
  meta.file.push_back({language?L"Orientation":L"Ориентация",orientation});
 }
 if(meta.dpiX>0&&meta.dpiY>0)meta.file.push_back({L"DPI",Round(meta.dpiX,1)+L" × "+Round(meta.dpiY,1)});
 if(job.pixels)meta.file.push_back({L"Alpha",job.pixels->hasAlpha?(language?L"Transparent":L"Есть прозрачность"):(language?L"Opaque":L"Нет прозрачности")});
 meta.file.push_back({language?L"Display":L"Отображение",extension==L".exr"?L"HDR → SDR":(RawExtension(extension)?L"RAW → SDR":L"SDR")});
 if(extension==L".jpg"||extension==L".jpeg"||extension==L".jfif"){
  std::ifstream jpeg(fs::path(job.path),std::ios::binary);std::vector<unsigned char> head(4*1024*1024);
  jpeg.read((char*)head.data(),head.size());size_t count=size_t(jpeg.gcount());
  auto decoder=tj3Init(TJINIT_DECOMPRESS);
  if(decoder){if(tj3DecompressHeader(decoder,head.data(),count)==0)
   meta.file.push_back({L"JPEG",tj3Get(decoder,TJPARAM_LOSSLESS)?L"Lossless":(tj3Get(decoder,TJPARAM_PROGRESSIVE)?L"Progressive":L"Baseline")});tj3Destroy(decoder);}
 }
 if(size)Add(meta.file,S_FileSize,Bytes(size));
 Add(meta.file,S_Created,created);
 Add(meta.file,S_Modified,modified);
 Add(meta.file,S_ColorProfile,profile);
 Add(meta.file,S_BitDepth,depth);
 meta.hasCamera=!meta.camera.empty();
 meta.hasLocation=!meta.location.empty();
 Histogram(job.pixels,meta);
 meta.ready=true;
}

void Run(){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 while(true){
  Job job;
  {
   std::unique_lock lock(mx);
   cv.wait(lock,[]{return stopping||hasPending;});
   if(stopping)break;
   job=std::move(pending);pending=Job{};hasPending=false;
  }
  if(wanted.load()!=job.id)continue;
  auto meta=std::make_unique<Meta>();
  try{Build(job,*meta);}catch(...){meta->ready=true;}
  if(wanted.load()!=job.id)continue;
  {std::lock_guard lock(mx);finished=std::move(meta);finishedId=job.id;}
  if(notifyWindow)PostMessageW(notifyWindow,notifyMessage,0,0);
 }
 CoUninitialize();
}
}

void MetaRequest(const std::wstring& path,uint64_t id,const std::shared_ptr<Image>& decoded,HWND notify,UINT message){
 notifyWindow=notify;notifyMessage=message;
 wanted=id;
 {
  std::lock_guard lock(mx);
  pending=Job{path,id,decoded};hasPending=true;finished.reset();
  if(!started){started=true;worker=std::thread(Run);}
 }
 cv.notify_one();
}
bool MetaCollect(uint64_t id,Meta& out){
 std::lock_guard lock(mx);
 if(!finished||finishedId!=id)return false;
 out=std::move(*finished);finished.reset();
 return true;
}
void MetaStop(){
 {std::lock_guard lock(mx);stopping=true;}
 cv.notify_one();
 if(worker.joinable())worker.join();
}
