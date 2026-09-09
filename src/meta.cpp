// Vetro Look, GPL-3.0-or-later.
// Metadata and histogram collection. Everything here runs on its own thread so
// opening an image never waits for EXIF parsing or pixel analysis.
//
// The reading itself is Exiv2's job (metaread.cpp) and the lens match is the
// Lensfun database's (lensdb.cpp). What is left here is presentation: turning
// one MetadataRecord into the rows the Info panel draws, plus the histogram
// and vectorscope, which need decoded pixels and so cannot come from a file
// reader at all.
#include "ui.h"
#include "metaread.h"
#include "metacache.h"
#include "lensdb.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <libraw/libraw.h>
#include <thread>
#include <chrono>
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
struct Job{std::wstring path;uint64_t id=0;std::shared_ptr<Image> pixels;bool quick=false;};
std::mutex mx;std::condition_variable cv;
Job pending;bool hasPending=false,stopping=false,started=false;
std::unique_ptr<Meta> finished;uint64_t finishedId=0;
std::atomic<uint64_t> wanted{0};
std::thread worker;HWND notifyWindow=nullptr;UINT notifyMessage=0;

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
void Add(std::vector<Field>& into,const std::wstring& label,const std::wstring& value){
 if(!value.empty())into.push_back({label,value});
}
std::wstring FormatName(const std::wstring& extension){
 struct Pair{const wchar_t* ext;const wchar_t* name;};
 static const Pair table[]={{L".jpg",L"JPEG"},{L".jpeg",L"JPEG"},{L".jfif",L"JPEG"},{L".png",L"PNG"},
  {L".gif",L"GIF"},{L".webp",L"WebP"},{L".bmp",L"BMP"},{L".tif",L"TIFF"},{L".tiff",L"TIFF"},{L".ico",L"ICO"},
  {L".heic",L"HEIF"},{L".heif",L"HEIF"},{L".avif",L"AVIF"},{L".exr",L"OpenEXR"},{L".cr2",L"Canon RAW"},
  {L".cr3",L"Canon RAW"},{L".nef",L"Nikon RAW"},{L".arw",L"Sony RAW"},{L".dng",L"Adobe DNG"},
  {L".raf",L"Fujifilm RAW"},{L".rw2",L"Panasonic RAW"},{L".orf",L"Olympus RAW"},{L".pef",L"Pentax RAW"},
  {L".psd",L"Photoshop"},{L".psb",L"Photoshop Large"}};
 for(auto& p:table)if(extension==p.ext)return p.name;
 auto bare=extension.empty()?std::wstring():extension.substr(1);
 for(auto& c:bare)c=towupper(c);
 return bare;
}
bool RawExtension(const std::wstring& e){
 return std::wstring(L"|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|").find(L"|"+e+L"|")!=std::wstring::npos;
}

// Dimensions, bit depth and colour context WIC can report and Exiv2 cannot:
// the pixel format of a decoded frame is a decoder question, not a metadata
// one, and WIC is already the decoder for these formats.
void ReadWic(const std::wstring& path,Meta& meta,unsigned& w,unsigned& h,std::wstring& depth,std::wstring& profile){
 ComPtr<IWICImagingFactory> factory;
 if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return;
 ComPtr<IWICBitmapDecoder> decoder;
 if(FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder)))return;
 ComPtr<IWICBitmapFrameDecode> frame;
 if(FAILED(decoder->GetFrame(0,&frame)))return;
 unsigned fw=0,fh=0;
 if(SUCCEEDED(frame->GetSize(&fw,&fh))&&fw&&fh){w=fw;h=fh;}
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
 if(!profile.empty())return;
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
}

// LibRaw for the two things Exiv2 does not answer for a RAW file: the size of
// the frame that will actually be produced, and the sensor's bit depth.
void ReadRawGeometry(const std::wstring& path,unsigned& w,unsigned& h,std::wstring& depth){
 LibRaw raw;
 if(raw.open_file(path.c_str())!=LIBRAW_SUCCESS)return;
 auto& sizes=raw.imgdata.sizes;
 if(sizes.width&&sizes.height){w=sizes.width;h=sizes.height;}
 depth=L"14-bit";
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
  // Broadcast-style chroma coordinates: B-Y across, R-Y up.
  int u=(-169*(int)r-331*(int)g+500*(int)b)/1000;
  int v=(500*(int)r-419*(int)g-81*(int)b)/1000;
  int sx=Meta::ScopeEdge/2+u*Meta::ScopeEdge/256,sy=Meta::ScopeEdge/2-v*Meta::ScopeEdge/256;
  if(sx>=0&&sy>=0&&sx<Meta::ScopeEdge&&sy<Meta::ScopeEdge)meta.scope[sy*Meta::ScopeEdge+sx]++;
  counted++;
 }
 if(!counted)return;
 for(int i=0;i<Meta::ScopeEdge*Meta::ScopeEdge;i++)meta.scopePeak=(std::max)(meta.scopePeak,meta.scope[i]);
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

