// Vetro Look, GPL-3.0-or-later.
// See streaming.h.
#include "streaming.h"
#include "media.h"
#include <algorithm>
#include <cwctype>
#include <windows.h>

namespace{

std::wstring Lower(std::wstring text){
 for(auto& c:text)c=wchar_t(towlower(c));
 return text;
}
bool Has(const std::wstring& haystack,const wchar_t* needle){
 return haystack.find(needle)!=std::wstring::npos;
}
bool HostIs(const std::wstring& host,const wchar_t* domain){
 std::wstring d=domain;
 if(host==d)return true;
 return host.size()>d.size()&&host.compare(host.size()-d.size(),d.size(),d)==0&&
        host[host.size()-d.size()-1]==L'.';
}
std::wstring Widen(const std::string& text){
 if(text.empty())return {};
 int length=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
 std::wstring out(size_t(length),L'\0');
 MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),out.data(),length);
 return out;
}
bool Available(const std::wstring& value){return !value.empty()&&value!=L"NA"&&value!=L"None";}

}

std::wstring AddressHost(const std::wstring& url){
 auto lower=Lower(url);
 auto scheme=lower.find(L"://");
 if(scheme==std::wstring::npos)return {};
 auto start=scheme+3;
 auto end=lower.find_first_of(L"/?#",start);
 auto authority=lower.substr(start,end==std::wstring::npos?std::wstring::npos:end-start);
 auto at=authority.rfind(L'@');
 if(at!=std::wstring::npos)authority=authority.substr(at+1);
 if(!authority.empty()&&authority[0]==L'['){
  auto close=authority.find(L']');
  return close==std::wstring::npos?authority:authority.substr(0,close+1);
 }
 auto colon=authority.find(L':');
 if(colon!=std::wstring::npos)authority=authority.substr(0,colon);
 return authority;
}

StreamSource ClassifyAddress(const std::wstring& url){
 if(!LooksLikeUrl(url))return StreamSource::Unsupported;
 auto host=AddressHost(url);
 if(host.empty())return StreamSource::Unsupported;
 // Services whose catalogue is DRM from end to end. Naming them is not a
 // judgement of them: it is the difference between a clear sentence now and a
 // retry loop that ends in "loading failed" (9.6, 66.10).
 const wchar_t* protectedHosts[]={
  L"netflix.com",L"disneyplus.com",L"primevideo.com",L"hulu.com",L"max.com",L"hbomax.com",
  L"tv.apple.com",L"music.apple.com",L"peacocktv.com",L"paramountplus.com",L"spotify.com",
  L"kinopoisk.ru",L"hd.kinopoisk.ru",L"okko.tv",L"ivi.ru",L"start.ru",L"wink.ru",
  L"more.tv",L"premier.one",L"kion.ru",
 };
 for(auto domain:protectedHosts)if(HostIs(host,domain))return StreamSource::Protected;
 auto lower=Lower(url);
 bool web=lower.rfind(L"http://",0)==0||lower.rfind(L"https://",0)==0;
 auto route=RouteForUrl(url);
 if(route.container==L"HLS")return StreamSource::Hls;
 if(route.container==L"DASH")return StreamSource::Dash;
 if(route.container==L"Direct URL")
  return route.Playable()?StreamSource::DirectMedia:StreamSource::Unsupported;
 // rtsp, rtmp, srt and udp are streams by their scheme; the engine speaks them.
 if(!web)return StreamSource::DirectMedia;
 return StreamSource::WebPage;
}

const wchar_t* StreamSourceName(StreamSource source){
 switch(source){
  case StreamSource::DirectMedia:return L"direct";
  case StreamSource::Hls:return L"HLS";
  case StreamSource::Dash:return L"DASH";
  case StreamSource::WebPage:return L"web page";
  case StreamSource::Protected:return L"DRM service";
  default:return L"unsupported";
 }
}

const wchar_t* StreamFailureName(StreamFailure failure){
 switch(failure){
  case StreamFailure::None:return L"none";
  case StreamFailure::Network:return L"network";
  case StreamFailure::NotFound:return L"not found";
  case StreamFailure::Forbidden:return L"forbidden";
  case StreamFailure::Certificate:return L"certificate";
  case StreamFailure::Protected:return L"DRM";
  case StreamFailure::SignInRequired:return L"sign-in required";
  case StreamFailure::NoPublicStream:return L"no public stream";
  case StreamFailure::ResolverMissing:return L"resolver missing";
  case StreamFailure::ResolverTimeout:return L"resolver timeout";
  case StreamFailure::ResolverCrashed:return L"resolver crashed";
  default:return L"unknown";
 }
}

