// Vetro Look, GPL-3.0-or-later.
// Shell-provided media facts and poster frames. See shellmedia.h.
#include "shellmedia.h"
#include <windows.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
using Microsoft::WRL::ComPtr;

namespace{

// Properties are named, not hard-coded: `System.Media.Duration` is stable across
// Windows versions and readable here, where a raw {GUID, 3} pair would be neither.
// The lookup is resolved once per name and then cached for the process.
const PROPERTYKEY* Key(const wchar_t* name){
 static std::unordered_map<std::wstring,PROPERTYKEY> cache;
 static std::mutex guard;
 std::lock_guard lock(guard);
 auto it=cache.find(name);
 if(it==cache.end()){
  PROPERTYKEY key{};
  if(FAILED(PSGetPropertyKeyFromName(name,&key)))return nullptr;
  it=cache.emplace(name,key).first;
 }
 return &it->second;
}
// A property read that leaves the output alone when the handler has nothing to
// say. A file whose duration is unknown must show no duration, not a zero.
bool ReadU64(IPropertyStore* store,const PROPERTYKEY& key,uint64_t& out){
 PROPVARIANT value;PropVariantInit(&value);
 bool ok=false;
 if(SUCCEEDED(store->GetValue(key,&value))){
  ULONGLONG number=0;
  if(SUCCEEDED(PropVariantToUInt64(value,&number))&&number){out=number;ok=true;}
 }
 PropVariantClear(&value);
 return ok;
}
bool ReadU32(IPropertyStore* store,const PROPERTYKEY& key,unsigned& out){
 uint64_t number=0;
 if(!ReadU64(store,key,number))return false;
 out=unsigned((std::min)(number,uint64_t(0xFFFFFFFFull)));
 return true;
}
bool ReadText(IPropertyStore* store,const PROPERTYKEY& key,std::wstring& out){
 PROPVARIANT value;PropVariantInit(&value);
 bool ok=false;
 if(SUCCEEDED(store->GetValue(key,&value))){
  wchar_t buffer[256]{};
  if(SUCCEEDED(PropVariantToString(value,buffer,ARRAYSIZE(buffer)))&&buffer[0]){out=buffer;ok=true;}
 }
 PropVariantClear(&value);
 return ok;
}
bool NamedU64(IPropertyStore* store,const wchar_t* name,uint64_t& out){
 auto key=Key(name);return key&&ReadU64(store,*key,out);
}
bool NamedU32(IPropertyStore* store,const wchar_t* name,unsigned& out){
 auto key=Key(name);return key&&ReadU32(store,*key,out);
}
bool NamedText(IPropertyStore* store,const wchar_t* name,std::wstring& out){
 auto key=Key(name);return key&&ReadText(store,*key,out);
}
// The shell hands out a 32-bit top-down DIB. D2D wants premultiplied BGRA, and
// a video thumbnail arrives opaque, so the copy is straight -- but an alpha
// channel of zeros would make the poster invisible, and some handlers do return
// that, so a fully transparent result is treated as opaque.
std::shared_ptr<Image> FromBitmap(HBITMAP bitmap){
 BITMAP header{};
 if(!GetObjectW(bitmap,sizeof header,&header)||!header.bmWidth||!header.bmHeight)return {};
 unsigned w=unsigned(header.bmWidth),h=unsigned(abs(header.bmHeight));
 auto image=std::make_shared<Image>();
 image->w=w;image->h=h;image->pixels.resize(size_t(w)*h*4);
 BITMAPINFO info{};
 info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
 info.bmiHeader.biWidth=LONG(w);
 info.bmiHeader.biHeight=-LONG(h);               // top-down
 info.bmiHeader.biPlanes=1;
 info.bmiHeader.biBitCount=32;
 info.bmiHeader.biCompression=BI_RGB;
 HDC screen=GetDC(nullptr);
 int copied=GetDIBits(screen,bitmap,0,h,image->pixels.data(),&info,DIB_RGB_COLORS);
 ReleaseDC(nullptr,screen);
 if(copied<=0)return {};
 bool anyAlpha=false;
 for(size_t i=3;i<image->pixels.size();i+=4)if(image->pixels[i]){anyAlpha=true;break;}
 if(!anyAlpha)for(size_t i=3;i<image->pixels.size();i+=4)image->pixels[i]=255;
 image->sourceW=w;image->sourceH=h;
 image->codec=L"Shell thumbnail";
 image->tier=TierScreenRes;
 return image;
}

}

