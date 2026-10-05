// Vetro Look, GPL-3.0-or-later.
// Direct3D 11 -> Direct2D 1.1 -> DirectComposition. The window has no
// redirection surface, so every pixel we present carries its own alpha and the
// rounded shape of the window is drawn rather than clipped by a region.
#include "ui.h"
#include <tuple>
#include "videomode.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <map>
#include <cstdio>
using Microsoft::WRL::ComPtr;

static ComPtr<ID3D11Device> d3d;
static ComPtr<IDXGISwapChain1> swap;
static ComPtr<ID2D1Factory1> factory;
static ComPtr<ID2D1Device> device;
static ComPtr<ID2D1DeviceContext> dc;
static ComPtr<ID2D1Bitmap1> backBuffer,scene,frost;
static ComPtr<ID2D1Effect> saturate,blur;
static ComPtr<ID2D1BitmapBrush1> sceneBrush,frostBrush;
static ComPtr<ID2D1SolidColorBrush> ink;
static ComPtr<IDCompositionDevice> compositor;
static ComPtr<IDCompositionTarget> compositionTarget;
static ComPtr<IDCompositionVisual> visual;
// The composition tree has two layers. Video sits underneath, because a film is
// the thing being looked at; the interface layer above it carries its own alpha,
// so glass, subtitles and chrome are composited by Windows over the video rather
// than being blended into it by us. With no video open the lower layer holds
// nothing and costs nothing.
static ComPtr<IDCompositionVisual> rootVisual,videoVisual;
static ComPtr<IDCompositionRectangleClip> videoClip;
// The frosted-glass backdrop: the same film, blurred by the compositor and
// clipped to each panel. See GfxGlassBegin in ui.h for why it is built this way
// rather than blurred in Direct2D.
static ComPtr<IDCompositionDevice3> effectDevice;
static IUnknown* glassContent=nullptr;   // the film, borrowed from the engine
static float glassBlurRadius=0;
static bool glassVisible=false;
static float videoCornerRadius=0;
static bool videoContentSet=false;
// The frosted panels of one frame. Each gets its own visual showing the same
// film, blurred by the compositor and clipped to that panel's rounded shape:
// one visual cannot have several holes, and a single visual over the whole
// window would blur the film itself.
namespace{
constexpr int MaxGlassPanels=10;
struct GlassPanel{D2D1_RECT_F rect{};float radius=0;float opacity=1;};
ComPtr<IDCompositionVisual> glassPanels[MaxGlassPanels];
// Opacity lives on the third revision of the visual interface. A panel fades
// with whatever it holds, so the glass never outlasts the controls on it.
ComPtr<IDCompositionVisual3> glassPanels3[MaxGlassPanels];
// One effect chain per panel. A composition effect object drives one visual: a
// single chain shared by every panel frosted only whichever panel was given it
// last, which is how the transport turned to clear plastic the moment the
// subtitle bubble joined the list.
ComPtr<IDCompositionGaussianBlurEffect> panelBlur[MaxGlassPanels];
ComPtr<IDCompositionSaturationEffect> panelSaturation[MaxGlassPanels];
ComPtr<IDCompositionColorMatrixEffect> panelContrast[MaxGlassPanels];
float panelEffectRadius[MaxGlassPanels]={};
GlassPanel glassShown[MaxGlassPanels];
ComPtr<IDCompositionRectangleClip> glassClips[MaxGlassPanels];
int glassShownCount=0;
GlassPanel glassPending[MaxGlassPanels];
int glassPendingCount=0;
}

static std::map<std::tuple<float,float,float>,ComPtr<ID2D1RoundedRectangleGeometry>> roundedImageMasks;
static ComPtr<IDWriteFactory> writer;
static ComPtr<IDWriteTextFormat> faces[F_COUNT];
static std::map<const wchar_t*,ComPtr<ID2D1PathGeometry>> icons;
static ComPtr<ID2D1Effect> clipMatrix[2],clipTransfer[2],clipFade[2];
static UINT surfaceW=0,surfaceH=0;static float surfaceDpi=1;static bool drawing=false,frostValid=false;

