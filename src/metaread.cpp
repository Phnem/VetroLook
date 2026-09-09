// Vetro Look, GPL-3.0-or-later.
// Exiv2 does the reading. Everything below is translation: Exiv2's values into
// the record the rest of the application understands, and nothing else.
//
// Exiv2 opens files by narrow path through fopen(), which on Windows goes
// through the ANSI code page and loses any path the user's locale cannot
// spell. Every file here is therefore opened with the wide Win32 API and
// handed to Exiv2 as memory.
#include "metaread.h"
#include <windows.h>
#include <exiv2/exiv2.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
namespace fs=std::filesystem;

namespace{

std::wstring Widen(const std::string& s){
 if(s.empty())return {};
 int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),nullptr,0);
 if(n<=0)return {};
 std::wstring out(size_t(n),L'\0');
 MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),out.data(),n);
 while(!out.empty()&&(out.back()==L' '||out.back()==L'\0'))out.pop_back();
 size_t first=out.find_first_not_of(L" \t");
 return first==std::wstring::npos?std::wstring():out.substr(first);
}

// A RAW file's metadata sits behind offsets that can point anywhere in it, so
// the whole file is read rather than a prefix. 256 MB is well past any camera
// file and stops a mis-detected video from being pulled into memory.
constexpr uint64_t MaxRead=256ull*1024*1024;
bool ReadWhole(const std::wstring& path,std::vector<uint8_t>& out){
 HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
  nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;
 LARGE_INTEGER size{};
 bool ok=GetFileSizeEx(file,&size)&&size.QuadPart>0&&uint64_t(size.QuadPart)<=MaxRead;
 if(ok){
  out.resize(size_t(size.QuadPart));
  size_t done=0;
  while(done<out.size()){
   DWORD chunk=DWORD((std::min)(out.size()-done,size_t(32u*1024*1024))),got=0;
   if(!ReadFile(file,out.data()+done,chunk,&got,nullptr)||!got){ok=false;break;}
   done+=got;
  }
  if(ok&&done!=out.size())ok=false;
 }
 CloseHandle(file);
 if(!ok)out.clear();
 return ok;
}

const Exiv2::Exifdatum* Find(const Exiv2::ExifData& data,const char* key){
 auto it=data.findKey(Exiv2::ExifKey(key));
 return it==data.end()?nullptr:&*it;
}
const Exiv2::Xmpdatum* FindXmp(const Exiv2::XmpData& data,const char* key){
 auto it=data.findKey(Exiv2::XmpKey(key));
 return it==data.end()?nullptr:&*it;
}

std::wstring Text(const Exiv2::Metadatum* d){
 if(!d)return {};
 try{return Widen(d->toString());}catch(...){return {};}
}
double Number(const Exiv2::Metadatum* d,bool& ok){
 ok=false;
 if(!d)return 0;
 try{double v=d->toFloat();ok=true;return v;}catch(...){ok=false;return 0;}
}

// Exiv2 prints many enumerations itself, in English, and does it better than a
// hand-kept table would. Rendering needs the full ExifData, because some
// MakerNote tags are only interpretable against other tags in the same file.
std::wstring PrintedAt(const Exiv2::ExifData& data,Exiv2::ExifData::const_iterator it){
 if(it==data.end())return {};
 try{std::ostringstream out;it->write(out,&data);return Widen(out.str());}catch(...){return {};}
}
std::wstring Printed(const Exiv2::ExifData& data,const char* key){
 return PrintedAt(data,data.findKey(Exiv2::ExifKey(key)));
}

double Sexagesimal(const Exiv2::Exifdatum& d,const std::wstring& ref){
 try{
  if(d.count()<3)return 0;
  double v=d.toFloat(0)+d.toFloat(1)/60.0+d.toFloat(2)/3600.0;
  if(!ref.empty()&&(ref[0]==L'S'||ref[0]==L'W'))v=-v;
  return v;
 }catch(...){return 0;}
}

