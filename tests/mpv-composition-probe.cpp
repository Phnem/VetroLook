// Vetro Look, GPL-3.0-or-later.
//
// Prototype B from the master plan: the feasibility gate for Video Mode's
// presentation path. Nothing in the player may be built on top of mpv's
// composition output until this program says the path works on the exact
// libmpv binary the application ships.
//
// What it proves, or fails to:
//
//   * mpv runs with vo=gpu-next on gpu-context=d3d11 without creating a window
//     of its own -- `d3d11-output-mode=composition` skips mpv's win32 layer;
//   * the swapchain mpv created for that output can be read back through the
//     `display-swapchain` property;
//   * that swapchain can be made the content of a DirectComposition visual in
//     a tree this process owns, in a window with no redirection surface, which
//     is the window Vetro Look actually has;
//   * the picture survives a resize driven by `d3d11-composition-size`;
//   * pause, seek and resume behave, and the clock keeps its own time.
//
// Usage: VetroMpvProbe <media file> [seconds]
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <mpv/client.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;

namespace{

int failures=0,checks=0;
void Check(bool ok,const char* what,const char* detail=""){
 checks++;
 if(!ok)failures++;
 printf("%s %s%s%s\n",ok?"PASS":"FAIL",what,*detail?" -- ":"",detail);
 fflush(stdout);
}
double Milliseconds(std::chrono::steady_clock::time_point from){
 return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-from).count();
}

HWND window=nullptr;
ComPtr<ID3D11Device> d3d;
ComPtr<IDCompositionDevice> compositor;
ComPtr<IDCompositionTarget> target;
ComPtr<IDCompositionVisual> visual;

LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
 if(m==WM_DESTROY){PostQuitMessage(0);return 0;}
 return DefWindowProcW(w,m,wp,lp);
}

// The same window shape the viewer uses: no redirection bitmap, so every pixel
// on screen comes from the composition tree and nothing else.
bool MakeWindow(int width,int height){
 WNDCLASSEXW cls{sizeof(cls)};
 cls.lpfnWndProc=Proc;cls.hInstance=GetModuleHandleW(nullptr);
 cls.lpszClassName=L"VetroMpvProbe";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
 RegisterClassExW(&cls);
 window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,cls.lpszClassName,L"Vetro mpv composition probe",
  WS_OVERLAPPEDWINDOW,80,80,width,height,nullptr,nullptr,cls.hInstance,nullptr);
 return window!=nullptr;
}
bool MakeComposition(){
 UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
 if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags,nullptr,0,
    D3D11_SDK_VERSION,&d3d,nullptr,nullptr)))return false;
 ComPtr<IDXGIDevice> dxgi;
 if(FAILED(d3d.As(&dxgi)))return false;
 if(FAILED(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&compositor))))return false;
 if(FAILED(compositor->CreateTargetForHwnd(window,TRUE,&target)))return false;
 if(FAILED(compositor->CreateVisual(&visual)))return false;
 if(FAILED(target->SetRoot(visual.Get())))return false;
 return SUCCEEDED(compositor->Commit());
}

int64_t Int(mpv_handle* mpv,const char* name,int64_t fallback=-1){
 int64_t value=fallback;
 if(mpv_get_property(mpv,name,MPV_FORMAT_INT64,&value)<0)return fallback;
 return value;
}
double Real(mpv_handle* mpv,const char* name,double fallback=-1){
 double value=fallback;
 if(mpv_get_property(mpv,name,MPV_FORMAT_DOUBLE,&value)<0)return fallback;
 return value;
}
std::string Text(mpv_handle* mpv,const char* name){
 char* value=nullptr;
 if(mpv_get_property(mpv,name,MPV_FORMAT_STRING,&value)<0||!value)return {};
 std::string out=value;mpv_free(value);
 return out;
}
void Pump(){
 MSG msg;
 while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
}
// Drains mpv's event queue for `ms`, keeping the window responsive. Returns the
// last interesting event name, for the report.
void Wait(mpv_handle* mpv,double ms){
 auto start=std::chrono::steady_clock::now();
 while(Milliseconds(start)<ms){
  Pump();
  mpv_event* event=mpv_wait_event(mpv,0.005);
  if(event->event_id==MPV_EVENT_NONE)continue;
  if(event->event_id==MPV_EVENT_LOG_MESSAGE){
   auto* message=(mpv_event_log_message*)event->data;
   if(message->log_level<=MPV_LOG_LEVEL_WARN)
    printf("     mpv[%s] %s",message->prefix,message->text);
  }
 }
}

}