static void Trace(const char* what,HRESULT hr){
 static FILE* file=nullptr;static int enabled=-1;
 if(enabled<0){char buffer[8]{};enabled=GetEnvironmentVariableA("VETRO_DEBUG",buffer,8)?1:0;}
 if(!enabled)return;
 if(!file){char path[MAX_PATH]{};GetTempPathA(MAX_PATH,path);strcat_s(path,"vetro-debug.log");fopen_s(&file,path,"w");}
 if(file){fprintf(file,"%s hr=0x%08lX\n",what,(unsigned long)hr);fflush(file);}
}
// Re-attaches the swap chain to a fresh composition visual.
void GfxRebind(){
 if(!compositor||!swap||!compositionTarget)return;
 visual.Reset();
 HRESULT hr=compositor->CreateVisual(&visual);Trace("rebind-visual",hr);
 if(FAILED(hr))return;
 hr=visual->SetContent(swap.Get());Trace("rebind-content",hr);
 // The video layer is rebuilt with the interface layer, not separately: a tree
 // that lost one of its two children would leave the film on screen with no
 // controls over it, or controls over nothing.
 if(!rootVisual)compositor->CreateVisual(&rootVisual);
 if(!videoVisual)compositor->CreateVisual(&videoVisual);
 if(rootVisual){
  rootVisual->RemoveAllVisuals();
  HRESULT addVideo=videoVisual?rootVisual->AddVisual(videoVisual.Get(),FALSE,nullptr):S_OK;
  Trace("rebind-add-video",addVideo);
  // The frosted panels are rebuilt on the next frame that asks for them.
  glassShownCount=0;
  for(int i=0;i<MaxGlassPanels;i++){glassPanels[i].Reset();glassPanels3[i].Reset();}
  HRESULT addUi=rootVisual->AddVisual(visual.Get(),TRUE,videoVisual?videoVisual.Get():nullptr);
  Trace("rebind-add-ui",addUi);
  hr=compositionTarget->SetRoot(rootVisual.Get());Trace("rebind-root",hr);
 }else{
  hr=compositionTarget->SetRoot(visual.Get());Trace("rebind-root",hr);
 }
 hr=compositor->Commit();Trace("rebind-commit",hr);
}
bool GfxSetVideoContent(IUnknown* content){
 if(!compositor||!videoVisual)return false;
 HRESULT hr=videoVisual->SetContent(content);Trace("video-content",hr);
 if(FAILED(hr))return false;
 videoContentSet=content!=nullptr;
 // The frosted panels show the same picture, so they follow the film in and out.
 glassContent=content;
 if(!content)GfxClearGlassBackdrop();
 hr=compositor->Commit();Trace("video-commit",hr);
 return SUCCEEDED(hr);
}
void GfxSetVideoCorners(float radiusPixels){
 if(!compositor||!videoVisual)return;
 if(radiusPixels==videoCornerRadius&&(videoClip||radiusPixels<=0))return;
 videoCornerRadius=radiusPixels;
 if(radiusPixels<=0){
  videoVisual->SetClip((IDCompositionClip*)nullptr);
  videoClip.Reset();
  compositor->Commit();
  return;
 }
 if(!videoClip&&FAILED(compositor->CreateRectangleClip(&videoClip)))return;
 videoClip->SetLeft(0.f);videoClip->SetTop(0.f);
 videoClip->SetRight(float(surfaceW));videoClip->SetBottom(float(surfaceH));
 videoClip->SetTopLeftRadiusX(radiusPixels);videoClip->SetTopLeftRadiusY(radiusPixels);
 videoClip->SetTopRightRadiusX(radiusPixels);videoClip->SetTopRightRadiusY(radiusPixels);
 videoClip->SetBottomLeftRadiusX(radiusPixels);videoClip->SetBottomLeftRadiusY(radiusPixels);
 videoClip->SetBottomRightRadiusX(radiusPixels);videoClip->SetBottomRightRadiusY(radiusPixels);
 videoVisual->SetClip(videoClip.Get());
 compositor->Commit();
}
bool GfxHasVideoContent(){return videoContentSet;}

// A device being capable of effects is not evidence that a frosted visual is
// currently under the control. The painter uses this for its opacity fallback,
// so report the live composited state rather than just the device capability.
bool GfxGlassBackdropAvailable(){return glassVisible&&effectDevice!=nullptr;}

// The frosted panels of one frame. Each gets its own visual showing the same
// film, blurred by the compositor and clipped to that panel's rounded shape:
// there is no way to give one visual several holes, and a single visual over the
// whole window would blur the film itself.
namespace{
bool SamePanel(const GlassPanel& a,const GlassPanel& b){
 return fabsf(a.radius-b.radius)<.25f&&fabsf(a.opacity-b.opacity)<.01f&&
  fabsf(a.rect.left-b.rect.left)<.5f&&fabsf(a.rect.top-b.rect.top)<.5f&&
  fabsf(a.rect.right-b.rect.right)<.5f&&fabsf(a.rect.bottom-b.rect.bottom)<.5f;
}
// This panel's own blur, then a restrained vibrancy lift, then a tiny contrast
// lift. The dark tint is painted by the panel itself after the chain.
IDCompositionEffect* PanelEffect(int index,float radius){
 if(!effectDevice)return nullptr;
 if(!panelBlur[index]||panelEffectRadius[index]!=radius){
  panelBlur[index].Reset();panelSaturation[index].Reset();panelContrast[index].Reset();
  HRESULT hr=effectDevice->CreateGaussianBlurEffect(&panelBlur[index]);Trace("glass-blur",hr);
  if(FAILED(hr)||!panelBlur[index])return nullptr;
  // Hard border mode: the blur may sample the film beyond the panel, which is
  // what stops the edges of the glass going dark.
  panelBlur[index]->SetBorderMode(D2D1_BORDER_MODE_HARD);
  panelBlur[index]->SetStandardDeviation(radius);
  if(SUCCEEDED(effectDevice->CreateSaturationEffect(&panelSaturation[index]))&&panelSaturation[index]){
   panelSaturation[index]->SetSaturation(1.16f);
   panelSaturation[index]->SetInput(0,panelBlur[index].Get(),0);
  }else panelSaturation[index].Reset();
  if(SUCCEEDED(effectDevice->CreateColorMatrixEffect(&panelContrast[index]))&&panelContrast[index]){
   constexpr float contrast=1.05f,midpoint=(1.f-contrast)*.5f;
   D2D1_MATRIX_5X4_F matrix={
    contrast,0,0,0,
    0,contrast,0,0,
    0,0,contrast,0,
    0,0,0,1,
    midpoint,midpoint,midpoint,0,
   };
   panelContrast[index]->SetMatrix(matrix);
   panelContrast[index]->SetInput(0,panelSaturation[index]?static_cast<IUnknown*>(panelSaturation[index].Get()):static_cast<IUnknown*>(panelBlur[index].Get()),0);
  }else panelContrast[index].Reset();
  panelEffectRadius[index]=radius;
 }
 if(panelContrast[index])return panelContrast[index].Get();
 if(panelSaturation[index])return panelSaturation[index].Get();
 return panelBlur[index].Get();
}
void ApplyPanel(int index,const GlassPanel& panel,float blurRadius){
 if(!glassPanels[index]){
  if(FAILED(compositor->CreateVisual(&glassPanels[index])))return;
  glassPanels[index].As(&glassPanels3[index]);
  if(rootVisual&&videoVisual)
   rootVisual->AddVisual(glassPanels[index].Get(),TRUE,videoVisual.Get());
 }
 auto& visualRef=glassPanels[index];
 visualRef->SetContent(videoContentSet?glassContent:nullptr);
 if(auto effect=PanelEffect(index,blurRadius))visualRef->SetEffect(effect);
 if(glassPanels3[index])glassPanels3[index]->SetOpacity(panel.opacity<0?0:(panel.opacity>1?1:panel.opacity));
 if(!glassClips[index]&&FAILED(compositor->CreateRectangleClip(&glassClips[index])))return;
 auto& clip=glassClips[index];
 clip->SetLeft(panel.rect.left);clip->SetTop(panel.rect.top);
 clip->SetRight(panel.rect.right);clip->SetBottom(panel.rect.bottom);
 clip->SetTopLeftRadiusX(panel.radius);clip->SetTopLeftRadiusY(panel.radius);
 clip->SetTopRightRadiusX(panel.radius);clip->SetTopRightRadiusY(panel.radius);
 clip->SetBottomLeftRadiusX(panel.radius);clip->SetBottomLeftRadiusY(panel.radius);
 clip->SetBottomRightRadiusX(panel.radius);clip->SetBottomRightRadiusY(panel.radius);
 visualRef->SetClip(clip.Get());
}
void HidePanel(int index){
 if(!glassPanels[index])return;
 glassPanels[index]->SetContent(nullptr);
 glassPanels[index]->SetEffect(nullptr);
 glassPanels[index]->SetClip((IDCompositionClip*)nullptr);
}
}


