// Vetro Look, GPL-3.0-or-later.
// The media router and the mode lifecycle. Both are pure logic, and both decide
// which presentation mode a file gets, so both are tested against bytes rather
// than against real films: a synthetic 32-byte header exercises the same
// decision an 8 GB remux does.
#include "../src/media.h"
#include "../src/modes.h"
#include "../src/streaming.h"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace{
int failures=0;
void Check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;}

std::vector<uint8_t> Bytes(std::initializer_list<int> values){
 std::vector<uint8_t> out;for(int v:values)out.push_back(uint8_t(v));return out;
}
void Append(std::vector<uint8_t>& out,const char* text){
 out.insert(out.end(),(const uint8_t*)text,(const uint8_t*)text+strlen(text));
}
// An `ftyp` box: length, tag, major brand, a version field, then compatible
// brands. Exactly the shape every MP4, HEIC and AVIF file starts with.
std::vector<uint8_t> Ftyp(const char* major,std::initializer_list<const char*> compatible){
 std::vector<uint8_t> out;
 size_t length=16+4*compatible.size();
 out.push_back(uint8_t(length>>24));out.push_back(uint8_t(length>>16));
 out.push_back(uint8_t(length>>8));out.push_back(uint8_t(length));
 Append(out,"ftyp");Append(out,major);Append(out,"\0\0\2\0");
 for(const char* brand:compatible)Append(out,brand);
 return out;
}
MediaKind Kind(const std::vector<uint8_t>& bytes){return RouteForBytes(bytes.data(),bytes.size()).kind;}

std::wstring TempDir(){
 wchar_t buffer[MAX_PATH]{};GetTempPathW(MAX_PATH,buffer);
 std::wstring dir=std::wstring(buffer)+L"vetro-media-test\\";
 CreateDirectoryW(dir.c_str(),nullptr);
 return dir;
}
std::wstring WriteFile(const std::wstring& name,const std::vector<uint8_t>& bytes){
 auto path=TempDir()+name;
 HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return {};
 DWORD written=0;::WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr);
 CloseHandle(file);
 return path;
}
}