std::wstring CameraName(const MetadataRecord& record){
 auto make=record.cameraMake,model=record.cameraModel;
 if(model.empty())return make;
 if(make.empty())return model;
 // "NIKON CORPORATION" + "NIKON Z 6" must not become "NIKON CORPORATION NIKON Z 6".
 auto lowerMake=make,lowerModel=model;
 for(auto& c:lowerMake)c=towlower(c);
 for(auto& c:lowerModel)c=towlower(c);
 auto firstWord=lowerMake.substr(0,lowerMake.find(L' '));
 if(!firstWord.empty()&&lowerModel.rfind(firstWord,0)==0)return model;
 return make+L" "+model;
}

void Build(const Job& job,Meta& meta){
 meta.path=job.path;
 auto extension=fs::path(job.path).extension().wstring();
 for(auto& c:extension)c=towlower(c);
 bool raw=RawExtension(extension);

 auto record=ReadMetadata(job.path);
 unsigned w=record.width,h=record.height;
 std::wstring depth=L"8-bit",profile=record.iccProfile;
 if(profile.empty()&&record.hasIcc)profile=language?L"Embedded ICC profile":L"Встроенный ICC-профиль";
 if(profile.empty())profile=record.colourSpace;

 if(raw)ReadRawGeometry(job.path,w,h,depth);
 else if(extension==L".exr")depth=L"32-bit float";
 else ReadWic(job.path,meta,w,h,depth,profile);
 if(!w||!h){if(job.pixels){w=job.pixels->w;h=job.pixels->h;}}
 if(profile.empty())profile=L"sRGB";
 meta.orientation=record.orientation;

 // ---- ADOBE / XMP.  Read only; nothing here is ever written back.
 meta.hasRating=record.hasRating;meta.rating=record.rating;
 meta.label=record.label;meta.xmpSource=record.xmpSource;

 WIN32_FILE_ATTRIBUTE_DATA attributes{};
 unsigned long long size=0;
 std::wstring created,modified;
 if(GetFileAttributesExW(job.path.c_str(),GetFileExInfoStandard,&attributes)){
  size=((unsigned long long)attributes.nFileSizeHigh<<32)|attributes.nFileSizeLow;
  created=Stamp(attributes.ftCreationTime);
  modified=Stamp(attributes.ftLastWriteTime);
 }

 // ---- FILE
 Add(meta.file,S_Filename,fs::path(job.path).filename().wstring());
 Add(meta.file,S_Format,FormatName(extension));
 if(w&&h){
  Add(meta.file,S_Dimensions,Grouped(w)+L" × "+Grouped(h));
  double mp=double(w)*double(h)/1e6;
  Add(meta.file,S_Megapixels,Round(mp,mp<1?2:(mp<10?1:0))+L" MP");
  unsigned dw=w,dh=h;if(meta.orientation>=5&&meta.orientation<=8)std::swap(dw,dh);
  unsigned divisor=std::gcd(dw,dh);
  std::wstring aspect=std::to_wstring(dw/divisor)+L":"+std::to_wstring(dh/divisor);
  if(dw/divisor==6&&dh/divisor==13)aspect=L"9:19.5";
  if(dw/divisor==13&&dh/divisor==6)aspect=L"19.5:9";
  meta.file.push_back({language?L"Aspect ratio":L"Соотношение",aspect});
  std::wstring orientation=dw==dh?L"Square":(dw<dh?L"Portrait":L"Landscape");
  if(meta.orientation>1)orientation+=L" · EXIF "+std::to_wstring(meta.orientation);
  meta.file.push_back({language?L"Orientation":L"Ориентация",orientation});
 }
 if(meta.dpiX>0&&meta.dpiY>0)meta.file.push_back({L"DPI",Round(meta.dpiX,1)+L" × "+Round(meta.dpiY,1)});
 if(job.pixels)meta.file.push_back({L"Alpha",job.pixels->hasAlpha?(language?L"Transparent":L"Есть прозрачность"):(language?L"Opaque":L"Нет прозрачности")});
 if(extension==L".jpg"||extension==L".jpeg"||extension==L".jfif"){
  std::ifstream jpeg(fs::path(job.path),std::ios::binary);std::vector<unsigned char> head(1024*1024);
  jpeg.read((char*)head.data(),std::streamsize(head.size()));size_t count=size_t(jpeg.gcount());
  auto decoder=tj3Init(TJINIT_DECOMPRESS);
  if(decoder){
   if(count&&tj3DecompressHeader(decoder,head.data(),count)==0)
    meta.file.push_back({L"JPEG",tj3Get(decoder,TJPARAM_LOSSLESS)?L"Lossless":(tj3Get(decoder,TJPARAM_PROGRESSIVE)?L"Progressive":L"Baseline")});
   tj3Destroy(decoder);
  }
 }
 if(size)Add(meta.file,S_FileSize,Bytes(size));
 Add(meta.file,S_Created,created);
 Add(meta.file,S_Modified,modified);

 // ---- CAMERA
 Add(meta.camera,S_Camera,CameraName(record));
 Add(meta.camera,S_Lens,record.lens);
 if(record.focalLength>0){
  auto text=Round(record.focalLength,1)+L" mm";
  if(record.focalLength35>0&&std::abs(record.focalLength35-record.focalLength)>0.6)
   text+=L" (" + Round(record.focalLength35,0)+L" mm eq.)";
  Add(meta.camera,S_FocalLength,text);
 }
 if(record.aperture>0)Add(meta.camera,S_Aperture,L"ƒ/"+Round(record.aperture,1));
 Add(meta.camera,S_Shutter,ShutterText(record.shutter));
 if(record.iso>0)Add(meta.camera,S_ISO,std::to_wstring(record.iso));
 if(record.exposureBias!=0)
  Add(meta.camera,S_ExposureComp,(record.exposureBias>0?L"+":L"")+Round(record.exposureBias,1)+L" EV");
 Add(meta.camera,S_WhiteBalance,record.whiteBalance);
 Add(meta.camera,S_Flash,record.flash);
 Add(meta.camera,S_Metering,record.metering);
 // EXIF spells a capture time "2026:03:14 09:26:53". Nobody reads dates that
 // way, and the file's own Created/Modified rows right above are localised.
 if(record.dateTaken.size()>=19&&record.dateTaken[4]==L':'&&record.dateTaken[7]==L':'){
  SYSTEMTIME taken{};
  taken.wYear =WORD(_wtoi(record.dateTaken.substr(0,4).c_str()));
  taken.wMonth=WORD(_wtoi(record.dateTaken.substr(5,2).c_str()));
  taken.wDay  =WORD(_wtoi(record.dateTaken.substr(8,2).c_str()));
  taken.wHour =WORD(_wtoi(record.dateTaken.substr(11,2).c_str()));
  taken.wMinute=WORD(_wtoi(record.dateTaken.substr(14,2).c_str()));
  wchar_t date[80]{},clock[40]{};
  if(taken.wYear&&taken.wMonth&&taken.wDay&&
     GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_LONGDATE,&taken,nullptr,date,80,nullptr)&&
     GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,&taken,nullptr,clock,40))
   Add(meta.camera,language?L"Taken":L"Снято",std::wstring(date)+L", "+clock);
  else Add(meta.camera,language?L"Taken":L"Снято",record.dateTaken);
 }else Add(meta.camera,record.dateTaken.empty()?std::wstring():(language?L"Taken":L"Снято"),record.dateTaken);

 // ---- COLOR
 Add(meta.colour,S_ColorProfile,profile);
 if(!record.colourSpace.empty()&&record.colourSpace!=profile)
  Add(meta.colour,language?L"Color space":L"Цветовое пространство",record.colourSpace);
 Add(meta.colour,S_BitDepth,depth);
 meta.colour.push_back({language?L"Display":L"Отображение",
  extension==L".exr"?L"HDR → SDR":(raw?L"RAW → SDR":L"SDR")});

 // ---- LOCATION
 if(record.hasGps){
  Add(meta.location,S_Latitude,Round(std::abs(record.lat),6)+L"° "+(record.lat<0?L"S":L"N"));
  Add(meta.location,S_Longitude,Round(std::abs(record.lon),6)+L"° "+(record.lon<0?L"W":L"E"));
  if(record.altitude!=0)Add(meta.location,language?L"Altitude":L"Высота",Round(record.altitude,0)+L" m");
  meta.lat=record.lat;meta.lon=record.lon;meta.hasGps=true;
 }

 // ---- AUTHOR
 Add(meta.author,S_Author,record.author);
 Add(meta.author,S_Copyright,record.copyright);
 Add(meta.author,S_Title,record.title);
 Add(meta.author,S_Description,record.description);
 if(!record.keywords.empty()){
  std::wstring list;
  for(size_t i=0;i<record.keywords.size()&&i<12;i++){if(i)list+=L", ";list+=record.keywords[i];}
  if(record.keywords.size()>12)list+=L"…";
  Add(meta.author,S_Keywords,list);
 }
 Add(meta.author,S_Software,record.creatorTool);

 // ---- ADOBE / XMP rows
 if(meta.hasRating){
  if(meta.rating<0)Add(meta.adobe,S_XmpRating,T(S_Rejected));
  else{
   std::wstring stars;
   for(int i=0;i<meta.rating;i++)stars+=L"★";
   for(int i=meta.rating;i<5;i++)stars+=L"☆";
   Add(meta.adobe,S_XmpRating,stars+L"  "+std::to_wstring(meta.rating)+L"/5");
  }
 }
 Add(meta.adobe,S_XmpLabel,meta.label);
 if(!meta.xmpSource.empty())
  Add(meta.adobe,S_XmpFrom,meta.xmpSource==L"sidecar"?T(S_Sidecar):T(S_Embedded));
 if(!record.warning.empty())Add(meta.adobe,language?L"Note":L"Замечание",record.warning);

 // ---- LENS (Lensfun)
 // The database is 5 MB of XML parsed on its own thread at startup, and the
 // first photograph opened can easily beat it. This is a background metadata
 // thread, not the UI, so waiting a moment for the answer is better than
 // leaving the section out of the panel until the next file.
 LensDbStart();
 for(int waited=0;waited<80&&!LensDbReady();waited++)
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
 auto lensInfo=LensLookup(record.cameraMake,record.cameraModel,record.lens,
                          record.focalLength,record.aperture);
 meta.lensReady=lensInfo.ready;meta.lensMatched=lensInfo.matched;
 meta.lensName=record.lens;
 if(lensInfo.ready){
  meta.lensProfile=lensInfo.lensMatch;
  meta.lensDistortion=lensInfo.distortion;
  meta.lensVignetting=lensInfo.vignetting;
  meta.lensTca=lensInfo.tca;
  meta.lensGeometry=lensInfo.geometry;
  meta.lensNote=lensInfo.note;
  Add(meta.lens,S_MatchedCamera,lensInfo.cameraMatch);
  Add(meta.lens,S_MatchedLens,lensInfo.lensMatch);
  if(lensInfo.matched){
   Add(meta.lens,S_LensProfile,T(S_ProfileFound));
   // "Calibrated" and not "On": these say what the database measured for this
   // lens, not what the viewer is doing with it. Nothing applies them yet.
   auto yesNo=[](bool on){return on?(language?L"Calibrated":L"Есть данные"):(language?L"—":L"—");};
   Add(meta.lens,S_Distortion,yesNo(lensInfo.distortion));
   Add(meta.lens,S_Vignette,yesNo(lensInfo.vignetting));
   Add(meta.lens,S_Chromatic,yesNo(lensInfo.tca));
  }else if(!record.lens.empty())Add(meta.lens,S_LensProfile,T(S_ProfileMissing));
 }

 meta.hasCamera=!meta.camera.empty();
 meta.hasLocation=!meta.location.empty();
 if(!job.quick)Histogram(job.pixels,meta);
 meta.ready=true;
}