void GfxGlassBegin(){glassPendingCount=0;}
void GfxGlassAdd(const D2D1_RECT_F& rect,float radius,float opacity){
 if(glassPendingCount>=MaxGlassPanels)return;
 if(rect.right<=rect.left||rect.bottom<=rect.top||opacity<=.004f)return;
 glassPending[glassPendingCount++]={rect,radius,opacity};
}
void GfxGlassCommit(float dpi,float blurRadiusPixels){
 if(!compositor||!videoContentSet||!effectDevice){GfxClearGlassBackdrop();return;}
 bool radiusChanged=blurRadiusPixels!=glassBlurRadius;
 glassBlurRadius=blurRadiusPixels;
 GlassPanel wanted[MaxGlassPanels];
 for(int i=0;i<glassPendingCount;i++){
  wanted[i].radius=glassPending[i].radius*dpi;
  wanted[i].opacity=glassPending[i].opacity;
  wanted[i].rect=D2D1::RectF(glassPending[i].rect.left*dpi,glassPending[i].rect.top*dpi,
                             glassPending[i].rect.right*dpi,glassPending[i].rect.bottom*dpi);
 }
 // Only what moved is touched: a fading panel changes its opacity every frame,
 // and the panels beside it have no reason to be rebuilt for that.
 bool committed=false;
 for(int i=0;i<glassPendingCount;i++){
  if(radiusChanged||i>=glassShownCount||!SamePanel(wanted[i],glassShown[i])){
   ApplyPanel(i,wanted[i],blurRadiusPixels);
   glassShown[i]=wanted[i];
   committed=true;
  }
 }
 if(glassShownCount!=glassPendingCount){
  for(int i=glassPendingCount;i<MaxGlassPanels;i++)HidePanel(i);
  committed=true;
 }
 glassShownCount=glassPendingCount;
 glassVisible=glassPendingCount>0;
 if(committed)compositor->Commit();
}
void GfxClearGlassBackdrop(){
 if(!compositor||glassShownCount==0)return;
 for(int i=0;i<MaxGlassPanels;i++)HidePanel(i);
 glassShownCount=0;glassPendingCount=0;glassVisible=false;
 compositor->Commit();
}
ID2D1DeviceContext* Dc(){return dc.Get();}
ID2D1SolidColorBrush* Ink(){return ink.Get();}
bool GfxReady(){return dc&&backBuffer;}

