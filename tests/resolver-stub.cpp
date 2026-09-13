// Vetro Look, GPL-3.0-or-later.
// A stand-in for yt-dlp that honours the same output contract as the real one
// under the arguments resolver.cpp passes. Built as `yt-dlp.exe` and dropped into
// %LOCALAPPDATA%\VetroLook\resolver\ for a test run, then removed.
//
// The address after "--" decides the behaviour:
//   ...drm...   -> a DRM error on stderr, exit 1
//   ...slow...  -> never answers (tests the timeout and cancellation)
//   ...live...  -> a live HLS stream
//   ...split... -> separate video and audio addresses
//   anything    -> a finite HLS film with a Cyrillic title
#include <windows.h>
#include <string>

namespace{
void Out(HANDLE handle,const std::string& text){
 DWORD written=0;
 WriteFile(handle,text.data(),DWORD(text.size()),&written,nullptr);
}
}

int wmain(int argc,wchar_t** argv){
 std::wstring url;
 bool ignoredConfig=false;
 for(int i=1;i<argc;i++){
  if(wcscmp(argv[i],L"--ignore-config")==0)ignoredConfig=true;
  if(wcscmp(argv[i],L"--")==0&&i+1<argc){url=argv[i+1];if(i+2!=argc)return 3;}
 }
 HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE),err=GetStdHandle(STD_ERROR_HANDLE);
 if(!ignoredConfig||url.empty()){Out(err,"ERROR: stub called without the contract\n");return 2;}
 const char* film="https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8";
 if(url.find(L"drm")!=std::wstring::npos){Out(err,"ERROR: [stub] This video is DRM protected\n");return 1;}
 if(url.find(L"slow")!=std::wstring::npos){Sleep(600000);return 0;}
 if(url.find(L"live")!=std::wstring::npos){
  Out(out,"VETRO-TITLE:Stub live\nVETRO-LIVE:True\nVETRO-DURATION:NA\nVETRO-UA:NA\nVETRO-REFERER:NA\n");
  Out(out,"VETRO-URLS:https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8\n");
  return 0;
 }
 if(url.find(L"split")!=std::wstring::npos){
  Out(out,"VETRO-TITLE:Stub split\nVETRO-LIVE:False\nVETRO-DURATION:634\nVETRO-UA:VetroStub/1.0\nVETRO-REFERER:NA\n");
  Out(out,std::string("VETRO-URLS:")+film+"\n"+film+"\n");
  return 0;
 }
 Out(out,"VETRO-TITLE:\xD0\xA1\xD1\x82\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x86\xD0\xB0 \xD1\x81 \xD1\x84\xD0\xB8\xD0\xBB\xD1\x8C\xD0\xBC\xD0\xBE\xD0\xBC\n");
 Out(out,"VETRO-LIVE:False\nVETRO-DURATION:634\nVETRO-UA:NA\nVETRO-REFERER:NA\n");
 Out(out,std::string("VETRO-URLS:")+film+"\n");
 return 0;
}