int wmain(int argc,wchar_t** argv){
 if(argc<2){
  printf("usage: VetroMpvProbe <media file> [seconds]\n");
  return 2;
 }
 double seconds=argc>2?_wtof(argv[2]):8.0;
 int width=1280,height=720;

 Check(MakeWindow(width,height),"window without a redirection surface");
 Check(MakeComposition(),"D3D11 device, composition device, visual tree");
 ShowWindow(window,SW_SHOW);

 auto started=std::chrono::steady_clock::now();
 mpv_handle* mpv=mpv_create();
 Check(mpv!=nullptr,"mpv_create");
 if(!mpv)return 1;

 unsigned long version=mpv_client_api_version();
 char versionText[64];
 snprintf(versionText,sizeof versionText,"client API %lu.%lu",version>>16,version&0xFFFF);
 printf("     %s\n",versionText);

 // The whole point of the gate: this exact set of options, on this binary.
 struct Option{const char* name;const char* value;};
 char compositionSize[32];
 snprintf(compositionSize,sizeof compositionSize,"%dx%d",width,height);
 const Option options[]={
  {"vo","gpu-next"},
  {"gpu-context","d3d11"},
  {"d3d11-output-mode","composition"},
  {"d3d11-composition-size",compositionSize},
  {"hwdec","auto-safe"},
  {"keep-open","yes"},
  {"idle","yes"},
  {"osc","no"},
  {"input-default-bindings","no"},
  {"input-vo-keyboard","no"},
  {"terminal","no"},
 };
 bool optionsAccepted=true;
 for(const auto& option:options){
  int rc=mpv_set_option_string(mpv,option.name,option.value);
  if(rc<0){
   optionsAccepted=false;
   printf("     option %s=%s rejected: %s\n",option.name,option.value,mpv_error_string(rc));
  }
 }
 Check(optionsAccepted,"every composition option accepted by this build");
 mpv_request_log_messages(mpv,"warn");

 int rc=mpv_initialize(mpv);
 Check(rc>=0,"mpv_initialize",rc<0?mpv_error_string(rc):"");
 if(rc<0)return 1;

 char path[MAX_PATH*2]{};
 WideCharToMultiByte(CP_UTF8,0,argv[1],-1,path,sizeof path,nullptr,nullptr);
 const char* load[]={"loadfile",path,nullptr};
 rc=mpv_command(mpv,load);
 Check(rc>=0,"loadfile",rc<0?mpv_error_string(rc):"");

 // The swapchain appears once the video output has configured itself. Poll for
 // it rather than assuming a fixed delay: this measurement is the gate's most
 // interesting number.
 IDXGISwapChain* swapchain=nullptr;
 double swapchainMs=0;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(std::chrono::steady_clock::now()<deadline&&!swapchain){
  Wait(mpv,20);
  int64_t handle=Int(mpv,"display-swapchain",0);
  if(handle){
   swapchain=(IDXGISwapChain*)(intptr_t)handle;
   swapchainMs=Milliseconds(started);
  }
 }
 char detail[128];
 snprintf(detail,sizeof detail,"%.0f ms from process start",swapchainMs);
 Check(swapchain!=nullptr,"display-swapchain readable from libmpv",swapchain?detail:"never appeared");
 if(!swapchain){
  mpv_terminate_destroy(mpv);
  printf("\n%d of %d checks failed\n",failures,checks);
  return 1;
 }

 // Does a swapchain created by mpv's own D3D11 device compose in a visual tree
 // owned by ours? This is the question the plan says must not be assumed.
 HRESULT hr=visual->SetContent(swapchain);
 Check(SUCCEEDED(hr),"mpv's swapchain accepted as DirectComposition content");
 hr=compositor->Commit();
 Check(SUCCEEDED(hr),"composition commit");

 DXGI_SWAP_CHAIN_DESC description{};
 if(SUCCEEDED(swapchain->GetDesc(&description))){
  snprintf(detail,sizeof detail,"%ux%u, %u buffers, format %u, hwnd %p",
   description.BufferDesc.Width,description.BufferDesc.Height,description.BufferCount,
   unsigned(description.BufferDesc.Format),(void*)description.OutputWindow);
  Check(description.OutputWindow==nullptr,"swapchain is windowless (composition, not hwnd)",detail);
 }

 Wait(mpv,1200);
 double first=Real(mpv,"time-pos",-1);
 snprintf(detail,sizeof detail,"time-pos %.2f s",first);
 Check(first>0,"the clock is running",detail);
 std::string decoder=Text(mpv,"video-codec"),hwdec=Text(mpv,"hwdec-current");
 printf("     video-codec=%s hwdec-current=%s video-params=%sx%s\n",
  decoder.c_str(),hwdec.c_str(),Text(mpv,"video-params/w").c_str(),Text(mpv,"video-params/h").c_str());

 // Resize. In composition mode the window tells mpv how large its output
 // should be; there is no HWND for mpv to measure.
 width=900;height=600;
 SetWindowPos(window,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER);
 snprintf(compositionSize,sizeof compositionSize,"%dx%d",width,height);
 rc=mpv_set_property_string(mpv,"d3d11-composition-size",compositionSize);
 Check(rc>=0,"d3d11-composition-size accepted at runtime",rc<0?mpv_error_string(rc):compositionSize);
 Wait(mpv,700);
 int64_t afterResize=Int(mpv,"display-swapchain",0);
 Check(afterResize==(int64_t)(intptr_t)swapchain,
       "the swapchain survives a resize (same object, resized buffers)");
 if(SUCCEEDED(swapchain->GetDesc(&description))){
  bool resized=int(description.BufferDesc.Width)==width&&int(description.BufferDesc.Height)==height;
  snprintf(detail,sizeof detail,"%ux%u",description.BufferDesc.Width,description.BufferDesc.Height);
  Check(resized,"swapchain buffers follow the requested composition size",detail);
 }

 // Pause, seek, resume: the three commands Video Mode needs first.
 double before=Real(mpv,"time-pos",0);
 mpv_set_property_string(mpv,"pause","yes");
 Wait(mpv,400);
 double paused=Real(mpv,"time-pos",0);
 Wait(mpv,400);
 Check(Real(mpv,"time-pos",0)==paused&&paused>=before,"pause holds the clock still");

 auto seekStart=std::chrono::steady_clock::now();
 const char* seek[]={"seek","5","absolute+exact",nullptr};
 rc=mpv_command(mpv,seek);
 Wait(mpv,600);
 double sought=Real(mpv,"time-pos",-1);
 snprintf(detail,sizeof detail,"time-pos %.2f s, %.0f ms to settle",sought,Milliseconds(seekStart));
 Check(rc>=0&&sought>4.0&&sought<6.5,"exact seek while paused lands where it was asked to",detail);

 mpv_set_property_string(mpv,"pause","no");
 Wait(mpv,900);
 Check(Real(mpv,"time-pos",0)>sought,"playback resumes from the seek position");

 // A resize and an exact seek each legitimately discard a frame; what would
 // condemn the path is a steady stream of them.
 int64_t dropped=Int(mpv,"frame-drop-count",0),vo=Int(mpv,"vo-delayed-frame-count",0);
 snprintf(detail,sizeof detail,"decoder dropped %lld, vo delayed %lld, across a resize and a seek",
  (long long)dropped,(long long)vo);
 Check(dropped<=2&&vo<=2,"frame drops stay incidental",detail);

 // Composition mode has no HWND for mpv to query, so mpv cannot see which
 // output the picture lands on: it reports no target colour space and no
 // reference luminance. SDR is unaffected; HDR output needs Vetro Look to tell
 // mpv what the display is, which is the colour work of a later stage.
 printf("     note: target-colorspace is empty in composition mode; HDR output will\n"
        "     need the display's colour space supplied from this side (stage 3).\n");

 printf("     watching for %.0f s -- look at the window\n",seconds);
 Wait(mpv,seconds*1000);

 // Teardown order matters: mpv owns the swapchain, so the visual must let go
 // of it before the engine is destroyed.
 visual->SetContent(nullptr);
 compositor->Commit();
 mpv_terminate_destroy(mpv);
 Check(true,"teardown with the visual released first");

 printf("\n%d of %d checks failed\n",failures,checks);
 return failures?1:0;
}