static void MakeFonts(){
 if(writer)return;
 DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)writer.GetAddressOf());
 if(!writer)return;
 struct Spec{Face face;float size;DWRITE_FONT_WEIGHT weight;DWRITE_TEXT_ALIGNMENT align;};
 const Spec specs[]={
  {F_Title,15.5f,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_CENTER},
  {F_VideoTitle,19.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_CENTER},
  {F_Meta,11.5f,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_TEXT_ALIGNMENT_CENTER},
  {F_Row,13.5f,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_TEXT_ALIGNMENT_LEADING},
  {F_Label,12.f,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_TEXT_ALIGNMENT_LEADING},
  {F_Value,12.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_TRAILING},
  {F_Section,10.5f,DWRITE_FONT_WEIGHT_BOLD,DWRITE_TEXT_ALIGNMENT_LEADING},
  {F_Button,12.5f,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_CENTER},
  {F_Big,26.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_CENTER},
  {F_Small,10.5f,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_TEXT_ALIGNMENT_CENTER},
  {F_Mono,11.f,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_TEXT_ALIGNMENT_LEADING},
  {F_Timeline,18.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_LEADING},
  // Subtitles are read at a glance from across a room, so they are the largest
  // type in the application and the only face that wraps.
  {F_Subtitle,23.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_CENTER},
  {F_Display,48.f,DWRITE_FONT_WEIGHT_BLACK,DWRITE_TEXT_ALIGNMENT_LEADING},
  {F_Heading,30.f,DWRITE_FONT_WEIGHT_BOLD,DWRITE_TEXT_ALIGNMENT_LEADING},
 };
 for(auto& s:specs){
  writer->CreateTextFormat(s.face==F_Mono?L"Consolas":(s.face==F_Display||s.face==F_Heading)?L"Arial":L"Segoe UI Variable",nullptr,s.weight,
   DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,s.size,L"en-us",&faces[s.face]);
  if(!faces[s.face])writer->CreateTextFormat(L"Segoe UI",nullptr,s.weight,DWRITE_FONT_STYLE_NORMAL,
   DWRITE_FONT_STRETCH_NORMAL,s.size,L"en-us",&faces[s.face]);
  if(faces[s.face]){faces[s.face]->SetTextAlignment(s.align);faces[s.face]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
   faces[s.face]->SetWordWrapping(s.face==F_Subtitle?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);}
 }
}
IDWriteTextFormat* Font(Face f){return faces[f].Get();}

static void ReleaseTargets(){
 if(dc)dc->SetTarget(nullptr);
 sceneBrush.Reset();frostBrush.Reset();blur.Reset();saturate.Reset();
 backBuffer.Reset();scene.Reset();frost.Reset();frostValid=false;
 for(int i=0;i<2;i++){clipFade[i].Reset();clipTransfer[i].Reset();clipMatrix[i].Reset();}
}
static bool MakeTargets(UINT w,UINT h,float dpi){
 ReleaseTargets();
 surfaceW=w=(std::max)(w,8u);surfaceH=h=(std::max)(h,8u);surfaceDpi=dpi;
 dc->SetDpi(96*dpi,96*dpi);
 ComPtr<IDXGISurface> surface;
 HRESULT hr=swap->GetBuffer(0,IID_PPV_ARGS(&surface));Trace("GetBuffer",hr);if(FAILED(hr))return false;
 auto back=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96*dpi,96*dpi);
 hr=dc->CreateBitmapFromDxgiSurface(surface.Get(),&back,&backBuffer);Trace("BitmapFromSurface",hr);if(FAILED(hr))return false;
 auto offscreen=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96*dpi,96*dpi);
 hr=dc->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,offscreen,&scene);Trace("scene",hr);if(FAILED(hr))return false;
 // The frost buffer matches the scene pixel for pixel. Effect graphs work in
 // input pixels and ignore bitmap DPI, so a reduced buffer would sample askew;
 // the blur is told to trade accuracy for speed instead.
 hr=dc->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,offscreen,&frost);Trace("frost",hr);if(FAILED(hr))return false;
 auto brushProps=D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_INTERPOLATION_MODE_LINEAR);
 dc->CreateBitmapBrush(scene.Get(),brushProps,&sceneBrush);
 dc->CreateBitmapBrush(frost.Get(),brushProps,&frostBrush);
 HRESULT sat=dc->CreateEffect(CLSID_D2D1Saturation,&saturate);Trace("saturation",sat);
 HRESULT gb=dc->CreateEffect(CLSID_D2D1GaussianBlur,&blur);Trace("blur",gb);
 if(SUCCEEDED(sat)&&SUCCEEDED(gb)){
  saturate->SetInput(0,scene.Get());
  saturate->SetValue(D2D1_SATURATION_PROP_SATURATION,1.f);
  blur->SetInputEffect(0,saturate.Get());
  blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,18.f);
  blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD);
  blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
 }
 // A resized swap chain has to be handed to the compositor again, otherwise the
 // visual keeps presenting the buffers it was bound to and the window goes blank.
 GfxRebind();
 return true;
}