StreamFailure ClassifyEngineFailure(const std::wstring& log){
 auto text=Lower(log);
 if(text.empty())return StreamFailure::None;
 // Most specific first: a 404 is also, in the same log, a "failed to open".
 if(Has(text,L"drm")||(Has(text,L"encrypted")&&Has(text,L"not supported")))return StreamFailure::Protected;
 if(Has(text,L"certificate")||Has(text,L"sec_e_untrusted_root")||(Has(text,L"ssl")&&Has(text,L"verif"))||
    (Has(text,L"tls")&&(Has(text,L"verif")||Has(text,L"handshake"))))
  return StreamFailure::Certificate;
 // Status codes only in the phrases that carry them: a bare "404" is as likely
 // to be a bitrate or a timestamp in some other warning.
 if(Has(text,L"http error 404")||Has(text,L"server returned 404")||Has(text,L"http error 410")||
    Has(text,L"server returned 410"))
  return StreamFailure::NotFound;
 if(Has(text,L"http error 403")||Has(text,L"server returned 403")||Has(text,L"http error 401")||
    Has(text,L"server returned 401")||Has(text,L"403 forbidden")||Has(text,L"401 unauthorized"))
  return StreamFailure::Forbidden;
 const wchar_t* network[]={
  L"resolve hostname",L"getaddrinfo",L"connection refused",L"connection reset",L"timed out",
  L"network is unreachable",L"host is unreachable",L"i/o error",L"input/output error",
  L"error number -10054",L"error number -10060",L"error number -10061",
  L"stream ends prematurely",L"will reconnect",L"http error 5",L"server returned 5",
  L"broken pipe",L"no route to host",L"connection timed out",
  // curl's own words: the engine may read HTTP through curl rather than ffmpeg,
  // and the same outage is then described differently.
  L"could not connect",L"couldn't connect",L"transfer failed",L"failure when receiving data",
  L"recv failure",L"send failure",L"empty reply from server",L"could not resolve host",
  L"couldn't resolve host",L"partial file",
 };
 for(auto phrase:network)if(Has(text,phrase))return StreamFailure::Network;
 return StreamFailure::Unknown;
}

StreamFailure ClassifyResolverFailure(const std::wstring& errors,unsigned long exitCode){
 auto text=Lower(errors);
 if(exitCode==0&&text.find(L"error")==std::wstring::npos)return StreamFailure::NoPublicStream;
 if(Has(text,L"drm"))return StreamFailure::Protected;
 if(Has(text,L"sign in")||Has(text,L"login")||Has(text,L"log in")||Has(text,L"private video")||
    Has(text,L"members-only")||Has(text,L"confirm your age")||Has(text,L"cookies"))
  return StreamFailure::SignInRequired;
 if(Has(text,L"http error 404")||Has(text,L"404: not found"))return StreamFailure::NotFound;
 if(Has(text,L"http error 403")||Has(text,L"403: forbidden"))return StreamFailure::Forbidden;
 if(Has(text,L"certificate"))return StreamFailure::Certificate;
 if(Has(text,L"getaddrinfo")||Has(text,L"unable to download webpage")||Has(text,L"timed out")||
    Has(text,L"connection"))
  return StreamFailure::Network;
 return StreamFailure::NoPublicStream;
}

bool FailureRetryable(StreamFailure failure){
 return failure==StreamFailure::Network;
}

bool EngineGaveUpOnNetwork(const std::wstring& log){
 // The newest non-empty line only: a give-up followed by anything else is old news.
 size_t end=log.size();
 while(end>0&&(log[end-1]==L'\n'||log[end-1]==L'\r'||log[end-1]==L' '))end--;
 if(!end)return false;
 size_t start=log.find_last_of(L'\n',end-1);
 start=start==std::wstring::npos?0:start+1;
 auto line=Lower(log.substr(start,end-start));
 if(Has(line,L"retrying"))return false;
 bool final=Has(line,L"transfer failed")||Has(line,L"failed to open")||Has(line,L"stream ends prematurely")||
            Has(line,L"giving up");
 return final&&ClassifyEngineFailure(line)==StreamFailure::Network;
}

double ReconnectDelay(int attempt){
 if(attempt<0)attempt=0;
 if(attempt>=5)return 30.0;
 return double(1<<attempt);
}

bool EndedPrematurely(bool live,double position,double duration){
 if(live)return true;
 if(duration<=0)return false;
 return position<duration-5.0;
}

bool ParseResolverOutput(const std::string& utf8,ResolvedStream& out){
 out=ResolvedStream{};
 auto text=Widen(utf8);
 size_t at=0;
 bool inUrls=false;
 std::wstring urls[2];int urlCount=0;
 while(at<=text.size()){
  auto end=text.find(L'\n',at);
  auto line=text.substr(at,end==std::wstring::npos?std::wstring::npos:end-at);
  at=end==std::wstring::npos?text.size()+1:end+1;
  while(!line.empty()&&(line.back()==L'\r'||line.back()==L' '))line.pop_back();
  auto value=[&](const wchar_t* tag)->bool{
   size_t n=wcslen(tag);
   if(line.compare(0,n,tag)!=0)return false;
   line=line.substr(n);
   return true;
  };
  if(value(L"VETRO-TITLE:")){inUrls=false;if(Available(line))out.title=line;continue;}
  if(value(L"VETRO-LIVE:")){inUrls=false;out.live=line==L"True";continue;}
  if(value(L"VETRO-DURATION:")){inUrls=false;if(Available(line))out.duration=_wtof(line.c_str());continue;}
  if(value(L"VETRO-UA:")){inUrls=false;if(Available(line))out.userAgent=line;continue;}
  if(value(L"VETRO-REFERER:")){inUrls=false;if(Available(line))out.referrer=line;continue;}
  if(value(L"VETRO-URLS:"))inUrls=true;
  if(!inUrls||line.empty())continue;
  auto lower=Lower(line);
  if(lower.rfind(L"http://",0)!=0&&lower.rfind(L"https://",0)!=0)continue;
  if(urlCount<2)urls[urlCount++]=line;
 }
 out.video=urls[0];
 out.audio=urlCount>1?urls[1]:std::wstring();
 return !out.video.empty();
}

bool ResolverAddressSafe(const std::wstring& url){
 if(url.empty()||url.size()>4096)return false;
 auto lower=Lower(url);
 if(lower.rfind(L"http://",0)!=0&&lower.rfind(L"https://",0)!=0)return false;
 for(wchar_t c:url){
  if(c<0x21||c==0x7F||c==L'"'||c==L'\\'||c==L'<'||c==L'>'||c==L'|'||c==L'^'||c==L'`')return false;
 }
 return !AddressHost(url).empty();
}
