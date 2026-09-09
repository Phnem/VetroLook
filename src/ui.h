#pragma once
// Vetro Look, GPL-3.0-or-later. Shared UI vocabulary: motion, palette, strings, glass.
#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <functional>
#include "image.h"

// ---------------------------------------------------------------- motion ---
// Every visible value travels through a spring so nothing in the interface
// teleports. Reduce Motion collapses the springs to instant assignment.
extern bool reducedMotion;

struct Spring{
 float v=0,target=0,vel=0,k=420,c=32;
 Spring()=default;
 Spring(float value,float stiffness,float damping):v(value),target(value),k(stiffness),c(damping){}
 void Reset(float value){v=target=value;vel=0;}
 void To(float value){target=value;}
 void Nudge(float amount){vel+=amount;}
 bool Moving()const{return fabsf(v-target)>0.0004f||fabsf(vel)>0.0025f;}
 bool Step(float dt){
  if(!Moving()){v=target;vel=0;return false;}
  if(reducedMotion){v=target;vel=0;return true;}
  dt=std::min(dt,.05f);
  const float h=1.f/600.f;
  for(float t=0;t<dt;t+=h){float step=std::min(h,dt-t);vel+=((target-v)*k-vel*c)*step;v+=vel*step;}
  if(!Moving()){v=target;vel=0;}
  return true;
 }
};
// Stiff and quick for controls, softer for panels, elastic for the filmstrip,
// critically damped for anything that moves the image itself.
constexpr float ButtonK=1100,ButtonC=52;
constexpr float PanelK=340,PanelC=31;
constexpr float GalleryK=300,GalleryC=24;
constexpr float ZoomK=420,ZoomC=41.f;
inline Spring MakeSpring(float v,float k,float c){return Spring(v,k,c);}

// --------------------------------------------------------------- palette ---
struct Palette{
 D2D1_COLOR_F glass,glassEdge,glassLift,card,cardEdge,text,dim,faint,accent,danger,hover,press,track,thumb,sunk,sep,plate;
};
Palette Theme(float light);
inline const D2D1::Matrix3x2F& Mat(const D2D1_MATRIX_3X2_F& m){return *D2D1::Matrix3x2F::ReinterpretBaseType(&m);}
inline D2D1_COLOR_F Fade(D2D1_COLOR_F c,float a){c.a*=a;return c;}
inline D2D1_COLOR_F Mix(const D2D1_COLOR_F&a,const D2D1_COLOR_F&b,float t){
 return D2D1::ColorF(a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a+(b.a-a.a)*t);
}

// --------------------------------------------------------------- strings ---
enum Str{
 S_Menu,S_MenuSub,S_Send,S_Save,S_SaveAs,S_Print,S_Delete,S_ThemeLabel,S_Dark,S_Light,S_Language,
 S_Edit,S_EditSub,S_Crop,S_Rotate,S_Draw,S_Arrow,S_Select,
 S_Information,S_SecFile,S_SecCamera,S_SecHistogram,S_SecExposure,S_SecLocation,
 S_Filename,S_Format,S_Dimensions,S_Megapixels,S_FileSize,S_Created,S_Modified,S_ColorProfile,S_BitDepth,
 S_Camera,S_Lens,S_FocalLength,S_Aperture,S_Shutter,S_ISO,S_ExposureComp,S_WhiteBalance,S_Flash,S_Metering,
 S_Highlights,S_Shadows,S_HighlightClip,S_ShadowClip,S_Latitude,S_Longitude,
 S_Free,S_Apply,S_Cancel,S_Undo,S_Color,S_Thickness,S_RecycleAsk,S_Deleted,S_Saved,S_NoChanges,S_Copied,S_SaveFirst,
 S_Saving,S_Opening,S_Reading,S_Tagline,S_Hint1,S_Hint2,S_Unavailable,S_RotateLeft,S_RotateRight,
 S_Back,S_Minimise,S_Maximise,S_Restore,S_Close,S_Info,S_Copy,S_Like,S_Fit,S_OneToOne,S_More,
 S_DefaultApp,S_DefaultDone,S_DefaultFailed,S_SpacePreview,S_On,S_Off,
 S_FilePath,S_OpenMap,
 S_SearchFolders,S_SearchPhotos,S_SortName,S_SortDate,S_SortSize,
 S_FilterTitle,S_FilterFormats,S_FilterSize,S_FilterKind,S_FilterRaw,S_FilterRegular,
 S_FilterProfile,S_FilterAny,S_FilterNoProfile,S_FilterClear,S_FilterApply,
 S_Indexing,S_Found,S_Folder1,S_Folder2to4,S_Folder5plus,S_Photos,S_LibraryUpdated,S_LibraryUpdating,
 S_NoFolders,S_NoPhotos,S_NoPhotosFound,S_AppTagline,S_Rescan,
 S_WheelLabel,S_WheelZoom,S_WheelNav,
 S_Vectorscope,S_Install,S_Remove,S_TcNotFound,S_TcRunning,S_TcReplace,S_TcCancelled,
 S_TcFailed,S_TcInstalled,S_TcRemoved,
 // Library modes, the Favourites collection, and the read-only Adobe fields.
 S_Timeline,S_Folders,S_Favourites,S_NoFavourites,S_NoFavouritesHint,S_NoFavouritesHint2,
 S_SecColour,S_SecAuthor,S_SecAdobe,S_SecLens,
 S_XmpRating,S_XmpLabel,S_Rejected,S_XmpFrom,S_Sidecar,S_Embedded,
 S_Author,S_Copyright,S_Keywords,S_Title,S_Description,S_Software,
 S_LensProfile,S_ProfileFound,S_ProfileMissing,S_Distortion,S_Vignette,S_Chromatic,
 S_LensCorrection,S_Auto,S_MatchedCamera,S_MatchedLens,
 S_COUNT
};
extern int language;               // 0 Russian, 1 English
const wchar_t* T(Str s);