// ICC profiles carry their own human-readable name in the 'desc' tag. Showing
// that beats showing "Embedded ICC profile" for every wide-gamut file.
std::wstring IccDescription(const Exiv2::DataBuf& profile){
 const uint8_t* p=profile.c_data();
 size_t n=profile.size();
 if(!p||n<132)return {};
 auto be32=[&](size_t at)->uint32_t{
  return (uint32_t(p[at])<<24)|(uint32_t(p[at+1])<<16)|(uint32_t(p[at+2])<<8)|p[at+3];
 };
 uint32_t count=be32(128);
 if(count>512||132+size_t(count)*12>n)return {};
 for(uint32_t i=0;i<count;i++){
  size_t entry=132+size_t(i)*12;
  if(be32(entry)!=0x64657363u)continue;              // 'desc'
  size_t offset=be32(entry+4),length=be32(entry+8);
  if(offset+length>n||length<12)return {};
  uint32_t type=be32(offset);
  if(type==0x64657363u){                             // ICC v2 'desc'
   size_t chars=be32(offset+8);
   if(!chars||offset+12+chars>n)return {};
   while(chars&&!p[offset+12+chars-1])chars--;
   return Widen(std::string(reinterpret_cast<const char*>(p+offset+12),chars));
  }
  if(type==0x6D6C7563u){                             // ICC v4 'mluc'
   if(offset+28>n||!be32(offset+8))return {};
   size_t chars=be32(offset+20),at=be32(offset+24);
   if(!chars||offset+at+chars>n)return {};
   std::wstring out;                                 // UTF-16 big endian
   for(size_t j=0;j+1<chars;j+=2)
    out+=wchar_t((uint16_t(p[offset+at+j])<<8)|p[offset+at+j+1]);
   while(!out.empty()&&out.back()==L'\0')out.pop_back();
   return out;
  }
  return {};
 }
 return {};
}

void ReadXmpInto(const Exiv2::XmpData& xmp,MetadataRecord& out,const wchar_t* source){
 if(xmp.empty())return;
 bool ok=false;
 if(auto rating=FindXmp(xmp,"Xmp.xmp.Rating")){
  double v=Number(rating,ok);
  if(ok){
   int value=int(v<0?v-0.5:v+0.5);
   out.rating=(std::max)(-1,(std::min)(5,value));
   // 0 means "unrated", which is indistinguishable from carrying no rating.
   out.hasRating=out.rating!=0;
   if(out.hasRating)out.xmpSource=source;
  }
 }
 auto label=Text(FindXmp(xmp,"Xmp.xmp.Label"));
 if(!label.empty()){out.label=label;if(out.xmpSource.empty())out.xmpSource=source;}
 if(out.creatorTool.empty())out.creatorTool=Text(FindXmp(xmp,"Xmp.xmp.CreatorTool"));
 if(out.lens.empty())out.lens=Text(FindXmp(xmp,"Xmp.aux.Lens"));
 if(out.title.empty())out.title=Text(FindXmp(xmp,"Xmp.dc.title"));
 if(out.description.empty())out.description=Text(FindXmp(xmp,"Xmp.dc.description"));
 if(out.author.empty())out.author=Text(FindXmp(xmp,"Xmp.dc.creator"));
 if(out.copyright.empty())out.copyright=Text(FindXmp(xmp,"Xmp.dc.rights"));
 if(auto subject=FindXmp(xmp,"Xmp.dc.subject")){
  try{
   for(size_t i=0;i<subject->count();i++){
    auto word=Widen(subject->toString(i));
    if(!word.empty()&&std::find(out.keywords.begin(),out.keywords.end(),word)==out.keywords.end())
     out.keywords.push_back(word);
   }
  }catch(...){}
 }
}

void ReadIptcInto(const Exiv2::IptcData& iptc,MetadataRecord& out){
 for(const auto& d:iptc){
  try{
   auto key=d.key();
   if(key=="Iptc.Application2.Keywords"){
    auto word=Widen(d.toString());
    if(!word.empty()&&std::find(out.keywords.begin(),out.keywords.end(),word)==out.keywords.end())
     out.keywords.push_back(word);
   }
   else if(key=="Iptc.Application2.Byline"&&out.author.empty())out.author=Widen(d.toString());
   else if(key=="Iptc.Application2.Copyright"&&out.copyright.empty())out.copyright=Widen(d.toString());
   else if(key=="Iptc.Application2.ObjectName"&&out.title.empty())out.title=Widen(d.toString());
   else if(key=="Iptc.Application2.Caption"&&out.description.empty())out.description=Widen(d.toString());
  }catch(...){}
 }
}