bool GfxCreate(HWND window,float dpi){
 MakeFonts();
 UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
 D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
 HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags,levels,ARRAYSIZE(levels),D3D11_SDK_VERSION,&d3d,nullptr,nullptr);
 if(FAILED(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,flags,levels,ARRAYSIZE(levels),D3D11_SDK_VERSION,&d3d,nullptr,nullptr);
 if(FAILED(hr))return false;
 ComPtr<IDXGIDevice1> dxgi;if(FAILED(d3d.As(&dxgi)))return false;dxgi->SetMaximumFrameLatency(1);
 D2D1_FACTORY_OPTIONS options{};
 if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,(void**)factory.GetAddressOf())))return false;
 if(FAILED(factory->CreateDevice(dxgi.Get(),&device)))return false;
 if(FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc)))return false;
 dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
 dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
 dc->CreateSolidColorBrush(D2D1::ColorF(1.f,1.f,1.f),&ink);
 ComPtr<IDXGIAdapter> adapter;if(FAILED(dxgi->GetAdapter(&adapter)))return false;
 ComPtr<IDXGIFactory2> dxgiFactory;if(FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory))))return false;
 RECT r{};GetClientRect(window,&r);
 DXGI_SWAP_CHAIN_DESC1 desc{};
 desc.Width=(std::max)((UINT)r.right,8u);desc.Height=(std::max)((UINT)r.bottom,8u);
 desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
 desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
 desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
 desc.Scaling=DXGI_SCALING_STRETCH;
 if(FAILED(dxgiFactory->CreateSwapChainForComposition(d3d.Get(),&desc,nullptr,&swap)))return false;
 // DCompositionCreateDevice returns the original factory interface only. It
 // cannot be queried for IDCompositionDevice3 on this machine, so the video
 // glass silently took its tint-only fallback. Create through the newer entry
 // point first, then retain the base interface for the ordinary visual tree.
 HRESULT compositionHr=DCompositionCreateDevice3(dxgi.Get(),__uuidof(IDCompositionDevice),
                                                  reinterpret_cast<void**>(compositor.GetAddressOf()));
 Trace("composition-device3",compositionHr);
 if(FAILED(compositionHr)){
  compositionHr=DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&compositor));
  Trace("composition-device",compositionHr);
 }
 if(FAILED(compositionHr))return false;
 if(FAILED(compositor->CreateTargetForHwnd(window,TRUE,&compositionTarget)))return false;
 if(FAILED(compositor->CreateVisual(&visual)))return false;
 if(FAILED(compositor->CreateVisual(&videoVisual)))return false;
 if(FAILED(compositor->CreateVisual(&rootVisual)))return false;
 // Filter effects live on the newer device interface. Where they are missing --
 // an older Windows, a compositor that declines -- the glass falls back to a
 // plain tint, which is a weaker look but never a broken one.
 HRESULT effectsHr=compositor.As(&effectDevice);Trace("composition-effects",effectsHr);
 visual->SetContent(swap.Get());
 // Order is stated against a reference rather than against the list, because
 // "top of an empty list" is the one thing worth not guessing about: the
 // interface has to be above the film, or the controls are behind the picture.
 // Film at the bottom, frosted panels above it as they are needed, interface on
 // top. The panels insert themselves directly above the film when they appear.
 // Order is stated against the film rather than against the list: "above
 // nothing" is the one thing worth not guessing about, and it is not the top.
 // Frosted panels insert themselves above the film later, which puts them
 // between the film and the interface, exactly where glass belongs.
 rootVisual->AddVisual(videoVisual.Get(),FALSE,nullptr);
 rootVisual->AddVisual(visual.Get(),TRUE,videoVisual.Get());
 compositionTarget->SetRoot(rootVisual.Get());compositor->Commit();
 return MakeTargets(desc.Width,desc.Height,dpi);
}
void GfxResize(UINT w,UINT h,float dpi){
 if(!swap||!dc)return;
 w=(std::max)(w,8u);h=(std::max)(h,8u);
 if(w==surfaceW&&h==surfaceH&&dpi==surfaceDpi)return;
 ReleaseTargets();
 HRESULT hr=swap->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0);Trace("ResizeBuffers",hr);if(FAILED(hr))return;
 MakeTargets(w,h,dpi);
 if(videoClip){
  videoClip->SetRight(float(w));videoClip->SetBottom(float(h));
  if(compositor)compositor->Commit();
 }
}
void GfxDestroy(){
 ReleaseTargets();ink.Reset();icons.clear();roundedImageMasks.clear();
 if(videoVisual)videoVisual->SetContent(nullptr);
 for(int i=0;i<MaxGlassPanels;i++){
  if(glassPanels[i]){glassPanels[i]->SetEffect(nullptr);glassPanels[i]->SetContent(nullptr);}
  glassPanels[i].Reset();glassPanels3[i].Reset();glassClips[i].Reset();
  panelContrast[i].Reset();panelSaturation[i].Reset();panelBlur[i].Reset();panelEffectRadius[i]=0;
 }
 glassContent=nullptr;glassShownCount=0;glassPendingCount=0;
 videoContentSet=false;glassVisible=false;glassBlurRadius=0;
 effectDevice.Reset();
 videoClip.Reset();videoVisual.Reset();rootVisual.Reset();
 visual.Reset();compositionTarget.Reset();compositor.Reset();swap.Reset();dc.Reset();device.Reset();factory.Reset();d3d.Reset();
 for(auto& f:faces)f.Reset();writer.Reset();
}

// Appendix F.3 asks for measured values rather than utilisation: the time from
// the first draw of a frame to that frame being ready to present. The vsync wait
// inside Present is not part of it -- waiting is not work -- so the clock stops
// before Present is called.
static LARGE_INTEGER renderBegan{},renderFreq{};
static double renderMs=0;
double GfxLastRenderMs(){return renderMs;}
struct ID3D11Device* GfxD3DDevice(){return d3d.Get();}

