// Vetro Look, GPL-3.0-or-later.
// The media router. Extension hint, bounded signature probe, one answer.
//
// Every table here is a product decision about what the unified viewer claims
// to open, so they live in one file rather than being spread across the
// decoders that happen to implement each format.
#include "media.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>

namespace{

std::wstring Lower(std::wstring s){
 std::transform(s.begin(),s.end(),s.begin(),towlower);return s;
}
std::wstring ExtensionOf(const std::wstring& path){
 std::error_code ec;
 auto e=std::filesystem::path(path).extension().wstring();
 return Lower(std::move(e));
}
bool InList(const wchar_t* list,const std::wstring& ext){
 return !ext.empty()&&std::wstring(list).find(L"|"+ext+L"|")!=std::wstring::npos;
}
// Images: exactly what the decode ladder in decoders.cpp implements.
constexpr const wchar_t* ImageExtensions=
 L"|.jpg|.jpeg|.jfif|.png|.gif|.webp|.bmp|.tif|.tiff|.ico|.heic|.heif|.avif|.exr|.psd|.psb"
 L"|.cr2|.cr3|.nef|.arw|.dng|.raf|.rw2|.orf|.pef|";
// Video: containers a mature playback core handles. The list is about what the
// viewer offers to route, not about which codecs decode on a given machine --
// that is the capability layer's answer, made later and per machine.
constexpr const wchar_t* VideoExtensions=
 L"|.mp4|.m4v|.mov|.qt|.mkv|.webm|.avi|.wmv|.asf|.flv|.f4v|.mpg|.mpeg|.mpe|.m1v|.m2v"
 L"|.ts|.m2ts|.mts|.m2t|.tp|.vob|.mod|.tod|.ogv|.ogm|.3gp|.3g2|.rm|.rmvb|.divx|.mxf"
 L"|.y4m|.ivf|.dv|.wtv|.dvr-ms|.amv|.mpv|";
constexpr const wchar_t* AudioExtensions=
 L"|.mp3|.m4a|.m4b|.aac|.flac|.wav|.wave|.ogg|.oga|.opus|.spx|.wma|.aiff|.aif|.aifc"
 L"|.ape|.mka|.dts|.ac3|.eac3|.tta|.wv|.mp2|.mpa|";

// ---------------------------------------------------------------- probe ----
// Every reader below is bounds-checked against the header it was handed: a
// four-byte file must not be able to walk off the end of a probe.
bool Has(const uint8_t* b,size_t size,size_t at,const char* tag,size_t length){
 if(at+length>size)return false;
 return memcmp(b+at,tag,length)==0;
}
bool Tag4(const uint8_t* b,size_t size,size_t at,const char* tag){return Has(b,size,at,tag,4);}
uint32_t Be32(const uint8_t* b,size_t at){
 return (uint32_t(b[at])<<24)|(uint32_t(b[at+1])<<16)|(uint32_t(b[at+2])<<8)|uint32_t(b[at+3]);
}
bool Contains(const uint8_t* b,size_t size,const char* needle){
 size_t length=strlen(needle);
 if(size<length)return false;
 for(size_t i=0;i+length<=size;i++)if(memcmp(b+i,needle,length)==0)return true;
 return false;
}
// ISO base media brands. The same box introduces a film, a still photograph and
// an audio file, so the brand -- not the box -- decides the kind.
struct Brand{const char* tag;MediaKind kind;const wchar_t* name;};
constexpr Brand Brands[]={
 {"avif",MediaKind::Image,L"AVIF"},      {"avio",MediaKind::Image,L"AVIF"},
 {"avis",MediaKind::AnimatedImage,L"AVIF sequence"},
 {"heic",MediaKind::Image,L"HEIF"},      {"heix",MediaKind::Image,L"HEIF"},
 {"heim",MediaKind::Image,L"HEIF"},      {"heis",MediaKind::Image,L"HEIF"},
 {"mif1",MediaKind::Image,L"HEIF"},      {"miaf",MediaKind::Image,L"HEIF"},
 {"msf1",MediaKind::AnimatedImage,L"HEIF sequence"},
 {"hevc",MediaKind::AnimatedImage,L"HEIF sequence"},
 {"hevx",MediaKind::AnimatedImage,L"HEIF sequence"},
 {"M4A ",MediaKind::Audio,L"MP4 audio"}, {"M4B ",MediaKind::Audio,L"MP4 audio"},
 {"F4A ",MediaKind::Audio,L"MP4 audio"},
 {"isom",MediaKind::Video,L"MP4"},       {"iso2",MediaKind::Video,L"MP4"},
 {"iso4",MediaKind::Video,L"MP4"},       {"iso5",MediaKind::Video,L"MP4"},
 {"iso6",MediaKind::Video,L"MP4"},       {"mp41",MediaKind::Video,L"MP4"},
 {"mp42",MediaKind::Video,L"MP4"},       {"mp4v",MediaKind::Video,L"MP4"},
 {"avc1",MediaKind::Video,L"MP4"},       {"dash",MediaKind::Video,L"MP4"},
 {"M4V ",MediaKind::Video,L"MP4"},       {"M4VH",MediaKind::Video,L"MP4"},
 {"M4VP",MediaKind::Video,L"MP4"},       {"M4P ",MediaKind::Video,L"MP4"},
 {"qt  ",MediaKind::Video,L"QuickTime"}, {"3gp4",MediaKind::Video,L"3GP"},
 {"3gp5",MediaKind::Video,L"3GP"},       {"3gp6",MediaKind::Video,L"3GP"},
 {"3g2a",MediaKind::Video,L"3GP2"},      {"3g2b",MediaKind::Video,L"3GP2"},
 {"crx ",MediaKind::Image,L"Canon CR3"},
};
bool BrandKind(const uint8_t* b,size_t size,size_t at,MediaKind& kind,std::wstring& name){
 if(at+4>size)return false;
 for(const auto& brand:Brands)
  if(memcmp(b+at,brand.tag,4)==0){kind=brand.kind;name=brand.name;return true;}
 return false;
}
// `ftyp`: the major brand, then every compatible brand in the box. A file whose
// major brand is unknown to us is still classifiable from the compatible list,
// which is how `mif1`-only HEIF and vendor-branded MP4 both land correctly.
bool IsoBmff(const uint8_t* b,size_t size,MediaRoute& route){
 if(!Tag4(b,size,4,"ftyp"))return false;
 uint32_t box=size>=4?Be32(b,0):0;
 size_t end=(std::min)(size,size_t(box>16&&box<HeaderProbeBytes?box:size));
 MediaKind kind=MediaKind::Unsupported;std::wstring name;
 if(BrandKind(b,size,8,kind,name)){route.kind=kind;route.container=name;return true;}
 for(size_t at=16;at+4<=end;at+=4)
  if(BrandKind(b,size,at,kind,name)){route.kind=kind;route.container=name;return true;}
 // An ISO file we cannot brand is still an ISO file, and the playback core is
 // far more likely to be right about it than the extension is.
 route.kind=MediaKind::Video;route.container=L"ISO base media";
 return true;
}
// Matroska and WebM share the EBML header; the DocType separates them, and
// within Matroska a track-less audio file keeps the same container.
bool Ebml(const uint8_t* b,size_t size,MediaRoute& route){
 static const uint8_t magic[4]={0x1A,0x45,0xDF,0xA3};
 if(size<4||memcmp(b,magic,4)!=0)return false;
 if(Contains(b,size,"webm")){route.kind=MediaKind::Video;route.container=L"WebM";return true;}
 route.kind=MediaKind::Video;route.container=L"Matroska";
 return true;
}
bool Riff(const uint8_t* b,size_t size,MediaRoute& route){
 if(!Tag4(b,size,0,"RIFF"))return false;
 if(Tag4(b,size,8,"WEBP")){
  // VP8X carries the feature flags; bit 1 of the first flag byte is ANIM.
  bool animated=Tag4(b,size,12,"VP8X")&&size>20&&(b[20]&0x02);
  route.kind=animated?MediaKind::AnimatedImage:MediaKind::Image;
  route.container=animated?L"Animated WebP":L"WebP";
  return true;
 }
 if(Tag4(b,size,8,"AVI ")){route.kind=MediaKind::Video;route.container=L"AVI";return true;}
 if(Tag4(b,size,8,"WAVE")){route.kind=MediaKind::Audio;route.container=L"WAV";return true;}
 return false;
}
// 188-byte packets, each starting with the sync byte. Two further packets are
// checked so that a file that merely begins with 0x47 is not taken for a
// transport stream.
bool TransportStream(const uint8_t* b,size_t size){
 if(size<377||b[0]!=0x47)return false;
 return b[188]==0x47&&b[376]==0x47;
}
bool Ogg(const uint8_t* b,size_t size,MediaRoute& route){
 if(!Tag4(b,size,0,"OggS"))return false;
 if(Contains(b,size,"theora")||Contains(b,size,"\x80theora")||Contains(b,size,"OVP80")){
  route.kind=MediaKind::Video;route.container=L"Ogg video";return true;
 }
 route.kind=MediaKind::Audio;
 route.container=Contains(b,size,"OpusHead")?L"Opus":Contains(b,size,"FLAC")?L"Ogg FLAC":L"Ogg audio";
 return true;
}
// PNG carries its animation in a chunk, not in the signature: acTL before the
// first frame makes it an APNG.
bool Png(const uint8_t* b,size_t size,MediaRoute& route){
 static const uint8_t magic[8]={0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
 if(size<8||memcmp(b,magic,8)!=0)return false;
 bool animated=Contains(b,size,"acTL");
 route.kind=animated?MediaKind::AnimatedImage:MediaKind::Image;
 route.container=animated?L"APNG":L"PNG";
 return true;
}
// A GIF with a loop-control extension is animated. A single-frame GIF has no
// such block, and is a still photograph as far as the viewer is concerned.
bool Gif(const uint8_t* b,size_t size,MediaRoute& route){
 if(!Has(b,size,0,"GIF8",4))return false;
 bool animated=Contains(b,size,"NETSCAPE2.0")||Contains(b,size,"ANIMEXTS1.0");
 route.kind=animated?MediaKind::AnimatedImage:MediaKind::Image;
 route.container=animated?L"Animated GIF":L"GIF";
 return true;
}

} // namespace

std::vector<std::wstring> ExtensionsFor(MediaKind kind){
 const wchar_t* list=kind==MediaKind::Image?ImageExtensions:
                     kind==MediaKind::Video?VideoExtensions:
                     kind==MediaKind::Audio?AudioExtensions:nullptr;
 std::vector<std::wstring> out;
 if(!list)return out;
 std::wstring text(list);
 for(size_t start=text.find(L'|');start!=std::wstring::npos;){
  size_t end=text.find(L'|',start+1);
  if(end==std::wstring::npos)break;
  // Past the separator, not from it: the lists are bar-delimited, and a leading
  // bar would turn every extension into a key nobody ever reads.
  if(end>start+1)out.push_back(text.substr(start+1,end-start-1));
  start=end;
 }
 return out;
}

bool ImageExtensionSupported(const std::wstring& ext){return InList(ImageExtensions,Lower(ext));}
bool VideoExtensionSupported(const std::wstring& ext){return InList(VideoExtensions,Lower(ext));}
bool AudioExtensionSupported(const std::wstring& ext){return InList(AudioExtensions,Lower(ext));}

MediaKind KindFromExtension(const std::wstring& path){
 auto ext=ExtensionOf(path);
 if(ImageExtensionSupported(ext))return MediaKind::Image;
 if(VideoExtensionSupported(ext))return MediaKind::Video;
 if(AudioExtensionSupported(ext))return MediaKind::Audio;
 return MediaKind::Unsupported;
}
bool SupportedMedia(const std::wstring& path){return KindFromExtension(path)!=MediaKind::Unsupported;}

MediaRoute RouteForBytes(const uint8_t* bytes,size_t size){
 MediaRoute route;
 if(!bytes||!size)return route;
 route.probed=true;
 if(IsoBmff(bytes,size,route))return route;
 if(Ebml(bytes,size,route))return route;
 if(Riff(bytes,size,route))return route;
 if(Ogg(bytes,size,route))return route;
 if(Png(bytes,size,route))return route;
 if(Gif(bytes,size,route))return route;
 struct Simple{const char* tag;size_t length;MediaKind kind;const wchar_t* name;};
 static const Simple simple[]={
  {"\xFF\xD8\xFF",3,MediaKind::Image,L"JPEG"},
  {"BM",2,MediaKind::Image,L"BMP"},
  {"II\x2A\x00",4,MediaKind::Image,L"TIFF"},
  {"MM\x00\x2A",4,MediaKind::Image,L"TIFF"},
  {"8BPS",4,MediaKind::Image,L"Photoshop"},
  {"\x76\x2F\x31\x01",4,MediaKind::Image,L"OpenEXR"},
  {"\xFF\x0A",2,MediaKind::Image,L"JPEG XL"},
  {"\x00\x00\x00\x0CJXL \x0D\x0A\x87\x0A",12,MediaKind::Image,L"JPEG XL"},
  {"\x00\x00\x01\x00",4,MediaKind::Image,L"Icon"},
  {"FLV\x01",4,MediaKind::Video,L"FLV"},
  {"\x30\x26\xB2\x75\x8E\x66\xCF\x11",8,MediaKind::Video,L"ASF"},
  {"\x00\x00\x01\xBA",4,MediaKind::Video,L"MPEG program stream"},
  {"\x00\x00\x01\xB3",4,MediaKind::Video,L"MPEG video"},
  {"\x06\x0E\x2B\x34",4,MediaKind::Video,L"MXF"},
  {"DKIF",4,MediaKind::Video,L"IVF"},
  {"YUV4MPEG2",9,MediaKind::Video,L"Y4M"},
  {".RMF",4,MediaKind::Video,L"RealMedia"},
  {"fLaC",4,MediaKind::Audio,L"FLAC"},
  {"ID3",3,MediaKind::Audio,L"MP3"},
  {"wvpk",4,MediaKind::Audio,L"WavPack"},
  {"MAC ",4,MediaKind::Audio,L"Monkey's Audio"},
  {"\x0B\x77",2,MediaKind::Audio,L"AC-3"},
 };
 for(const auto& entry:simple)
  if(Has(bytes,size,0,entry.tag,entry.length)){
   route.kind=entry.kind;route.container=entry.name;return route;
  }
 if(Tag4(bytes,size,0,"FORM")&&(Tag4(bytes,size,8,"AIFF")||Tag4(bytes,size,8,"AIFC"))){
  route.kind=MediaKind::Audio;route.container=L"AIFF";return route;
 }
 if(TransportStream(bytes,size)){route.kind=MediaKind::Video;route.container=L"MPEG transport stream";return route;}
 // An MPEG audio frame sync: eleven set bits, and a layer that is not the
 // reserved value. Checked last, because it is the weakest signature here.
 if(size>=2&&bytes[0]==0xFF&&(bytes[1]&0xE0)==0xE0&&(bytes[1]&0x06)!=0){
  route.kind=MediaKind::Audio;route.container=L"MPEG audio";return route;
 }
 route.probed=true;route.kind=MediaKind::Unsupported;
 return route;
}

MediaRoute RouteForPath(const std::wstring& path){
 MediaRoute route;
 route.origin=MediaOrigin::LocalFile;
 auto hint=KindFromExtension(path);
 route.kind=hint;
 // Sharing flags matter: a file still being written by a camera import or a
 // screen recorder must not be locked out of being classified.
 HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,
  FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
  FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
 if(file==INVALID_HANDLE_VALUE)return route;          // the name is all we have
 uint8_t header[HeaderProbeBytes];
 DWORD read=0;
 bool ok=ReadFile(file,header,DWORD(sizeof header),&read,nullptr)&&read>0;
 CloseHandle(file);
 if(!ok)return route;
 auto probed=RouteForBytes(header,read);
 if(probed.kind==MediaKind::Unsupported)return route; // signature unknown: trust the name
 probed.origin=MediaOrigin::LocalFile;
 // A still and an animated form of the same format are not a mislabelling, and
 // neither is an extension that simply has no entry in our tables.
 bool sameFamily=(probed.IsImage()&&(hint==MediaKind::Image))||
                 (probed.kind==MediaKind::Video&&hint==MediaKind::Video)||
                 (probed.kind==MediaKind::Audio&&hint==MediaKind::Audio);
 probed.mislabelled=(hint!=MediaKind::Unsupported&&!sameFamily);
 return probed;
}

bool LooksLikeUrl(const std::wstring& text){
 auto lower=Lower(text);
 return lower.rfind(L"http://",0)==0||lower.rfind(L"https://",0)==0||
        lower.rfind(L"rtsp://",0)==0||lower.rfind(L"rtmp://",0)==0||
        lower.rfind(L"srt://",0)==0||lower.rfind(L"udp://",0)==0;
}

MediaRoute RouteForUrl(const std::wstring& url){
 MediaRoute route;
 route.origin=MediaOrigin::Url;
 auto lower=Lower(url);
 // Strip the query and fragment before looking at the path's extension: a
 // signed CDN URL carries far more after the `?` than before it.
 auto cut=lower.find_first_of(L"?#");
 auto stem=cut==std::wstring::npos?lower:lower.substr(0,cut);
 auto dot=stem.find_last_of(L'.');
 auto slash=stem.find_last_of(L'/');
 std::wstring ext=(dot!=std::wstring::npos&&(slash==std::wstring::npos||dot>slash))?stem.substr(dot):L"";
 if(ext==L".m3u8"){route.kind=MediaKind::Video;route.container=L"HLS";return route;}
 if(ext==L".mpd"){route.kind=MediaKind::Video;route.container=L"DASH";return route;}
 if(VideoExtensionSupported(ext)){route.kind=MediaKind::Video;route.container=L"Direct URL";return route;}
 if(AudioExtensionSupported(ext)){route.kind=MediaKind::Audio;route.container=L"Direct URL";return route;}
 if(ImageExtensionSupported(ext)){route.kind=MediaKind::Image;route.container=L"Direct URL";return route;}
 // Anything else needs the source layer -- and possibly a resolver -- to say
 // what it is. Routing it as video is the only useful guess, and the open path
 // is what reports failure, not the router.
 route.kind=MediaKind::Video;route.container=L"Unresolved URL";
 return route;
}

std::wstring MediaSignature(const std::wstring& path){
 WIN32_FILE_ATTRIBUTE_DATA info{};
 uint64_t size=0,written=0;
 if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)){
  size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
  written=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
 }
 std::error_code ec;
 auto absolute=std::filesystem::absolute(std::filesystem::path(path),ec);
 auto normalised=Lower(ec?path:absolute.wstring());
 return normalised+L"|"+std::to_wstring(size)+L"|"+std::to_wstring(written);
}