void ReadExifInto(const Exiv2::ExifData& exif,MetadataRecord& out){
 if(exif.empty())return;
 bool ok=false;
 // easyaccess knows where each manufacturer hides these, MakerNotes included:
 // a Nikon lens name lives in the MakerNote, not in Exif.Photo.LensModel.
 out.cameraMake=PrintedAt(exif,Exiv2::make(exif));
 out.cameraModel=PrintedAt(exif,Exiv2::model(exif));
 out.lens=PrintedAt(exif,Exiv2::lensName(exif));
 out.lensMake=Text(Find(exif,"Exif.Photo.LensMake"));
 out.serial=PrintedAt(exif,Exiv2::serialNumber(exif));
 out.metering=PrintedAt(exif,Exiv2::meteringMode(exif));
 out.flash=PrintedAt(exif,Exiv2::flash(exif));
 out.whiteBalance=PrintedAt(exif,Exiv2::whiteBalance(exif));
 out.exposureProgram=Printed(exif,"Exif.Photo.ExposureProgram");
 {
  auto it=Exiv2::dateTimeOriginal(exif);
  if(it!=exif.end())out.dateTaken=Text(&*it);
 }
 {
  auto it=Exiv2::orientation(exif);
  if(it!=exif.end()){double v=Number(&*it,ok);if(ok&&v>=1&&v<=8)out.orientation=unsigned(v);}
 }
 {
  auto it=Exiv2::isoSpeed(exif);
  if(it!=exif.end()){double v=Number(&*it,ok);if(ok&&v>0)out.iso=long(v+.5);}
 }
 {
  auto it=Exiv2::focalLength(exif);
  if(it!=exif.end()){double v=Number(&*it,ok);if(ok)out.focalLength=v;}
 }
 {
  auto it=Exiv2::fNumber(exif);
  if(it!=exif.end()){double v=Number(&*it,ok);if(ok)out.aperture=v;}
  if(out.aperture<=0){
   auto value=Exiv2::apertureValue(exif);
   if(value!=exif.end()){double v=Number(&*value,ok);if(ok)out.aperture=std::pow(2.0,v/2.0);}
  }
 }
 {
  auto it=Exiv2::exposureTime(exif);
  if(it!=exif.end()){double v=Number(&*it,ok);if(ok)out.shutter=v;}
 }
 {
  auto it=Exiv2::exposureBiasValue(exif);
  if(it!=exif.end()){double v=Number(&*it,ok);if(ok)out.exposureBias=v;}
 }
 {double v=Number(Find(exif,"Exif.Photo.FocalLengthIn35mmFilm"),ok);if(ok)out.focalLength35=v;}
 {
  double v=Number(Find(exif,"Exif.Photo.ColorSpace"),ok);
  if(ok)out.colourSpace=v==1?L"sRGB":(v==65535?L"Uncalibrated":L"Adobe RGB");
 }
 if(out.author.empty())out.author=Text(Find(exif,"Exif.Image.Artist"));
 if(out.copyright.empty())out.copyright=Text(Find(exif,"Exif.Image.Copyright"));
 if(out.description.empty())out.description=Text(Find(exif,"Exif.Image.ImageDescription"));
 if(out.creatorTool.empty())out.creatorTool=Text(Find(exif,"Exif.Image.Software"));

 auto lat=Find(exif,"Exif.GPSInfo.GPSLatitude"),lon=Find(exif,"Exif.GPSInfo.GPSLongitude");
 if(lat&&lon){
  double la=Sexagesimal(*lat,Text(Find(exif,"Exif.GPSInfo.GPSLatitudeRef")));
  double lo=Sexagesimal(*lon,Text(Find(exif,"Exif.GPSInfo.GPSLongitudeRef")));
  if(la||lo){
   out.lat=la;out.lon=lo;out.hasGps=true;
   double alt=Number(Find(exif,"Exif.GPSInfo.GPSAltitude"),ok);
   if(ok){
    bool hasRef=false;
    double sign=Number(Find(exif,"Exif.GPSInfo.GPSAltitudeRef"),hasRef);
    out.altitude=(hasRef&&sign==1)?-alt:alt;
   }
  }
 }
}

bool started=false;
void EnsureStarted(){
 // XMPsdk keeps process-wide state; Exiv2 wants it started exactly once.
 static const bool once=[]{
  Exiv2::XmpParser::initialize();
  Exiv2::enableBMFF(false);
  // Exiv2 writes warnings about slightly non-conforming EXIF to stderr, which
  // in a windowed application goes nowhere useful and in a test run buries the
  // results. Anything that matters comes back in MetadataRecord::warning.
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
  return true;
 }();
 started=once;
}

}   // namespace

bool MetadataReaderAvailable(){return started;}