// ------------------------------------------------------------------ gfx ----
bool  GfxCreate(HWND window,float dpi);
void  GfxResize(UINT pixelsW,UINT pixelsH,float dpi);
void  GfxDestroy();
bool  GfxReady();
ID2D1DeviceContext* Dc();
ID2D1Factory1* GfxFactory();
void GfxRebind();
ID2D1StrokeStyle* DashStyle();
ID2D1SolidColorBrush* Ink();          // scratch solid brush
void  GfxBeginScene(float w,float h);
void  GfxEndScene(const D2D1_ROUNDED_RECT& windowShape,bool needGlass);
void  GfxPresent(bool vsync);
ID2D1BitmapBrush* GlassSource(const D2D1_MATRIX_3X2_F& world);
void  Glass(const D2D1_ROUNDED_RECT& rr,const Palette& p,float opacity,const D2D1_MATRIX_3X2_F& world);
void  SoftShadow(const D2D1_ROUNDED_RECT& rr,float opacity,float spread=1.f);
// Marks blown highlights or crushed shadows entirely on the GPU.
void  DrawClipping(ID2D1Bitmap* source,bool high,float opacity);
// Draws base plus caller supplied vector content into an offscreen target and
// reads it back, so annotations can be burned in only when the file is saved.
std::shared_ptr<Image> Rasterise(const Image& base,const std::function<void(ID2D1DeviceContext*)>& draw);

// Text formats, created once.
enum Face{F_Title,F_Meta,F_Row,F_Label,F_Value,F_Section,F_Button,F_Big,F_Small,F_Mono,F_Timeline,F_COUNT};
IDWriteTextFormat* Font(Face f);
void  Write(const std::wstring& s,D2D1_RECT_F r,Face f,D2D1_COLOR_F colour);
float Measure(const std::wstring& s,Face f,float maxWidth);

// Icons are 24x24 path data, cached as geometry and stroked or filled.
void  Icon(const wchar_t* path,D2D1_RECT_F box,D2D1_COLOR_F colour,float stroke=1.7f,bool fill=false,float rotation=0);
extern const wchar_t *IcBack,*IcMinus,*IcPlus,*IcInfo,*IcCopy,*IcCheck,*IcHeart,*IcRotate,*IcExpand,*IcCompress,
 *IcDots,*IcMin,*IcMax,*IcRestore,*IcClose,*IcShare,*IcSave,*IcSaveAs,*IcPrint,*IcTrash,*IcMoon,*IcSun,*IcGlobe,
 *IcCrop,*IcPen,*IcArrow,*IcMarquee,*IcChevron,*IcChevronL,*IcDefault,*IcSpace,*IcMouse,
 *IcSearch,*IcSortLines,*IcFilter,*IcFolderIc,*IcGridPhoto,*IcCheckbox;

// ------------------------------------------------------------- metadata ----
struct Field{std::wstring label,value;};
struct Meta{
 std::wstring path;
 // One section per Info heading. A section with no rows is not drawn, so a
 // JPEG off a phone and a RAW off a calibrated body both look deliberate.
 std::vector<Field> file,camera,colour,location,author,adobe,lens;
 bool ready=false,hasCamera=false,hasLocation=false;
 double dpiX=0,dpiY=0;unsigned orientation=0;
 double lat=0,lon=0;bool hasGps=false;

 // External XMP rating and label. Read-only: these come from Lightroom or
 // Bridge, they are never written here, and they are never mixed with the
 // Vetro Look favourite in either direction.
 bool hasRating=false;int rating=0;      // -1 rejected, 1..5 stars
 std::wstring label,xmpSource;

 // Lensfun match for this frame.
 bool lensReady=false,lensMatched=false;
 bool lensDistortion=false,lensVignetting=false,lensTca=false,lensGeometry=false;
 std::wstring lensName,lensProfile,lensNote;
 uint32_t hist[4][256]{};           // R G B Luma
 uint32_t histPeak[4]{};
 // Vectorscope: chroma density on the UV plane, hue as angle, saturation as
 // distance from the centre. Filled from the same sampled pixels as the histogram.
 static constexpr int ScopeEdge=128;
 uint32_t scope[ScopeEdge*ScopeEdge]{};
 uint32_t scopePeak=0;
 bool histReady=false;
 float clippedHigh=0,clippedLow=0;
};
// Two stages, because they cost two very different amounts. The quick pass
// reads metadata only and lands in tens of milliseconds, so the viewer header
// can show a rating before a RAW has finished demosaicing; the full pass adds
// the histogram and scopes, and needs the decoded pixels.
void MetaRequest(const std::wstring& path,uint64_t id,const std::shared_ptr<Image>& decoded,HWND notify,UINT message);
void MetaRequestQuick(const std::wstring& path,uint64_t id,HWND notify,UINT message);
bool MetaCollect(uint64_t id,Meta& out);   // true when a fresh result matched id
void MetaStop();

// ---------------------------------------------------------- integrations ---
enum TcStatus{TcMissing,TcAvailable,TcInstalled};
TcStatus TotalCommanderStatus();
bool TotalCommanderInstall(HWND owner,std::wstring& note);
bool TotalCommanderRemove(std::wstring& note);

// ------------------------------------------------------------ thumbnails ---
void ThumbStart(HWND notify,UINT message);
void ThumbStop();
void ThumbRequest(const std::wstring& path);
void ThumbPrioritize(const std::wstring& path);
std::shared_ptr<Image> ThumbLookup(const std::wstring& path);
void ThumbTrim(const std::vector<std::wstring>& keep);