void GfxBeginScene(float,float){
 if(!GfxReady())return;
 if(!renderFreq.QuadPart)QueryPerformanceFrequency(&renderFreq);
 QueryPerformanceCounter(&renderBegan);
 dc->SetTarget(scene.Get());dc->BeginDraw();dc->SetTransform(D2D1::Matrix3x2F::Identity());
 dc->Clear(D2D1::ColorF(0,0,0,0));drawing=true;
}
void GfxEndScene(const D2D1_ROUNDED_RECT& shape,bool needGlass){
 if(!GfxReady()||!drawing)return;
 dc->EndDraw();drawing=false;
 frostValid=false;
 if(needGlass&&blur&&saturate){
  // The scene bitmap is a render target, so Direct2D cannot tell that its
  // contents changed and would keep serving the blur it cached on the first
  // frame. Reconnecting the input marks the graph dirty again.
  saturate->SetInput(0,nullptr);
  saturate->SetInput(0,scene.Get());
  dc->SetTarget(frost.Get());dc->BeginDraw();dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->Clear(D2D1::ColorF(0,0,0,0));dc->DrawImage(blur.Get());
  HRESULT hr=dc->EndDraw();Trace("frost-enddraw",hr);frostValid=SUCCEEDED(hr);
 }
 dc->SetTarget(backBuffer.Get());dc->BeginDraw();dc->SetTransform(D2D1::Matrix3x2F::Identity());
 dc->Clear(D2D1::ColorF(0,0,0,0));
 if(sceneBrush){sceneBrush->SetTransform(D2D1::Matrix3x2F::Identity());dc->FillRoundedRectangle(shape,sceneBrush.Get());}
 drawing=true;
}
void GfxPresent(bool vsync){
 if(!GfxReady())return;
 if(drawing){HRESULT hr=dc->EndDraw();if(FAILED(hr))Trace("EndDraw",hr);drawing=false;}
 if(renderBegan.QuadPart&&renderFreq.QuadPart){
  LARGE_INTEGER ready{};QueryPerformanceCounter(&ready);
  renderMs=double(ready.QuadPart-renderBegan.QuadPart)*1000.0/double(renderFreq.QuadPart);
  renderBegan.QuadPart=0;
 }
 HRESULT present=swap->Present(vsync?1:0,0);if(FAILED(present))Trace("Present",present);
 if(compositor)compositor->Commit();
}
ID2D1BitmapBrush* GlassSource(const D2D1_MATRIX_3X2_F& world){
 if(!frostBrush)return nullptr;
 D2D1::Matrix3x2F inverse=Mat(world);
 if(!inverse.Invert())inverse=D2D1::Matrix3x2F::Identity();
 frostBrush->SetTransform(inverse);
 return frostBrush.Get();
}
void SoftShadow(const D2D1_ROUNDED_RECT& rr,float opacity,float spread){
 if(opacity<=.01f||!ink)return;
 const float grow[]={1.f,2.f,3.f,4.f,5.f,6.f},alpha[]={.018f,.016f,.014f,.012f,.010f,.008f};
 for(int i=0;i<6;i++){
  float g=grow[i]*spread;
  auto s=D2D1::RoundedRect(D2D1::RectF(rr.rect.left-g,rr.rect.top-g+g*.45f,rr.rect.right+g,rr.rect.bottom+g+g*.7f),rr.radiusX+g,rr.radiusY+g);
  ink->SetColor(D2D1::ColorF(0,0,0,alpha[i]*opacity));dc->FillRoundedRectangle(s,ink.Get());
 }
}
void DrawRoundedBitmap(ID2D1Bitmap* bitmap,const D2D1_RECT_F& destination,float radius,const D2D1_RECT_F& source){
 if(!bitmap||!dc||!factory)return;
 float w=destination.right-destination.left,h=destination.bottom-destination.top;
 if(w<=0||h<=0)return;
 radius=(std::min)(radius,(std::min)(w,h)*.5f);
 if(roundedImageMasks.size()>128)roundedImageMasks.clear();
 auto key=std::make_tuple(w,h,radius);
 auto& mask=roundedImageMasks[key];
 if(!mask)factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0,0,w,h),radius,radius),&mask);
 if(mask)dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),mask.Get(),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
   D2D1::Matrix3x2F::Translation(destination.left,destination.top)),nullptr);
 dc->DrawBitmap(bitmap,destination,1.f,D2D1_INTERPOLATION_MODE_LINEAR,&source);
 if(mask)dc->PopLayer();
}
void Glass(const D2D1_ROUNDED_RECT& rr,const Palette& p,float opacity,const D2D1_MATRIX_3X2_F& world){
 if(!GfxReady()||opacity<=.004f)return;
 // The live video sits in a separate DirectComposition visual, so ordinary
 // Direct2D frosting cannot sample it. Its blur is supplied by the compositor;
 // draw exactly the same dense matte layers as the transport above that blur.
 if(GfxHasVideoContent()){
  SoftShadow(rr,.25f*opacity,1.35f);
  VideoModePaintMatteSurface(rr,opacity);
  return;
 }
 SoftShadow(rr,opacity);
 if(frostValid){auto b=GlassSource(world);if(b){b->SetOpacity(opacity);dc->FillRoundedRectangle(rr,b);}}
 ink->SetColor(Fade(p.glass,opacity));dc->FillRoundedRectangle(rr,ink.Get());
 ink->SetColor(Fade(p.glassEdge,opacity));dc->DrawRoundedRectangle(rr,ink.Get(),1.f);
 auto inner=rr;inner.rect=D2D1::RectF(rr.rect.left+1.5f,rr.rect.top+1.5f,rr.rect.right-1.5f,rr.rect.bottom-1.5f);
 inner.radiusX=(std::max)(0.f,rr.radiusX-1.5f);inner.radiusY=(std::max)(0.f,rr.radiusY-1.5f);
 ink->SetColor(Fade(p.glassLift,opacity));dc->DrawRoundedRectangle(inner,ink.Get(),.8f);
}