std::wstring SidecarFor(const std::wstring& path){
 std::error_code ec;
 fs::path file(path);
 // Lightroom writes "DSC_1248.xmp"; Bridge and some versions write
 // "DSC_1248.NEF.xmp". Prefer the bare stem, which is Adobe's default.
 fs::path stem=file;stem.replace_extension(L".xmp");
 if(fs::exists(stem,ec))return stem.wstring();
 fs::path suffixed=file;suffixed+=L".xmp";
 if(fs::exists(suffixed,ec))return suffixed.wstring();
 fs::path upper=file;upper.replace_extension(L".XMP");
 if(fs::exists(upper,ec))return upper.wstring();
 return {};
}

bool SidecarStamp(const std::wstring& path,uint64_t& size,uint64_t& written){
 size=written=0;
 auto sidecar=SidecarFor(path);
 if(sidecar.empty())return false;
 WIN32_FILE_ATTRIBUTE_DATA info{};
 if(!GetFileAttributesExW(sidecar.c_str(),GetFileExInfoStandard,&info))return false;
 size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
 written=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
 return true;
}

MetadataRecord ReadMetadata(const std::wstring& path){
 MetadataRecord out;
 EnsureStarted();
 auto extension=fs::path(path).extension().wstring();
 for(auto& c:extension)c=towlower(c);
 out.format=extension.empty()?std::wstring():extension.substr(1);
 for(auto& c:out.format)c=towupper(c);

 std::vector<uint8_t> bytes;
 if(ReadWhole(path,bytes)){
  try{
   auto image=Exiv2::ImageFactory::open(bytes.data(),bytes.size());
   if(image){
    image->readMetadata();
    out.reader=L"Exiv2 "+Widen(Exiv2::versionString());
    out.mime=Widen(image->mimeType());
    if(image->pixelWidth()&&image->pixelHeight()){
     out.width=image->pixelWidth();out.height=image->pixelHeight();
    }
    ReadExifInto(image->exifData(),out);
    ReadIptcInto(image->iptcData(),out);
    ReadXmpInto(image->xmpData(),out,L"embedded");
    if(image->iccProfileDefined()){
     out.hasIcc=true;
     out.iccProfile=IccDescription(image->iccProfile());
    }
    out.ready=true;
   }
  }catch(const std::exception& failure){
   out.warning=L"Exiv2: "+Widen(failure.what());
  }catch(...){
   out.warning=L"Exiv2: unreadable metadata";
  }
 }else out.warning=L"The file could not be read for metadata.";

 // A sidecar is the more recent statement about rating and label: Lightroom
 // edits it without touching the RAW. It therefore wins over the embedded
 // packet for those two fields, and only fills in the rest where it is empty.
 auto sidecar=SidecarFor(path);
 if(!sidecar.empty()){
  std::vector<uint8_t> side;
  if(ReadWhole(sidecar,side)&&!side.empty()){
   try{
    Exiv2::XmpData xmp;
    std::string packet(reinterpret_cast<const char*>(side.data()),side.size());
    if(Exiv2::XmpParser::decode(xmp,packet)==0&&!xmp.empty()){
     MetadataRecord fromSidecar;
     ReadXmpInto(xmp,fromSidecar,L"sidecar");
     if(fromSidecar.hasRating){out.hasRating=true;out.rating=fromSidecar.rating;out.xmpSource=L"sidecar";}
     if(!fromSidecar.label.empty()){out.label=fromSidecar.label;out.xmpSource=L"sidecar";}
     if(!fromSidecar.creatorTool.empty())out.creatorTool=fromSidecar.creatorTool;
     if(out.lens.empty())out.lens=fromSidecar.lens;
     if(out.title.empty())out.title=fromSidecar.title;
     if(out.description.empty())out.description=fromSidecar.description;
     if(out.author.empty())out.author=fromSidecar.author;
     if(out.copyright.empty())out.copyright=fromSidecar.copyright;
     for(auto& word:fromSidecar.keywords)
      if(std::find(out.keywords.begin(),out.keywords.end(),word)==out.keywords.end())
       out.keywords.push_back(word);
     out.ready=true;
    }else if(out.warning.empty())out.warning=L"The XMP sidecar could not be parsed.";
   }catch(...){
    // A damaged sidecar must never cost the file its own metadata.
    if(out.warning.empty())out.warning=L"The XMP sidecar is damaged and was ignored.";
   }
  }
 }
 return out;
}