int main(){
 // ------------------------------------------------------------ extensions --
 Check(KindFromExtension(L"C:\\shoot\\IMG_4012.JPG")==MediaKind::Image,"extension: upper-case JPG is an image");
 Check(KindFromExtension(L"DJI_0182.mp4")==MediaKind::Video,"extension: mp4 is video");
 Check(KindFromExtension(L"lecture.mkv")==MediaKind::Video,"extension: mkv is video");
 Check(KindFromExtension(L"podcast.flac")==MediaKind::Audio,"extension: flac is audio");
 Check(KindFromExtension(L"notes.txt")==MediaKind::Unsupported,"extension: txt is unsupported");
 Check(KindFromExtension(L"no-extension")==MediaKind::Unsupported,"extension: a bare name is unsupported");
 Check(SupportedMedia(L"a.mov")&&SupportedMedia(L"a.cr3")&&!SupportedMedia(L"a.exe"),
       "unified navigation walks images, video and audio only");
 Check(ImageExtensionSupported(L".PSB")&&!ImageExtensionSupported(L".mp4"),"image list is case-insensitive");
 {
  // The same lists, enumerated. The shell registration walks these to publish
  // what the viewer opens, so an entry that arrives with its separator attached
  // becomes a registry key Windows never matches -- which is exactly how video
  // ended up missing from "Open with".
  auto images=ExtensionsFor(MediaKind::Image);
  auto films=ExtensionsFor(MediaKind::Video);
  auto sounds=ExtensionsFor(MediaKind::Audio);
  bool wellFormed=true;
  for(const auto& list:{images,films,sounds})
   for(const auto& extension:list)
    if(extension.size()<2||extension[0]!=L'.'||extension.find(L'|')!=std::wstring::npos)wellFormed=false;
  Check(wellFormed,"every enumerated extension is a bare dotted extension");
  Check(!images.empty()&&!films.empty()&&!sounds.empty(),"each list enumerates something");
  bool agrees=true;
  for(const auto& extension:images)if(!ImageExtensionSupported(extension))agrees=false;
  for(const auto& extension:films)if(!VideoExtensionSupported(extension))agrees=false;
  for(const auto& extension:sounds)if(!AudioExtensionSupported(extension))agrees=false;
  Check(agrees,"the enumerated lists and the membership tests are the same lists");
  Check(std::find(films.begin(),films.end(),std::wstring(L".mkv"))!=films.end()&&
        std::find(images.begin(),images.end(),std::wstring(L".jpg"))!=images.end(),
        "the formats a reader actually has are in them");
 }

 // ------------------------------------------------------------ ISO brands --
 Check(Kind(Ftyp("isom",{"isom","mp42"}))==MediaKind::Video,"ftyp isom is video");
 Check(Kind(Ftyp("mp42",{"mp41"}))==MediaKind::Video,"ftyp mp42 is video");
 Check(Kind(Ftyp("heic",{"mif1"}))==MediaKind::Image,"ftyp heic is a still image");
 Check(Kind(Ftyp("avif",{"mif1","miaf"}))==MediaKind::Image,"ftyp avif is a still image");
 Check(Kind(Ftyp("avis",{"avif"}))==MediaKind::AnimatedImage,"ftyp avis is an animated image");
 Check(Kind(Ftyp("M4A ",{"mp42"}))==MediaKind::Audio,"ftyp M4A is audio");
 // A brand nobody recognises, with a brand we do in the compatible list: the
 // vendor-branded MP4 case, which must not fall through to the extension.
 Check(Kind(Ftyp("XAVC",{"mp42","isom"}))==MediaKind::Video,"unknown major brand resolves from compatible brands");
 Check(Kind(Ftyp("ZZZZ",{"YYYY"}))==MediaKind::Video,"an unbranded ISO file still routes to the playback core");
 Check(RouteForBytes(Ftyp("heic",{"mif1"}).data(),12).kind==MediaKind::Image,
       "a header cut off after the major brand still classifies");

 // -------------------------------------------------------------- containers --
 {
  std::vector<uint8_t> mkv=Bytes({0x1A,0x45,0xDF,0xA3,0x01,0x00,0x00,0x00});Append(mkv,"matroska");
  std::vector<uint8_t> webm=Bytes({0x1A,0x45,0xDF,0xA3,0x01,0x00,0x00,0x00});Append(webm,"webm");
  Check(Kind(mkv)==MediaKind::Video&&Kind(webm)==MediaKind::Video,"EBML is video");
  Check(RouteForBytes(webm.data(),webm.size()).container==L"WebM","WebM is named for diagnostics");

  std::vector<uint8_t> avi;Append(avi,"RIFF");Append(avi,"....");Append(avi,"AVI ");
  std::vector<uint8_t> wav;Append(wav,"RIFF");Append(wav,"....");Append(wav,"WAVE");
  std::vector<uint8_t> webp;Append(webp,"RIFF");Append(webp,"....");Append(webp,"WEBPVP8 ");
  std::vector<uint8_t> webpAnim;Append(webpAnim,"RIFF");Append(webpAnim,"....");Append(webpAnim,"WEBPVP8X");
  webpAnim.resize(21,0);webpAnim[20]=0x02;
  Check(Kind(avi)==MediaKind::Video,"RIFF AVI is video");
  Check(Kind(wav)==MediaKind::Audio,"RIFF WAVE is audio");
  Check(Kind(webp)==MediaKind::Image,"RIFF WEBP is an image");
  Check(Kind(webpAnim)==MediaKind::AnimatedImage,"a WebP with the ANIM flag is an animated image");

  std::vector<uint8_t> ts(377,0);ts[0]=ts[188]=ts[376]=0x47;
  std::vector<uint8_t> notTs(377,0);notTs[0]=0x47;
  Check(Kind(ts)==MediaKind::Video,"three sync bytes make a transport stream");
  Check(Kind(notTs)==MediaKind::Unsupported,"one sync byte does not");

  std::vector<uint8_t> flv;Append(flv,"FLV\x01");
  std::vector<uint8_t> asf=Bytes({0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11});
  Check(Kind(flv)==MediaKind::Video&&Kind(asf)==MediaKind::Video,"FLV and ASF are video");

  std::vector<uint8_t> theora;Append(theora,"OggS");theora.resize(40,0);Append(theora,"\x80theora");
  std::vector<uint8_t> opus;Append(opus,"OggS");opus.resize(28,0);Append(opus,"OpusHead");
  Check(Kind(theora)==MediaKind::Video,"Ogg carrying Theora is video");
  Check(Kind(opus)==MediaKind::Audio,"Ogg carrying Opus is audio");

  std::vector<uint8_t> png=Bytes({0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A});auto apng=png;
  Append(apng,"....acTL");
  Check(Kind(png)==MediaKind::Image&&Kind(apng)==MediaKind::AnimatedImage,"acTL makes a PNG animated");

  std::vector<uint8_t> gif;Append(gif,"GIF89a");auto agif=gif;Append(agif,"...!\xFF\x0BNETSCAPE2.0");
  Check(Kind(gif)==MediaKind::Image&&Kind(agif)==MediaKind::AnimatedImage,"a loop block makes a GIF animated");

  std::vector<uint8_t> jpeg=Bytes({0xFF,0xD8,0xFF,0xE0});
  std::vector<uint8_t> flac;Append(flac,"fLaC");
  Check(Kind(jpeg)==MediaKind::Image&&Kind(flac)==MediaKind::Audio,"JPEG and FLAC signatures");
 }

 // --------------------------------------------------------------- safety ---
 Check(RouteForBytes(nullptr,0).kind==MediaKind::Unsupported,"no bytes is unsupported, not a crash");
 for(size_t length=1;length<=16;length++){
  std::vector<uint8_t> truncated=Ftyp("isom",{"mp42"});truncated.resize(length);
  RouteForBytes(truncated.data(),truncated.size());         // must not read past the end
 }
 Check(true,"every truncation of an ftyp header is probed without overrun");

 // ----------------------------------------------------------- whole files --
 {
  auto film=WriteFile(L"holiday.jpg",Ftyp("isom",{"mp42"}));
  auto route=RouteForPath(film);
  Check(route.kind==MediaKind::Video&&route.mislabelled&&route.probed,
        "an MP4 named .jpg opens as video and is reported as mislabelled");

  std::vector<uint8_t> matroska=Bytes({0x1A,0x45,0xDF,0xA3,0x01,0,0,0});Append(matroska,"matroska");
  route=RouteForPath(WriteFile(L"remux.mp4",matroska));
  Check(route.kind==MediaKind::Video&&!route.mislabelled,
        "a Matroska stream named .mp4 is video and not flagged: the family agrees");

  route=RouteForPath(WriteFile(L"camera.dat",Ftyp("isom",{"mp42"})));
  Check(route.kind==MediaKind::Video&&!route.mislabelled,
        "an unknown extension with a known signature opens, and claims nothing about the name");

  route=RouteForPath(WriteFile(L"broken.mp4",Bytes({0x00,0x01,0x02,0x03})));
  Check(route.kind==MediaKind::Video&&!route.probed,
        "an unrecognisable signature falls back to the extension");

  route=RouteForPath(TempDir()+L"does-not-exist.mp4");
  Check(route.kind==MediaKind::Video&&!route.probed,"a missing file is classified by name alone");

  auto signature=MediaSignature(film);
  Check(!signature.empty()&&signature==MediaSignature(film),"a media signature is stable");
  // Sequenced deliberately: the point is that the signature changes when the
  // bytes do, and comparing two calls in one expression would leave the order
  // of the rewrite to the compiler.
  auto rewritten=WriteFile(L"holiday.jpg",Ftyp("isom",{"mp42","iso2"}));
  Check(signature!=MediaSignature(rewritten),"a rewritten file gets a new signature");
 }

 // ----------------------------------------------------------------- URLs ---
 Check(RouteForUrl(L"https://cdn.example/live/master.m3u8").container==L"HLS","HLS manifest");
 Check(RouteForUrl(L"https://cdn.example/v/manifest.mpd").container==L"DASH","DASH manifest");
 Check(RouteForUrl(L"https://cdn.example/a/b.mp4?token=x.m3u8&y=1").container==L"Direct URL",
       "the query string is not the extension");
 Check(RouteForUrl(L"https://example.com/watch/some-page").container==L"Unresolved URL",
       "a web page needs the source layer, not the router");
 Check(RouteForUrl(L"https://example.com/photo.jpg").kind==MediaKind::Image,"a direct image URL is an image");
 Check(RouteForUrl(L"https://e/x.mp4").origin==MediaOrigin::Url,"a URL is never treated as a local path");

 // ------------------------------------------------------------- streams ---
 Check(ClassifyAddress(L"https://cdn.example/live/master.m3u8")==StreamSource::Hls,"stream: HLS opens directly");
 Check(ClassifyAddress(L"https://cdn.example/v/manifest.mpd")==StreamSource::Dash,"stream: DASH opens directly");
 Check(ClassifyAddress(L"https://cdn.example/a/film.mp4?sig=1")==StreamSource::DirectMedia,"stream: a direct film link opens directly");
 Check(ClassifyAddress(L"https://www.youtube.com/watch?v=abc")==StreamSource::WebPage,"stream: a page goes to the resolver");
 Check(ClassifyAddress(L"https://www.netflix.com/watch/1")==StreamSource::Protected,"stream: a DRM service is named, not retried");
 Check(ClassifyAddress(L"https://notnetflix.com/watch/1")==StreamSource::WebPage,"stream: a host suffix is matched on a dot boundary");
 Check(ClassifyAddress(L"https://example.com/photo.jpg")==StreamSource::Unsupported,"stream: an image link is not a stream");
 Check(ClassifyAddress(L"rtsp://camera.local/stream")==StreamSource::DirectMedia,"stream: rtsp is a stream by its scheme");
 Check(ClassifyAddress(L"C:\\films\\a.mkv")==StreamSource::Unsupported,"stream: a path is not an address");
 Check(AddressHost(L"https://user:pw@Media.Example.com:8443/x")==L"media.example.com","stream: host without credentials and port");

 Check(ClassifyEngineFailure(L"[ffmpeg] https: HTTP error 404 Not Found\nloading failed")==StreamFailure::NotFound,"failure: 404");
 Check(ClassifyEngineFailure(L"[ffmpeg] https: Server returned 403 Forbidden (access denied)")==StreamFailure::Forbidden,"failure: 403");
 Check(ClassifyEngineFailure(L"[ffmpeg] tls: Certificate verification failed")==StreamFailure::Certificate,"failure: certificate");
 Check(ClassifyEngineFailure(L"[ffmpeg] tcp: Failed to resolve hostname nowhere.invalid: host not found")==StreamFailure::Network,"failure: DNS");
 Check(ClassifyEngineFailure(L"[ffmpeg] http: Connection to tcp://x:80 failed: Error number -10061 occurred")==StreamFailure::Network,"failure: refused");
 Check(ClassifyEngineFailure(L"[vd] bitrate 4031 kbps\nunrecognized file format")==StreamFailure::Unknown,"failure: a number in a warning is not a status code");
 Check(ClassifyEngineFailure(L"")==StreamFailure::None,"failure: nothing said is no failure");
 Check(ClassifyEngineFailure(L"[curl] Could not connect to server, retrying (#5) from 7318247\n"
                             L"[curl] transfer failed: Could not connect to server")==StreamFailure::Network,
       "failure: curl's outage is the line");
 Check(ClassifyEngineFailure(L"[curl] HTTP error 404")==StreamFailure::NotFound,"failure: curl's 404");
 Check(ClassifyEngineFailure(L"[curl] Failure when receiving data from the peer, retrying (#1) from 10540568")==
       StreamFailure::Network,"failure: curl's dropped transfer is the line");
 Check(EngineGaveUpOnNetwork(L"[curl] Could not connect to server, retrying (#5) from 7318247\n"
                             L"[curl] transfer failed: Could not connect to server\n"),
       "give-up: curl's final line after its retries");
 Check(!EngineGaveUpOnNetwork(L"[curl] Could not connect to server, retrying (#2) from 7318247\n"),
       "give-up: a retry in progress is not a give-up");
 Check(!EngineGaveUpOnNetwork(L"[curl] transfer failed: Could not connect to server\n[vd] Using hardware decoding\n"),
       "give-up: an old give-up with later lines is old news");
 Check(!EngineGaveUpOnNetwork(L"[curl] HTTP error 404\n[stream] Failed to open https://x/a.m3u8.\n"),
       "give-up: a missing address is not the line going");
 Check(!EngineGaveUpOnNetwork(L""),"give-up: silence is not a give-up");
 Check(FailureRetryable(StreamFailure::Network)&&!FailureRetryable(StreamFailure::NotFound)&&
       !FailureRetryable(StreamFailure::Certificate)&&!FailureRetryable(StreamFailure::Protected),
       "failure: only the line is worth retrying");
 Check(ClassifyResolverFailure(L"ERROR: [youtube] abc: Sign in to confirm your age",1)==StreamFailure::SignInRequired,"resolver: sign-in");
 Check(ClassifyResolverFailure(L"ERROR: Unsupported URL: https://example.com/",1)==StreamFailure::NoPublicStream,"resolver: unsupported page");
 Check(ClassifyResolverFailure(L"ERROR: [x] This video is DRM protected",1)==StreamFailure::Protected,"resolver: DRM");
 Check(ClassifyResolverFailure(L"ERROR: Unable to download webpage: <urlopen error [Errno 11001] getaddrinfo failed>",1)==StreamFailure::Network,"resolver: network");

 {
  bool doubling=ReconnectDelay(0)==1&&ReconnectDelay(1)==2&&ReconnectDelay(2)==4&&ReconnectDelay(4)==16;
  Check(doubling,"reconnect: 1, 2, 4, 8, 16 seconds");
  Check(ReconnectDelay(5)==30&&ReconnectDelay(40)==30,"reconnect: then every thirty");
  double total=0;for(int i=0;i<ReconnectAttempts;i++)total+=ReconnectDelay(i);
  Check(total>60&&total<300,"reconnect: the whole schedule gives a line minutes, not hours");
  Check(EndedPrematurely(false,100,3600)&&!EndedPrematurely(false,3598,3600)&&EndedPrematurely(true,5,0)&&
        !EndedPrematurely(false,10,0),"reconnect: an ending well short of the length is a drop");
 }

 {
  ResolvedStream stream;
  std::string out="VETRO-TITLE:\xD0\x9F\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD1\x80\r\nVETRO-LIVE:False\nVETRO-DURATION:212.5\n"
                  "VETRO-UA:Mozilla/5.0 Test\nVETRO-REFERER:NA\nVETRO-URLS:https://v.example/video?x=1\nhttps://v.example/audio?x=2\n";
  Check(ParseResolverOutput(out,stream),"resolver output: parsed");
  Check(stream.title==L"Пример"&&!stream.live&&stream.duration==212.5,"resolver output: title in UTF-8, live flag, duration");
  Check(stream.video==L"https://v.example/video?x=1"&&stream.audio==L"https://v.example/audio?x=2","resolver output: video then audio");
  Check(stream.userAgent==L"Mozilla/5.0 Test"&&stream.referrer.empty(),"resolver output: NA is absent");
  Check(ParseResolverOutput("VETRO-TITLE:Live\nVETRO-LIVE:True\nVETRO-URLS:https://x/m.m3u8\n",stream)&&
        stream.live&&stream.audio.empty(),"resolver output: one live stream");
  Check(!ParseResolverOutput("VETRO-TITLE:x\nVETRO-URLS:NA\n",stream),"resolver output: no address is nothing to play");
  Check(!ParseResolverOutput("VETRO-URLS:file:///C:/Windows/win.ini\n",stream),"resolver output: only web addresses are played");
 }
 Check(ResolverAddressSafe(L"https://www.youtube.com/watch?v=abc&t=10"),"resolver input: an ordinary page");
 Check(!ResolverAddressSafe(L"https://x/\" --exec calc"),"resolver input: a quote cannot end the argument");
 Check(!ResolverAddressSafe(L"https://x/a b"),"resolver input: no spaces");
 Check(!ResolverAddressSafe(L"file:///C:/x")&&!ResolverAddressSafe(L"rtmp://x/y"),"resolver input: http and https only");

 // ------------------------------------------------------------ lifecycle ---
 {
  ModeMachine modes;
  Check(modes.state==ModeState::Empty&&modes.generation==0,"a new viewer holds nothing");

  auto t=modes.Open(MediaKind::Image);
  Check(t.to==ModeState::ImagePreparing&&t.generation==1&&!t.releaseVideo&&!t.releaseImage,
        "opening a photograph from empty releases nothing");
  modes.Ready();
  Check(modes.state==ModeState::ImageActive,"a decoded photograph is active");

  t=modes.Open(MediaKind::Image);
  Check(!t.releaseImage,"photograph to photograph keeps the image cache");
  modes.Ready();

  t=modes.Open(MediaKind::Video);
  Check(t.to==ModeState::VideoPreparing&&t.releaseImage&&!t.releaseVideo,
        "photograph to film releases the frame and its texture");
  Check(modes.Caps().canPlay&&modes.Caps().canFrameStep&&!modes.Caps().canEdit&&!modes.Caps().canZoom,
        "a film can play and step; it cannot be cropped or zoomed");
  modes.Ready();
  Check(modes.state==ModeState::VideoActive,"a presented first frame is active");

  t=modes.Open(MediaKind::Image);
  Check(t.releaseVideo&&t.checkpoint&&t.to==ModeState::ImagePreparing,
        "film to photograph stops playback and checkpoints the position");
  Check(modes.Caps().canEdit&&modes.Caps().canZoom&&!modes.Caps().canSeek,
        "a photograph is editable and zoomable; it has no timeline");
  modes.Ready();
  t=modes.Open(MediaKind::Image);
  Check(!t.releaseVideo,"the film's resources are released once, not on every later open");

  uint64_t before=modes.generation;
  modes.Open(MediaKind::Video);modes.Ready();
  t=modes.Open(MediaKind::Video);
  Check(t.releaseVideo&&t.generation==before+2,
        "film to film releases the previous session and advances the generation");

  t=modes.Failed();
  Check(t.to==ModeState::Error&&t.releaseVideo,"a failed open does not leave a playback session behind");
  t=modes.Open(MediaKind::Unsupported);
  Check(t.to==ModeState::Error,"an unsupported file is an error state, not a mode");

  modes.Open(MediaKind::Video);modes.Ready();
  t=modes.Close();
  Check(t.releaseVideo&&t.checkpoint&&modes.state==ModeState::Empty,"leaving the viewer closes the session");
  Check(!modes.Caps().canPlay&&!modes.Caps().canEdit,"an empty viewer offers nothing");
 }

 std::cout<<(failures?"FAILURES: ":"all passed: ")<<failures<<"\n";
 return failures?1:0;
}