void Write(const std::wstring& s,D2D1_RECT_F r,Face f,D2D1_COLOR_F colour){
 if(s.empty()||!faces[f]||colour.a<=.004f)return;
 ink->SetColor(colour);
 dc->DrawTextW(s.c_str(),UINT32(s.size()),faces[f].Get(),r,ink.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
float Measure(const std::wstring& s,Face f,float maxWidth){
 if(s.empty()||!faces[f]||!writer)return 0;
 ComPtr<IDWriteTextLayout> layout;
 if(FAILED(writer->CreateTextLayout(s.c_str(),UINT32(s.size()),faces[f].Get(),maxWidth,40,&layout)))return 0;
 DWRITE_TEXT_METRICS m{};layout->GetMetrics(&m);return m.widthIncludingTrailingWhitespace;
}

void MeasureBlock(const std::wstring& s,Face f,float maxWidth,float& width,float& height){
 width=height=0;
 if(s.empty()||!faces[f]||!writer)return;
 ComPtr<IDWriteTextLayout> layout;
 if(FAILED(writer->CreateTextLayout(s.c_str(),UINT32(s.size()),faces[f].Get(),maxWidth,4096.f,&layout)))return;
 DWRITE_TEXT_METRICS metrics{};
 if(FAILED(layout->GetMetrics(&metrics)))return;
 width=metrics.widthIncludingTrailingWhitespace;
 height=metrics.height;
}

// Compact SVG path reader. The app's own icons are 24x24; the video transport
// also consumes Phosphor's official 256-unit SVG paths, including relative
// commands and arcs, without a runtime SVG dependency.
static ComPtr<ID2D1PathGeometry> BuildPath(const wchar_t* d){
 ComPtr<ID2D1PathGeometry> geometry;
 if(!factory||FAILED(factory->CreatePathGeometry(&geometry)))return geometry;
 ComPtr<ID2D1GeometrySink> sink;
 if(FAILED(geometry->Open(&sink))){geometry.Reset();return geometry;}
 sink->SetFillMode(D2D1_FILL_MODE_WINDING);
 const wchar_t* p=d;wchar_t command=0;bool open=false;D2D1_POINT_2F at{},start{};
 auto number=[&](float& out)->bool{
  while(*p==L' '||*p==L',')p++;
  if(!*p||(!iswdigit(*p)&&*p!=L'-'&&*p!=L'.'))return false;
  wchar_t* end=nullptr;out=wcstof(p,&end);p=end;return true;
 };
 while(*p){
  while(*p==L' '||*p==L',')p++;
  if(!*p)break;
  if(iswalpha(*p))command=*p++;
  bool relative=iswlower(command);wchar_t op=towupper(command);
  if(op==L'Z'){if(open){sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false;at=start;}continue;}
  float a=0,b=0,c=0,e=0,f=0,g=0;
  auto point=[&](float x,float y){return D2D1::Point2F(relative?at.x+x:x,relative?at.y+y:y);};
  if(op==L'M'){
   if(!number(a)||!number(b))break;
   if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
   at=point(a,b);start=at;sink->BeginFigure(at,D2D1_FIGURE_BEGIN_FILLED);open=true;command=relative?L'l':L'L';continue;
  }
  if(!open){sink->BeginFigure(at,D2D1_FIGURE_BEGIN_FILLED);open=true;}
  if(op==L'L'){if(!number(a)||!number(b))break;at=point(a,b);sink->AddLine(at);}
  else if(op==L'H'){if(!number(a))break;at.x=relative?at.x+a:a;sink->AddLine(at);}
  else if(op==L'V'){if(!number(a))break;at.y=relative?at.y+a:a;sink->AddLine(at);}
  else if(op==L'C'){
   if(!number(a)||!number(b)||!number(c)||!number(e)||!number(f)||!number(g))break;
   auto p1=point(a,b),p2=point(c,e),p3=point(f,g);
   sink->AddBezier(D2D1::BezierSegment(p1,p2,p3));at=p3;
  }
  else if(op==L'Q'){
   if(!number(a)||!number(b)||!number(c)||!number(e))break;
   auto p1=point(a,b),p2=point(c,e);sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(p1,p2));at=p2;
  }
  else if(op==L'A'){
   float rotation=0,large=0,sweep=0;
   if(!number(a)||!number(b)||!number(rotation)||!number(large)||!number(sweep)||!number(c)||!number(e))break;
   auto end=point(c,e);
   D2D1_ARC_SEGMENT arc={end,D2D1::SizeF(fabsf(a),fabsf(b)),rotation,
                         sweep>.5f?D2D1_SWEEP_DIRECTION_CLOCKWISE:D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                         large>.5f?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL};
   sink->AddArc(arc);at=end;
  }
  else break;
 }
 if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
 if(FAILED(sink->Close()))geometry.Reset();
 return geometry;
}
static void DrawIcon(const wchar_t* path,D2D1_RECT_F box,D2D1_COLOR_F colour,float stroke,bool fill,float rotation,float viewBox){
 if(!path||!GfxReady()||colour.a<=.004f)return;
 auto found=icons.find(path);
 if(found==icons.end())found=icons.emplace(path,BuildPath(path)).first;
 if(!found->second)return;
 float side=(std::min)(box.right-box.left,box.bottom-box.top),scale=side/viewBox;
 if(scale<=0)return;
 float cx=(box.left+box.right)/2,cy=(box.top+box.bottom)/2;
 D2D1_MATRIX_3X2_F previous;dc->GetTransform(&previous);
 auto local=D2D1::Matrix3x2F::Translation(-viewBox/2,-viewBox/2)*D2D1::Matrix3x2F::Rotation(rotation)*
  D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(cx,cy);
 dc->SetTransform(local*Mat(previous));
 ink->SetColor(colour);
 if(fill)dc->FillGeometry(found->second.Get(),ink.Get());
 else dc->DrawGeometry(found->second.Get(),ink.Get(),stroke/scale);
 dc->SetTransform(previous);
}
void Icon(const wchar_t* path,D2D1_RECT_F box,D2D1_COLOR_F colour,float stroke,bool fill,float rotation){
 DrawIcon(path,box,colour,stroke,fill,rotation,24.f);
}
void PhosphorIcon(const wchar_t* path,D2D1_RECT_F box,D2D1_COLOR_F colour,float stroke,bool fill,float rotation){
 DrawIcon(path,box,colour,stroke,fill,rotation,256.f);
}

std::shared_ptr<Image> Rasterise(const Image& base,const std::function<void(ID2D1DeviceContext*)>& draw){
 if(!GfxReady()||!base.w||!base.h||base.w>16384||base.h>16384)return {};
 ComPtr<ID2D1Bitmap1> canvas,readback,source;
 auto rgba=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
 if(FAILED(dc->CreateBitmap(D2D1::SizeU(base.w,base.h),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,rgba,96,96),&canvas)))return {};
 if(FAILED(dc->CreateBitmap(D2D1::SizeU(base.w,base.h),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,rgba,96,96),&readback)))return {};
 if(FAILED(dc->CreateBitmap(D2D1::SizeU(base.w,base.h),base.pixels.data(),base.w*4,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,rgba,96,96),&source)))return {};
 ComPtr<ID2D1Image> previous;dc->GetTarget(&previous);
 dc->SetTarget(canvas.Get());dc->BeginDraw();dc->SetTransform(D2D1::Matrix3x2F::Identity());
 dc->Clear(D2D1::ColorF(0,0,0,0));
 dc->DrawBitmap(source.Get(),D2D1::RectF(0,0,float(base.w),float(base.h)));
 draw(dc.Get());
 HRESULT hr=dc->EndDraw();
 dc->SetTarget(previous.Get());
 if(FAILED(hr))return {};
 if(FAILED(readback->CopyFromBitmap(nullptr,canvas.Get(),nullptr)))return {};
 D2D1_MAPPED_RECT mapped{};
 if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped)))return {};
 auto out=std::make_shared<Image>();
 out->w=base.w;out->h=base.h;out->pixels.resize(size_t(base.w)*base.h*4);
 for(unsigned y=0;y<base.h;y++)memcpy(out->pixels.data()+size_t(y)*base.w*4,mapped.bits+size_t(y)*mapped.pitch,size_t(base.w)*4);
 readback->Unmap();
 return out;
}