// The quick pass: enough for the viewer header and the library, without
// touching pixels and without the Lensfun match, and served from the on-disk
// cache when the file and its sidecar have not changed.
void BuildQuick(const Job& job,Meta& meta){
 meta.path=job.path;
 auto cached=MetaCacheLookup(job.path);
 meta.hasRating=cached.hasRating;meta.rating=cached.rating;
 meta.label=cached.label;meta.xmpSource=cached.xmpSource;
 meta.orientation=cached.orientation;
 meta.ready=false;   // not a full record: the Info panel must still ask for one
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
  try{if(job.quick)BuildQuick(job,*meta);else Build(job,*meta);}
  catch(...){meta->ready=!job.quick;}
  if(wanted.load()!=job.id)continue;
  {std::lock_guard lock(mx);finished=std::move(meta);finishedId=job.id;}
  if(notifyWindow)PostMessageW(notifyWindow,notifyMessage,0,0);
 }
 MetaCacheFlush();
 CoUninitialize();
}
}

static void Post(const std::wstring& path,uint64_t id,const std::shared_ptr<Image>& decoded,
                 HWND notify,UINT message,bool quick){
 notifyWindow=notify;notifyMessage=message;
 wanted=id;
 {
  std::lock_guard lock(mx);
  pending=Job{path,id,decoded,quick};hasPending=true;finished.reset();
  if(!started){started=true;worker=std::thread(Run);}
 }
 cv.notify_one();
}

void MetaRequest(const std::wstring& path,uint64_t id,const std::shared_ptr<Image>& decoded,HWND notify,UINT message){
 Post(path,id,decoded,notify,message,false);
}
void MetaRequestQuick(const std::wstring& path,uint64_t id,HWND notify,UINT message){
 Post(path,id,{},notify,message,true);
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