MediaFacts ReadMediaFacts(const std::wstring& path){
 MediaFacts facts;
 WIN32_FILE_ATTRIBUTE_DATA attributes{};
 if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes))
  facts.bytes=(uint64_t(attributes.nFileSizeHigh)<<32)|attributes.nFileSizeLow;
 ComPtr<IPropertyStore> store;
 if(FAILED(SHGetPropertyStoreFromParsingName(path.c_str(),nullptr,GPS_DEFAULT,
     IID_PPV_ARGS(&store)))||!store)
  return facts;
 uint64_t hundredNanoseconds=0;
 // Duration is in 100-nanosecond units, the unit every Windows media API uses.
 if(NamedU64(store.Get(),L"System.Media.Duration",hundredNanoseconds))
  facts.seconds=double(hundredNanoseconds)/1e7;
 NamedU32(store.Get(),L"System.Video.FrameWidth",facts.width);
 NamedU32(store.Get(),L"System.Video.FrameHeight",facts.height);
 unsigned rate=0;
 // System.Video.FrameRate is frames per 1000 seconds, which is how 23.976 is
 // reported exactly, as 23976.
 if(NamedU32(store.Get(),L"System.Video.FrameRate",rate))facts.frameRate=double(rate)/1000.0;
 NamedU32(store.Get(),L"System.Video.TotalBitrate",facts.bitrate);
 NamedU32(store.Get(),L"System.Audio.ChannelCount",facts.channels);
 NamedU32(store.Get(),L"System.Audio.SampleRate",facts.sampleRate);
 NamedText(store.Get(),L"System.Video.Compression",facts.videoCodec);
 NamedText(store.Get(),L"System.Audio.Format",facts.audioCodec);
 NamedText(store.Get(),L"System.Title",facts.title);
 facts.ready=true;
 return facts;
}

std::shared_ptr<Image> ShellPoster(const std::wstring& path,unsigned maxEdge){
 ComPtr<IShellItemImageFactory> factory;
 if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&factory)))||!factory)return {};
 HBITMAP bitmap=nullptr;
 // THUMBNAILONLY on purpose: a file-type icon is not a frame of this film, and
 // drawing one would be the viewer inventing content.
 //
 // The size ladder matters. A thumbnail provider that cannot produce the size
 // asked for fails outright rather than returning a smaller one, and the shell
 // cache tops out at 1024: asking a phone recording for 1280 pixels returns
 // nothing at all, which is how a poster frame goes missing on a file whose
 // filmstrip tile draws perfectly.
 const unsigned ladder[]={maxEdge,1024u,512u,256u};
 unsigned previous=0;
 for(unsigned edge:ladder){
  if(!edge||edge==previous||edge>4096u)continue;
  previous=edge;
  SIZE size{LONG(edge),LONG(edge)};
  if(SUCCEEDED(factory->GetImage(size,SIIGBF_THUMBNAILONLY|SIIGBF_BIGGERSIZEOK,&bitmap))&&bitmap)break;
  bitmap=nullptr;
 }
 if(!bitmap)return {};
 auto image=FromBitmap(bitmap);
 DeleteObject(bitmap);
 return image;
}

std::wstring FormatDuration(double seconds){
 if(seconds<=0)return {};
 auto total=(unsigned long long)(seconds+0.5);
 unsigned long long hours=total/3600,minutes=(total/60)%60,rest=total%60;
 wchar_t buffer[32];
 if(hours)swprintf_s(buffer,L"%llu:%02llu:%02llu",hours,minutes,rest);
 else swprintf_s(buffer,L"%llu:%02llu",minutes,rest);
 return buffer;
}