ID2D1Factory1* GfxFactory(){return factory.Get();}
ID2D1StrokeStyle* DashStyle(){
 static ComPtr<ID2D1StrokeStyle> dashed;
 if(!dashed&&factory){
  auto props=D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,
   D2D1_LINE_JOIN_MITER,10.f,D2D1_DASH_STYLE_CUSTOM,0.f);
  const float pattern[]={4.f,3.f};
  factory->CreateStrokeStyle(props,pattern,2,&dashed);
 }
 return dashed.Get();
}

// Threshold the picture on the GPU: a colour matrix folds the channels into
// alpha, a discrete transfer turns that into a hard mask, and the flood colour
// rides along in the matrix offsets.
static bool MakeClip(int slot){
 bool high=slot==0;
 if(FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix,&clipMatrix[slot])))return false;
 if(FAILED(dc->CreateEffect(CLSID_D2D1DiscreteTransfer,&clipTransfer[slot])))return false;
 if(FAILED(dc->CreateEffect(CLSID_D2D1Opacity,&clipFade[slot])))return false;
 D2D1_MATRIX_5X4_F m={0,0,0,1/3.f, 0,0,0,1/3.f, 0,0,0,1/3.f, 0,0,0,0,
  high?1.f:.24f, high?.20f:.55f, high?.20f:1.f, 0};
 clipMatrix[slot]->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,m);
 clipMatrix[slot]->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE,D2D1_COLORMATRIX_ALPHA_MODE_STRAIGHT);
 float table[50]={};
 if(high)table[49]=1.f;else table[0]=1.f;
 clipTransfer[slot]->SetValue(D2D1_DISCRETETRANSFER_PROP_ALPHA_TABLE,(const BYTE*)table,sizeof(table));
 clipTransfer[slot]->SetValue(D2D1_DISCRETETRANSFER_PROP_ALPHA_DISABLE,FALSE);
 clipTransfer[slot]->SetValue(D2D1_DISCRETETRANSFER_PROP_RED_DISABLE,TRUE);
 clipTransfer[slot]->SetValue(D2D1_DISCRETETRANSFER_PROP_GREEN_DISABLE,TRUE);
 clipTransfer[slot]->SetValue(D2D1_DISCRETETRANSFER_PROP_BLUE_DISABLE,TRUE);
 clipTransfer[slot]->SetInputEffect(0,clipMatrix[slot].Get());
 clipFade[slot]->SetInputEffect(0,clipTransfer[slot].Get());
 return true;
}
void DrawClipping(ID2D1Bitmap* source,bool high,float opacity){
 if(!source||!dc||opacity<=.004f)return;
 int slot=high?0:1;
 if(!clipFade[slot]&&!MakeClip(slot))return;
 clipMatrix[slot]->SetInput(0,source);
 clipFade[slot]->SetValue(D2D1_OPACITY_PROP_OPACITY,opacity);
 dc->DrawImage(clipFade[slot].Get());
}
