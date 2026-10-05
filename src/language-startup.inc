// First-run language selection shares the application's glass renderer.
// It runs before library work, session restore or opening the requested file.
namespace LanguageOnboarding {
constexpr auto SettingsKey=L"Software\\VetroLook\\Settings";
struct State {
 std::wstring key,error;
 int selected=-1,focus=-1,hover=-1,pressed=-1;
 bool dirty=true;
};
bool Load(const std::wstring& key,int& value){
 DWORD stored=0,size=sizeof(stored);
 if(RegGetValueW(HKEY_CURRENT_USER,key.c_str(),L"Language",RRF_RT_REG_DWORD,nullptr,&stored,&size)!=ERROR_SUCCESS||stored>1)return false;
 value=int(stored);return true;
}
D2D1_RECT_F ChoiceRect(HWND hwnd,int choice){
 RECT client{};GetClientRect(hwnd,&client);float w=client.right/dpi,h=client.bottom/dpi;
 float mid=w/2;
 return choice==1?D2D1::RectF(48,h-104,mid-8,h-48):D2D1::RectF(mid+8,h-104,w-48,h-48);
}
int At(HWND hwnd,LPARAM lp){
 float x=GET_X_LPARAM(lp)/dpi,y=GET_Y_LPARAM(lp)/dpi;
 for(int i=0;i<2;i++){auto r=ChoiceRect(hwnd,i);if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom)return i;}
 return -1;
}
void Choose(HWND hwnd,State& state,int value){
 DWORD stored=DWORD(value);
 if(RegSetKeyValueW(HKEY_CURRENT_USER,state.key.c_str(),L"Language",REG_DWORD,&stored,sizeof(stored))!=ERROR_SUCCESS){
  state.error=L"Could not save your choice. Please try again.";state.dirty=true;return;
 }
 language=value;state.selected=value;
}
LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
 auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
 if(msg==WM_NCCREATE){SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));return DefWindowProcW(hwnd,msg,wp,lp);}
 if(!state)return DefWindowProcW(hwnd,msg,wp,lp);
 switch(msg){
 case WM_CLOSE:return 0;
 case WM_SYSCOMMAND:if((wp&0xfff0)==SC_CLOSE)return 0;break;
 case WM_PAINT:{PAINTSTRUCT paint;BeginPaint(hwnd,&paint);EndPaint(hwnd,&paint);state->dirty=true;return 0;}
 case WM_SIZE:{RECT client{};GetClientRect(hwnd,&client);GfxResize(client.right,client.bottom,dpi);state->dirty=true;return 0;}
 case WM_DPICHANGED:{dpi=HIWORD(wp)/96.f;auto* r=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);state->dirty=true;return 0;}
 case WM_MOUSEMOVE:{int hit=At(hwnd,lp);if(hit!=state->hover){state->hover=hit;state->dirty=true;}TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);return 0;}
 case WM_MOUSELEAVE:state->hover=-1;state->dirty=true;return 0;
 case WM_LBUTTONDOWN:state->pressed=At(hwnd,lp);if(state->pressed>=0){state->focus=state->pressed;SetCapture(hwnd);state->dirty=true;}return 0;
 case WM_LBUTTONUP:{int hit=At(hwnd,lp),pressed=state->pressed;state->pressed=-1;ReleaseCapture();if(hit>=0&&hit==pressed)Choose(hwnd,*state,hit);state->dirty=true;return 0;}
 case WM_CAPTURECHANGED:state->pressed=-1;state->dirty=true;return 0;
 case WM_KEYDOWN:
  if(wp==VK_TAB){state->focus=state->focus<0?1:1-state->focus;state->dirty=true;}
  else if(wp==VK_LEFT||wp==VK_RIGHT){state->focus=wp==VK_LEFT?1:0;state->dirty=true;}
  else if((wp==VK_RETURN||wp==VK_SPACE)&&state->focus>=0)Choose(hwnd,*state,state->focus);
  return 0; // Escape cannot dismiss the mandatory choice.
 case WM_NCHITTEST:{POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&point);return point.y<64*dpi?HTCAPTION:HTCLIENT;}
 }
 return DefWindowProcW(hwnd,msg,wp,lp);
}
void Paint(HWND hwnd,const State& state){
 RECT client{};GetClientRect(hwnd,&client);float w=client.right/dpi,h=client.bottom/dpi;auto p=Pal();
 GfxBeginScene(w,h);
 Ink()->SetColor(Mix(D2D1::ColorF(.070f,.061f,.050f,1.f),D2D1::ColorF(.965f,.916f,.805f,1.f),themeMix.v));
 Dc()->FillRectangle(D2D1::RectF(0,0,w,h),Ink());
 Ink()->SetColor(Fade(p.accent,.27f));Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(w-82,100),104,104),Ink());
 Ink()->SetColor(Fade(p.text,.09f));Dc()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(50,h-34),126,126),Ink(),34);
 GfxEndScene(D2D1::RoundedRect(D2D1::RectF(0,0,w,h),32,32),true);
 auto surface=D2D1::RoundedRect(D2D1::RectF(20,20,w-20,h-20),28,28);
 SoftShadow(surface,.4f);Glass(surface,p,1,D2D1::Matrix3x2F::Identity());
 Write(L"VETRO LOOK",D2D1::RectF(48,46,w-48,72),F_Section,p.accent);
 Write(L"Choose your language",D2D1::RectF(48,100,w-48,146),F_Heading,p.text);
 auto* font=Font(F_Row);auto wrapping=font->GetWordWrapping();auto alignment=font->GetParagraphAlignment();
 font->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
 Write(L"Select a language to continue. You can change it later in Settings.",D2D1::RectF(48,161,w-48,222),F_Row,p.dim);
 font->SetWordWrapping(wrapping);font->SetParagraphAlignment(alignment);
 if(!state.error.empty())Write(state.error,D2D1::RectF(48,h-145,w-48,h-112),F_Small,p.accent);
 for(int choice=0;choice<2;choice++){
  auto r=ChoiceRect(hwnd,choice);auto button=D2D1::RoundedRect(r,28,28);
  Glass(button,p,1,D2D1::Matrix3x2F::Identity());
  if(state.hover==choice||state.pressed==choice){Ink()->SetColor(Fade(p.accent,state.pressed==choice?.25f:.13f));Dc()->FillRoundedRectangle(button,Ink());}
  if(state.focus==choice){Ink()->SetColor(p.accent);Dc()->DrawRoundedRectangle(button,Ink(),2);}
  Write(choice?L"English":L"Русский",r,F_Button,p.text);
 }
 GfxPresent(true);
}
bool Ask(HINSTANCE instance,bool light,const std::wstring& key=SettingsKey){
 State state;state.key=key;
 HWND previousWindow=win;float previousDpi=dpi;themeMix.Reset(light?1.f:0.f);
 WNDCLASSW cls{};cls.lpfnWndProc=Proc;cls.hInstance=instance;cls.lpszClassName=L"VetroLook.LanguageOnboarding";
 cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=ThemedAppIcon();RegisterClassW(&cls);
 RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
 float scale=GetDpiForSystem()/96.f;int width=int(620*scale),height=int(390*scale);
 win=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_NOREDIRECTIONBITMAP,cls.lpszClassName,L"VetroLook — Choose your language",WS_POPUP,
  work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,&state);
 if(!win){win=previousWindow;return false;}
 dpi=GetDpiForWindow(win)/96.f;BOOL dark=!light;DwmSetWindowAttribute(win,20,&dark,sizeof(dark));
 DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;DwmSetWindowAttribute(win,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
 bool ready=GfxCreate(win,dpi);
 if(ready){
  Paint(win,state);ShowWindow(win,SW_SHOW);SetForegroundWindow(win);
  MSG event{};bool running=true;
  while(running&&state.selected<0){
   while(PeekMessageW(&event,nullptr,0,0,PM_REMOVE)){
    if(event.message==WM_QUIT){running=false;break;}
    TranslateMessage(&event);DispatchMessageW(&event);
   }
   if(state.dirty&&running){state.dirty=false;Paint(win,state);}
   if(running&&state.selected<0&&!state.dirty)WaitMessage();
  }
 }
 GfxDestroy();DestroyWindow(win);win=previousWindow;dpi=previousDpi;
 return state.selected>=0;
}
}
