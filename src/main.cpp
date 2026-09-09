// Vetro Look, GPL-3.0-or-later.
// Every control floats over the photograph, carries its own spring, and is laid
// out once per frame into a hot list that both painting and hit testing read.
#include "ui.h"
#include "actions.h"
#include "index.h"
#include "dnd.h"
#include "favourites.h"
#include "pipeline.h"
#include "metacache.h"
#include "lensdb.h"
#include <roapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <ole2.h>
#include <psapi.h>
#include <wincodec.h>
#include <dbt.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;

constexpr UINT Loaded=WM_APP+1,Preview=WM_APP+2,Tray=WM_APP+3,Saved=WM_APP+4,ActivateNormal=WM_APP+5,
 MetaReady=WM_APP+6,ThumbReady=WM_APP+7,IndexFolders=WM_APP+8,IndexProgress=WM_APP+9;

// ---------------------------------------------------------------- ids ------
enum Id{
 IdNone=0,IdBack,IdZoomOut,IdZoomIn,IdTrack,
 IdInfo,IdCopy,IdLike,IdRotate,IdFit,IdMore,IdEdit,
 IdWinMin,IdWinMax,IdWinClose,
 IdSend,IdSave,IdSaveAs,IdPrint,IdDelete,IdDefaultApp,IdSpacePreview,IdTotalCmd,IdThemeDark,IdThemeLight,IdLanguage,
 IdLangBack,IdLangRu,IdLangEn,IdConfirmDelete,IdConfirmCancel,
 IdToolCrop,IdToolRotate,IdToolDraw,IdToolArrow,IdToolSelect,
 IdRotLeft,IdRotRight,
 IdHistRGB,IdHistLuma,IdHistR,IdHistG,IdHistB,IdClipHigh,IdClipLow,IdScopeHist,IdScopeVector,
 IdCropFree,IdCrop11,IdCrop43,IdCrop32,IdCrop169,IdCropApply,IdCropCancel,
 IdUndo,IdThin,IdMedium,IdThick,IdSwatch0,IdSwatch1,IdSwatch2,IdSwatch3,IdSwatch4,IdSwatch5,
 IdPanelBody,IdGalleryBody,IdZoomBar,IdDockBar,IdWinBar,IdThemeRow,IdWheelRow,IdWheelZoom,IdWheelNav,
 IdPrevPhoto,IdNextPhoto,IdShapeRect,IdShapeEllipse,IdCopyPath,IdOpenMap,
 IdLibBack,IdLibSearch,IdLibSort,IdLibFilter,IdLibViewFolders,IdLibViewPhotos,IdLibRescan,
 IdSortName,IdSortDate,IdSortSize,
 IdFilterRawOnly,IdFilterRegularOnly,IdFilterProfileAny,IdFilterProfileSRGB,IdFilterProfileP3,
 IdFilterProfileAdobe,IdFilterProfileNone,IdFilterSizeLo,IdFilterSizeHi,IdFilterClear,IdFilterApply,
 IdFilterPopup,IdSortPopup,IdLibBrand,
 IdFavourites,IdFavouritesChip,
 IdGallery0=4096,
 IdFilterExt0=1<<19,       // +one per supported extension
 IdCard0=1<<21             // +one per visible library/album grid cell
};
enum Panel{PanelNone,PanelInfo,PanelMenu,PanelEdit};
enum Tool{ToolNone,ToolCrop,ToolRotate,ToolDraw,ToolArrow,ToolSelect};
enum Screen{ScrLibrary,ScrAlbum,ScrViewer};
// The library opens on the timeline. Every modern gallery — Photos, Google
// Photos, Samsung Gallery — puts a chronological grid first, and a folder
// tree second, because that is how people look for a photograph they took.
enum LibView{ViewFolders,ViewPhotosFlat};
constexpr int DefaultLibView=ViewPhotosFlat;

// -------------------------------------------------------------- state ------
HWND win=nullptr,explorer=nullptr;HHOOK hook=nullptr;
ComPtr<ID2D1Bitmap> bitmap,backdrop;
std::map<std::wstring,ComPtr<ID2D1Bitmap>> thumbBitmaps;
std::shared_ptr<Image> current;
std::wstring currentPath,errorText;
// Which cached frame the renderer is currently showing, in the cache's own
// key space. Used to address GPU slots without re-deriving identity.
std::wstring currentFrameKey;
bool loading=false,preview=false,compactWindow=false,stopping=false,actionBusy=false;
// A direct --quicklook invocation is a short-lived window, unlike Explorer's
// reusable preview host.  Closing it must therefore end the process.
bool quickLookInvocation=false;
bool lowMemoryActive=false;
bool showingPreview=false;   // the frame on screen is the fast first look, not the full decode
bool backgroundMode=false;
bool requestPreview=false,requestForce=false,fit=true,dragImage=false,liked=false,confirmDelete=false;
bool clipHigh=false,clipLow=false,testing=false,autostart=false;
float dpi=1;
std::vector<std::wstring> siblings;
RECT normalBounds{150,100,1280,880};

Spring themeMix(0,PanelK,PanelC),wheelMix(0,PanelK,PanelC);
// 0 zooms with the wheel, 1 walks the filmstrip with it. Free-spinning wheels
// and touchpads send fractions of a notch, so navigation accumulates them.
int wheelMode=0;float wheelCarry=0;
int tcStatus=TcMissing;   // refreshed whenever the menu opens
Spring zoomLog(0,ZoomK,ZoomC),panSX(0,ZoomK,ZoomC),panSY(0,ZoomK,ZoomC),rotate(0,PanelK,PanelC);
Spring panelSlide(0,PanelK,PanelC),levelSlide(0,PanelK,PanelC),titleIn(1,PanelK,PanelC);
Spring toastIn(0,PanelK,PanelC),likePop(1,ButtonK,ButtonC),thumbLift(0,ButtonK,ButtonC);
Spring clipHighFade(0,PanelK,PanelC),clipLowFade(0,PanelK,PanelC),histRise(0,GalleryK,GalleryC);
int panel=PanelNone,pendingPanel=PanelNone,menuLevel=0,tool=ToolNone;
// No lens-correction switches. The database is read and the profile is
// matched — Info reports both — but nothing applies those coefficients to
// pixels yet, and a control that corrects nothing is worse than no control.
// See "Lens correction" in REPORT.md.
float rotateSpan=90;
std::wstring prevName,prevMeta;
std::wstring toast;double toastUntil=0,copyUntil=0,likeBurst=0;
Meta info;uint64_t metaId=0;bool metaPending=false;
uint64_t metaQuickId=0,metaQuickCounter=1ull<<40;   // a separate id space from the full pass
ComPtr<ID2D1PathGeometry> histPath[4];
ComPtr<ID2D1Bitmap> scopeBitmap;
int histMode=0,scopeMode=0;   // scopeMode: 0 histogram, 1 vectorscope
float panelScroll=0,panelScrollVel=0,panelExtent=0;

// crop, annotations and selection all live in the rotated display image space
bool hasCrop=false;D2D1_RECT_F crop{};int cropAspect=0,cropGrab=-1;
struct Stroke{std::vector<D2D1_POINT_2F> pts;D2D1_COLOR_F colour;float width;int kind;};
std::vector<Stroke> strokes;Stroke live;bool painting=false;
int swatch=0,shapeKind=2;float thickness=4;
Spring chrome(1,180,28);double lastInteraction=0;
bool customMax=false;RECT customRestore{};
bool WindowMaximized(){return customMax||IsZoomed(win);}
void Frame();
void PaintStrokes();
const D2D1_COLOR_F swatches[6]={{1.f,.27f,.25f,1.f},{1.f,.78f,.20f,1.f},{.30f,.85f,.45f,1.f},
 {.30f,.62f,1.f,1.f},{.78f,.44f,1.f,1.f},{1.f,1.f,1.f,1.f}};

// gallery
std::vector<Spring> galleryWidth;
float galleryScroll=0,galleryVel=0,galleryTarget=0;
bool galleryDragging=false,galleryHasTarget=false;
float galleryGrabX=0,galleryGrabScroll=0;
double galleryGrabTime=0;
// Navigation samples determine both whether decorative image transitions are
// worth drawing and which side of the filmstrip receives prefetch priority.
int navigationDirection=1,navigationBurst=0;
double lastNavigateAt=0,fastNavigationUntil=0;
bool fastNavigation=false;

// -------------------------------------------------------------- library ----
// The browseable collection: folders holding photos, a single folder's grid,
// and the single-image viewer, connected by a real navigation stack so Back
// always lands where the user actually came from — including when the app
// was launched straight onto a file from Explorer, which synthesises the
// two frames underneath it so Back still walks Viewer -> Album -> Library.
struct NavFrame{int screen;std::wstring folder;uint64_t photoId;float scroll;};
std::vector<NavFrame> navStack;
int screen=ScrLibrary,libView=DefaultLibView;
// The virtual Favourites collection: not a folder, no files moved or copied,
// just the local database filtered to favourite=true.
bool favouritesOpen=false;
std::vector<PhotoEntry> favouritePhotos;
uint64_t favouritesSeen=~0ull;
std::vector<FolderEntry> libFolders;
std::vector<PhotoEntry> albumPhotos;
std::unordered_map<uint64_t,std::vector<PhotoEntry>> assetVariants;
std::wstring albumFolder;
std::wstring libSearch;bool libSearchFocused=false;double libSearchCaretOn=0;
int libSortField=1;bool libSortDesc=true;               // 0 name, 1 date, 2 size
bool filterOpen=false,sortOpen=false;
Spring filterReveal(0,PanelK,PanelC),sortReveal(0,PanelK,PanelC);
Spring libViewSlide(0,PanelK,PanelC),libContentIn(1,PanelK,PanelC);
float libContentDirection=1;
// Empty means "all formats".  Once the user picks a chip it becomes an
// explicit inclusion set, so one click on NEF really means "only NEF".
std::unordered_map<std::wstring,bool> filterExtOn;
bool filterRawOnly=false,filterRegularOnly=false;
int filterProfile=0;                                    // 0 any,1 sRGB,2 P3,3 AdobeRGB,4 none
uint64_t filterSizeLo=0,filterSizeHi=~0ull;
bool filtersActive=false;
float libScroll=0,libScrollVel=0,libScrollExtent=0;
float albumScroll=0,albumScrollVel=0,albumScrollExtent=0;
float filterScroll=0,filterScrollVel=0,filterScrollExtent=0;
float scanSpin=0;
std::unordered_map<std::wstring,std::wstring> profileCache; // lower path -> profile, filled lazily
std::mutex profileMx;
struct TimelineGroup{uint32_t day=0;std::vector<size_t> photos;};
std::vector<TimelineGroup> timelineGroups;
struct Btn{Spring hover{0,ButtonK,ButtonC},press{0,ButtonK,ButtonC};};
std::map<int,Btn> buttons;
Btn& B(int id){return buttons[id];}

// Shared-element transitions. Both ride the same spring family so they feel
// like one physical system: a rect grows/shrinks with the image inside it,
// crossfading from whatever bitmap was already on screen to the real one.
struct HeroTransition{
 bool active=false,closing=false;
 uint64_t photoId=0;
 Spring left{0,230,25},top{0,230,25},right{0,230,25},bottom{0,230,25},radius{0,230,25},shadow{0,230,25};
 Spring crossfade{0,240,26};
 ComPtr<ID2D1Bitmap> fromBitmap;
 unsigned fromW=0,fromH=0;
};
HeroTransition hero;
struct FolderTransition{
 bool active=false,closing=false;
 double started=0;
 std::wstring folder;
 Spring blend{0,210,24};
 struct Photo{std::wstring path;D2D1_RECT_F from{},to{};float angle=0;ComPtr<ID2D1Bitmap> texture;};
 std::vector<Photo> photos;
 D2D1_RECT_F front{};
};
FolderTransition folderTx;
std::unordered_map<std::wstring,std::vector<FolderTransition::Photo>> folderPreviews;
std::unordered_map<std::wstring,D2D1_RECT_F> folderFronts;
void PrepareFolderTransition(const std::wstring& folder,bool closing);
void PaintFolderTransition();
bool dragArmed=false,dragActive=false;POINT dragOrigin{};std::wstring dragPath;
std::unordered_set<uint64_t> selectedPhotos;
uint64_t selectionAnchor=0;bool selectionGesture=false;

struct Hot{int id;D2D1_RECT_F r;};
std::vector<Hot> hots;
std::map<int,D2D1_RECT_F> rects;
int hover=IdNone,pressed=IdNone;
POINT mouse{};bool sliderGrab=false;

std::mutex mx;std::condition_variable cv;std::wstring requested;uint64_t generation=0;std::atomic<uint64_t> latest{0};
unsigned requestEdge=2048;         // long edge, in pixels, worth showing before the full decode lands
// Whether this request should go past the screen tier. Fit-to-window browsing
// does not: a screen frame covers the viewport and costs a seventh of the
// bytes to put on the GPU.
bool requestFull=false;
// RAW full development costs seconds. It only starts once the reader has
// actually settled on a photograph, so walking a folder never queues thirty of
// them behind the one being looked at.
constexpr double RawFullDebounceSeconds=0.45;
std::atomic<uint64_t> rawFullStarted{0},rawFullCompleted{0},rawFullCancelled{0},rawFullSkippedBrowsing{0};
std::atomic<uint64_t> fullGpuUploads{0},screenGpuUploads{0};
// Counting uploads by tier over-reports: a 400 KB PNG decoded at its own size
// is a "full" frame and costs nothing. Bytes are what the bus actually moves,
// and a large upload is the thing that must not appear on the navigation path.
std::atomic<uint64_t> gpuUploadBytes{0},gpuLargeUploads{0},gpuMaxUploadBytes{0};
constexpr size_t LargeUploadBytes=32ull*1024*1024;
std::vector<std::wstring> requestFiles;
// A partial result is the fast first look: the camera's embedded preview or a
// scaled decode, replaced in place once the full-resolution frame is ready.
struct Result{uint64_t id;std::wstring path,error,key;std::shared_ptr<Image> image;std::vector<std::wstring> files;bool partial=false;};
void RequestFullResolution();
// Preview and full-resolution results may be produced faster than the window
// thread handles WM_APP.  A single slot lets the full frame overwrite the
// preview before it was ever painted, defeating the two-stage contract.
std::deque<std::unique_ptr<Result>> ready;std::thread worker,actionWorker;
struct SaveResult{bool ok;std::wstring path,error;uint64_t imageId;};
std::unique_ptr<SaveResult> saveResult;
// Decoded frames are expensive, especially for RAW.  This cache is measured
// in real pixel bytes and adjusts to the machine's currently available RAM;
// it is intentionally separate from the thumbnail subsystem's L3 cache.
enum CacheTier{CacheFull=0,CacheScreen=1,CacheWarm=2};
struct CacheEntry{std::shared_ptr<Image> image;size_t cost=0;ULONGLONG used=0;int tier=CacheWarm;std::wstring path;};
std::map<std::wstring,CacheEntry> frameCache;
std::mutex cacheMx;
size_t cacheBytes=0,cacheBudget=0;
// Bytes held per tier. A full-resolution frame is fifteen to twenty times the
// size of the screen-ready frame of the same photograph, so three of them can
// evict thirty useful neighbours. The tiers are budgeted separately to stop
// exactly that: full frames get a small allowance, screen frames get the rest.
size_t cacheBytesByTier[3]={0,0,0};
HANDLE lowMemoryNotice=nullptr;
std::atomic<uint64_t> prefetchEpoch{0};
std::atomic<uint64_t> cacheHits{0},cacheMisses{0},cacheEvictions{0},prefetchCancelled{0};
// One navigation's anatomy. Filled by Open(), CacheGet() and GpuTextureFor()
// as they run, so a transition that took 40 ms can say which of them it spent
// the time in instead of leaving it to be guessed at.
enum CacheTierHit{TierMiss=-1,TierFull=0,TierScreen=1,TierWarm=2,TierThumb=3};
struct NavTrace{
 double cacheLookupMs=0,gpuMs=0,presentMs=0,lockWaitMs=0;
 int tier=TierMiss;
 bool gpuResident=false,gpuReused=false,gpuCreated=false;
 bool directionChanged=false,viewportChanged=false;
 uint64_t evictionsBefore=0,evictionsDuring=0;
 unsigned width=0,height=0;
};
NavTrace navTrace;
// A snapshot taken as Open() returns. The live `navTrace` keeps being written
// after that — idle pre-upload also goes through GpuTextureFor — so anything
// reading a navigation's anatomy afterwards must read the snapshot.
NavTrace navLastTrace;
// Time spent waiting for the frame-cache lock, accumulated across one
// navigation. Contention here would show up as an unexplained stall.
std::atomic<double> cacheLockWaitMs{0.0};
// GPU slot accounting, so a navigation that felt slow can be attributed to an
// upload rather than guessed at.
double lastUploadMs=0;bool lastUploadWasReuse=false,lastFrameWasPreUploaded=false;
std::atomic<uint64_t> gpuUploads{0},gpuReuses{0},gpuPreUploads{0},gpuHits{0};
constexpr size_t CacheMB=1024ull*1024ull,CacheGB=1024ull*CacheMB;
constexpr size_t CacheHardCap=4ull*CacheGB,CacheLowMin=128ull*CacheMB,CacheLowMax=256ull*CacheMB;
std::wofstream logFile;int testStage=0,testWait=0,testFailures=0;std::vector<std::wstring> testFiles;
// --nav-bench: a real navigation stress run through the real window, so
// cache-hit latency is measured where it actually happens — including the GPU
// upload — rather than in a headless harness that skips the renderer.
bool navBench=false;int navPhase=0,navIndex=0,navRepeat=0,navSettle=0,navBenchSteps=200;
// Remembered between navigations so a transition can say whether the reader
// changed direction or resized the window — both legitimate reasons for a
// miss that must not be filed as a cache fault.
int navPreviousDirection=0;unsigned navPreviousWidth=0,navPreviousHeight=0;
unsigned navBenchInterval=16;
std::vector<std::wstring> navFiles;
struct NavSample{
 double ms=0,cacheLookupMs=0,gpuMs=0,lockWaitMs=0;
 int phase=0,tier=-1;
 bool gpuResident=false,gpuReused=false,gpuCreated=false;
 bool directionChanged=false,viewportChanged=false;
 uint64_t evictions=0;
 std::wstring file;
};
std::vector<NavSample> navSamples;
double navLastOpenMs=0;
ULONGLONG boot=GetTickCount64();
bool needFrame=true;
std::chrono::steady_clock::time_point openStarted;
uint64_t latencyId=0,currentGeneration=0;bool firstVisibleLogged=false,fullVisibleLogged=false;

void Open(const std::wstring& path,bool force=false);
void Fit();
void SyncGallery();

// ------------------------------------------------------------- helpers -----
double Now(){return double(GetTickCount64())/1000.0;}
std::mutex logMx;   // the decode worker reports its own timings
void Log(const std::wstring& s){
 std::lock_guard lock(logMx);
 if(logFile){logFile<<GetTickCount64()-boot<<L"ms "<<s<<L"\n";logFile.flush();}
 OutputDebugStringW((s+L"\n").c_str());
}
void QuickLog(const std::wstring& s){
 if(!backgroundMode)return;wchar_t local[MAX_PATH]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH))return;
 std::wstring dir=std::wstring(local)+L"\\VetroLook";CreateDirectoryW(dir.c_str(),nullptr);
 std::wofstream out(fs::path(dir+L"\\quicklook.log"),std::ios::app);if(out)out<<GetTickCount64()<<L" "<<s<<L"\n";
}
void Wake(){needFrame=true;}
void Size(float& w,float& h){RECT r{};GetClientRect(win,&r);w=r.right/dpi;h=r.bottom/dpi;}
Palette Pal(){return Theme(themeMix.v);}
float Clamp(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);}
bool Inside(const D2D1_RECT_F& r,float x,float y){return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom;}
D2D1_RECT_F Inset(D2D1_RECT_F r,float d){return D2D1::RectF(r.left+d,r.top+d,r.right-d,r.bottom-d);}
D2D1_RECT_F Shift(D2D1_RECT_F r,float dx,float dy){return D2D1::RectF(r.left+dx,r.top+dy,r.right+dx,r.bottom+dy);}
float Width(const D2D1_RECT_F& r){return r.right-r.left;}
float Height(const D2D1_RECT_F& r){return r.bottom-r.top;}
D2D1_RECT_F Grow(D2D1_RECT_F r,float dx,float dy){return D2D1::RectF(r.left-dx,r.top-dy,r.right+dx,r.bottom+dy);}
void Hotspot(int id,D2D1_RECT_F r){hots.push_back({id,r});rects[id]=r;}
D2D1_RECT_F R(int id){auto f=rects.find(id);return f==rects.end()?D2D1::RectF(0,0,0,0):f->second;}
int HitTest(float x,float y){
 if(chrome.v<.1f)return IdNone;
 auto body=R(IdPanelBody);
 for(auto it=hots.rbegin();it!=hots.rend();++it)if(Inside(it->r,x,y)){
  // Panel controls must never hit through their clipped scroll viewport.
  bool panelControl=it->id>=IdSend&&it->id<=IdSwatch5;
  panelControl|=it->id==IdShapeRect||it->id==IdShapeEllipse||it->id==IdCopyPath||it->id==IdOpenMap;
  if(panelControl&&(!Inside(body,x,y)||y<body.top+72))continue;
  return it->id;
 }
 return IdNone;
}
std::wstring Name(){return currentPath.empty()?std::wstring():fs::path(currentPath).filename().wstring();}
void Notify(const std::wstring& text){toast=text;toastUntil=Now()+2.6;toastIn.To(1);Wake();}

constexpr float MinZoom=.02f,MaxZoom=32.f;
float Zoom(){return expf(zoomLog.v);}
float ZoomToSlider(float z){return logf(Clamp(z,MinZoom,MaxZoom)/MinZoom)/logf(MaxZoom/MinZoom);}
float SliderToZoom(float t){return MinZoom*powf(MaxZoom/MinZoom,Clamp(t,0,1));}
bool Turned(){return int(fabsf(rotate.target))%180!=0;}
// Display space: the pixels the renderer is actually drawing. Everything that
// positions, zooms, crops or hit-tests works here.
unsigned DisplayW(){return current?(Turned()?current->h:current->w):0;}
unsigned DisplayH(){return current?(Turned()?current->w:current->h):0;}
// Asset space: what the file contains. The header reports this, Save writes
// this, and neither may be derived from whichever tier happens to be on screen.
float FrameScale(){return current?current->Scale():1.f;}
unsigned SourceW(){return current?(Turned()?current->SourceH():current->SourceW()):0;}
unsigned SourceH(){return current?(Turned()?current->SourceW():current->SourceH()):0;}
bool CropActive(){return hasCrop&&tool!=ToolCrop;}
// Full resolution is about the photograph, not about the frame in hand.
//
// The test is the on-screen magnification against the *asset*: at 1:1 every
// original pixel is wanted, so the file's own pixels are needed. Comparing
// against the displayed frame instead would mean that merely enlarging the
// window — which raises Zoom() without asking for any more detail than the
// asset already has — demanded a full-resolution decode.
bool NeedsFullResolution(){return !fit&&Zoom()*FrameScale()>0.95f;}
float EffW(){return CropActive()?(std::max)(1.f,crop.right-crop.left):float((std::max)(1u,DisplayW()));}
float EffH(){return CropActive()?(std::max)(1.f,crop.bottom-crop.top):float((std::max)(1u,DisplayH()));}
float CropDX(){return CropActive()?((crop.left+crop.right)/2-DisplayW()/2.f):0.f;}
float CropDY(){return CropActive()?((crop.top+crop.bottom)/2-DisplayH()/2.f):0.f;}
float FitZoom(){
 float w,h;Size(w,h);
 if(!current)return 1;
 // The controls float over the photograph; they do not reserve canvas space.
 float aw=(std::max)(1.f,w),ah=(std::max)(1.f,h);
 return (std::max)(.001f,(std::min)(aw/EffW(),ah/EffH()));
}
bool zoomAnchored=false;
bool windowAnimating=false,windowAnimationMax=false;
float windowAnimationTime=0;
RECT windowAnimationFrom{},windowAnimationTo{};
WINDOWPLACEMENT windowRestore{sizeof(WINDOWPLACEMENT)};
WINDOWPLACEMENT beforePreview{sizeof(WINDOWPLACEMENT)};
bool beforePreviewValid=false;
float zoomAnchorX=0,zoomAnchorY=0,zoomPointX=0,zoomPointY=0;
// Any zoom that crosses into magnifying the displayed frame needs the asset's
// own pixels; below that the screen tier is exact.
void MaybeRequestFull(){
 if(NeedsFullResolution())RequestFullResolution();
}
void SetZoom(float z,float anchorX,float anchorY){
 z=Clamp(z,MinZoom,MaxZoom);
 float from=Zoom();
 zoomLog.To(logf(z));
 zoomAnchored=true;zoomAnchorX=anchorX;zoomAnchorY=anchorY;
 zoomPointX=(anchorX-panSX.v)/from;zoomPointY=(anchorY-panSY.v)/from;
 panSX.To(anchorX-zoomPointX*z);
 panSY.To(anchorY-zoomPointY*z);
 fit=false;MaybeRequestFull();Wake();
}
void Fit(){
 zoomAnchored=false;
 float z=FitZoom();
 zoomLog.To(logf(Clamp(z,MinZoom,MaxZoom)));
 panSX.To(0);panSY.To(0);fit=true;Wake();
}
void Snap(){zoomLog.Reset(zoomLog.target);panSX.Reset(panSX.target);panSY.Reset(panSY.target);}
void AnimateWindow(bool maximise){
 if(preview||windowAnimating)return;
 GetWindowRect(win,&windowAnimationFrom);
 MONITORINFO monitor{sizeof(monitor)};
 GetMonitorInfoW(MonitorFromWindow(win,MONITOR_DEFAULTTONEAREST),&monitor);
 BOOL disable=TRUE;DwmSetWindowAttribute(win,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable));
 if(maximise){
  GetWindowPlacement(win,&windowRestore);
  customRestore=windowAnimationFrom;
  windowAnimationTo=monitor.rcWork;
 }else{
  // Restore the OS state before interpolating, but keep the visible rectangle
  // at the old bounds until the first animation frame is ready.
  if(customMax){windowAnimationTo=customRestore;customMax=false;}
  else {ShowWindow(win,SW_RESTORE);GetWindowRect(win,&windowAnimationTo);}
  SetWindowPos(win,nullptr,windowAnimationFrom.left,windowAnimationFrom.top,
   windowAnimationFrom.right-windowAnimationFrom.left,windowAnimationFrom.bottom-windowAnimationFrom.top,
   SWP_NOZORDER|SWP_NOACTIVATE);
 }
 windowAnimationMax=maximise;windowAnimationTime=reducedMotion?.22f:0;windowAnimating=true;Wake();
}
bool StepWindow(float dt){
 if(!windowAnimating)return false;
 windowAnimationTime+=dt;
 float t=Clamp(windowAnimationTime/.22f,0,1);
 float ease=t*t*(3.f-2.f*t);
 auto lerp=[&](LONG a,LONG b){return LONG(lroundf(a+(b-a)*ease));};
 RECT r{lerp(windowAnimationFrom.left,windowAnimationTo.left),lerp(windowAnimationFrom.top,windowAnimationTo.top),
  lerp(windowAnimationFrom.right,windowAnimationTo.right),lerp(windowAnimationFrom.bottom,windowAnimationTo.bottom)};
 SetWindowPos(win,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
 if(t>=1){
  windowAnimating=false;
  if(windowAnimationMax){
   // Do not ask ShowWindow/SetWindowPlacement to replay a second OS transition.
   customMax=true;
  }
  BOOL disable=FALSE;DwmSetWindowAttribute(win,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable));
 }
 return true;
}
D2D1::Matrix3x2F ImageMatrix(){
 float w,h;Size(w,h);
 float away=Clamp(fabsf(rotate.v-rotate.target)/(std::max)(1.f,rotateSpan),0,1);
 float dip=1.f-.09f*sinf(3.14159265f*(1.f-away));
 float z=Zoom()*dip;
 float cx=current?current->w/2.f:0,cy=current?current->h/2.f:0;
 return D2D1::Matrix3x2F::Translation(-cx,-cy)*D2D1::Matrix3x2F::Scale(z,z)*
  D2D1::Matrix3x2F::Rotation(rotate.v)*D2D1::Matrix3x2F::Translation(w/2+panSX.v-CropDX()*z,h/2+panSY.v-CropDY()*z);
}
// Display space is the image after its settled rotation; crop and annotations
// are stored there so they survive zoom and pan untouched.
D2D1::Matrix3x2F DisplayMatrix(){
 float w,h;Size(w,h);
 float away=Clamp(fabsf(rotate.v-rotate.target)/(std::max)(1.f,rotateSpan),0,1);
 float dip=1.f-.09f*sinf(3.14159265f*(1.f-away));
 float z=Zoom()*dip;
 float extra=rotate.v-rotate.target;
 return D2D1::Matrix3x2F::Translation(-float(DisplayW())/2.f,-float(DisplayH())/2.f)*D2D1::Matrix3x2F::Scale(z,z)*
  D2D1::Matrix3x2F::Rotation(extra)*D2D1::Matrix3x2F::Translation(w/2+panSX.v-CropDX()*z,h/2+panSY.v-CropDY()*z);
}
D2D1_POINT_2F ToDisplay(float x,float y){
 auto m=DisplayMatrix();
 if(!m.Invert())return D2D1::Point2F(0,0);
 return m.TransformPoint(D2D1::Point2F(x,y));
}
D2D1_RECT_F ToScreen(const D2D1_RECT_F& r){
 auto m=DisplayMatrix();
 auto a=m.TransformPoint(D2D1::Point2F(r.left,r.top)),b=m.TransformPoint(D2D1::Point2F(r.right,r.bottom));
 return D2D1::RectF((std::min)(a.x,b.x),(std::min)(a.y,b.y),(std::max)(a.x,b.x),(std::max)(a.y,b.y));
}

// ---------------------------------------------------------- image cache ----
size_t ProcessWorkingSet(){
 PROCESS_MEMORY_COUNTERS_EX counters{sizeof(counters)};
 return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))
  ?size_t(counters.WorkingSetSize):0;
}
size_t CacheBudgetFor(uint64_t available,uint64_t total,uint64_t workingSet){
 // The hard cap is deliberately independent of installed RAM.  The cache is
 // a good citizen on 8 GB laptops and still useful on high-memory workstations.
 (void)total;
 uint64_t safeAvailable=available>workingSet/2?available-workingSet/2:available/2;
 auto bounded=[](size_t value,size_t low,size_t high){return (std::max)(low,(std::min)(value,high));};
 if(safeAvailable<2*CacheGB)return bounded(size_t(safeAvailable/8),CacheLowMin,CacheLowMax);
 if(safeAvailable<8*CacheGB)return bounded(size_t(safeAvailable/10),256*CacheMB,1*CacheGB);
 if(safeAvailable<24*CacheGB)return bounded(size_t(safeAvailable/8),1*CacheGB,2*CacheGB);
 return bounded(size_t(safeAvailable/8),2*CacheGB,CacheHardCap);
}
size_t RefreshCacheBudget(){
 // VETRO_CACHE_MB pins the budget. It exists so the memory behaviour can be
 // tested at 256 MB on a machine with 32 GB free, which is otherwise only
 // reachable by filling the machine's memory with something else.
 static int forced=-1;
 if(forced<0){
  wchar_t value[16]{};
  forced=GetEnvironmentVariableW(L"VETRO_CACHE_MB",value,16)?(std::max)(0,_wtoi(value)):0;
 }
 if(forced>0){cacheBudget=size_t(forced)*CacheMB;return cacheBudget;}
 MEMORYSTATUSEX state{sizeof(state)};
 if(!GlobalMemoryStatusEx(&state))return cacheBudget?cacheBudget:CacheLowMin;
 cacheBudget=CacheBudgetFor(state.ullAvailPhys,state.ullTotalPhys,ProcessWorkingSet());
 return cacheBudget;
}
std::wstring FrameKey(const std::wstring& path,int tier,unsigned edge){
 std::error_code error;uintmax_t bytes=fs::file_size(fs::path(path),error);if(error)bytes=0;
 error.clear();auto stamp=fs::last_write_time(fs::path(path),error);
 auto ticks=error?0ll:stamp.time_since_epoch().count();
 // Source EXIF orientation is applied during every decode, so it is part of
 // the key's decode contract even though it is read from the stable file.
 return NormalisePath(path)+L"|"+std::to_wstring(bytes)+L"|"+std::to_wstring(ticks)+L"|"+
  std::to_wstring(tier)+L"|"+std::to_wstring(edge)+L"|source-orientation";
}
// The full tier is allowed a quarter of the budget and never more than about
// three frames' worth; everything else belongs to the screen and thumb tiers,
// which is what makes navigation feel instant.
size_t FullTierBudget(){return (std::max)(size_t(64*CacheMB),cacheBudget/4);}

void EraseLocked(std::map<std::wstring,CacheEntry>::iterator it){
 cacheBytes-=it->second.cost;
 cacheBytesByTier[it->second.tier<3?it->second.tier:2]-=it->second.cost;
 frameCache.erase(it);cacheEvictions++;
}
// Evicts within one tier only, oldest first, never touching the frame the
// viewer is currently showing.
void TrimTierLocked(int tier,size_t target,const std::wstring& protectedPath){
 while(cacheBytesByTier[tier]>target){
  auto victim=frameCache.end();
  for(auto it=frameCache.begin();it!=frameCache.end();++it){
   if(it->second.tier!=tier)continue;
   if(it->second.path==protectedPath&&tier!=CacheWarm)continue;   // pinned
   if(victim==frameCache.end()||it->second.used<victim->second.used)victim=it;
  }
  if(victim==frameCache.end())break;
  EraseLocked(victim);
 }
}
void CacheTrimLocked(const std::wstring& protectedPath,bool aggressive){
 size_t budget=aggressive?(std::min)(cacheBudget,CacheLowMin):cacheBudget;
 // Under pressure the order matters, and it is the reverse of usefulness:
 // speculative full frames first, then distant screen frames, then thumbs.
 TrimTierLocked(CacheFull,aggressive?0:FullTierBudget(),protectedPath);
 size_t rest=budget>cacheBytesByTier[CacheFull]?budget-cacheBytesByTier[CacheFull]:0;
 TrimTierLocked(CacheScreen,rest*3/4,protectedPath);
 TrimTierLocked(CacheWarm,rest/4,protectedPath);
 // A last sweep in case the tier splits still leave the total over budget.
 while(cacheBytes>budget&&!frameCache.empty()){
  auto victim=frameCache.end();
  for(auto it=frameCache.begin();it!=frameCache.end();++it){
   if(it->second.path==protectedPath&&it->second.tier!=CacheWarm)continue;
   if(victim==frameCache.end()||it->second.tier>victim->second.tier||
      (it->second.tier==victim->second.tier&&it->second.used<victim->second.used))victim=it;
  }
  if(victim==frameCache.end())break;
  EraseLocked(victim);
 }
}
std::shared_ptr<Image> CacheGet(const std::wstring& key){
 auto waitStart=std::chrono::steady_clock::now();
 std::unique_lock lock(cacheMx);
 {
  double waited=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-waitStart).count();
  // A relaxed read-modify-write is fine: this is a diagnostic counter, and the
  // only writer that matters is the thread doing the navigation.
  cacheLockWaitMs.store(cacheLockWaitMs.load(std::memory_order_relaxed)+waited,std::memory_order_relaxed);
 }
 auto found=frameCache.find(key);
 if(found==frameCache.end()){cacheMisses++;return {};}
 found->second.used=GetTickCount64();cacheHits++;return found->second.image;
}
void CachePut(const std::wstring& key,const std::wstring& path,int tier,const std::shared_ptr<Image>& image,const std::wstring& protectedPath){
 if(!image)return;
 std::lock_guard lock(cacheMx);
 size_t cost=image->pixels.size();
 if(!cacheBudget)RefreshCacheBudget();
 if(cost>cacheBudget&&tier!=CacheFull)return;
 // At most two full frames: the one on screen, and one neighbour so that
 // stepping back to the photograph just left does not re-decode it.
 if(tier==CacheFull){
  size_t fullCount=0;
  for(auto& [_,entry]:frameCache)if(entry.tier==CacheFull)fullCount++;
  while(fullCount>=2){
   auto oldest=frameCache.end();
   for(auto it=frameCache.begin();it!=frameCache.end();++it){
    if(it->second.tier==CacheFull&&it->second.path!=NormalisePath(path)){
     if(oldest==frameCache.end()||it->second.used<oldest->second.used)oldest=it;
    }
   }
   if(oldest==frameCache.end())break;
   EraseLocked(oldest);
   fullCount--;
  }
 }
 auto old=frameCache.find(key);if(old!=frameCache.end())EraseLocked(old);
 frameCache.emplace(key,CacheEntry{image,cost,GetTickCount64(),tier,NormalisePath(path)});
 cacheBytes+=cost;cacheBytesByTier[tier<3?tier:2]+=cost;
 CacheTrimLocked(NormalisePath(protectedPath),false);
}
void CacheHandlePressure(const std::wstring& protectedPath){
 prefetchEpoch++;
 RefreshCacheBudget();
 std::lock_guard lock(cacheMx);CacheTrimLocked(NormalisePath(protectedPath),true);
}
void CacheLogTelemetry(){
 if(!testing)return;
 size_t full=0,screen=0,warm=0,bytes=0,fullBytes=0,screenBytes=0;
 {
  std::lock_guard lock(cacheMx);
  bytes=cacheBytes;fullBytes=cacheBytesByTier[CacheFull];screenBytes=cacheBytesByTier[CacheScreen];
  for(auto& [_,entry]:frameCache){if(entry.tier==CacheFull)full++;else if(entry.tier==CacheScreen)screen++;else warm++;}
 }
 Log(L"cache budget="+std::to_wstring(cacheBudget)+L" bytes="+std::to_wstring(bytes)+
  L" fullItems="+std::to_wstring(full)+L" fullBytes="+std::to_wstring(fullBytes)+
  L" screenItems="+std::to_wstring(screen)+L" screenBytes="+std::to_wstring(screenBytes)+
  L" thumbItems="+std::to_wstring(warm)+L" L3="+std::to_wstring(thumbBitmaps.size())+
  L" hit="+std::to_wstring(cacheHits.load())+L" miss="+std::to_wstring(cacheMisses.load())+
  L" evict="+std::to_wstring(cacheEvictions.load())+L" cancelled="+std::to_wstring(prefetchCancelled.load())+
  L" gpuUpload="+std::to_wstring(gpuUploads.load())+L" gpuReuse="+std::to_wstring(gpuReuses.load())+
  L" gpuHit="+std::to_wstring(gpuHits.load())+L" gpuPre="+std::to_wstring(gpuPreUploads.load())+
  L" fullUploads="+std::to_wstring(fullGpuUploads.load())+
  L" screenUploads="+std::to_wstring(screenGpuUploads.load())+
  L" uploadMB="+std::to_wstring(gpuUploadBytes.load()/(1024*1024))+
  L" largeUploads="+std::to_wstring(gpuLargeUploads.load())+
  L" maxUploadMB="+std::to_wstring(gpuMaxUploadBytes.load()/(1024*1024))+
  L" rawFullStarted="+std::to_wstring(rawFullStarted.load())+
  L" rawFullCompleted="+std::to_wstring(rawFullCompleted.load())+
  L" rawFullCancelled="+std::to_wstring(rawFullCancelled.load())+
  L" rawFullSkipped="+std::to_wstring(rawFullSkippedBrowsing.load()));
}
// The backdrop behind a photograph is a heavily blurred, heavily tinted wash.
// It used to be built by walking every pixel of the decoded frame on the UI
// thread: 34 ms for a 36 megapixel JPEG, once per navigation, in the middle of
// the frame that was supposed to show the new picture. Two cheaper sources now
// stand in, in order of preference: the filmstrip thumbnail that already
// exists for this file, and failing that a strided average of the frame that
// reads a few thousand pixels rather than tens of millions.
std::shared_ptr<Image> backdropSource;
uint8_t backdropAverage[4]={0,0,0,0};
bool backdropIsAverage=false;

// ---------------------------------------------------------- gpu textures ---
// One Direct2D bitmap per navigation step meant allocating and freeing tens of
// megabytes of video memory for every photograph, on the UI thread, in the
// frame that was supposed to show the new picture. These slots are reused
// instead: a folder of same-sized frames — which is what a camera produces —
// never allocates again after the first, and the frame the user is about to
// reach can be uploaded ahead of time, so arriving at it costs a pointer swap.
struct GpuSlot{ComPtr<ID2D1Bitmap> texture;unsigned w=0,h=0;std::wstring key;ULONGLONG used=0;};
// Three. Five was tried, on the theory that a folder mixing four frame sizes
// always evicts the one it is about to need again; measured, it was worse —
// slow transitions rose from 19-27 to 31-40 per run and the maximum more than
// doubled. More resident textures cost more than the allocations they save.
constexpr int GpuSlotCount=3;
GpuSlot gpuSlots[GpuSlotCount];

void GpuRelease(){for(auto& slot:gpuSlots){slot.texture.Reset();slot.w=slot.h=0;slot.key.clear();slot.used=0;}}

// Returns the texture for `image`, uploading only when this exact frame is not
// already resident. `keepKey` is the frame currently on screen, which must not
// be recycled out from under the renderer.
ComPtr<ID2D1Bitmap> GpuTextureFor(const std::shared_ptr<Image>& image,const std::wstring& key,
                                  const std::wstring& keepKey){
 ComPtr<ID2D1Bitmap> none;
 if(!image||!image->w||!image->h||!GfxReady())return none;
 auto start=std::chrono::steady_clock::now();
 for(auto& slot:gpuSlots){
  if(slot.texture&&slot.key==key&&slot.w==image->w&&slot.h==image->h){
   slot.used=GetTickCount64();
   lastUploadMs=0;lastUploadWasReuse=true;lastFrameWasPreUploaded=true;gpuHits++;
   navTrace.gpuResident=true;
   return slot.texture;
  }
 }
 lastFrameWasPreUploaded=false;
 auto properties=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
 // A slot of exactly the right size can take the new pixels in place.
 GpuSlot* reuse=nullptr;
 for(auto& slot:gpuSlots){
  if(!slot.texture||slot.key==keepKey)continue;
  if(slot.w!=image->w||slot.h!=image->h)continue;
  if(!reuse||slot.used<reuse->used)reuse=&slot;
 }
 if(reuse){
  if(SUCCEEDED(reuse->texture->CopyFromMemory(nullptr,image->pixels.data(),image->w*4))){
   if(image->tier==TierFullRes)fullGpuUploads++;else screenGpuUploads++;
   gpuUploadBytes+=image->pixels.size();
   if(image->pixels.size()>=LargeUploadBytes)gpuLargeUploads++;
   for(uint64_t seen=gpuMaxUploadBytes.load();image->pixels.size()>seen&&
       !gpuMaxUploadBytes.compare_exchange_weak(seen,image->pixels.size());){}
   reuse->key=key;reuse->used=GetTickCount64();
   lastUploadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   lastUploadWasReuse=true;gpuReuses++;
   navTrace.gpuReused=true;
   return reuse->texture;
  }
  // A failed copy means the device is gone; fall through and rebuild.
  reuse->texture.Reset();reuse->w=reuse->h=0;reuse->key.clear();
 }
 GpuSlot* victim=nullptr;
 for(auto& slot:gpuSlots){
  if(slot.key==keepKey&&slot.texture)continue;
  if(!slot.texture){victim=&slot;break;}
  if(!victim||slot.used<victim->used)victim=&slot;
 }
 if(!victim)victim=&gpuSlots[0];
 victim->texture.Reset();
 auto made=Dc()->CreateBitmap(D2D1::SizeU(image->w,image->h),image->pixels.data(),image->w*4,
                              properties,&victim->texture);
 if(FAILED(made)){victim->w=victim->h=0;victim->key.clear();return none;}
 if(image->tier==TierFullRes)fullGpuUploads++;else screenGpuUploads++;
 gpuUploadBytes+=image->pixels.size();
 if(image->pixels.size()>=LargeUploadBytes)gpuLargeUploads++;
 for(uint64_t seen=gpuMaxUploadBytes.load();image->pixels.size()>seen&&
     !gpuMaxUploadBytes.compare_exchange_weak(seen,image->pixels.size());){}
 victim->w=image->w;victim->h=image->h;victim->key=key;victim->used=GetTickCount64();
 lastUploadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 lastUploadWasReuse=false;gpuUploads++;
 navTrace.gpuCreated=true;
 return victim->texture;
}
// Puts a frame into a spare slot before anybody asks for it. Only ever called
// when the interface is idle, and never over the slot in use.
void GpuPreUpload(const std::shared_ptr<Image>& image,const std::wstring& key,const std::wstring& keepKey){
 if(!image||!GfxReady()||key.empty()||key==keepKey)return;
 for(auto& slot:gpuSlots)if(slot.texture&&slot.key==key)return;
 int free=0;
 for(auto& slot:gpuSlots)if(!slot.texture||slot.key!=keepKey)free++;
 if(free<2)return;   // never spend the last slot that is not the live one
 auto before=gpuUploads.load()+gpuReuses.load();
 GpuTextureFor(image,key,keepKey);
 if(gpuUploads.load()+gpuReuses.load()!=before)gpuPreUploads++;
}

void ReleaseBitmaps(){bitmap.Reset();backdrop.Reset();GpuRelease();thumbBitmaps.clear();folderPreviews.clear();folderTx.photos.clear();hero.fromBitmap.Reset();for(auto& g:histPath)g.Reset();scopeBitmap.Reset();}

// ------------------------------------------------------------ worker -------
// Prefetch order, rebuilt from how the reader is actually moving. A fixed
// +1/-1/+2/-2 ring spends half its work behind somebody who is walking
// forwards; the frames worth preparing are the ones ahead, and how far ahead
// depends on how fast they are going.
std::vector<int> PrefetchSteps(int direction,bool fast){
 std::vector<int> steps;
 int depth=fast?6:3;
 for(int distance=1;distance<=depth;distance++)steps.push_back(direction*distance);
 // One or two behind, so a change of mind is not a cold start.
 steps.push_back(-direction);
 if(!fast)steps.push_back(-direction*2);
 return steps;
}

void Worker(){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 while(true){
  std::wstring path;uint64_t id;bool quick,force,wantFull;unsigned edge;
  std::vector<std::wstring> files;
  {
   std::unique_lock lock(mx);
   cv.wait(lock,[]{return stopping||!requested.empty();});
   if(stopping)break;
   path=std::move(requested);requested.clear();id=generation;quick=requestPreview;force=requestForce;edge=requestEdge;
   files=std::move(requestFiles);wantFull=requestFull;
  }
  auto post=[&](std::unique_ptr<Result> result){
   {std::lock_guard lock(mx);ready.push_back(std::move(result));}
   PostMessageW(win,Loaded,0,0);
  };
  auto stale=[id]{return latest!=id;};
  std::error_code ec;
  bool hasCurrent=(!files.empty()&&std::find(files.begin(),files.end(),path)!=files.end());
  if(!hasCurrent&&!quick){
   files.clear();
   for(fs::directory_iterator it(fs::path(path).parent_path(),fs::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;it.increment(ec)){
    auto& e=*it;if(latest!=id)break;
    if(e.is_regular_file(ec)&&Supported(e.path().wstring()))files.push_back(e.path().wstring());
   }
   std::sort(files.begin(),files.end(),[](auto&a,auto&b){return StrCmpLogicalW(a.c_str(),b.c_str())<0;});
  }
  if(latest!=id)continue;
  auto fullKey=FrameKey(path,CacheFull,0),screenKey=FrameKey(path,CacheScreen,edge);
  auto full=force?std::shared_ptr<Image>():CacheGet(fullKey);
  auto begin=std::chrono::steady_clock::now();
  // Stage one: a screen-ready frame. For a RAW that is the camera's own
  // embedded JPEG, or a half-size develop when that preview is too small for
  // this window; for a JPEG a DCT-scaled decode. Either way it is the frame
  // the window will actually draw, at the size it will draw it.
  bool servedPreview=false;
  if(!full){
   auto preview=CacheGet(screenKey);
   if(!preview)preview=CacheGet(FrameKey(path,CacheWarm,edge));
   if(!preview)preview=DecodeScreen(path,edge,stale);
   if(preview){
    CachePut(screenKey,path,CacheScreen,preview,path);
    if(latest!=id)continue;
    auto first=std::make_unique<Result>();
    first->id=id;first->path=path;first->key=screenKey;first->files=files;first->image=preview;
    // Only "partial" when something better is genuinely on its way. At fit
    // this frame is the finished article, and saying otherwise would leave
    // the interface reporting a load that is never going to complete.
    first->partial=wantFull;
    Log(L"screen_ms="+std::to_wstring(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count())+L" "+path);
    servedPreview=true;
    post(std::move(first));
   }
  }
  // Neighbours before the expensive stage. Preparing the next screen-ready
  // frames is what makes the following keypress instant; the full-resolution
  // develop of the frame already on screen can wait for that to be done.
  if(files.size()>1){
   auto pos=std::find(files.begin(),files.end(),path);
   if(pos!=files.end()){
    int direction=navigationDirection?navigationDirection:1;
    for(int step:PrefetchSteps(direction,fastNavigation)){
     if(latest!=id)break;
     auto index=(pos-files.begin())+step;
     if(index<0||index>=ptrdiff_t(files.size()))continue;
     auto& neighbour=files[size_t(index)];
     auto neighbourKey=FrameKey(neighbour,CacheScreen,edge);
     if(CacheGet(neighbourKey))continue;
     auto next=DecodeScreen(neighbour,edge,[id]{return latest!=id;});
     if(latest!=id)break;
     if(next)CachePut(neighbourKey,neighbour,CacheScreen,next,path);
    }
   }
  }
  if(latest!=id)continue;

  // Stage two: full resolution, and only when something actually needs it —
  // a zoom past the screen frame's own pixel grid, the Info panel's histogram,
  // or an export. Fit-to-window browsing stops here.
  if(!wantFull&&servedPreview){
   if(IsRawPath(path))rawFullSkippedBrowsing++;
   Log(L"full_not_requested "+path);
   continue;
  }
  bool heavy=IsRawPath(path);
  // Even when full resolution is wanted, a RAW develop waits for the reader to
  // settle. Walking a folder must not queue a five-second demosaic per frame.
  if(heavy&&servedPreview&&!full){
   auto settleUntil=std::chrono::steady_clock::now()+
    std::chrono::milliseconds(int(RawFullDebounceSeconds*1000));
   while(std::chrono::steady_clock::now()<settleUntil){
    if(latest!=id)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
   }
   if(latest!=id){rawFullSkippedBrowsing++;Log(L"full_debounced "+path);continue;}
  }
  if(heavy&&!full)rawFullStarted++;
  auto result=std::make_unique<Result>();result->id=id;result->path=path;result->files=std::move(files);
  if(!full)full=Decode(path,result->error,stale);
  if(latest!=id)continue;
  // A file whose full decode fails after a preview appeared keeps the preview
  // rather than replacing a visible photograph with an error.
  if(!full){
   full=CacheGet(screenKey);
   if(full){result->error.clear();result->key=screenKey;result->partial=true;}
  }else result->key=fullKey;
  bool servedFull=full&&!result->partial;
  if(heavy){
   if(servedFull)rawFullCompleted++;else rawFullCancelled++;
  }
  result->image=full;
  Log(L"full_ms="+std::to_wstring(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count())+L" "+path);
  post(std::move(result));
  if(servedFull)CachePut(fullKey,path,CacheFull,full,path);
 }
 {std::lock_guard lock(cacheMx);CacheTrimLocked(L"",true);}
 CoUninitialize();
}

// ------------------------------------------------------------ commands -----
void ApplyCorners(){
 if(!win)return;
 DWORD preference=1u;   // The composition surface already has its own rounded alpha.
 DwmSetWindowAttribute(win,33,&preference,sizeof(preference));
 DWORD border=0xfffffffe;DwmSetWindowAttribute(win,34,&border,sizeof(border));
 DWMNCRENDERINGPOLICY policy=DWMNCRP_DISABLED;
 DwmSetWindowAttribute(win,DWMWA_NCRENDERING_POLICY,&policy,sizeof(policy));
 MARGINS margins{};DwmExtendFrameIntoClientArea(win,&margins);
 RECT r{};GetWindowRect(win,&r);
 int radius=int((preview?32.f:26.f)*dpi);
 if(WindowMaximized())SetWindowRgn(win,nullptr,FALSE);
 else SetWindowRgn(win,CreateRoundRectRgn(0,0,r.right-r.left+1,r.bottom-r.top+1,radius*2,radius*2),FALSE);
}
void SetTheme(bool light){
 themeMix.To(light?1.f:0.f);
 DWORD value=light?1:0;
 RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"LightTheme",REG_DWORD,&value,sizeof(value));
 BOOL dark=!light;DwmSetWindowAttribute(win,20,&dark,sizeof(dark));
 Wake();
}
void SetWheelMode(int value){
 wheelMode=value;wheelMix.To(float(value));wheelCarry=0;
 DWORD stored=DWORD(value);
 RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"WheelMode",REG_DWORD,&stored,sizeof(stored));
 Wake();
}
void SaveLibView(){
 // The factory default is the timeline; a deliberate switch to Folders is
 // remembered, so somebody who works in folders stays in folders.
 DWORD stored=DWORD(libView);
 RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"LibraryView",REG_DWORD,&stored,sizeof(stored));
}
void SetLanguage(int value){
 language=value;
 DWORD stored=DWORD(value);
 RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"Language",REG_DWORD,&stored,sizeof(stored));
 Wake();
}
void ClosePanel(){panel=PanelNone;pendingPanel=PanelNone;panelSlide.To(0);confirmDelete=false;Wake();}
void RequestFullResolution();
void OpenPanel(int which){
 // The histogram and the clipping overlays are read from pixels, so the panel
 // is the one reader of the full tier that is not a zoom.
 if(which==PanelInfo)RequestFullResolution();
 if(panel==which&&panelSlide.target>0){ClosePanel();return;}
 confirmDelete=false;menuLevel=0;levelSlide.Reset(0);panelScroll=0;panelScrollVel=0;
 if(which==PanelMenu)tcStatus=TotalCommanderStatus();
 if(panel!=PanelNone&&panel!=which){pendingPanel=which;panelSlide.To(0);}
 else{panel=which;panelSlide.To(1);}
 if(fit)Fit();
 Wake();
}
void ResetEdits(){strokes.clear();hasCrop=false;tool=ToolNone;painting=false;}
void RotatePoints(int direction){
 // Turn stored annotations with the picture so nothing drifts off the frame.
 float w=float(DisplayW()),h=float(DisplayH());
 auto turn=[&](D2D1_POINT_2F p){return direction>0?D2D1::Point2F(h-p.y,p.x):D2D1::Point2F(p.y,w-p.x);};
 for(auto& s:strokes)for(auto& p:s.pts)p=turn(p);
 if(hasCrop){
  auto a=turn(D2D1::Point2F(crop.left,crop.top)),b=turn(D2D1::Point2F(crop.right,crop.bottom));
  crop=D2D1::RectF((std::min)(a.x,b.x),(std::min)(a.y,b.y),(std::max)(a.x,b.x),(std::max)(a.y,b.y));
 }
}
void Turn(int direction){
 if(!current)return;
 RotatePoints(direction);
 rotateSpan=90;
 rotate.To(rotate.target+90.f*direction);
 Fit();
 Wake();
}
bool Dirty(){return fmodf(rotate.target,360.f)!=0.f||hasCrop||!strokes.empty();}

// The frame the file actually contains, not the one being displayed.
//
// Save, Save As, Print and Copy all go through here. Writing whatever tier
// happened to be on screen would silently hand somebody a third-resolution
// copy of their own photograph, which is the worst possible way to fail.
std::shared_ptr<Image> SourceFrame(){
 if(!current)return {};
 if(current->tier==TierFullRes)return current;
 auto key=FrameKey(currentPath,CacheFull,0);
 if(auto full=CacheGet(key))return full;
 std::wstring error;
 auto full=Decode(currentPath,error);
 if(full)CachePut(key,currentPath,CacheFull,full,currentPath);
 // A file that will not decode in full still gets saved rather than refused;
 // the displayed frame is all there is.
 return full?full:current;
}
std::shared_ptr<Image> Composite(){
 if(!current)return {};
 auto source=SourceFrame();
 if(!source)return {};
 // Crop rectangles and annotation strokes are authored against the displayed
 // frame. Compositing onto the asset means scaling them up by exactly the
 // ratio between the two.
 float scale=(source->w&&current->w)?float(source->w)/float(current->w):1.f;
 auto rotated=std::make_shared<Image>(RotatedPixels(*source,int(rotate.target)));
 if(!strokes.empty()){
  auto painted=Rasterise(*rotated,[scale](ID2D1DeviceContext* dc){
   auto previous=D2D1::Matrix3x2F::Identity();
   dc->GetTransform(&previous);
   dc->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*previous);
   PaintStrokes();
   dc->SetTransform(previous);
  });
  if(painted)rotated=painted;
 }
 if(hasCrop){
  int x=int(Clamp(crop.left*scale,0,float(rotated->w))),y=int(Clamp(crop.top*scale,0,float(rotated->h)));
  unsigned w=unsigned((std::max)(1.f,(crop.right-crop.left)*scale));
  unsigned h=unsigned((std::max)(1.f,(crop.bottom-crop.top)*scale));
  rotated=std::make_shared<Image>(CropPixels(*rotated,x,y,w,h));
 }
 return rotated;
}
void Save(bool as){
 if(!current||loading||actionBusy)return;
 std::wstring path=currentPath;
 auto ext=fs::path(path).extension().wstring();
 std::transform(ext.begin(),ext.end(),ext.begin(),towlower);
 bool encodable=ext==L".png"||ext==L".jpg"||ext==L".jpeg"||ext==L".jfif"||ext==L".bmp"||ext==L".tif"||ext==L".tiff";
 if(!as&&!Dirty()){Notify(T(S_NoChanges));return;}
 if(as||!encodable){
  wchar_t file[32768]{};
  auto proposed=fs::path(currentPath).stem().wstring()+L"-edited.png";
  wcscpy_s(file,proposed.c_str());
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner=win;dialog.lpstrFile=file;dialog.nMaxFile=32768;
  dialog.lpstrFilter=L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg;*.jpeg\0BMP (*.bmp)\0*.bmp\0TIFF (*.tiff)\0*.tif;*.tiff\0";
  dialog.lpstrDefExt=L"png";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST;
  if(!GetSaveFileNameW(&dialog))return;
  path=file;
 }
 auto flattened=Composite();
 if(!flattened){Notify(T(S_Unavailable));return;}
 actionBusy=true;
 auto id=latest.load();
 if(actionWorker.joinable())actionWorker.join();
 actionWorker=std::thread([flattened,path,id]{
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  auto result=std::make_unique<SaveResult>();
  result->path=path;result->imageId=id;
  result->ok=WriteImage(*flattened,0,path,result->error);
  {std::lock_guard lock(mx);saveResult=std::move(result);}
  PostMessageW(win,Saved,0,0);
  CoUninitialize();
 });
 Notify(T(S_Saving));
}
void RemoveCurrent(){
 if(!current||loading||actionBusy)return;
 std::wstring error;
 if(!RecycleImage(win,currentPath,error)){Notify(error);return;}
 auto it=std::find(siblings.begin(),siblings.end(),currentPath);
 if(it!=siblings.end())it=siblings.erase(it);
 galleryWidth.assign(siblings.size(),Spring(0,GalleryK,GalleryC));
 if(!siblings.empty()){if(it==siblings.end())it=siblings.begin();Open(*it,true);}
 else{current.reset();ReleaseBitmaps();currentPath.clear();rotate.Reset(0);ResetEdits();}
 Notify(T(S_Deleted));
}
void CopyCurrent(){
 if(!current||loading)return;
 auto flattened=Composite();
 if(flattened&&CopyToClipboard(win,*flattened,currentPath)){copyUntil=Now()+.95;Notify(T(S_Copied));}
 else Notify(T(S_Unavailable));
}
void RefreshFavourites();
void RefreshAlbum();
void ToggleLike(){
 if(currentPath.empty())return;
 liked=!liked;FavouriteSet(currentPath,liked);
 likePop.Reset(liked?.82f:1.1f);likePop.To(1);
 if(liked)likeBurst=Now();
 // The virtual collection is derived state; rebuild it now so backing out to
 // the library shows the change rather than the view from before the press.
 RefreshFavourites();
 if(favouritesOpen)RefreshAlbum();
 Wake();
}
// Opening a file from outside the album flow (the file picker, a drag onto
// the window) still needs Back to make sense afterwards, so it synthesises
// the same two-frame stack a direct Explorer launch gets.
void OpenExternal(const std::wstring& path){
 std::error_code ec;auto absolute=fs::absolute(fs::path(path),ec);
 auto parent=(ec?fs::path(path):absolute).parent_path().wstring();
 if(navStack.empty()){
  navStack.push_back({ScrLibrary,L"",0,screen==ScrLibrary?libScroll:0.f});
  navStack.push_back({ScrAlbum,parent,0,0});
  IndexTouchFolder(parent);
 }
 Open(path);
}
void Choose(){
 wchar_t file[32768]{};
 OPENFILENAMEW of{sizeof(of)};
 of.hwndOwner=win;
 of.lpstrFilter=L"Images\0*.jpg;*.jpeg;*.png;*.gif;*.webp;*.bmp;*.tif;*.tiff;*.ico;*.exr;*.avif;*.heic;*.psd;*.psb;*.cr2;*.cr3;*.nef;*.arw;*.dng;*.raf;*.rw2\0All files\0*.*\0";
 of.lpstrFile=file;of.nMaxFile=32768;of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
 if(GetOpenFileNameW(&of))OpenExternal(file);
}
void NormalWindow(){
 bool wasPreview=compactWindow;
 compactWindow=false;preview=false;ClosePanel();
 SetWindowLongPtrW(win,GWLP_HWNDPARENT,0);
 SetWindowPos(win,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
 if(wasPreview)SetWindowPos(win,nullptr,normalBounds.left,normalBounds.top,
  normalBounds.right-normalBounds.left,normalBounds.bottom-normalBounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
 if(wasPreview&&beforePreviewValid){
  auto placement=beforePreview;
  if(placement.showCmd==SW_SHOWMINIMIZED)placement.showCmd=SW_SHOWNORMAL;
  SetWindowPlacement(win,&placement);beforePreviewValid=false;
 }
 ApplyCorners();Fit();Snap();
 ApplyCorners();Frame();ShowWindow(win,SW_SHOW);SetForegroundWindow(win);Wake();
}
void PreviewBounds(){
 if(!preview||!current)return;
 compactWindow=true;
 MONITORINFO monitor{sizeof(monitor)};
 GetMonitorInfoW(MonitorFromWindow(explorer?explorer:win,MONITOR_DEFAULTTONEAREST),&monitor);
 auto r=monitor.rcWork;
 float scale=(std::min)({1.f,(r.right-r.left)*.8f/current->w,(r.bottom-r.top)*.8f/current->h});
 int w=(std::max)(60,int(current->w*scale)),h=(std::max)(60,int(current->h*scale));
 SetWindowPos(win,HWND_TOPMOST,r.left+(r.right-r.left-w)/2,r.top+(r.bottom-r.top-h)/2,w,h,SWP_NOACTIVATE);
 ApplyCorners();Fit();Snap();
 Frame();ShowWindow(win,SW_SHOWNA);SetWindowPos(win,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
 SetTimer(win,6,45,nullptr);
 Wake();
}
void Close(){
 ClosePanel();
 if(preview){
  ShowWindow(win,SW_HIDE);
  SetWindowPos(win,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
  preview=false;SetWindowLongPtrW(win,GWLP_HWNDPARENT,0);ApplyCorners();
  {std::lock_guard lock(mx);requested.clear();latest=++generation;}
  // --quicklook is an entry point, not a resident Explorer integration.  A
  // visible close (Esc or Space) must also terminate that one-shot process.
  if(quickLookInvocation){DestroyWindow(win);return;}
  if(IsWindow(explorer))SetForegroundWindow(explorer);
 }else if(autostart&&!testing){
  // Keep the Explorer listener alive after the viewer is closed.
  ShowWindow(win,SW_HIDE);
 }else DestroyWindow(win);
}
void Open(const std::wstring& path,bool force){
 if(path.empty())return;
 if(!preview){KillTimer(win,6);SetWindowLongPtrW(win,GWLP_HWNDPARENT,0);SetWindowPos(win,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}
 screen=ScrViewer;
 std::error_code ec;
 auto absolute=fs::absolute(fs::path(path),ec);
 prevName=Name();prevMeta=current?std::to_wstring(DisplayW())+L" × "+std::to_wstring(DisplayH()):std::wstring();
 if(fastNavigation)titleIn.Reset(1);else{titleIn.Reset(0);titleIn.To(1);}
 auto nextPath=ec?path:absolute.wstring();
 auto normNext=NormalisePath(nextPath);
 auto normCurr=NormalisePath(currentPath);
 bool changed=(normNext!=normCurr);

 MONITORINFO monitor{sizeof(monitor)};
 unsigned edge=2560;
 if(GetMonitorInfoW(MonitorFromWindow(win,MONITOR_DEFAULTTONEAREST),&monitor))
  edge=unsigned((std::min)(4096.f,1.25f*float((std::max)(monitor.rcMonitor.right-monitor.rcMonitor.left,
                                                         monitor.rcMonitor.bottom-monitor.rcMonitor.top))));

 std::error_code fec;
 auto fpath=fs::path(nextPath);
 uintmax_t fbytes=fs::file_size(fpath,fec);if(fec)fbytes=0;
 fec.clear();
 auto stamp=fs::last_write_time(fpath,fec);
 auto ticks=fec?0ll:stamp.time_since_epoch().count();
 auto baseKey=normNext+L"|"+std::to_wstring(fbytes)+L"|"+std::to_wstring(ticks)+L"|";
 auto fullKey=baseKey+std::to_wstring(CacheFull)+L"|0|source-orientation";
 auto screenKey=baseKey+std::to_wstring(CacheScreen)+L"|"+std::to_wstring(edge)+L"|source-orientation";
 auto warmKey=baseKey+std::to_wstring(CacheWarm)+L"|"+std::to_wstring(edge)+L"|source-orientation";

 // Everything from here to the GPU texture is what a cache hit actually costs.
 // It is timed so a slow transition can name its cause instead of leaving it
 // to be guessed at.
 navTrace=NavTrace{};
 navTrace.evictionsBefore=cacheEvictions.load();
 navTrace.directionChanged=(navigationDirection!=navPreviousDirection);
 navPreviousDirection=navigationDirection;
 cacheLockWaitMs.store(0.0,std::memory_order_relaxed);
 {
  // A window that changed size invalidates every screen-tier key, which is a
  // legitimate reason for a miss and must not be filed as a cache fault.
  float vw,vh;Size(vw,vh);
  unsigned pw=unsigned(vw*dpi+.5f),ph=unsigned(vh*dpi+.5f);
  navTrace.viewportChanged=(navPreviousWidth&&(pw!=navPreviousWidth||ph!=navPreviousHeight));
  navTrace.width=navPreviousWidth=pw;navTrace.height=navPreviousHeight=ph;
 }
 // The Info panel's histogram is read from real pixels, so it is the one
 // reader of the full tier that is not a zoom.
 bool wantFull=NeedsFullResolution()||panel==PanelInfo;
 auto lookupStart=std::chrono::steady_clock::now();
 std::shared_ptr<Image> cached;
 std::wstring cachedKey;
 bool isPartial=false;
 // At fit the screen tier is asked for first. Preferring the full tier here is
 // what put a 138 MB upload on the navigation path for a 1400 px window.
 if(!force&&!wantFull){
  cached=CacheGet(screenKey);
  if(cached){cachedKey=screenKey;navTrace.tier=TierScreen;isPartial=false;}
 }
 if(!cached&&!force){cached=CacheGet(fullKey);if(cached){cachedKey=fullKey;navTrace.tier=TierFull;}}
 if(!cached){cached=CacheGet(screenKey);if(cached){cachedKey=screenKey;navTrace.tier=TierScreen;}isPartial=wantFull;}
 if(!cached){cached=CacheGet(warmKey);if(cached){cachedKey=warmKey;navTrace.tier=TierWarm;}isPartial=true;}
 if(!cached){cached=ThumbLookup(nextPath);if(cached){cachedKey=normNext+L"|thumb";navTrace.tier=TierThumb;}isPartial=true;}
 // A frame that is already the full tier is never "partial", whichever key
 // found it.
 if(cached&&cached->tier==TierFullRes&&navTrace.tier!=TierThumb)isPartial=false;
 navTrace.cacheLookupMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-lookupStart).count();
 navTrace.lockWaitMs=cacheLockWaitMs.load(std::memory_order_relaxed);

 if(changed){
  currentPath=std::move(nextPath);
  rotate.Reset(0);ResetEdits();
  liked=FavouriteGet(currentPath);
  info=Meta{};metaPending=true;
  // The rating badge should not have to wait for a RAW to finish demosaicing,
  // so the cheap metadata pass is asked for here and the expensive one — the
  // histogram, which needs pixels — later, when the full frame lands.
  metaQuickId=++metaQuickCounter;
  MetaRequestQuick(currentPath,metaQuickId,win,MetaReady);for(auto& g:histPath)g.Reset();scopeBitmap.Reset();histRise.Reset(0);
  clipHigh=clipLow=false;clipHighFade.Reset(0);clipLowFade.Reset(0);
  panelScroll=0;panelScrollVel=0;
  SetWindowTextW(win,(fs::path(currentPath).filename().wstring()+L" - Vetro Look").c_str());
 }

 errorText.clear();
 if(cached){
  auto shown=current;
  current=cached;
  currentFrameKey=cachedKey;
  backdropSource=cached;
  // The fast path. A frame already uploaded into a GPU slot is adopted here
  // and drawn on the next present with no allocation, no upload and no
  // decode: navigation costs a pointer swap.
  auto gpuStart=std::chrono::steady_clock::now();
  bitmap=GpuTextureFor(current,cachedKey,cachedKey);
  navTrace.gpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-gpuStart).count();
  backdrop.Reset();
  showingPreview=isPartial;
  loading=isPartial;
  if(preview){if(current)PreviewBounds();else Close();}
  SyncGallery();
  bool refined=shown&&current&&shown!=current&&shown->w&&current->w&&!fit;
  if(refined)zoomLog.Reset(zoomLog.target+logf(float(shown->w)/float(current->w)));
  else if(fit){Fit();Snap();}
 }else{
  ThumbRequest(currentPath);
  ThumbPrioritize(currentPath);
  currentFrameKey.clear();
  backdropSource.reset();
  bitmap.Reset();backdrop.Reset();
  loading=true;
  showingPreview=false;
 }

 {
  std::lock_guard lock(mx);
  requested=currentPath;requestPreview=preview;requestForce=force;
  requestFiles=siblings;
  requestEdge=edge;requestFull=wantFull;
  generation++;latest=generation;
  latencyId=generation;openStarted=std::chrono::steady_clock::now();
  firstVisibleLogged=(cached!=nullptr);fullVisibleLogged=(cached&&!isPartial);
  currentGeneration=(cached?latencyId:0);
 }
 cv.notify_one();Wake();
 navTrace.evictionsDuring=cacheEvictions.load()-navTrace.evictionsBefore;
 navLastTrace=navTrace;
 if((testing||GetEnvironmentVariableW(L"VETRO_DEBUG",nullptr,0))&&!navBench){
  double lookupMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-openStarted).count();
  Log(L"NAV path="+fs::path(currentPath).filename().wstring()+
      L" cacheHit="+std::wstring(cached?L"1":L"0")+
      L" cacheTier="+std::to_wstring(navTrace.tier)+
      L" gpuPreUploaded="+std::wstring(lastFrameWasPreUploaded?L"1":L"0")+
      L" gpuUploadMs="+std::to_wstring(lastUploadMs)+
      L" openPathMs="+std::to_wstring(lookupMs));
 }
}
// Asks the worker for the full tier of the photograph already on screen. The
// displayed frame stays exactly where it is; when the full one arrives the
// Loaded handler compensates zoom so nothing moves.
void RequestFullResolution(){
 if(currentPath.empty()||preview)return;
 if(current&&current->tier==TierFullRes)return;
 {
  std::lock_guard lock(mx);
  if(requestFull&&requested==currentPath)return;   // already asked
  requested=currentPath;requestPreview=false;requestForce=false;
  requestFiles=siblings;requestFull=true;
  generation++;latest=generation;
  loading=true;
 }
 cv.notify_one();Wake();
}
int CurrentIndex(){
 auto p=std::find(siblings.begin(),siblings.end(),currentPath);
 return p==siblings.end()?-1:int(p-siblings.begin());
}
void Navigate(int step){
 if(siblings.empty())return;
 double now=Now();int direction=step<0?-1:1;
 if(direction==navigationDirection&&now-lastNavigateAt<.22)navigationBurst++;
 else navigationBurst=1;
 navigationDirection=direction;lastNavigateAt=now;
 if(navigationBurst>=3||abs(step)>1){
  fastNavigation=true;fastNavigationUntil=now+.35;
  // Repeated wheel/button input should favour throughput over decorative
  // transitions.  The next normal-speed navigation automatically restores
  // them after this short quiet interval.
  hero.active=false;folderTx.active=false;
 }
 int i=CurrentIndex();if(i<0)i=0;
 i=int((i+step+(int)siblings.size())%(int)siblings.size());
 galleryHasTarget=false;
 Open(siblings[size_t(i)]);
}

// -------------------------------------------------------------- layout -----
constexpr float Margin=14,Bubble=44,Cell=40,SideWidth=350,GalleryH=92,ThumbH=72;
constexpr float RowH=54,RowGap=8,RowRadius=17,PanelHead=82,GroupGap=14;
constexpr float ThumbNarrow=34,ThumbWide=118,ThumbGap=6;
float SideTop(){return Margin+Bubble+14;}
float SideBottom(float h){return h-Margin-(siblings.size()>1?GalleryH+12:12);}
// Shared by the viewer's top bar and the library/album chrome, so the window
// controls sit in the same place and answer to the same hit tests everywhere.
D2D1_RECT_F LayoutWindowButtons(float w){
 float y0=Margin,y1=Margin+Bubble,right=w-Margin;
 D2D1_RECT_F winBar=D2D1::RectF(right-118,y0,right,y1);
 rects[IdWinBar]=winBar;
 for(int i=0;i<3;i++)Hotspot(IdWinMin+i,D2D1::RectF(winBar.left+7+i*36,y0+5,winBar.left+41+i*36,y1-5));
 return winBar;
}

// ================================================================ library ==
// The startup screen: every folder the index knows holds at least one photo,
// browsable, searchable and filterable, opening into a per-folder grid and
// from there into the same single-image viewer the app always had.
constexpr float LibTop=88,LibCard=184,LibCardH=182,LibGap=22,AlbumCell=168;
std::wstring RuPlural(uint64_t n,Str one,Str few,Str many){
 uint64_t m10=n%10,m100=n%100;
 if(m10==1&&m100!=11)return T(one);
 if(m10>=2&&m10<=4&&(m100<12||m100>14))return T(few);
 return T(many);
}
std::wstring FormatBytes(uint64_t size){
 const wchar_t* units[]={L"B",L"KB",L"MB",L"GB",L"TB"};
 double v=double(size);int unit=0;
 while(v>=1024.&&unit<4){v/=1024.;unit++;}
 wchar_t buf[32];swprintf_s(buf,v<10&&unit?L"%.1f %s":L"%.0f %s",v,units[unit]);
 return buf;
}
std::wstring ExtOf(const std::wstring& path){
 auto dot=path.find_last_of(L'.');if(dot==std::wstring::npos)return L"";
 auto e=path.substr(dot+1);for(auto& c:e)c=towlower(c);return e;
}
bool RawExt(std::wstring e){
 for(auto& c:e)c=towlower(c);
 return std::wstring(L"|cr2|cr3|nef|arw|dng|raf|rw2|orf|pef|").find(L"|"+e+L"|")!=std::wstring::npos;
}
// Colour profile is read lazily and cached: doing this for every photo up
// front would mean opening tens of thousands of files just to enable one
// filter nobody may ever touch. An unclassified photo passes any profile
// filter until its turn comes; the background classifier is a plain loop
// kicked off from the filter UI itself, so it only ever runs when needed.
std::wstring ProfileOf(const std::wstring& path){
 std::wstring lower=NormalisePath(path);
 std::lock_guard lock(profileMx);
 auto found=profileCache.find(lower);
 return found==profileCache.end()?L"":found->second;
}
void ClassifyProfile(const std::wstring& path){
 std::wstring lower=NormalisePath(path);
 {std::lock_guard lock(profileMx);if(profileCache.count(lower))return;}
 ComPtr<IWICImagingFactory> factory;
 std::wstring profile=L"sRGB";
 if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))){
  ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
  if(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))&&
     SUCCEEDED(decoder->GetFrame(0,&frame))){
   UINT contexts=0;
   if(SUCCEEDED(frame->GetColorContexts(0,nullptr,&contexts))&&contexts){
    std::vector<ComPtr<IWICColorContext>> owned(contexts);std::vector<IWICColorContext*> raw(contexts);
    bool ok=true;for(UINT i=0;i<contexts;i++){if(FAILED(factory->CreateColorContext(&owned[i]))){ok=false;break;}raw[i]=owned[i].Get();}
    if(ok&&SUCCEEDED(frame->GetColorContexts(contexts,raw.data(),&contexts))&&contexts){
     WICColorContextType type=WICColorContextUninitialized;raw[0]->GetType(&type);
     if(type==WICColorContextExifColorSpace){UINT v=0;raw[0]->GetExifColorSpace(&v);profile=v==1?L"sRGB":L"none";}
     else if(type==WICColorContextProfile){
      BYTE header[132]{};UINT got=0;
      if(SUCCEEDED(raw[0]->GetProfileBytes(sizeof(header),header,&got))&&got>=132){
       // The ICC profile description tag is variable-offset; a cheap and
       // reliable enough signal instead is the media white point / rendering
       // intent pairing is not worth decoding here, so classify by size
       // bucket alone: this only ever feeds an optional filter chip.
       profile=L"other";
      }
     }
    }
   }
  }
 }
 std::lock_guard lock(profileMx);profileCache[lower]=profile;
}

std::vector<FolderEntry> RefreshLibraryFolders(){
 auto all=IndexSnapshotFolders();
 // Only generic siblings are automatic candidates. Named collections such as
 // Wedding/Japan/Cats stay independent even when their parent is shared.
 static const std::unordered_set<std::wstring> generic={L"images",L"img",L"assets",L"media",L"renders",L"render",L"exports",L"export",L"output",L"raw",L"edited",L"final",L"screenshots",L"previews"};
 std::unordered_map<std::wstring,std::vector<size_t>> candidates;
 for(size_t i=0;i<all.size();++i){
  auto name=fs::path(all[i].path).filename().wstring();for(auto& c:name)c=towlower(c);
  if(generic.count(name)){auto parent=fs::path(all[i].path).parent_path();
   if(parent.has_relative_path()&&parent.parent_path()!=parent.root_path())candidates[NormalisePath(parent.wstring())].push_back(i);}
 }
 std::unordered_set<size_t> grouped;std::vector<FolderEntry> families;
 for(auto& [parent,indices]:candidates)if(indices.size()>1){
  DWORD split=0,bytes=sizeof(split);std::wstring value=std::to_wstring(PhysicalId(parent));
  RegGetValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\LibrarySeparate",value.c_str(),RRF_RT_REG_DWORD,nullptr,&split,&bytes);
  if(split)continue;
  for(size_t i=0;i<all.size();++i)if(NormalisePath(all[i].path)==parent)indices.push_back(i);
  FolderEntry family;family.path=parent;family.name=fs::path(parent).filename().wstring();family.id=PhysicalId(parent);
  for(auto i:indices){auto& f=all[i];family.members.push_back(f.path);family.photoCount+=f.photoCount;family.totalBytes+=f.totalBytes;family.modified=(std::max)(family.modified,f.modified);
   if(f.sampleCount&&family.sampleCount<4)family.samples[family.sampleCount++]=f.samples[0];grouped.insert(i);}
  families.push_back(std::move(family));
 }
 std::vector<FolderEntry> combined;for(size_t i=0;i<all.size();++i)if(!grouped.count(i))combined.push_back(std::move(all[i]));
 for(auto& family:families)combined.push_back(std::move(family));all=std::move(combined);
 std::wstring needle=libSearch;for(auto& c:needle)c=towlower(c);
 std::vector<FolderEntry> out;
 out.reserve(all.size());
 for(auto& f:all){
  if(!needle.empty()&&libView==ViewFolders){
   std::wstring lowerPath=f.path;for(auto& c:lowerPath)c=towlower(c);
   if(lowerPath.find(needle)==std::wstring::npos)continue;
  }
  if(f.totalBytes<filterSizeLo||f.totalBytes>filterSizeHi)continue;
  if(filtersActive){
   if(!filterExtOn.empty()){
    bool anyKept=false;
    for(uint8_t i=0;i<f.sampleCount&&!anyKept;i++)if(filterExtOn.count(ExtOf(f.samples[i])))anyKept=true;
    // Samples are only the first few files; when none matches we still show
    // an incompletely sampled folder rather than hide a possible match.
    if(!anyKept&&f.sampleCount==std::min<uint32_t>(4,f.photoCount))continue;
   }
   if(filterRawOnly){
    bool anyRaw=false;
    for(uint8_t i=0;i<f.sampleCount&&!anyRaw;i++)if(RawExt(ExtOf(f.samples[i])))anyRaw=true;
    if(!anyRaw&&f.sampleCount==std::min<uint32_t>(4,f.photoCount))continue;
   }
   if(filterRegularOnly){
    bool anyReg=false;
    for(uint8_t i=0;i<f.sampleCount&&!anyReg;i++)if(!RawExt(ExtOf(f.samples[i])))anyReg=true;
    if(!anyReg&&f.sampleCount==std::min<uint32_t>(4,f.photoCount))continue;
   }
  }
  out.push_back(f);
 }
 auto byName=[](const FolderEntry& a,const FolderEntry& b){return StrCmpLogicalW(a.name.c_str(),b.name.c_str())<0;};
 auto byDate=[](const FolderEntry& a,const FolderEntry& b){return a.modified<b.modified;};
 auto bySize=[](const FolderEntry& a,const FolderEntry& b){return a.totalBytes<b.totalBytes;};
 if(libSortField==0)std::sort(out.begin(),out.end(),byName);
 else if(libSortField==1)std::sort(out.begin(),out.end(),byDate);
 else std::sort(out.begin(),out.end(),bySize);
 if(libSortDesc&&libSortField!=0)std::reverse(out.begin(),out.end());
 if(libSortDesc&&libSortField==0)std::reverse(out.begin(),out.end());
 return out;
}
bool PhotoPasses(const PhotoEntry& photo){
 if(!libSearch.empty()){
  std::wstring needle=libSearch,name=photo.name;
  for(auto& c:needle)c=towlower(c);for(auto& c:name)c=towlower(c);
  if(name.find(needle)==std::wstring::npos)return false;
 }
 if(photo.size<filterSizeLo||photo.size>filterSizeHi)return false;
 std::wstring ext=photo.ext;for(auto& c:ext)c=towlower(c);
 if(!filterExtOn.empty()&&!filterExtOn.count(ext))return false;
 bool raw=RawExt(ext);
 if(filterRawOnly&&!raw)return false;
 if(filterRegularOnly&&raw)return false;
 if(filterProfile){
  auto profile=ProfileOf(photo.path);
  if(!profile.empty()){
   bool match=(filterProfile==1&&profile==L"sRGB")||(filterProfile==4&&profile==L"none")||
    ((filterProfile==2||filterProfile==3)&&profile==L"other");
   if(!match)return false;
  }
 }
 return true;
}
void SortPhotos(std::vector<PhotoEntry>& list){
 auto byName=[](const PhotoEntry& a,const PhotoEntry& b){return StrCmpLogicalW(a.name.c_str(),b.name.c_str())<0;};
 auto byDate=[](const PhotoEntry& a,const PhotoEntry& b){return a.modified<b.modified;};
 auto bySize=[](const PhotoEntry& a,const PhotoEntry& b){return a.size<b.size;};
 if(libSortField==0)std::sort(list.begin(),list.end(),byName);
 else if(libSortField==1)std::sort(list.begin(),list.end(),byDate);
 else std::sort(list.begin(),list.end(),bySize);
 if(libSortDesc&&libSortField!=0)std::reverse(list.begin(),list.end());
 if(libSortDesc&&libSortField==0)std::reverse(list.begin(),list.end());
}
std::wstring AssetStem(const PhotoEntry& photo,bool& edited){
 auto stem=fs::path(photo.name).stem().wstring();for(auto& c:stem)c=towlower(c);edited=false;
 static const std::wstring suffixes[]={L"_edited",L"-edited",L"_edit",L"-edit",L"_final",L"-final",L"_export",L"-export",L"_copy",L"-copy"};
 for(auto& suffix:suffixes)if(stem.size()>suffix.size()&&stem.ends_with(suffix)){stem.resize(stem.size()-suffix.size());edited=true;break;}
 if(stem.size()>4&&stem.back()==L')'){
  auto open=stem.find_last_of(L'(');if(open!=std::wstring::npos&&open>0){bool digits=true;for(size_t i=open+1;i+1<stem.size();++i)digits&=iswdigit(stem[i])!=0;
   if(digits){while(open&&stem[open-1]==L' ')--open;stem.resize(open);edited=true;}}
 }
 return stem;
}
void BuildAssetStacks(){
 assetVariants.clear();
 struct Candidate{std::vector<PhotoEntry> files;bool raw=false,regular=false,edited=false,plain=false;};
 std::unordered_map<std::wstring,Candidate> groups;
 for(auto& photo:albumPhotos){bool edit=false;auto stem=AssetStem(photo,edit);auto parent=NormalisePath(fs::path(photo.path).parent_path().wstring());auto& group=groups[parent+L"\n"+stem];
  group.raw|=RawExt(photo.ext);group.regular|=!RawExt(photo.ext);group.edited|=edit;group.plain|=!edit;group.files.push_back(photo);}
 std::vector<PhotoEntry> collapsed;collapsed.reserve(albumPhotos.size());
 for(auto& [key,group]:groups){
  bool stack=group.files.size()>1&&((group.raw&&group.regular)||(group.edited&&group.plain));
  if(!stack){for(auto& file:group.files)collapsed.push_back(std::move(file));continue;}
  auto score=[](const PhotoEntry& file){bool edit=false;AssetStem(file,edit);return edit&&!RawExt(file.ext)?4:!RawExt(file.ext)?3:edit?2:1;};
  auto primary=std::max_element(group.files.begin(),group.files.end(),[&](auto& a,auto& b){return score(a)<score(b);});
  primary->variantCount=uint32_t(group.files.size());assetVariants[primary->id]=group.files;collapsed.push_back(*primary);
 }
 albumPhotos=std::move(collapsed);SortPhotos(albumPhotos);
}
uint32_t LocalDay(uint64_t fileTime){
 if(!fileTime)return 0;
 FILETIME utc{DWORD(fileTime),DWORD(fileTime>>32)},local{};SYSTEMTIME date{};
 if(!FileTimeToLocalFileTime(&utc,&local)||!FileTimeToSystemTime(&local,&date))return 0;
 return uint32_t(date.wYear)*10000u+uint32_t(date.wMonth)*100u+uint32_t(date.wDay);
}
void RebuildTimeline(){
 std::map<uint32_t,std::vector<size_t>,std::greater<uint32_t>> days;
 for(size_t i=0;i<albumPhotos.size();++i)days[LocalDay(albumPhotos[i].modified)].push_back(i);
 timelineGroups.clear();timelineGroups.reserve(days.size());
 for(auto& [day,photos]:days)timelineGroups.push_back({day,std::move(photos)});
}
std::wstring TimelineDateLabel(uint32_t key){
 if(!key)return language?L"Unknown date":L"Без даты";
 SYSTEMTIME now{};GetLocalTime(&now);
 auto dayKey=[](const SYSTEMTIME& value){return uint32_t(value.wYear)*10000u+uint32_t(value.wMonth)*100u+uint32_t(value.wDay);};
 if(key==dayKey(now))return language?L"Today":L"Сегодня";
 FILETIME today{};SystemTimeToFileTime(&now,&today);
 ULARGE_INTEGER ticks{};ticks.LowPart=today.dwLowDateTime;ticks.HighPart=today.dwHighDateTime;
 ticks.QuadPart-=864000000000ull;
 FILETIME previous{ticks.LowPart,ticks.HighPart};SYSTEMTIME yesterday{};FileTimeToSystemTime(&previous,&yesterday);
 if(key==dayKey(yesterday))return language?L"Yesterday":L"Вчера";
 SYSTEMTIME date{};date.wYear=WORD(key/10000);date.wMonth=WORD((key/100)%100);date.wDay=WORD(key%100);
 wchar_t result[96]{};
 GetDateFormatEx(language?L"en-US":L"ru-RU",0,&date,language?L"MMMM d, yyyy":L"d MMMM yyyy",result,96,nullptr);
 return result;
}
void RefreshAlbum(){
 albumPhotos.clear();
 if(favouritesOpen){
  RefreshFavourites();
  for(auto& photo:favouritePhotos)if(PhotoPasses(photo))albumPhotos.push_back(photo);
  SortPhotos(albumPhotos);RebuildTimeline();
  return;
 }
 auto collect=[&](const FolderEntry& f){
  auto addPhotos=[&](const std::wstring& p){
   auto list=IndexPhotosIn(p);
   if(list.empty()){
    FolderEntry probe;
    list=IndexInspectDirectory(p,probe);
   }
   for(auto& photo:list)if(PhotoPasses(photo))albumPhotos.push_back(photo);
  };
  if(f.members.empty())addPhotos(f.path);
  else for(auto& member:f.members)addPhotos(member);
 };
 if(libView==ViewPhotosFlat){
  for(auto& f:libFolders)collect(f);
 }else{
  auto family=std::find_if(libFolders.begin(),libFolders.end(),[](auto& f){return NormalisePath(f.path)==NormalisePath(albumFolder);});
  if(family!=libFolders.end())collect(*family);
  else for(auto& photo:IndexPhotosIn(albumFolder))if(PhotoPasses(photo))albumPhotos.push_back(photo);
  // The card carries a photo count from the summary scan, but the per-folder
  // photo list arrives later on the indexer's own thread. Opening a folder
  // before that lands would otherwise show an empty grid for a moment and,
  // worse, give the opening animation no destination cells to fly into — the
  // reason the first open of a folder had no animation and the second did.
  // One directory listing costs milliseconds and makes both opens identical.
  if(albumPhotos.empty()&&!albumFolder.empty()){
   FolderEntry probe;
   for(auto& photo:IndexInspectDirectory(albumFolder,probe))if(PhotoPasses(photo))albumPhotos.push_back(photo);
  }
 }
 SortPhotos(albumPhotos);
 BuildAssetStacks();
 RebuildTimeline();
}
// The Favourites collection. Nothing is copied and nothing is moved: this
// reads the local database and turns each surviving path into the same
// PhotoEntry the rest of the grid already knows how to draw.
void RefreshFavourites(){
 favouritePhotos.clear();
 auto paths=FavouritePaths();
 favouritePhotos.reserve(paths.size());
 for(auto& path:paths){
  std::error_code ec;
  PhotoEntry photo;
  photo.path=path;
  photo.name=fs::path(path).filename().wstring();
  photo.ext=ExtOf(path);
  auto bytes=fs::file_size(fs::path(path),ec);photo.size=ec?0:uint64_t(bytes);
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info))
   photo.modified=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
  photo.id=PhysicalId(path);
  favouritePhotos.push_back(std::move(photo));
 }
 favouritesSeen=FavouritesRevision();
}
void RefreshLibrary(){
 libFolders=RefreshLibraryFolders();
 RefreshFavourites();
 if(favouritesOpen){
  albumPhotos.clear();
  for(auto& photo:favouritePhotos)if(PhotoPasses(photo))albumPhotos.push_back(photo);
  SortPhotos(albumPhotos);RebuildTimeline();
 }else if(libView==ViewPhotosFlat||screen==ScrAlbum)RefreshAlbum();
 Wake();
}

// A single virtualised-grid layout shared by the folder cards and the photo
// cells: given how many square-ish tiles fit per row, only the rows that
// intersect the viewport (plus a little overscan) are ever asked to paint or
// hold a thumbnail bitmap — everything else is just arithmetic.
struct GridPlacement{int columns;float cellW,cellH,contentH;};
GridPlacement PlanGrid(float areaW,float cellW,float cellH,float gap,size_t count){
 int columns=(std::max)(1,int((areaW+gap)/(cellW+gap)));
 int rows=int((count+size_t(columns)-1)/size_t(columns));
 return {columns,cellW,cellH,rows*(cellH+gap)-(rows>0?gap:0)};
}
D2D1_RECT_F GridCell(const GridPlacement& grid,float areaLeft,float areaTop,float gap,size_t index){
 int col=int(index)%grid.columns,row=int(index)/grid.columns;
 float x=areaLeft+col*(grid.cellW+gap),y=areaTop+row*(grid.cellH+gap);
 return D2D1::RectF(x,y,x+grid.cellW,y+grid.cellH);
}

constexpr float TimelineHeaderH=38,TimelineGroupGap=24;
// Height reserved above the timeline for the Favourites shortcut chip.
constexpr float FavouriteChipH=54;
struct TimelinePlacement{int columns=1;float contentH=0;std::vector<float> groupTops;};
TimelinePlacement PlanTimeline(float areaW){
 TimelinePlacement plan;
 plan.columns=(std::max)(1,int((areaW+LibGap)/(AlbumCell+LibGap)));
 float y=0;plan.groupTops.reserve(timelineGroups.size());
 for(auto& group:timelineGroups){
  plan.groupTops.push_back(y);y+=TimelineHeaderH;
  int rows=int((group.photos.size()+size_t(plan.columns)-1)/size_t(plan.columns));
  y+=rows*(AlbumCell+LibGap)-(rows?LibGap:0)+TimelineGroupGap;
 }
 plan.contentH=(std::max)(0.f,y-(timelineGroups.empty()?0.f:TimelineGroupGap));
 return plan;
}
D2D1_RECT_F TimelinePhotoRect(uint64_t photoId,float areaLeft,float areaTop,float areaW,float scroll){
 auto plan=PlanTimeline(areaW);
 for(size_t g=0;g<timelineGroups.size();++g){
  auto& photos=timelineGroups[g].photos;
  for(size_t local=0;local<photos.size();++local)if(albumPhotos[photos[local]].id==photoId){
   int col=int(local)%plan.columns,row=int(local)/plan.columns;
   float x=areaLeft+col*(AlbumCell+LibGap);
   float y=areaTop-scroll+FavouriteChipH+plan.groupTops[g]+TimelineHeaderH+row*(AlbumCell+LibGap);
   return D2D1::RectF(x,y,x+AlbumCell,y+AlbumCell);
  }
 }
 return D2D1::RectF(0,0,0,0);
}

void OpenFavourites(){
 if(screen==ScrLibrary)navStack.push_back({ScrLibrary,L"",0,libScroll});
 favouritesOpen=true;albumFolder.clear();albumScroll=0;albumScrollVel=0;
 RefreshAlbum();
 screen=ScrAlbum;Wake();
}
void OpenAlbum(const std::wstring& folder){
 favouritesOpen=false;
 auto entry=std::find_if(libFolders.begin(),libFolders.end(),[&](auto& f){return f.path==folder;});
 if(entry!=libFolders.end()&&!entry->members.empty()){for(auto& member:entry->members)IndexTouchFolder(member);}
 else IndexTouchFolder(folder);
 if(screen==ScrLibrary)navStack.push_back({ScrLibrary,L"",0,libScroll});
 albumFolder=folder;albumScroll=0;albumScrollVel=0;
 RefreshAlbum();
 folderTx.active=true;folderTx.closing=false;folderTx.started=Now();folderTx.folder=folder;folderTx.blend.Reset(0);folderTx.blend.To(1);
 screen=ScrAlbum;Wake();
 PrepareFolderTransition(folder,false);
}
void GoToLibrary(){
 SetWindowPos(win,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
 RefreshLibrary();
 auto previousFolder=albumFolder;
 PrepareFolderTransition(previousFolder,true);
 albumFolder.clear();favouritesOpen=false;
 screen=ScrLibrary;
 folderTx.active=true;folderTx.closing=true;folderTx.started=Now();folderTx.blend.Reset(1);folderTx.blend.To(0);
 Wake();
}
D2D1_RECT_F FindAlbumCellRect(uint64_t photoId,float w,float h);
// The grid already carries each photo's real stable id (NTFS file identity,
// not a path hash); look it up there instead of independently re-deriving an
// id from the path, so hero-transition matching always agrees with the grid.
// Compares two paths the way the filesystem would: case-insensitively, and
// treating any run of separators as one. A cached entry written before the
// drive-root fix still spells itself "C:\\Users\..."; a plain string compare
// against the ordinary spelling misses it and the transition silently finds
// nothing to fly.
bool SamePath(const std::wstring& a,const std::wstring& b){
 auto sep=[](wchar_t c){return c==L'\\'||c==L'/';};
 size_t i=0,j=0;
 while(i<a.size()&&j<b.size()){
  if(sep(a[i])&&sep(b[j])){while(i<a.size()&&sep(a[i]))i++;while(j<b.size()&&sep(b[j]))j++;continue;}
  if(towlower(a[i])!=towlower(b[j]))return false;
  i++;j++;
 }
 while(i<a.size()&&sep(a[i]))i++;
 while(j<b.size()&&sep(b[j]))j++;
 return i==a.size()&&j==b.size();
}
uint64_t AlbumPhotoIdFor(const std::wstring& path){
 for(auto& p:albumPhotos)if(SamePath(p.path,path))return p.id;
 return 0;
}
void OpenPhotoFromAlbum(const std::wstring& path,uint64_t photoId,D2D1_RECT_F sourceRect){
 libScrollVel=albumScrollVel=0;
 navStack.push_back({screen,albumFolder,photoId,screen==ScrLibrary?libScroll:albumScroll});
 hero.active=true;hero.closing=false;hero.photoId=photoId;
 hero.left.Reset(sourceRect.left);hero.top.Reset(sourceRect.top);
 hero.right.Reset(sourceRect.right);hero.bottom.Reset(sourceRect.bottom);
 hero.radius.Reset(12);hero.crossfade.Reset(0);hero.crossfade.To(1);hero.shadow.Reset(0);hero.shadow.To(1);
 hero.fromBitmap.Reset();hero.fromW=hero.fromH=0;
 if(auto thumb=ThumbLookup(path)){
  hero.fromW=thumb->w;hero.fromH=thumb->h;
  Dc()->CreateBitmap(D2D1::SizeU(thumb->w,thumb->h),thumb->pixels.data(),thumb->w*4,
   D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&hero.fromBitmap);
 }
 screen=ScrViewer;
 Open(path);
 float w,h;Size(w,h);
 float iw=float(hero.fromW?hero.fromW:1),ih=float(hero.fromH?hero.fromH:1);
 float fitScale=(std::min)(w/iw,h/ih);
 hero.left.To((w-iw*fitScale)*.5f);hero.right.To((w+iw*fitScale)*.5f);
 hero.top.To((h-ih*fitScale)*.5f);hero.bottom.To((h+ih*fitScale)*.5f);hero.radius.To(0);
 hero.crossfade.Reset(0);
 Wake();
}
void ViewerBack(){
 // Navigation always wins over decoration.  A shared-element animation must
 // never make the window temporarily modal or swallow a Back press.
 hero.active=false;folderTx.active=false;
 libScrollVel=albumScrollVel=0;
 SetWindowPos(win,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
 if(navStack.empty()){Close();return;}
 auto back=navStack.back();navStack.pop_back();
 if(back.screen==ScrAlbum){
  albumFolder=back.folder;favouritesOpen=back.folder.empty()&&favouritesOpen;RefreshAlbum();
  screen=ScrAlbum;albumScroll=back.scroll;
 }else{
  albumFolder.clear();favouritesOpen=false;
  screen=ScrLibrary;libScroll=back.scroll;
 }
 float w,h;Size(w,h);
 {
  auto& scroll=screen==ScrLibrary?libScroll:albumScroll;
  auto photoId=AlbumPhotoIdFor(currentPath);
  D2D1_RECT_F cell{};
  if(screen==ScrLibrary&&libView==ViewPhotosFlat)
   cell=TimelinePhotoRect(photoId,Margin+10,LibTop,w-2*(Margin+10),scroll);
  else{
   auto grid=PlanGrid(w-2*(Margin+10),AlbumCell,AlbumCell,LibGap,albumPhotos.size());
   for(size_t i=0;i<albumPhotos.size();++i)if(albumPhotos[i].id==photoId){cell=GridCell(grid,Margin+10,LibTop-scroll,LibGap,i);break;}
  }
  if(Width(cell)>0){
   if(cell.top<LibTop)scroll-=(LibTop-cell.top);
   else if(cell.bottom>h-40)scroll+=cell.bottom-(h-40);
   scroll=(std::max)(0.f,scroll);
  }
 }
 auto currentGridId=AlbumPhotoIdFor(currentPath);auto targetId=currentGridId?currentGridId:back.photoId;
 auto target=FindAlbumCellRect(targetId,w,h);
 hero.active=true;hero.closing=true;hero.photoId=targetId;
 // The image's current on-screen rect is where it already is; ImageMatrix
 // gives the exact drawn bounds so the fly-back starts from the real frame.
 auto m=ImageMatrix();
 auto tl=m.TransformPoint(D2D1::Point2F(0,0)),br=m.TransformPoint(D2D1::Point2F(float(current?current->w:1),float(current?current->h:1)));
 hero.left.Reset((std::min)(tl.x,br.x));hero.top.Reset((std::min)(tl.y,br.y));
 hero.right.Reset((std::max)(tl.x,br.x));hero.bottom.Reset((std::max)(tl.y,br.y));
 hero.radius.Reset(0);hero.shadow.Reset(1);hero.crossfade.Reset(1);
 if(Width(target)>0){
  hero.left.To(target.left);hero.top.To(target.top);hero.right.To(target.right);hero.bottom.To(target.bottom);
  hero.radius.To(12);hero.shadow.To(0);hero.crossfade.To(1);
 }else{
  // The thumbnail this came from is no longer on screen (filtered out, or
  // the folder changed underneath us): settle for a plain scale-to-centre.
  float cx=(hero.left.v+hero.right.v)/2,cy=(hero.top.v+hero.bottom.v)/2;
  hero.left.To(cx-4);hero.right.To(cx+4);hero.top.To(cy-4);hero.bottom.To(cy+4);
  hero.shadow.To(0);hero.crossfade.To(0);
 }
 hero.fromBitmap=bitmap;hero.fromW=current?current->w:0;hero.fromH=current?current->h:0;
 Wake();
}

void Layout(){
 hots.clear();rects.clear();
 float w,h;Size(w,h);
 if(preview)return;

 float y0=Margin,y1=Margin+Bubble;
 Hotspot(IdBack,D2D1::RectF(Margin,y0,Margin+Bubble,y1));
 bool wideEnough=w>940;
 if(wideEnough){
  float left=Margin+Bubble+10,width=236;
  D2D1_RECT_F zoomBar=D2D1::RectF(left,y0,left+width,y1);
  rects[IdZoomBar]=zoomBar;                       // the bubble itself
  Hotspot(IdZoomOut,D2D1::RectF(left+7,y0+5,left+41,y1-5));
  Hotspot(IdTrack,D2D1::RectF(left+48,y0+8,left+width-48,y1-8));
  Hotspot(IdZoomIn,D2D1::RectF(left+width-41,y0+5,left+width-7,y1-5));
 }

 D2D1_RECT_F winBar=LayoutWindowButtons(w);
 float editRight=winBar.left-10;
 D2D1_RECT_F editBubble=D2D1::RectF(editRight-74,y0,editRight,y1);
 Hotspot(IdEdit,editBubble);
 float dockRight=editBubble.left-8;
 float dockWidth=6*Cell+12;
 D2D1_RECT_F dock=D2D1::RectF(dockRight-dockWidth,y0,dockRight,y1);
 rects[IdDockBar]=dock;
 const int dockIds[6]={IdInfo,IdCopy,IdLike,IdRotate,IdFit,IdMore};
 for(int i=0;i<6;i++)Hotspot(dockIds[i],D2D1::RectF(dock.left+6+i*Cell,y0+2,dock.left+6+i*Cell+Cell,y1-2));

 // Filmstrip
 if(siblings.size()>1){
  Hotspot(IdPrevPhoto,D2D1::RectF(Margin,h/2-22,Margin+44,h/2+22));
  Hotspot(IdNextPhoto,D2D1::RectF(w-Margin-44,h/2-22,w-Margin,h/2+22));
  float top=h-Margin-GalleryH;
  float content=0;
  for(size_t i=0;i<galleryWidth.size();i++)content+=galleryWidth[i].v+(i?ThumbGap:0);
  float maxWidth=w-2*Margin-40;
  float barWidth=(std::min)(maxWidth,content+20);
  D2D1_RECT_F bar=D2D1::RectF((w-barWidth)/2,top,(w+barWidth)/2,top+GalleryH);
  rects[IdGalleryBody]=bar;
  hots.push_back({IdGalleryBody,bar});
  float x=bar.left+10+galleryScroll;
  for(size_t i=0;i<galleryWidth.size();i++){
   float width=galleryWidth[i].v;
   D2D1_RECT_F item=D2D1::RectF(x,top+(GalleryH-ThumbH)/2,x+width,top+(GalleryH+ThumbH)/2);
   if(item.right>bar.left+4&&item.left<bar.right-4)Hotspot(int(IdGallery0+i),item);
   else rects[int(IdGallery0+i)]=item;
   x+=width+ThumbGap;
  }
 }

 // Side panel
 if(panel!=PanelNone||panelSlide.v>.004f){
  float top=SideTop(),bottom=SideBottom(h);
  D2D1_RECT_F body=D2D1::RectF(w-16-SideWidth,top,w-16,(std::max)(top+120,bottom));
  body=Shift(body,(1.f-panelSlide.v)*24.f,0);
  rects[IdPanelBody]=body;
  hots.push_back({IdPanelBody,body});
  float rowTop=body.top+PanelHead-panelScroll;
  auto row=[&](int id,float height)->D2D1_RECT_F{
   D2D1_RECT_F r=D2D1::RectF(body.left+12,rowTop,body.right-12,rowTop+height);
   rowTop+=height+RowGap;
   Hotspot(id,r);
   return r;
  };
  if(panel==PanelMenu){
   if(menuLevel==0){
    row(IdSend,RowH);row(IdSave,RowH);row(IdSaveAs,RowH);row(IdPrint,RowH);
    auto del=row(IdDelete,RowH);
    if(confirmDelete){
     rowTop=del.bottom+8;
     float mid=(body.left+body.right)/2;
     Hotspot(IdConfirmCancel,D2D1::RectF(body.left+12,rowTop,mid-4,rowTop+44));
     Hotspot(IdConfirmDelete,D2D1::RectF(mid+4,rowTop,body.right-12,rowTop+44));
     rowTop+=52;
    }
    rowTop+=GroupGap;
    row(IdDefaultApp,RowH);row(IdSpacePreview,RowH);
    if(tcStatus!=TcMissing)row(IdTotalCmd,RowH);
    rowTop+=GroupGap;
    D2D1_RECT_F theme=D2D1::RectF(body.left+12,rowTop,body.right-12,rowTop+RowH);
    rects[IdThemeRow]=theme;
    auto segments=[&](int leftId,int rightId,D2D1_RECT_F row,const wchar_t* label){
     // Keep the selector to the right of its translated label.  Fixed 92 px
     // halves overlapped the Russian wheel label in a compact side panel.
     float labelRight=row.left+56+Measure(label,F_Row,120)+12;
     float half=Clamp((row.right-8-labelRight-4)*.5f,76.f,92.f);
     float groupWidth=half*2+4;
     float left=(std::max)(labelRight,row.right-8-groupWidth);
     Hotspot(leftId,D2D1::RectF(left,row.top+9,left+half,row.bottom-9));
     Hotspot(rightId,D2D1::RectF(left+half+4,row.top+9,left+half*2+4,row.bottom-9));
    };
    segments(IdThemeDark,IdThemeLight,theme,T(S_ThemeLabel));
    rowTop=theme.bottom+RowGap;
    D2D1_RECT_F wheel=D2D1::RectF(body.left+12,rowTop,body.right-12,rowTop+RowH);
    rects[IdWheelRow]=wheel;
    segments(IdWheelZoom,IdWheelNav,wheel,T(S_WheelLabel));
    rowTop=wheel.bottom+RowGap;
    row(IdLanguage,RowH);
   }else{
    row(IdLangBack,46);rowTop+=6;
    row(IdLangRu,RowH);row(IdLangEn,RowH);
   }
  }else if(panel==PanelEdit){
   row(IdToolCrop,RowH);row(IdToolRotate,RowH);
   rowTop+=GroupGap;
   row(IdToolDraw,RowH);row(IdToolArrow,RowH);
   rowTop+=GroupGap;
   row(IdToolSelect,RowH);
   if(tool==ToolCrop){
    rowTop+=12;
    float chip=(Width(body)-20-4*6)/5.6f,wide=chip*1.6f,x=body.left+10;
    const int ids[5]={IdCropFree,IdCrop11,IdCrop43,IdCrop32,IdCrop169};
    for(int i=0;i<5;i++){
     float width=i?chip:wide;
     Hotspot(ids[i],D2D1::RectF(x,rowTop,x+width,rowTop+34));
     x+=width+6;
    }
    rowTop+=44;
    float mid=(body.left+body.right)/2;
    Hotspot(IdCropCancel,D2D1::RectF(body.left+10,rowTop,mid-3,rowTop+40));
    Hotspot(IdCropApply,D2D1::RectF(mid+3,rowTop,body.right-10,rowTop+40));
   }
   if(tool==ToolRotate){
    rowTop+=12;
    float mid=(body.left+body.right)/2;
    Hotspot(IdRotLeft,D2D1::RectF(body.left+10,rowTop,mid-3,rowTop+42));
    Hotspot(IdRotRight,D2D1::RectF(mid+3,rowTop,body.right-10,rowTop+42));
   }
   if(tool==ToolDraw||tool==ToolArrow||tool==ToolSelect){
    rowTop+=12;
    if(tool==ToolSelect){
     float mid=(body.left+body.right)/2;
     Hotspot(IdShapeRect,D2D1::RectF(body.left+12,rowTop,mid-3,rowTop+34));
     Hotspot(IdShapeEllipse,D2D1::RectF(mid+3,rowTop,body.right-12,rowTop+34));rowTop+=44;
    }
    float chip=(Width(body)-20-5*6)/6;
    for(int i=0;i<6;i++)Hotspot(IdSwatch0+i,D2D1::RectF(body.left+10+i*(chip+6),rowTop,body.left+10+i*(chip+6)+chip,rowTop+30));
    rowTop+=40;
    float third=(Width(body)-20-12)/3;
    Hotspot(IdThin,D2D1::RectF(body.left+10,rowTop,body.left+10+third,rowTop+34));
    Hotspot(IdMedium,D2D1::RectF(body.left+16+third,rowTop,body.left+16+2*third,rowTop+34));
    Hotspot(IdThick,D2D1::RectF(body.left+22+2*third,rowTop,body.left+22+3*third,rowTop+34));
    rowTop+=44;
    Hotspot(IdUndo,D2D1::RectF(body.left+12,rowTop,body.right-12,rowTop+44));
    rowTop+=44;
   }
  }
  if(panel==PanelMenu||panel==PanelEdit){
   // Rows that overflow the panel scroll under the fixed header.
   panelExtent=(std::max)(0.f,(rowTop+panelScroll)-body.bottom+16);
   panelScroll=Clamp(panelScroll,0,panelExtent);
  }
 }
}

// --------------------------------------------------------------- paint -----
float Press(int id){return B(id).press.v;}
float Hover(int id){return B(id).hover.v;}
D2D1::Matrix3x2F Lift(D2D1_RECT_F r,int id,float extra=0){
 float scale=1.f+.012f*Hover(id)-.025f*Press(id)+extra;
 float cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
 return D2D1::Matrix3x2F::Translation(-cx,-cy)*D2D1::Matrix3x2F::Scale(scale,scale)*
  D2D1::Matrix3x2F::Translation(cx,cy-1.2f*Hover(id));
}
void PlateFor(int id,D2D1_RECT_F r,const Palette& p,float alpha,float radius=11){
 float strength=Hover(id)*.9f+Press(id)*.6f;
 if(strength<=.004f)return;
 auto colour=Mix(p.hover,p.press,Clamp(Press(id),0,1));
 Ink()->SetColor(Fade(colour,Clamp(strength,0,1)*alpha));
 Dc()->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),Ink());
}
void IconButton(int id,const wchar_t* path,const Palette& p,float alpha,D2D1_COLOR_F tint,float rotation=0,bool fill=false){
 auto r=R(id);
 if(Width(r)<=0)return;
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 // A lone icon sitting in a shared glass capsule (dock, zoom bar, window
 // controls) reads as hovered from the scale alone; a background plate here
 // would just be a stray rounded-rect showing through the capsule's corners.
 Dc()->SetTransform(Lift(r,id)*Mat(previous));
 Icon(path,Inset(r,10),Fade(tint,alpha),1.7f,fill,rotation);
 Dc()->SetTransform(previous);
 (void)p;
}

void PaintBackdrop(float w,float h,const Palette& p){
 auto target=Dc();
 if(backdrop&&current&&!current->hasAlpha){
  auto size=backdrop->GetSize();
  float scale=(std::max)(w/size.width,h/size.height)*1.25f;
  float bw=size.width*scale,bh=size.height*scale;
  target->DrawBitmap(backdrop.Get(),D2D1::RectF((w-bw)/2,(h-bh)/2,(w+bw)/2,(h+bh)/2),1.f,
   D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr);
  Ink()->SetColor(Mix(D2D1::ColorF(.02f,.03f,.05f,.26f),D2D1::ColorF(.97f,.975f,.99f,.36f),themeMix.v));
  target->FillRectangle(D2D1::RectF(0,0,w,h),Ink());
 }else{
  Ink()->SetColor(Mix(D2D1::ColorF(.055f,.061f,.074f,1.f),D2D1::ColorF(.95f,.955f,.965f,1.f),themeMix.v));
  target->FillRectangle(D2D1::RectF(0,0,w,h),Ink());
 }
 (void)p;
}
void PaintStrokes(){
 auto target=Dc();
 auto paint=[&](const Stroke& s){
  if(s.pts.size()<2)return;
  Ink()->SetColor(s.colour);
  if(s.kind>=1){
   auto a=s.pts.front(),b=s.pts.back();
   if(s.kind>=2){
    auto r=D2D1::RectF((std::min)(a.x,b.x),(std::min)(a.y,b.y),(std::max)(a.x,b.x),(std::max)(a.y,b.y));
    if(s.kind==3)target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((a.x+b.x)/2,(a.y+b.y)/2),Width(r)/2,Height(r)/2),Ink(),s.width);
    else {float radius=(std::min)({Width(r)*.12f,Height(r)*.12f,s.width*3});target->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),Ink(),s.width);}
   }else{
    // A single filled polygon: shaft and head merge with no seam, and the
    // head is sized off a fixed multiple of the shaft so it stays a proper
    // arrowhead at any zoom level instead of shrinking to a pin at high zoom.
    float dx=b.x-a.x,dy=b.y-a.y,len=hypotf(dx,dy);if(len<.01f)return;
    dx/=len;dy/=len;float nx=-dy,ny=dx;
    float headLen=Clamp(s.width*8.5f,s.width*2.4f,len*.65f);
    float headHalf=headLen*.52f,shaftHalf=s.width*.42f;
    auto neck=D2D1::Point2F(b.x-dx*headLen,b.y-dy*headLen);
    ComPtr<ID2D1PathGeometry> arrow;ComPtr<ID2D1GeometrySink> sink;
    if(SUCCEEDED(GfxFactory()->CreatePathGeometry(&arrow))&&SUCCEEDED(arrow->Open(&sink))){
     sink->BeginFigure(D2D1::Point2F(a.x+nx*shaftHalf,a.y+ny*shaftHalf),D2D1_FIGURE_BEGIN_FILLED);
     sink->AddLine(D2D1::Point2F(neck.x+nx*shaftHalf,neck.y+ny*shaftHalf));
     sink->AddLine(D2D1::Point2F(neck.x+nx*headHalf,neck.y+ny*headHalf));
     sink->AddLine(b);
     sink->AddLine(D2D1::Point2F(neck.x-nx*headHalf,neck.y-ny*headHalf));
     sink->AddLine(D2D1::Point2F(neck.x-nx*shaftHalf,neck.y-ny*shaftHalf));
     sink->AddLine(D2D1::Point2F(a.x-nx*shaftHalf,a.y-ny*shaftHalf));
     sink->EndFigure(D2D1_FIGURE_END_CLOSED);sink->Close();target->FillGeometry(arrow.Get(),Ink());
    }
   }
   return;
  }
  // Freehand pen only reaches here (kind==0): arrow and shapes return above.
  ComPtr<ID2D1PathGeometry> geometry;ComPtr<ID2D1GeometrySink> sink;
  if(FAILED(GfxFactory()->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink)))return;
  sink->BeginFigure(s.pts.front(),D2D1_FIGURE_BEGIN_HOLLOW);
  for(size_t i=1;i<s.pts.size();i++)sink->AddLine(s.pts[i]);
  sink->EndFigure(D2D1_FIGURE_END_OPEN);sink->Close();
  target->DrawGeometry(geometry.Get(),Ink(),s.width);
 };
 for(auto& s:strokes)paint(s);
 if(painting)paint(live);
}
void PaintImage(float w,float h,const Palette& p){
 if(!bitmap||!current)return;
 auto target=Dc();
 bool clipped=CropActive();
 if(clipped)target->PushAxisAlignedClip(ToScreen(crop),D2D1_ANTIALIAS_MODE_ALIASED);
 auto matrix=ImageMatrix();
 target->SetTransform(matrix);
 target->DrawBitmap(bitmap.Get(),D2D1::RectF(0,0,float(current->w),float(current->h)),1.f,
  Zoom()>8.f?D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR:D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr);
 if(clipHighFade.v>.004f||clipLowFade.v>.004f){
  DrawClipping(bitmap.Get(),true,clipHighFade.v);
  DrawClipping(bitmap.Get(),false,clipLowFade.v);
 }
 target->SetTransform(DisplayMatrix());
 PaintStrokes();
 target->SetTransform(D2D1::Matrix3x2F::Identity());
 if(clipped)target->PopAxisAlignedClip();
 if(preview)return;
 if(hasCrop&&tool==ToolCrop){
  auto s=ToScreen(crop);
  Ink()->SetColor(D2D1::ColorF(0,0,0,.55f));
  target->FillRectangle(D2D1::RectF(0,0,w,s.top),Ink());
  target->FillRectangle(D2D1::RectF(0,s.bottom,w,h),Ink());
  target->FillRectangle(D2D1::RectF(0,s.top,s.left,s.bottom),Ink());
  target->FillRectangle(D2D1::RectF(s.right,s.top,w,s.bottom),Ink());
  Ink()->SetColor(D2D1::ColorF(1,1,1,.9f));
  target->DrawRectangle(s,Ink(),1.4f);
  Ink()->SetColor(D2D1::ColorF(1,1,1,.28f));
  for(int i=1;i<3;i++){
   target->DrawLine(D2D1::Point2F(s.left+Width(s)*i/3,s.top),D2D1::Point2F(s.left+Width(s)*i/3,s.bottom),Ink(),.8f);
   target->DrawLine(D2D1::Point2F(s.left,s.top+Height(s)*i/3),D2D1::Point2F(s.right,s.top+Height(s)*i/3),Ink(),.8f);
  }
  for(int i=0;i<8;i++){
   float hx=(i==0||i==6||i==7)?s.left:(i==1||i==5)?(s.left+s.right)/2:s.right;
   float hy=(i<3)?s.top:(i==3||i==7)?(s.top+s.bottom)/2:s.bottom;
   float size=cropGrab==i?7.5f:5.5f;
   Ink()->SetColor(D2D1::ColorF(1,1,1,.96f));
   target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hx,hy),size,size),Ink());
  }
 }
 (void)p;
}
// The external rating, drawn between the file name and the dimensions.
//
// It is not a control. Nobody can click it, and it changes nothing in this
// application: it is what Lightroom or Bridge already recorded about this
// file, shown because a photographer who rated a shoot elsewhere wants to see
// that here. The heart on the toolbar is the interactive one, and the two are
// never derived from each other.
void PaintXmpRating(D2D1_RECT_F row,const Palette& p,float alpha){
 if(!info.hasRating)return;
 if(info.rating<0){
  // A rejection is not "zero stars"; a five-star track would misread it.
  // A word, not a glyph. A cross small enough to fit this row reads as a
  // rendering artefact next to the file name, and "Rejected" is unambiguous.
  auto colour=Mix(p.danger,D2D1::ColorF(1.f,.42f,.42f,1.f),.4f);
  float w=Measure(T(S_Rejected),F_Small,200)+18;
  float mid=(row.left+row.right)/2;
  D2D1_RECT_F pill=D2D1::RectF(mid-w/2,row.top-1,mid+w/2,row.bottom+1);
  float radius=(pill.bottom-pill.top)/2;
  Ink()->SetColor(Fade(colour,alpha*.16f));
  Dc()->FillRoundedRectangle(D2D1::RoundedRect(pill,radius,radius),Ink());
  Ink()->SetColor(Fade(colour,alpha*.45f));
  Dc()->DrawRoundedRectangle(D2D1::RoundedRect(pill,radius,radius),Ink(),1.f);
  Write(T(S_Rejected),pill,F_Small,Fade(colour,alpha));
  return;
 }
 // Only the stars that are actually set. Five hollow outlines on every
 // unrated photograph would turn a header into a rating widget nobody asked
 // for; a short row of filled stars reads as a fact about the file.
 std::wstring stars(size_t((std::max)(0,(std::min)(5,info.rating))),L'★');
 Write(stars,row,F_Small,Fade(Mix(p.text,D2D1::ColorF(1.f,.78f,.28f,1.f),.85f),alpha));
}
void PaintTitle(float w,const Palette& p,float dockLeft,float leftEdge){
 if(currentPath.empty())return;
 float centre=(leftEdge+dockLeft)/2;
 float half=(std::min)(230.f,(dockLeft-leftEdge)/2-10);
 if(half<70)return;
 std::wstring meta;
 if(current){
  // Both figures are about the photograph, not about the tier being drawn: a
  // screen-tier frame must still say 7360 x 4912 and report the magnification
  // of the original, or the header quietly lies about the file.
  float scale=(std::max)(0.0001f,FrameScale());
  unsigned shownW=CropActive()?(unsigned)(EffW()/scale+.5f):SourceW();
  unsigned shownH=CropActive()?(unsigned)(EffH()/scale+.5f):SourceH();
  meta=std::to_wstring(shownW)+L" × "+std::to_wstring(shownH)+L"   •   "+
   std::to_wstring(int(Zoom()*scale*dpi*100.f+.5f))+L"%";
 }
 float t=titleIn.v;
 // The rating row only takes space when there is a rating to show, so an
 // ordinary photograph keeps exactly the header it had before.
 float ratingH=info.hasRating?15.f:0.f;
 auto name=D2D1::RectF(centre-half,Margin-2,centre+half,Margin+24);
 auto rating=D2D1::RectF(centre-half,Margin+21,centre+half,Margin+21+ratingH);
 auto line=D2D1::RectF(centre-half,Margin+22+ratingH,centre+half,Margin+44+ratingH);
 if(t<.98f&&!prevName.empty()){
  Write(prevName,Shift(name,0,-4*t),F_Title,Fade(p.text,1-t));
  Write(prevMeta,Shift(line,0,-4*t),F_Meta,Fade(p.dim,1-t));
 }
 Write(Name(),Shift(name,0,4*(1-t)),F_Title,Fade(p.text,t));
 if(ratingH>0){
  Hotspot(IdNone,rating);   // no command: hovering only reveals the tooltip
  PaintXmpRating(Shift(rating,0,4*(1-t)),p,t);
 }
 Write(meta,Shift(line,0,4*(1-t)),F_Meta,Fade(p.dim,t));
 (void)w;
}
void PaintZoomBar(const Palette& p){
 auto bubble=R(IdZoomBar);
 if(Width(bubble)<=0)return;
 auto target=Dc();
 Glass(D2D1::RoundedRect(bubble,Bubble/2,Bubble/2),p,1.f,D2D1::Matrix3x2F::Identity());
 IconButton(IdZoomOut,IcMinus,p,1.f,p.text);
 IconButton(IdZoomIn,IcPlus,p,1.f,p.text);
 auto track=R(IdTrack);
 float mid=(track.top+track.bottom)/2;
 Ink()->SetColor(p.track);
 target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left,mid-2,track.right,mid+2),2,2),Ink());
 float t=ZoomToSlider(Zoom());
 float x=track.left+Width(track)*t;
 Ink()->SetColor(p.accent);
 target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left,mid-2,(std::max)(track.left,x),mid+2),2,2),Ink());
 float grab=thumbLift.v;
 float radius=7.f+2.2f*Hover(IdTrack)+3.4f*grab;
 Ink()->SetColor(D2D1::ColorF(0,0,0,.22f*(1.f-.3f*grab)));
 target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,mid+1.5f+grab),radius,radius),Ink());
 Ink()->SetColor(p.thumb);
 target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,mid),radius,radius),Ink());
}
void PaintDock(const Palette& p){
 auto dock=R(IdDockBar);
 if(Width(dock)<=0)return;
 Glass(D2D1::RoundedRect(dock,Bubble/2,Bubble/2),p,1.f,D2D1::Matrix3x2F::Identity());
 IconButton(IdInfo,IcInfo,p,1.f,panel==PanelInfo?p.accent:p.text);
 bool checked=Now()<copyUntil;
 IconButton(IdCopy,checked?IcCheck:IcCopy,p,1.f,checked?D2D1::ColorF(.30f,.85f,.45f,1.f):p.text);
 // Favourite: the heart itself overshoots, with a restrained particle burst.
 {
  auto r=R(IdLike);
  D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
  Dc()->SetTransform(Lift(r,IdLike,likePop.v-1.f)*Mat(previous));
  Icon(IcHeart,Inset(r,10),liked?D2D1::ColorF(1.f,.32f,.38f,1.f):p.text,1.7f,liked);
  Dc()->SetTransform(previous);
  double age=Now()-likeBurst;
  if(age>=0&&age<.5){
   float k=float(age/.5);
   float cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
   Ink()->SetColor(D2D1::ColorF(1.f,.36f,.42f,(1.f-k)*.75f));
   for(int i=0;i<4;i++){
    float a=1.1f+i*1.57f;
    Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx+cosf(a)*(9+14*k),cy+sinf(a)*(9+14*k)),2.2f*(1-k)+.6f,2.2f*(1-k)+.6f),Ink());
   }
  }
 }
 IconButton(IdRotate,IcRotate,p,1.f,p.text,rotate.v);
 IconButton(IdFit,fit?IcExpand:IcCompress,p,1.f,fit?p.text:p.accent);
 IconButton(IdMore,IcDots,p,1.f,panel==PanelMenu?p.accent:p.text,0,true);
}
void PaintEditBubble(const Palette& p){
 auto r=R(IdEdit);
 if(Width(r)<=0)return;
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 auto lift=Lift(r,IdEdit);
 Dc()->SetTransform(lift*Mat(previous));
 Glass(D2D1::RoundedRect(r,Bubble/2,Bubble/2),p,1.f,lift*Mat(previous));
 if(panel==PanelEdit){
  Ink()->SetColor(Fade(p.accent,.18f));
  Dc()->FillRoundedRectangle(D2D1::RoundedRect(Inset(r,3),Bubble/2,Bubble/2),Ink());
 }
 PlateFor(IdEdit,Inset(r,3),p,1.f,Bubble/2);
 Write(T(S_Edit),r,F_Button,panel==PanelEdit?p.accent:p.text);
 Dc()->SetTransform(previous);
}
void PaintWindowButtons(const Palette& p){
 auto bar=R(IdWinBar);
 if(Width(bar)<=0)return;
 Glass(D2D1::RoundedRect(bar,Bubble/2,Bubble/2),p,1.f,D2D1::Matrix3x2F::Identity());
 IconButton(IdWinMin,IcMin,p,1.f,p.dim);
 IconButton(IdWinMax,WindowMaximized()?IcRestore:IcMax,p,1.f,p.dim);
 auto close=R(IdWinClose);
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 Dc()->SetTransform(Lift(close,IdWinClose)*Mat(previous));
 float heat=Hover(IdWinClose);
 if(heat>.004f){
  Ink()->SetColor(D2D1::ColorF(.85f,.22f,.20f,.85f*heat));
  Dc()->FillRoundedRectangle(D2D1::RoundedRect(Inset(close,2),11,11),Ink());
 }
 Icon(IcClose,Inset(close,11),Mix(p.dim,D2D1::ColorF(1,1,1,1),heat),1.8f);
 Dc()->SetTransform(previous);
}
void PaintBack(const Palette& p){
 auto r=R(IdBack);
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 auto lift=Lift(r,IdBack);
 Dc()->SetTransform(lift*Mat(previous));
 Glass(D2D1::RoundedRect(r,Bubble/2,Bubble/2),p,1.f,lift*Mat(previous));
 PlateFor(IdBack,Inset(r,3),p,1.f,Bubble/2);
 Icon(IcBack,Shift(Inset(r,12),-1.5f*Hover(IdBack),0),p.text,1.9f);
 Dc()->SetTransform(previous);
}

// A row is a card in its own right: lighter than the glass it sits on, with a
// hairline edge so it keeps its shape over a busy photograph.
void PanelRow(int id,const wchar_t* icon,const std::wstring& label,const Palette& p,float alpha,bool chevron,
 D2D1_COLOR_F tint,const wchar_t* trailing=nullptr){
 auto r=R(id);
 if(Width(r)<=0)return;
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 Dc()->SetTransform(Lift(r,id)*Mat(previous));
 auto card=D2D1::RoundedRect(r,RowRadius,RowRadius);
 Ink()->SetColor(Fade(p.card,alpha));
 Dc()->FillRoundedRectangle(card,Ink());
 PlateFor(id,r,p,alpha,RowRadius);
 Ink()->SetColor(Fade(p.cardEdge,alpha*.8f));
 Dc()->DrawRoundedRectangle(card,Ink(),1.f);
 float mid=(r.top+r.bottom)/2;
 if(icon)Icon(icon,D2D1::RectF(r.left+16,mid-13,r.left+42,mid+13),Fade(tint,alpha),1.75f);
 float tail=chevron?40.f:(trailing?96.f:20.f);
 Write(label,D2D1::RectF(r.left+(icon?56:20),r.top,r.right-tail,r.bottom),F_Row,Fade(tint,alpha));
 if(trailing)Write(trailing,D2D1::RectF(r.right-120,r.top,r.right-(chevron?38:20),r.bottom),F_Value,Fade(p.dim,alpha));
 if(chevron)Icon(IcChevron,D2D1::RectF(r.right-32,mid-9,r.right-14,mid+9),Fade(p.faint,alpha),1.6f);
 Dc()->SetTransform(previous);
}
void Chip(int id,const std::wstring& label,bool active,const Palette& p,float alpha){
 auto r=R(id);
 if(Width(r)<=0)return;
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 Dc()->SetTransform(Lift(r,id)*Mat(previous));
 Ink()->SetColor(Fade(active?p.accent:p.card,alpha*(active?.92f:1.f)));
 Dc()->FillRoundedRectangle(D2D1::RoundedRect(r,11,11),Ink());
 PlateFor(id,r,p,alpha,11);
 if(!active){Ink()->SetColor(Fade(p.cardEdge,alpha*.7f));Dc()->DrawRoundedRectangle(D2D1::RoundedRect(r,11,11),Ink(),1.f);}
 Write(label,r,F_Button,Fade(active?D2D1::ColorF(1,1,1,1):p.dim,alpha));
 Dc()->SetTransform(previous);
}
void FilterChip(int id,const std::wstring& label,bool active,const Palette& p,float alpha){
 auto r=R(id);if(Width(r)<=0)return;
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 Dc()->SetTransform(Lift(r,id)*Mat(previous));
 auto shape=D2D1::RoundedRect(r,11,11);
 Ink()->SetColor(Fade(active?Mix(p.card,p.text,.10f):p.card,alpha));Dc()->FillRoundedRectangle(shape,Ink());
 PlateFor(id,r,p,alpha,11);
 Ink()->SetColor(Fade(active?Mix(p.cardEdge,p.text,.20f):p.cardEdge,alpha*.9f));Dc()->DrawRoundedRectangle(shape,Ink(),1.f);
 Write(label,r,F_Button,Fade(active?p.text:p.dim,alpha));
 Dc()->SetTransform(previous);
}
void PaintFields(const std::vector<Field>& fields,D2D1_RECT_F body,float& y,Str heading,const Palette& p,float alpha){
 if(fields.empty())return;
 Write(T(heading),D2D1::RectF(body.left+18,y,body.right-18,y+20),F_Section,Fade(p.faint,alpha));
 y+=26;
 for(auto& f:fields){
  Write(f.label,D2D1::RectF(body.left+18,y,body.left+150,y+22),F_Label,Fade(p.dim,alpha));
  Write(f.value,D2D1::RectF(body.left+140,y,body.right-18,y+22),F_Value,Fade(p.text,alpha));
  y+=23;
 }
 y+=12;
}
// Shortens a path from the middle ("...") to fit maxWidth, since Direct2D has
// no built-in ellipsis trimming for a plain DrawText call.
std::wstring Elide(const std::wstring& text,Face face,float maxWidth){
 if(Measure(text,face,maxWidth+400)<=maxWidth)return text;
 // Shrink a window around a fixed ellipsis until the remaining head and tail
 // fit, the way Explorer trims a path while keeping both ends readable.
 size_t l=text.size()/2,r=text.size()/2;
 while(l>0||r<text.size()){
  auto candidate=text.substr(0,l)+L"…"+text.substr(r);
  if(Measure(candidate,face,maxWidth+400)<=maxWidth)return candidate;
  if(l>0)l--;
  if(r<text.size())r++;
 }
 return L"…";
}
// A labelled row with an inline value and a physical sliding toggle, used for
// the highlight/shadow clipping overlays: one glance shows the reading and
// whether the on-photo warning is currently switched on.
void ToggleRow(int id,const std::wstring& label,float percent,bool on,float knobT,const Palette& p,float alpha){
 auto r=R(id);
 if(Width(r)<=0)return;
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 Dc()->SetTransform(Lift(r,id)*Mat(previous));
 auto card=D2D1::RoundedRect(r,RowRadius,RowRadius);
 Ink()->SetColor(Fade(p.card,alpha));Dc()->FillRoundedRectangle(card,Ink());
 PlateFor(id,r,p,alpha,RowRadius);
 Ink()->SetColor(Fade(p.cardEdge,alpha*.8f));Dc()->DrawRoundedRectangle(card,Ink(),1.f);
 float mid=(r.top+r.bottom)/2;
 wchar_t buf[32];swprintf_s(buf,L"%.1f%%",percent);
 Write(label,D2D1::RectF(r.left+18,r.top,r.left+180,r.bottom),F_Row,Fade(p.text,alpha));
 Write(buf,D2D1::RectF(r.right-140,r.top,r.right-66,r.bottom),F_Value,Fade(p.dim,alpha));
 D2D1_RECT_F track=D2D1::RectF(r.right-54,mid-11,r.right-18,mid+11);
 Ink()->SetColor(Fade(Mix(p.sunk,p.accent,knobT),alpha));
 Dc()->FillRoundedRectangle(D2D1::RoundedRect(track,11,11),Ink());
 float knobX=track.left+11+(Width(track)-22)*knobT;
 Ink()->SetColor(Fade(D2D1::ColorF(1,1,1,1),alpha));
 Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX,mid),9,9),Ink());
 Dc()->SetTransform(previous);
 (void)on;
}
void BuildHistogram(){
 if(!info.histReady||histPath[0])return;
 for(int c=0;c<4;c++){
  ComPtr<ID2D1PathGeometry> geometry;ComPtr<ID2D1GeometrySink> sink;
  if(FAILED(GfxFactory()->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink)))return;
  sink->SetFillMode(D2D1_FILL_MODE_WINDING);
  sink->BeginFigure(D2D1::Point2F(0,1),D2D1_FIGURE_BEGIN_FILLED);
  float peak=float((std::max)(1u,info.histPeak[c]));
  for(int i=0;i<256;i++){
   float value=(std::min)(1.f,info.hist[c][i]/peak);
   sink->AddLine(D2D1::Point2F(i/255.f,1.f-value));
  }
  sink->AddLine(D2D1::Point2F(1,1));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  if(FAILED(sink->Close()))return;
  histPath[c]=geometry;
 }
}
void PaintHistogram(D2D1_RECT_F card,const Palette& p,float alpha,float rise){
 auto target=Dc();
 Ink()->SetColor(Fade(p.plate,alpha));
 target->FillRoundedRectangle(D2D1::RoundedRect(card,12,12),Ink());
 if(!histPath[0]){
  Write(T(S_Reading),card,F_Small,Fade(p.faint,alpha));
  return;
 }
 D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
 target->PushAxisAlignedClip(card,D2D1_ANTIALIAS_MODE_ALIASED);
 auto plot=Inset(card,8);
 auto place=D2D1::Matrix3x2F::Scale(Width(plot),Height(plot)*rise)*
  D2D1::Matrix3x2F::Translation(plot.left,plot.bottom-Height(plot)*rise);
 target->SetTransform(place*Mat(previous));
 const D2D1_COLOR_F tints[4]={{1.f,.30f,.30f,.42f},{.28f,.92f,.42f,.42f},{.36f,.55f,1.f,.48f},{0,0,0,0}};
 auto drawChannel=[&](int c,D2D1_COLOR_F colour){
  if(!histPath[c])return;
  Ink()->SetColor(Fade(colour,alpha));
  target->FillGeometry(histPath[c].Get(),Ink());
 };
 if(histMode==0){for(int c=0;c<3;c++)drawChannel(c,tints[c]);}
 else if(histMode==1)drawChannel(3,Fade(p.text,.42f));
 else drawChannel(histMode-2,tints[histMode-2]);
 target->SetTransform(previous);
 target->PopAxisAlignedClip();
}
// The chroma cloud, drawn once per photograph into a small bitmap: counts are
// logarithmic, since the neutral centre otherwise buries every colour in the frame.
void BuildScope(){
 if(!info.histReady||scopeBitmap)return;
 constexpr int N=Meta::ScopeEdge;
 std::vector<uint8_t> pixels(size_t(N)*N*4);
 float peak=logf(1.f+float((std::max)(1u,info.scopePeak)));
 for(int y=0;y<N;y++)for(int x=0;x<N;x++){
  uint32_t count=info.scope[y*N+x];
  float t=count?logf(1.f+float(count))/peak:0.f;
  float u=float(x-N/2)*256.f/N,v=float(N/2-y)*256.f/N;
  float r=190+1.402f*v,g=190-.344f*u-.714f*v,b=190+1.772f*u;
  uint8_t* out=&pixels[(size_t(y)*N+x)*4];
  out[0]=uint8_t(Clamp(b,0,255)*t);out[1]=uint8_t(Clamp(g,0,255)*t);
  out[2]=uint8_t(Clamp(r,0,255)*t);out[3]=uint8_t(255*t);
 }
 Dc()->CreateBitmap(D2D1::SizeU(N,N),pixels.data(),N*4,
  D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&scopeBitmap);
}
void PaintVectorscope(D2D1_RECT_F card,const Palette& p,float alpha){
 auto target=Dc();
 Ink()->SetColor(Fade(p.plate,alpha));
 target->FillRoundedRectangle(D2D1::RoundedRect(card,12,12),Ink());
 BuildScope();
 if(!scopeBitmap){Write(T(S_Reading),card,F_Small,Fade(p.faint,alpha));return;}
 float radius=((std::min)(Width(card),Height(card))-30)/2;
 D2D1_POINT_2F centre{(card.left+card.right)/2,(card.top+card.bottom)/2};
 // The rim carries the hue wheel itself, so a cluster's direction reads as a
 // colour rather than as an angle the eye has to decode.
 for(int i=0;i<108;i++){
  float angle=float(i)*6.2831853f/108,u=cosf(angle)*127,v=sinf(angle)*127;
  Ink()->SetColor(Fade(D2D1::ColorF(Clamp(190+1.402f*v,0,255)/255,Clamp(190-.344f*u-.714f*v,0,255)/255,
   Clamp(190+1.772f*u,0,255)/255,1),alpha*.85f));
  target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centre.x+cosf(angle)*radius,centre.y-sinf(angle)*radius),2.2f,2.2f),Ink());
 }
 Ink()->SetColor(Fade(p.faint,alpha*.45f));
 for(int i=0;i<3;i++){
  float angle=float(i)*3.14159265f/3;
  target->DrawLine(D2D1::Point2F(centre.x-cosf(angle)*radius,centre.y+sinf(angle)*radius),
   D2D1::Point2F(centre.x+cosf(angle)*radius,centre.y-sinf(angle)*radius),Ink(),1.f,DashStyle());
 }
 // The skin tone line: healthy skin of any complexion lands along this
 // direction, so a portrait's cast shows as a drift away from it.
 {
  float u=-26.9f,v=37.5f,length=sqrtf(u*u+v*v);
  Ink()->SetColor(Fade(p.text,alpha*.5f));
  target->DrawLine(centre,D2D1::Point2F(centre.x+u/length*radius,centre.y-v/length*radius),Ink(),1.4f,DashStyle());
 }
 ComPtr<ID2D1BitmapBrush> brush;
 D2D1_BITMAP_BRUSH_PROPERTIES props=D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,
  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
 if(SUCCEEDED(target->CreateBitmapBrush(scopeBitmap.Get(),props,&brush))){
  float scale=radius*2/float(Meta::ScopeEdge);
  brush->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*
   D2D1::Matrix3x2F::Translation(centre.x-radius,centre.y-radius));
  brush->SetOpacity(alpha);
  target->FillEllipse(D2D1::Ellipse(centre,radius,radius),brush.Get());
 }
}
void PaintInfoPanel(D2D1_RECT_F body,const Palette& p,float alpha){
 auto target=Dc();
 Write(T(S_Information),D2D1::RectF(body.left+18,body.top+14,body.right-18,body.top+40),F_Title,Fade(p.text,alpha));
 Write(Name(),D2D1::RectF(body.left+18,body.top+38,body.right-18,body.top+58),F_Meta,Fade(p.dim,alpha));
 // Scrollable fields are clipped below the fixed header, same as the other
 // panels, so a scrolled row can never slide up underneath the title.
 target->PushAxisAlignedClip(D2D1::RectF(body.left,body.top+PanelHead-10,body.right,body.bottom),D2D1_ANTIALIAS_MODE_ALIASED);
 float y=body.top+PanelHead-6-panelScroll;
 PaintFields(info.file,body,y,S_SecFile,p,alpha);
 if(!currentPath.empty()){
  D2D1_RECT_F pathRow=D2D1::RectF(body.left+12,y,body.right-12,y+RowH);
  Hotspot(IdCopyPath,pathRow);
  PanelRow(IdCopyPath,IcCopy,Elide(currentPath,F_Row,Width(pathRow)-76),p,alpha,false,p.text);
  y+=RowH+12;
 }
 if(info.hasCamera)PaintFields(info.camera,body,y,S_SecCamera,p,alpha);
 // Sections that carry no rows are not drawn at all: a screenshot should not
 // show an empty CAMERA heading, and a calibrated RAW should not have to
 // compete with five blank ones.
 if(!info.colour.empty())PaintFields(info.colour,body,y,S_SecColour,p,alpha);
 if(!info.adobe.empty())PaintFields(info.adobe,body,y,S_SecAdobe,p,alpha);
 if(!info.lens.empty())PaintFields(info.lens,body,y,S_SecLens,p,alpha);
 if(!info.author.empty())PaintFields(info.author,body,y,S_SecAuthor,p,alpha);
 if(info.histReady||metaPending){
  // Two readings of the same frame, one at a time: tone, or colour.
  float pick=(Width(body)-32-6)/2;
  D2D1_RECT_F tone=D2D1::RectF(body.left+16,y,body.left+16+pick,y+28);
  D2D1_RECT_F colour=D2D1::RectF(tone.right+6,y,tone.right+6+pick,y+28);
  Hotspot(IdScopeHist,tone);Chip(IdScopeHist,T(S_SecHistogram),scopeMode==0,p,alpha);
  Hotspot(IdScopeVector,colour);Chip(IdScopeVector,T(S_Vectorscope),scopeMode==1,p,alpha);
  y+=38;
  if(scopeMode==1){
   PaintVectorscope(D2D1::RectF(body.left+16,y,body.right-16,y+Width(body)-32),p,alpha);
   y+=Width(body)-32+12;
  }else{
  BuildHistogram();
  D2D1_RECT_F histCard=D2D1::RectF(body.left+16,y,body.right-16,y+108);
  PaintHistogram(histCard,p,alpha,histRise.v);
  // Hovering the plot reads the tonal level under the cursor back as exact
  // per-channel pixel counts, the way a RAW editor's histogram would.
  float hoverX=mouse.x/dpi,hoverY=mouse.y/dpi;
  if(histPath[0]&&panel==PanelInfo&&Inside(histCard,hoverX,hoverY)){
   auto plot=Inset(histCard,8);
   int level=int(Clamp((hoverX-plot.left)/(std::max)(1.f,Width(plot)),0,1)*255.f+.5f);
   Ink()->SetColor(Fade(D2D1::ColorF(1,1,1,.4f),alpha));
   target->DrawLine(D2D1::Point2F(hoverX,plot.top),D2D1::Point2F(hoverX,plot.bottom),Ink(),1.f);
   wchar_t buf[110];
   swprintf_s(buf,L"%d   R %u   G %u   B %u",level,info.hist[0][level],info.hist[1][level],info.hist[2][level]);
   float tw=(std::min)(Width(body)-24,Measure(buf,F_Small,400)+18);
   float tx=Clamp(hoverX-tw/2,histCard.left,histCard.right-tw);
   // Anchored to the top of the card itself, never above it, so it can never
   // collide with the section heading sitting just outside the histogram.
   D2D1_RECT_F tip=D2D1::RectF(tx,histCard.top+6,tx+tw,histCard.top+26);
   Ink()->SetColor(Fade(p.plate,(std::min)(1.f,alpha*1.8f)));
   target->FillRoundedRectangle(D2D1::RoundedRect(tip,7,7),Ink());
   Write(buf,tip,F_Small,Fade(p.text,alpha));
  }
  y+=116;
  const int modes[5]={IdHistRGB,IdHistLuma,IdHistR,IdHistG,IdHistB};
  const wchar_t* labels[5]={L"RGB",L"Luma",L"R",L"G",L"B"};
  float chip=(Width(body)-32-4*5)/5;
  for(int i=0;i<5;i++){
   D2D1_RECT_F r=D2D1::RectF(body.left+16+i*(chip+5),y,body.left+16+i*(chip+5)+chip,y+28);
   Hotspot(modes[i],r);
   Chip(modes[i],labels[i],histMode==i,p,alpha);
  }
  y+=40;
  }
 }
 if(info.histReady){
  Write(T(S_SecExposure),D2D1::RectF(body.left+18,y,body.right-18,y+20),F_Section,Fade(p.faint,alpha));
  y+=26;
  D2D1_RECT_F high=D2D1::RectF(body.left+12,y,body.right-12,y+RowH);
  Hotspot(IdClipHigh,high);ToggleRow(IdClipHigh,T(S_HighlightClip),info.clippedHigh,clipHigh,clipHighFade.v,p,alpha);
  y+=RowH+RowGap;
  D2D1_RECT_F low=D2D1::RectF(body.left+12,y,body.right-12,y+RowH);
  Hotspot(IdClipLow,low);ToggleRow(IdClipLow,T(S_ShadowClip),info.clippedLow,clipLow,clipLowFade.v,p,alpha);
  y+=RowH+12;
 }
 if(info.hasLocation){
  PaintFields(info.location,body,y,S_SecLocation,p,alpha);
  if(info.hasGps){
   D2D1_RECT_F mapRow=D2D1::RectF(body.left+12,y,body.right-12,y+RowH);
   Hotspot(IdOpenMap,mapRow);
   PanelRow(IdOpenMap,IcGlobe,T(S_OpenMap),p,alpha,true,p.text);
   y+=RowH+12;
  }
 }
 panelExtent=(std::max)(0.f,(y+panelScroll)-body.bottom+20);
 target->PopAxisAlignedClip();
}
// A labelled card with a two-position selector: the theme and the mouse wheel
// are the same control, so they share the drawing and the sliding knob.
void SegmentedRow(int rowId,int leftId,int rightId,const wchar_t* icon,const wchar_t* label,
 const wchar_t* leftText,const wchar_t* rightText,float k,const Palette& p,float alpha){
 auto row=R(rowId);
 auto card=D2D1::RoundedRect(row,RowRadius,RowRadius);
 Ink()->SetColor(Fade(p.card,alpha));
 Dc()->FillRoundedRectangle(card,Ink());
 Ink()->SetColor(Fade(p.cardEdge,alpha*.8f));
 Dc()->DrawRoundedRectangle(card,Ink(),1.f);
 auto leftChip=R(leftId),rightChip=R(rightId);
 float mid=(row.top+row.bottom)/2;
 Icon(icon,D2D1::RectF(row.left+16,mid-13,row.left+42,mid+13),Fade(p.text,alpha),1.75f);
 Write(label,D2D1::RectF(row.left+56,row.top,leftChip.left-8,row.bottom),F_Row,Fade(p.text,alpha));
 D2D1_RECT_F group=D2D1::RectF(leftChip.left-3,leftChip.top-3,rightChip.right+3,rightChip.bottom+3);
 Ink()->SetColor(Fade(p.sunk,alpha));
 Dc()->FillRoundedRectangle(D2D1::RoundedRect(group,11,11),Ink());
 // The selection slides between the two halves rather than snapping.
 D2D1_RECT_F knob=D2D1::RectF(leftChip.left+(rightChip.left-leftChip.left)*k,leftChip.top,
  leftChip.right+(rightChip.right-leftChip.right)*k,leftChip.bottom);
 auto knobShape=D2D1::RoundedRect(knob,9,9);
 SoftShadow(knobShape,alpha*.35f,1.f);
 Ink()->SetColor(Fade(Mix(p.card,p.text,.16f),alpha));
 Dc()->FillRoundedRectangle(knobShape,Ink());
 Ink()->SetColor(Fade(p.cardEdge,alpha*.9f));
 Dc()->DrawRoundedRectangle(knobShape,Ink(),1.f);
 Write(leftText,leftChip,F_Button,Fade(p.text,alpha*(k<.5f?1.f:.45f)));
 Write(rightText,rightChip,F_Button,Fade(p.text,alpha*(k>.5f?1.f:.45f)));
}
void PaintMenuPanel(D2D1_RECT_F body,const Palette& p,float alpha){
 Write(T(S_Menu),D2D1::RectF(body.left+18,body.top+14,body.right-18,body.top+40),F_Title,Fade(p.text,alpha));
 Write(T(S_MenuSub),D2D1::RectF(body.left+18,body.top+38,body.right-18,body.top+58),F_Meta,Fade(p.dim,alpha));
 float progress=menuLevel?levelSlide.v:1.f-levelSlide.v;
 D2D1_MATRIX_3X2_F outer;Dc()->GetTransform(&outer);
 Dc()->PushAxisAlignedClip(D2D1::RectF(body.left,body.top+PanelHead-10,body.right,body.bottom),D2D1_ANTIALIAS_MODE_ALIASED);
 Dc()->SetTransform(D2D1::Matrix3x2F::Translation((menuLevel?15.f:-15.f)*(1.f-progress),0)*Mat(outer));
 alpha*=progress;
 float enabled=(current&&!loading&&!actionBusy)?1.f:.35f;
 if(menuLevel==0){
  PanelRow(IdSend,IcShare,T(S_Send),p,alpha*enabled,true,p.text);
  PanelRow(IdSave,IcSave,T(S_Save),p,alpha*enabled,true,p.text);
  PanelRow(IdSaveAs,IcSaveAs,T(S_SaveAs),p,alpha*enabled,true,p.text);
  PanelRow(IdPrint,IcPrint,T(S_Print),p,alpha*enabled,true,p.text);
  PanelRow(IdDelete,IcTrash,T(S_Delete),p,alpha*enabled,!confirmDelete,p.danger);
  if(confirmDelete){
   Write(T(S_RecycleAsk),D2D1::RectF(body.left+16,R(IdConfirmCancel).top-22,body.right-16,R(IdConfirmCancel).top-2),F_Small,Fade(p.dim,alpha));
   Chip(IdConfirmCancel,T(S_Cancel),false,p,alpha);
   auto r=R(IdConfirmDelete);
   D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
   Dc()->SetTransform(Lift(r,IdConfirmDelete)*Mat(previous));
   Ink()->SetColor(Fade(p.danger,alpha*.92f));
   Dc()->FillRoundedRectangle(D2D1::RoundedRect(r,10,10),Ink());
   PlateFor(IdConfirmDelete,r,p,alpha,10);
   Write(T(S_Delete),r,F_Button,Fade(D2D1::ColorF(1,1,1,1),alpha));
   Dc()->SetTransform(previous);
  }
  SegmentedRow(IdThemeRow,IdThemeDark,IdThemeLight,themeMix.v>.5f?IcSun:IcMoon,T(S_ThemeLabel),T(S_Dark),T(S_Light),themeMix.v,p,alpha);
  SegmentedRow(IdWheelRow,IdWheelZoom,IdWheelNav,IcMouse,T(S_WheelLabel),T(S_WheelZoom),T(S_WheelNav),wheelMix.v,p,alpha);
  PanelRow(IdDefaultApp,IcDefault,T(S_DefaultApp),p,alpha,true,p.text);
  PanelRow(IdSpacePreview,IcSpace,T(S_SpacePreview),p,alpha,false,p.text,T(autostart?S_On:S_Off));
  if(tcStatus!=TcMissing)
   PanelRow(IdTotalCmd,IcDefault,L"Total Commander",p,alpha,false,p.text,T(tcStatus==TcInstalled?S_Remove:S_Install));
  PanelRow(IdLanguage,IcGlobe,T(S_Language),p,alpha,true,p.text);
  auto hairline=[&](float top,float bottom){
   float y=(top+bottom)/2;
   Ink()->SetColor(Fade(p.sep,alpha));
   Dc()->DrawLine(D2D1::Point2F(body.left+24,y),D2D1::Point2F(body.right-24,y),Ink(),1.f);
  };
  hairline(confirmDelete?R(IdConfirmDelete).bottom:R(IdDelete).bottom,R(IdDefaultApp).top);
  hairline(R(IdSpacePreview).bottom,R(IdThemeRow).top);
 }else{
  PanelRow(IdLangBack,IcChevronL,T(S_Language),p,alpha,false,p.dim);
  PanelRow(IdLangRu,nullptr,L"Русский",p,alpha,false,language==0?p.accent:p.text);
  PanelRow(IdLangEn,nullptr,L"English",p,alpha,false,language==1?p.accent:p.text);
 }
 Dc()->SetTransform(outer);
 Dc()->PopAxisAlignedClip();
}
void PaintEditPanel(D2D1_RECT_F body,const Palette& p,float alpha){
 Write(T(S_Edit),D2D1::RectF(body.left+18,body.top+14,body.right-18,body.top+40),F_Title,Fade(p.text,alpha));
 Write(T(S_EditSub),D2D1::RectF(body.left+18,body.top+38,body.right-18,body.top+58),F_Meta,Fade(p.dim,alpha));
 Dc()->PushAxisAlignedClip(D2D1::RectF(body.left,body.top+PanelHead-10,body.right,body.bottom),D2D1_ANTIALIAS_MODE_ALIASED);
 float enabled=current?1.f:.35f;
 PanelRow(IdToolCrop,IcCrop,T(S_Crop),p,alpha*enabled,true,tool==ToolCrop?p.accent:p.text);
 PanelRow(IdToolRotate,IcRotate,T(S_Rotate),p,alpha*enabled,true,tool==ToolRotate?p.accent:p.text);
 PanelRow(IdToolDraw,IcPen,T(S_Draw),p,alpha*enabled,true,tool==ToolDraw?p.accent:p.text);
 PanelRow(IdToolArrow,IcArrow,T(S_Arrow),p,alpha*enabled,true,tool==ToolArrow?p.accent:p.text);
 PanelRow(IdToolSelect,IcMarquee,T(S_Select),p,alpha*enabled,true,tool==ToolSelect?p.accent:p.text);
 if(tool==ToolCrop){
  const int ids[5]={IdCropFree,IdCrop11,IdCrop43,IdCrop32,IdCrop169};
  const wchar_t* labels[5]={T(S_Free),L"1:1",L"4:3",L"3:2",L"16:9"};
  for(int i=0;i<5;i++)Chip(ids[i],labels[i],cropAspect==i,p,alpha);
  Chip(IdCropCancel,T(S_Cancel),false,p,alpha);
  Chip(IdCropApply,T(S_Apply),true,p,alpha);
 }
 if(tool==ToolRotate){
  Chip(IdRotLeft,T(S_RotateLeft),false,p,alpha);
  Chip(IdRotRight,T(S_RotateRight),false,p,alpha);
 }
 if(tool==ToolDraw||tool==ToolArrow||tool==ToolSelect){
  if(tool==ToolSelect){Chip(IdShapeRect,language?L"Rectangle":L"Прямоугольник",shapeKind==2,p,alpha);Chip(IdShapeEllipse,language?L"Ellipse":L"Овал",shapeKind==3,p,alpha);}
  for(int i=0;i<6;i++){
   auto r=R(IdSwatch0+i);
   D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
   Dc()->SetTransform(Lift(r,IdSwatch0+i)*Mat(previous));
   Ink()->SetColor(Fade(swatches[i],alpha));
   Dc()->FillRoundedRectangle(D2D1::RoundedRect(r,9,9),Ink());
   if(swatch==i){
    Ink()->SetColor(Fade(p.text,alpha));
    Dc()->DrawRoundedRectangle(D2D1::RoundedRect(Grow(r,2.5f,2.5f),11,11),Ink(),1.6f);
   }
   Dc()->SetTransform(previous);
  }
  Chip(IdThin,L"2",thickness<3,p,alpha);
  Chip(IdMedium,L"4",thickness>=3&&thickness<7,p,alpha);
  Chip(IdThick,L"9",thickness>=7,p,alpha);
  PanelRow(IdUndo,IcBack,T(S_Undo),p,alpha*(strokes.empty()?.35f:1.f),false,p.text);
 }
 Dc()->PopAxisAlignedClip();
}
void PaintPanel(const Palette& p){
 if(panel==PanelNone&&panelSlide.v<=.004f)return;
 auto body=R(IdPanelBody);
 if(Width(body)<=0)return;
 float alpha=panelSlide.v;
 float scale=.985f+.015f*alpha;
 float cx=(body.left+body.right)/2,cy=body.top+40;
 auto world=D2D1::Matrix3x2F::Translation(-cx,-cy)*D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(cx,cy);
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 Dc()->SetTransform(world*Mat(previous));
 Glass(D2D1::RoundedRect(body,22,22),p,alpha,world*Mat(previous));
 if(panel==PanelInfo)PaintInfoPanel(body,p,alpha);
 else if(panel==PanelMenu)PaintMenuPanel(body,p,alpha);
 else if(panel==PanelEdit)PaintEditPanel(body,p,alpha);
 Dc()->SetTransform(previous);
}
void PaintGallery(const Palette& p){
 if(siblings.size()<2)return;
 auto bar=R(IdGalleryBody);
 if(Width(bar)<=0)return;
 auto target=Dc();
 Glass(D2D1::RoundedRect(bar,20,20),p,1.f,D2D1::Matrix3x2F::Identity());
 target->PushAxisAlignedClip(Inset(bar,4),D2D1_ANTIALIAS_MODE_ALIASED);
 int active=CurrentIndex();
 for(size_t i=0;i<siblings.size();i++){
  auto item=R(int(IdGallery0+i));
  if(Width(item)<=0.5f||item.right<bar.left-40||item.left>bar.right+40)continue;
  ThumbRequest(siblings[i]);
  auto id=int(IdGallery0+i);
  float scale=1.f+.05f*Hover(id)-.07f*Press(id);
  float cx=(item.left+item.right)/2,cy=(item.top+item.bottom)/2;
  auto world=D2D1::Matrix3x2F::Translation(-cx,-cy)*D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(cx,cy);
  D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
  target->SetTransform(world*Mat(previous));
  auto rounded=D2D1::RoundedRect(item,9,9);
  auto found=thumbBitmaps.find(siblings[i]);
  if(found==thumbBitmaps.end()){
   if(auto thumb=ThumbLookup(siblings[i])){
    ComPtr<ID2D1Bitmap> made;
    if(SUCCEEDED(Dc()->CreateBitmap(D2D1::SizeU(thumb->w,thumb->h),thumb->pixels.data(),thumb->w*4,
      D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&made)))
     found=thumbBitmaps.emplace(siblings[i],made).first;
   }
  }
  if(found!=thumbBitmaps.end()&&found->second){
   auto size=found->second->GetSize();
   float coverage=(std::max)(Width(item)/size.width,Height(item)/size.height);
   float sw=Width(item)/coverage,sh=Height(item)/coverage;
   D2D1_RECT_F source=D2D1::RectF((size.width-sw)/2,(size.height-sh)/2,(size.width+sw)/2,(size.height+sh)/2);
   ComPtr<ID2D1BitmapBrush> brush;
   D2D1_BITMAP_BRUSH_PROPERTIES props=D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,
    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
   if(SUCCEEDED(target->CreateBitmapBrush(found->second.Get(),props,&brush))){
    brush->SetTransform(D2D1::Matrix3x2F::Translation(-source.left,-source.top)*
     D2D1::Matrix3x2F::Scale(coverage,coverage)*D2D1::Matrix3x2F::Translation(item.left,item.top));
    target->FillRoundedRectangle(rounded,brush.Get());
   }
  }else{
   Ink()->SetColor(Fade(p.sunk,.8f));
   target->FillRoundedRectangle(rounded,Ink());
  }
  if(int(i)==active){
   Ink()->SetColor(D2D1::ColorF(1,1,1,.82f));
   target->DrawRoundedRectangle(rounded,Ink(),1.6f);
  }else{
   Ink()->SetColor(D2D1::ColorF(0,0,0,.28f*(1.f-Hover(id))));
   target->FillRoundedRectangle(rounded,Ink());
   Ink()->SetColor(Fade(p.glassEdge,.5f));
   target->DrawRoundedRectangle(rounded,Ink(),.8f);
  }
  target->SetTransform(previous);
 }
 // Requests above are inserted in paint order into a LIFO queue. Move the
 // selected photograph to the very back after all of them so it is always the
 // next item a worker claims.
 if(active>=0&&active<int(siblings.size()))ThumbPrioritize(siblings[size_t(active)]);
 target->PopAxisAlignedClip();
}
void PaintToast(float w,float h,const Palette& p){
 float alpha=toastIn.v;
 if(alpha<=.01f||toast.empty())return;
 float width=(std::min)(w-80,Measure(toast,F_Button,w)+44);
 float bottom=h-Margin-(siblings.size()>1?GalleryH+14:14);
 D2D1_RECT_F pill=D2D1::RectF((w-width)/2,bottom-40,(w+width)/2,bottom-4);
 pill=Shift(pill,0,(1.f-alpha)*10.f);
 Glass(D2D1::RoundedRect(pill,18,18),p,alpha,D2D1::Matrix3x2F::Identity());
 Write(toast,pill,F_Button,Fade(p.text,alpha));
}
void PaintEmpty(float w,float h,const Palette& p){
 Write(L"Vetro Look",D2D1::RectF(w/2-220,h/2-70,w/2+220,h/2-28),F_Big,p.text);
 Write(T(S_Tagline),D2D1::RectF(w/2-260,h/2-18,w/2+260,h/2+8),F_Row,p.dim);
 Write(T(S_Hint1),D2D1::RectF(w/2-300,h/2+24,w/2+300,h/2+48),F_Meta,p.dim);
 Write(T(S_Hint2),D2D1::RectF(w/2-300,h/2+52,w/2+300,h/2+74),F_Meta,p.faint);
}

// ============================================================ library UI ==
const wchar_t* LibraryExtensions[]={L"jpg",L"png",L"webp",L"avif",L"heic",L"bmp",L"tif",L"gif",
 L"psd",L"cr2",L"cr3",L"nef",L"arw",L"dng",L"raf",L"rw2",L"orf",L"pef",L"exr",L"ico"};
constexpr uint64_t GB=1024ull*1024*1024;
float SizeToT(uint64_t bytes){if(!bytes)return 0;float t=logf(float(double(bytes)/1024.0))/logf(float(100.0*GB/1024.0));return Clamp(t,0,1);}
uint64_t TToSize(float t){if(t<=.004f)return 0;if(t>=.996f)return ~0ull;return uint64_t(1024.0*pow(100.0*GB/1024.0,double(t)));}

ComPtr<ID2D1Bitmap> GridBitmap(const std::wstring& path){
 if(thumbBitmaps.size()>800)thumbBitmaps.clear(); // crude cap; cheap to repopulate from thumbs.cpp's own cache
 ThumbRequest(path);
 auto found=thumbBitmaps.find(path);
 if(found!=thumbBitmaps.end())return found->second;
 if(auto thumb=ThumbLookup(path)){
  ComPtr<ID2D1Bitmap> made;
  if(SUCCEEDED(Dc()->CreateBitmap(D2D1::SizeU(thumb->w,thumb->h),thumb->pixels.data(),thumb->w*4,
    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&made)))
   thumbBitmaps.emplace(path,made);
  return made;
 }
 return nullptr;
}
void DrawCover(D2D1_RECT_F cell,float radius,ID2D1Bitmap* image,D2D1_COLOR_F fallback){
 auto rounded=D2D1::RoundedRect(cell,radius,radius);
 if(!image){Ink()->SetColor(fallback);Dc()->FillRoundedRectangle(rounded,Ink());return;}
 auto size=image->GetSize();
 float coverage=(std::max)(Width(cell)/size.width,Height(cell)/size.height);
 float sw=Width(cell)/coverage,sh=Height(cell)/coverage;
 D2D1_RECT_F source=D2D1::RectF((size.width-sw)/2,(size.height-sh)/2,(size.width+sw)/2,(size.height+sh)/2);
 ComPtr<ID2D1BitmapBrush> brush;
 auto props=D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
 if(SUCCEEDED(Dc()->CreateBitmapBrush(image,props,&brush))){
  brush->SetTransform(D2D1::Matrix3x2F::Translation(-source.left,-source.top)*
   D2D1::Matrix3x2F::Scale(coverage,coverage)*D2D1::Matrix3x2F::Translation(cell.left,cell.top));
  Dc()->FillRoundedRectangle(rounded,brush.Get());
 }
}
// Top bar shared by Library and Album: branding-or-back on the left, a
// search field taking the middle, and a floating icon dock plus the
// folders/photos toggle on the right.
constexpr float AlbumTitleW=200;
void LayoutLibraryChrome(float w,bool isAlbum){
 float y0=Margin,y1=Margin+Bubble;
 float brandWidth=isAlbum?Bubble+AlbumTitleW+20:176.f;
 rects[IdLibBrand]=D2D1::RectF(Margin,y0,Margin+brandWidth,y1);
 if(isAlbum)Hotspot(IdLibBack,D2D1::RectF(Margin+4,y0+4,Margin+Bubble-4,y1-4));
 auto winBar=LayoutWindowButtons(w);
 float dockWidth=3*Cell+12,dockRight=winBar.left-10;
 D2D1_RECT_F dock=D2D1::RectF(dockRight-dockWidth,y0,dockRight,y1);
 rects[IdDockBar]=dock;
 Hotspot(IdLibRescan,D2D1::RectF(dock.left+6,y0+2,dock.left+6+Cell,y1-2));
 Hotspot(IdLibSort,D2D1::RectF(dock.left+6+Cell,y0+2,dock.left+6+2*Cell,y1-2));
 Hotspot(IdLibFilter,D2D1::RectF(dock.left+6+2*Cell,y0+2,dock.left+6+3*Cell,y1-2));
 float toggleWidth=2*Cell+10,toggleRight=dock.left-10;
 D2D1_RECT_F toggle=D2D1::RectF(toggleRight-toggleWidth,y0,toggleRight,y1);
 rects[IdThemeRow]=toggle; // reused purely as a rect slot, no relation to the theme row
 Hotspot(IdLibViewFolders,D2D1::RectF(toggle.left+5,y0+5,toggle.left+5+Cell,y1-5));
 Hotspot(IdLibViewPhotos,D2D1::RectF(toggle.right-5-Cell,y0+5,toggle.right-5,y1-5));
 float searchLeft=R(IdLibBrand).right+12,searchRight=toggle.left-14;
 D2D1_RECT_F search=D2D1::RectF(searchLeft,y0+2,(std::max)(searchLeft+120,searchRight),y1-2);
 Hotspot(IdLibSearch,search);
}
void PaintLibraryChrome(float w,const Palette& p,bool isAlbum,const std::wstring& title){
 (void)w;
 auto brand=R(IdLibBrand);
 Glass(D2D1::RoundedRect(brand,Bubble/2,Bubble/2),p,1.f,D2D1::Matrix3x2F::Identity());
 if(isAlbum){
  auto r=R(IdLibBack);
  D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
  auto lift=Lift(r,IdLibBack);Dc()->SetTransform(lift*Mat(previous));
  if(Hover(IdLibBack)>.01f||Press(IdLibBack)>.01f){Ink()->SetColor(Fade(p.hover,Hover(IdLibBack)));Dc()->FillRoundedRectangle(D2D1::RoundedRect(r,Height(r)/2,Height(r)/2),Ink());}
  Icon(IcBack,Shift(Inset(r,12),-1.5f*Hover(IdLibBack),0),p.text,1.9f);
  Dc()->SetTransform(previous);
  Write(Elide(title,F_Row,AlbumTitleW),D2D1::RectF(r.right+8,brand.top,brand.right-14,brand.bottom),F_Row,p.text);
 }else{
  Icon(IcGridPhoto,D2D1::RectF(Margin+2,Margin+8,Margin+30,Margin+36),p.accent,1.7f,true);
  Write(L"Vetro Look",D2D1::RectF(Margin+38,Margin+2,Margin+180,Margin+22),F_Row,p.text);
  Write(T(S_AppTagline),D2D1::RectF(Margin+38,Margin+21,Margin+180,Margin+38),F_Small,p.faint);
 }
 auto search=R(IdLibSearch);
 bool active=libSearchFocused||!libSearch.empty();
 Glass(D2D1::RoundedRect(search,Bubble/2,Bubble/2),p,1,D2D1::Matrix3x2F::Identity());
 Ink()->SetColor(Fade(libSearchFocused?p.accent:p.cardEdge,libSearchFocused?.9f:.7f));
 Dc()->DrawRoundedRectangle(D2D1::RoundedRect(search,Bubble/2,Bubble/2),Ink(),libSearchFocused?1.6f:1.f);
 Icon(IcSearch,D2D1::RectF(search.left+13,search.top+11,search.left+35,search.bottom-11),p.faint,1.6f);
 std::wstring shown=libSearch.empty()?((!isAlbum&&libView==ViewFolders)?T(S_SearchFolders):T(S_SearchPhotos)):libSearch;
 Write(shown,D2D1::RectF(search.left+42,search.top,search.right-16,search.bottom),F_Row,libSearch.empty()?p.faint:p.text);
 if(active&&fmod(Now(),1.0)<0.55&&libSearchFocused){
  float tx=search.left+42+Measure(libSearch,F_Row,Width(search))+2;
  Ink()->SetColor(Fade(p.text,.85f));
  Dc()->FillRectangle(D2D1::RectF(tx,search.top+13,tx+1.4f,search.bottom-13),Ink());
 }
 auto dock=R(IdDockBar);
 Glass(D2D1::RoundedRect(dock,Bubble/2,Bubble/2),p,1.f,D2D1::Matrix3x2F::Identity());
 IconButton(IdLibRescan,IcRotate,p,1.f,IndexIsScanning()?p.accent:p.text,IndexIsScanning()?scanSpin:0.f);
 IconButton(IdLibSort,IcSortLines,p,1.f,sortReveal.target>.5f?p.accent:p.text);
 IconButton(IdLibFilter,IcFilter,p,1.f,filtersActive||filterReveal.target>.5f?p.accent:p.text);
 auto toggle=R(IdThemeRow);
 Glass(D2D1::RoundedRect(toggle,Bubble/2,Bubble/2),p,1.f,D2D1::Matrix3x2F::Identity());
 auto folderBtn=R(IdLibViewFolders),photoBtn=R(IdLibViewPhotos);
 float switchT=Clamp(libViewSlide.v,0,1);
 auto lerp=[&](float a,float b){return a+(b-a)*switchT;};
 D2D1_RECT_F knob=D2D1::RectF(lerp(folderBtn.left,photoBtn.left),lerp(folderBtn.top,photoBtn.top),
  lerp(folderBtn.right,photoBtn.right),lerp(folderBtn.bottom,photoBtn.bottom));
 Ink()->SetColor(Mix(p.glassLift,p.accent,.16f));Dc()->FillRoundedRectangle(D2D1::RoundedRect(knob,Height(knob)/2,Height(knob)/2),Ink());
 Ink()->SetColor(Fade(p.glassEdge,.9f));Dc()->DrawRoundedRectangle(D2D1::RoundedRect(knob,Height(knob)/2,Height(knob)/2),Ink(),1.f);
 IconButton(IdLibViewFolders,IcFolderIc,p,1.f,Mix(p.accent,p.faint,switchT));
 IconButton(IdLibViewPhotos,IcGridPhoto,p,1.f,Mix(p.faint,p.accent,switchT),0,true);
 PaintWindowButtons(p);
}
void PaintSortPopup(float w,const Palette& p,float alpha){
 alpha=Clamp(alpha,0,1);
 float panelW=220,x=R(IdLibSort).left+Cell/2-panelW/2,y=Margin+Bubble+10;
 x=Clamp(x,Margin,w-Margin-panelW);
 D2D1_RECT_F body=D2D1::RectF(x,y,x+panelW,y+3*RowH+3*RowGap+16);
 rects[IdSortPopup]=body;if(sortOpen&&alpha>.82f)hots.push_back({IdSortPopup,body});
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 float scale=.96f+.04f*alpha,cx=(body.left+body.right)/2;
 auto reveal=D2D1::Matrix3x2F::Translation(-cx,-body.top)*D2D1::Matrix3x2F::Scale(scale,scale)*
  D2D1::Matrix3x2F::Translation(cx,body.top-(1-alpha)*10.f);
 Dc()->SetTransform(reveal*Mat(previous));
 Glass(D2D1::RoundedRect(body,18,18),p,alpha,reveal*Mat(previous));
 const int ids[3]={IdSortName,IdSortDate,IdSortSize};
 const Str labels[3]={S_SortName,S_SortDate,S_SortSize};
 float rowTop=body.top+8;
 for(int i=0;i<3;i++){
  D2D1_RECT_F r=D2D1::RectF(body.left+8,rowTop,body.right-8,rowTop+RowH);
  if(sortOpen&&alpha>.82f)Hotspot(ids[i],r);else rects[ids[i]]=r;
  bool active=libSortField==i;
  PanelRow(ids[i],nullptr,std::wstring(T(labels[i]))+(active?(libSortDesc?L"  ▾":L"  ▴"):L""),
   p,alpha,false,active?p.accent:p.text);
  rowTop+=RowH+RowGap;
 }
 Dc()->SetTransform(previous);
}
void PaintFilterPopup(float w,float h,const Palette& p,float alpha){
 alpha=Clamp(alpha,0,1);
 float panelW=SideWidth,x=w-Margin-panelW,y=Margin+Bubble+14;
 D2D1_RECT_F body=D2D1::RectF(x,y,x+panelW,h-Margin);
 rects[IdFilterPopup]=body;if(filterOpen&&alpha>.82f)hots.push_back({IdFilterPopup,body});
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 auto reveal=D2D1::Matrix3x2F::Translation((1-alpha)*26.f,0)*Mat(previous);
 Dc()->SetTransform(reveal);
 Glass(D2D1::RoundedRect(body,22,22),p,alpha,reveal);
 float cy=body.top+16;
 Write(T(S_FilterTitle),D2D1::RectF(body.left+18,cy,body.right-18,cy+24),F_Title,Fade(p.text,alpha));
 cy=body.top+58-filterScroll;
 Dc()->PushAxisAlignedClip(D2D1::RectF(body.left,body.top+50,body.right,body.bottom-10),D2D1_ANTIALIAS_MODE_ALIASED);
 auto filterHot=[&](int id,D2D1_RECT_F r){
  rects[id]=r;
  if(filterOpen&&alpha>.82f&&r.bottom>body.top+50&&r.top<body.bottom-10)hots.push_back({id,r});
 };
 Write(T(S_FilterFormats),D2D1::RectF(body.left+18,cy,body.right-18,cy+18),F_Section,Fade(p.faint,alpha));
 cy+=24;
 float chipX=body.left+16,chipY=cy,chipH=28;
 for(size_t i=0;i<std::size(LibraryExtensions);i++){
  std::wstring label=LibraryExtensions[i];for(auto& c:label)c=towupper(c);
  float cw=Measure(label,F_Button,200)+28;
  if(chipX+cw>body.right-16){chipX=body.left+16;chipY+=chipH+6;}
  int id=IdFilterExt0+int(i);
  D2D1_RECT_F chip=D2D1::RectF(chipX,chipY,chipX+cw,chipY+chipH);
  filterHot(id,chip);
  FilterChip(id,label,filterExtOn.empty()||filterExtOn.count(LibraryExtensions[i]),p,alpha);
  chipX+=cw+6;
 }
 cy=chipY+chipH+18;
 Write(T(S_FilterKind),D2D1::RectF(body.left+18,cy,body.right-18,cy+18),F_Section,Fade(p.faint,alpha));
 cy+=24;
 float half=(Width(body)-36-8)/2;
 D2D1_RECT_F rawChip=D2D1::RectF(body.left+18,cy,body.left+18+half,cy+32);
 D2D1_RECT_F regChip=D2D1::RectF(body.right-18-half,cy,body.right-18,cy+32);
 filterHot(IdFilterRawOnly,rawChip);filterHot(IdFilterRegularOnly,regChip);
 FilterChip(IdFilterRawOnly,T(S_FilterRaw),filterRawOnly,p,alpha);
 FilterChip(IdFilterRegularOnly,T(S_FilterRegular),filterRegularOnly,p,alpha);
 cy+=44;
 Write(T(S_FilterSize),D2D1::RectF(body.left+18,cy,body.right-18,cy+18),F_Section,Fade(p.faint,alpha));
 cy+=26;
 D2D1_RECT_F track=D2D1::RectF(body.left+20,cy+16,body.right-20,cy+20);
 filterHot(IdFilterSizeLo,D2D1::RectF(track.left-10,cy,track.left+10,cy+34));
 filterHot(IdFilterSizeHi,D2D1::RectF(track.right-10,cy,track.right+10,cy+34));
 Ink()->SetColor(Fade(p.track,alpha));Dc()->FillRoundedRectangle(D2D1::RoundedRect(track,2,2),Ink());
 float tLo=SizeToT(filterSizeLo),tHi=filterSizeHi==~0ull?1.f:SizeToT(filterSizeHi);
 D2D1_RECT_F fill=D2D1::RectF(track.left+Width(track)*tLo,track.top,track.left+Width(track)*tHi,track.bottom);
 Ink()->SetColor(Fade(p.dim,alpha));Dc()->FillRoundedRectangle(D2D1::RoundedRect(fill,2,2),Ink());
 for(float t:{tLo,tHi}){
  Ink()->SetColor(Fade(D2D1::ColorF(0,0,0,.22f),alpha));
  Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(track.left+Width(track)*t,(track.top+track.bottom)/2+1.5f),8,8),Ink());
  Ink()->SetColor(Fade(p.thumb,alpha));
  Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(track.left+Width(track)*t,(track.top+track.bottom)/2),8,8),Ink());
 }
 std::wstring loText=filterSizeLo==0?L"0 B":FormatBytes(filterSizeLo);
 std::wstring hiText=filterSizeHi==~0ull?L"100 GB+":FormatBytes(filterSizeHi);
 Write(loText,D2D1::RectF(body.left+18,cy+34,body.left+150,cy+52),F_Small,Fade(p.dim,alpha));
 Write(hiText,D2D1::RectF(body.right-150,cy+34,body.right-18,cy+52),F_Small,Fade(p.dim,alpha));
 cy+=64;
 Write(T(S_FilterProfile),D2D1::RectF(body.left+18,cy,body.right-18,cy+18),F_Section,Fade(p.faint,alpha));
 cy+=24;
 const int profileIds[5]={IdFilterProfileAny,IdFilterProfileSRGB,IdFilterProfileP3,IdFilterProfileAdobe,IdFilterProfileNone};
 const wchar_t* profileLabels[5]={T(S_FilterAny),L"sRGB",L"Display P3",L"Adobe RGB",T(S_FilterNoProfile)};
 float px=body.left+16,py=cy;
 for(int i=0;i<5;i++){
  float cw=Measure(profileLabels[i],F_Button,220)+28;
  if(px+cw>body.right-16){px=body.left+16;py+=36;}
  auto chip=D2D1::RectF(px,py,px+cw,py+30);
  filterHot(profileIds[i],chip);
  FilterChip(profileIds[i],profileLabels[i],filterProfile==i,p,alpha);
  px+=cw+6;
 }
 cy=py+30+18;
 D2D1_RECT_F clearBtn=D2D1::RectF(body.left+18,cy,(body.left+body.right)/2-4,cy+40);
 D2D1_RECT_F applyBtn=D2D1::RectF((body.left+body.right)/2+4,cy,body.right-18,cy+40);
 filterHot(IdFilterClear,clearBtn);filterHot(IdFilterApply,applyBtn);
 FilterChip(IdFilterClear,T(S_FilterClear),false,p,alpha);Chip(IdFilterApply,T(S_FilterApply),true,p,alpha);
 Dc()->PopAxisAlignedClip();
 float contentBottom=cy+40+16+filterScroll;
 filterScrollExtent=(std::max)(0.f,contentBottom-(body.bottom-12));
 filterScroll=Clamp(filterScroll,0,filterScrollExtent);
 Dc()->SetTransform(previous);
}
// A real folder shape (back tab + front body, both simple rounded rects that
// overlap into a silhouette) with up to four preview photos fanned out and
// tucked into its opening, macOS-Finder style, rather than a flat photo
// stack — folders should read as folders even before you know what's in them.
void PaintFolderCard(int id,const FolderEntry& folder,D2D1_RECT_F cell,const Palette& p,float alpha){
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 float scale=1.f+.02f*Hover(id)-.03f*Press(id);
 float cx=(cell.left+cell.right)/2,cy=(cell.top+cell.bottom)/2;
 auto base=D2D1::Matrix3x2F::Translation(-cx,-cy)*D2D1::Matrix3x2F::Scale(scale,scale)*
  D2D1::Matrix3x2F::Translation(cx,cy-2.f*Hover(id))*Mat(previous);
 Dc()->SetTransform(base);

 float capH=38.f;
 D2D1_RECT_F icon=D2D1::RectF(cell.left+12,cell.top+32,cell.right-12,cell.bottom-capH);
 float iw=Width(icon),ih=Height(icon);
 float tabH=ih*.17f,tabW=iw*.46f;
 D2D1_RECT_F tabRect=D2D1::RectF(icon.left,icon.top,icon.left+tabW,icon.top+tabH+8);
 D2D1_RECT_F bodyRect=D2D1::RectF(icon.left,icon.top+ih*.42f+Hover(id)*12,icon.right,icon.bottom);
 D2D1_COLOR_F back=D2D1::ColorF(.08f,.34f,.76f,1);
 D2D1_COLOR_F front=D2D1::ColorF(.08f,.46f,.94f,1);

 Ink()->SetColor(Fade(back,alpha));Dc()->FillRoundedRectangle(D2D1::RoundedRect(tabRect,8,8),Ink());
 Dc()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(icon.left,icon.top+tabH*.5f,icon.right,icon.bottom),10,10),Ink());

 // Up to four samples fan out from the opening, most-rotated drawn first so
 // the "cover" sample (index 0) ends up centred and on top.
 int count=(std::min)(4,int(folder.sampleCount));
 static const float angles1[1]={0},angles2[2]={-7,7},angles3[3]={-10,0,10},angles4[4]={-12,-4,4,12};
 const float* angles=count==1?angles1:count==2?angles2:count==3?angles3:angles4;
 float thumbW=iw*.60f,thumbH=ih*.78f;
 float fcx=(icon.left+icon.right)/2,fcy=icon.top+ih*.34f;
 D2D1_RECT_F thumbRect=D2D1::RectF(fcx-thumbW/2,fcy-thumbH/2,fcx+thumbW/2,fcy+thumbH/2);
 std::vector<FolderTransition::Photo> captures;
 for(int i=count-1;i>=0;i--){
  float fan=reducedMotion?0:Hover(id);
  float dx=(i-(count-1)*.5f)*(5+fan*10),dy=-fan*(10+i*5)+Press(id)*2;
  auto drawn=Shift(thumbRect,dx,dy);float angle=angles[i]*(1+fan*.25f);
  Dc()->SetTransform(D2D1::Matrix3x2F::Rotation(angle,D2D1::Point2F(fcx+dx,fcy+dy))*base);
  auto bmp=GridBitmap(folder.samples[i]);
  auto tl=base.TransformPoint(D2D1::Point2F(drawn.left,drawn.top)),br=base.TransformPoint(D2D1::Point2F(drawn.right,drawn.bottom));
  captures.push_back({folder.samples[i],D2D1::RectF(tl.x,tl.y,br.x,br.y),{},angle,bmp});
  bool flying=folderTx.active&&folderTx.folder==folder.path;
  if(!flying){DrawCover(drawn,7,bmp.Get(),Mix(p.sunk,p.accent,.3f));
   Ink()->SetColor(Fade(D2D1::ColorF(1,1,1,.8f),alpha));Dc()->DrawRoundedRectangle(D2D1::RoundedRect(drawn,7,7),Ink(),1.f);}
 }
 if(!folderTx.active){folderPreviews[NormalisePath(folder.path)]=std::move(captures);
  auto tl=base.TransformPoint(D2D1::Point2F(bodyRect.left,bodyRect.top)),br=base.TransformPoint(D2D1::Point2F(bodyRect.right,bodyRect.bottom));
  folderFronts[NormalisePath(folder.path)]=D2D1::RectF(tl.x,tl.y,br.x,br.y);}
 Dc()->SetTransform(base);

 // The front panel sits over the lower half of the fan, tucking the photos
 // into the folder's mouth instead of leaving them floating on top of it.
 D2D1_GRADIENT_STOP stops[]={{0,Fade(D2D1::ColorF(.24f,.70f,1.f,1),alpha)},{1,Fade(front,alpha)}};
 ComPtr<ID2D1GradientStopCollection> colors;ComPtr<ID2D1LinearGradientBrush> gradient;
 Dc()->CreateGradientStopCollection(stops,2,&colors);
 Dc()->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,bodyRect.top),D2D1::Point2F(0,bodyRect.bottom)),colors.Get(),&gradient);
 if(gradient)Dc()->FillRoundedRectangle(D2D1::RoundedRect(bodyRect,12,12),gradient.Get());
 Ink()->SetColor(Fade(p.glassEdge,alpha*.8f));Dc()->DrawRoundedRectangle(D2D1::RoundedRect(bodyRect,12,12),Ink(),1.f);

 // A folder that holds a favourite says so, counting its whole virtual family
 // rather than only the directory the card happens to be named after.
 uint32_t favourites=FolderFavouriteCount(folder.path);
 for(auto& member:folder.members)if(NormalisePath(member)!=NormalisePath(folder.path))
  favourites+=FolderFavouriteCount(member);
 if(favourites){
  auto heart=D2D1::RectF(icon.right-30,icon.top+2,icon.right-6,icon.top+26);
  Ink()->SetColor(Fade(D2D1::ColorF(0,0,0,.30f),alpha));
  Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F((heart.left+heart.right)/2,(heart.top+heart.bottom)/2),13,13),Ink());
  Icon(IcHeart,Inset(heart,4),Fade(D2D1::ColorF(1.f,.42f,.5f,1.f),alpha),1.5f,true);
 }
 Write(folder.name,D2D1::RectF(cell.left+4,cell.bottom-capH+4,cell.right-4,cell.bottom-18),F_Row,Fade(p.text,alpha));
 std::wstring photoCount=std::to_wstring(folder.photoCount)+L" "+T(S_Photos);
 if(!folder.members.empty())photoCount+=L" · "+std::to_wstring(folder.members.size())+L" папки";
 Write(photoCount,D2D1::RectF(cell.left+4,cell.bottom-18,cell.right-4,cell.bottom-2),F_Small,Fade(p.faint,alpha));
 Dc()->SetTransform(previous);
}
// The Favourites card. Visibly not a folder: no tab, no manila body, a heart
// instead, and the real liked photographs behind it.
void PaintFavouritesCard(int id,D2D1_RECT_F cell,const Palette& p,float alpha){
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 float scale=1.f+.02f*Hover(id)-.03f*Press(id);
 float cx=(cell.left+cell.right)/2,cy=(cell.top+cell.bottom)/2;
 auto base=D2D1::Matrix3x2F::Translation(-cx,-cy)*D2D1::Matrix3x2F::Scale(scale,scale)*
  D2D1::Matrix3x2F::Translation(cx,cy-2.f*Hover(id))*Mat(previous);
 Dc()->SetTransform(base);

 float capH=38.f;
 D2D1_RECT_F face=D2D1::RectF(cell.left+12,cell.top+30,cell.right-12,cell.bottom-capH);
 D2D1_GRADIENT_STOP stops[]={
  {0,Fade(D2D1::ColorF(.95f,.28f,.42f,1.f),alpha)},
  {1,Fade(D2D1::ColorF(.76f,.16f,.44f,1.f),alpha)}};
 ComPtr<ID2D1GradientStopCollection> colours;ComPtr<ID2D1LinearGradientBrush> gradient;
 Dc()->CreateGradientStopCollection(stops,2,&colours);
 Dc()->CreateLinearGradientBrush(
  D2D1::LinearGradientBrushProperties(D2D1::Point2F(face.left,face.top),D2D1::Point2F(face.right,face.bottom)),
  colours.Get(),&gradient);
 if(gradient)Dc()->FillRoundedRectangle(D2D1::RoundedRect(face,14,14),gradient.Get());

 size_t shown=favouritePhotos.size()<3?favouritePhotos.size():size_t(3);
 if(shown){
  // A fan of three real favourites, so the card is about the pictures rather
  // than about the idea of favourites.
  float iw=Width(face),ih=Height(face);
  float thumbW=iw*.52f,thumbH=ih*.62f;
  float fcx=(face.left+face.right)/2,fcy=(face.top+face.bottom)/2+ih*.02f;
  static const float angles[3]={-9,0,9};
  const float* fan=shown==1?angles+1:(shown==2?angles:angles);
  for(size_t i=shown;i-->0;){
   float lift=reducedMotion?0:Hover(id);
   float dx=(float(i)-(shown-1)*.5f)*(7+lift*10),dy=-lift*8;
   auto drawn=D2D1::RectF(fcx-thumbW/2+dx,fcy-thumbH/2+dy,fcx+thumbW/2+dx,fcy+thumbH/2+dy);
   Dc()->SetTransform(D2D1::Matrix3x2F::Rotation(fan[shown==2&&i==1?2:i],D2D1::Point2F(fcx+dx,fcy+dy))*base);
   auto bmp=GridBitmap(favouritePhotos[i].path);
   DrawCover(drawn,8,bmp.Get(),Mix(p.sunk,p.accent,.25f));
   Ink()->SetColor(Fade(D2D1::ColorF(1,1,1,.85f),alpha));
   Dc()->DrawRoundedRectangle(D2D1::RoundedRect(drawn,8,8),Ink(),1.f);
  }
  Dc()->SetTransform(base);
 }else{
  float mid=(face.top+face.bottom)/2;
  Icon(IcHeart,D2D1::RectF((face.left+face.right)/2-26,mid-38,(face.left+face.right)/2+26,mid+14),
   Fade(D2D1::ColorF(1,1,1,.92f),alpha),2.f,true);
  Write(T(S_NoFavourites),D2D1::RectF(face.left+8,mid+14,face.right-8,mid+34),F_Small,
   Fade(D2D1::ColorF(1,1,1,.86f),alpha));
  // The hint is deliberately two short lines: a card this narrow elides a
  // single long one in the middle, which reads as a rendering fault.
  Write(T(S_NoFavouritesHint),D2D1::RectF(face.left+6,mid+31,face.right-6,mid+47),F_Small,
   Fade(D2D1::ColorF(1,1,1,.62f),alpha));
  Write(T(S_NoFavouritesHint2),D2D1::RectF(face.left+6,mid+45,face.right-6,mid+61),F_Small,
   Fade(D2D1::ColorF(1,1,1,.62f),alpha));
 }
 // The badge, always: it is what tells this card apart at a glance.
 D2D1_RECT_F badge=D2D1::RectF(face.right-38,face.top+8,face.right-8,face.top+38);
 Ink()->SetColor(Fade(D2D1::ColorF(0,0,0,.28f),alpha));
 Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F((badge.left+badge.right)/2,(badge.top+badge.bottom)/2),15,15),Ink());
 Icon(IcHeart,Inset(badge,5),Fade(D2D1::ColorF(1,1,1,1),alpha),1.8f,true);

 Write(T(S_Favourites),D2D1::RectF(cell.left+4,cell.bottom-capH+4,cell.right-4,cell.bottom-18),F_Row,Fade(p.text,alpha));
 auto count=favouritePhotos.size();
 Write(std::to_wstring(count)+L" "+T(S_Photos),
  D2D1::RectF(cell.left+4,cell.bottom-18,cell.right-4,cell.bottom-2),F_Small,Fade(p.faint,alpha));
 Dc()->SetTransform(previous);
}
void PaintPhotoCell(int id,const PhotoEntry& photo,D2D1_RECT_F cell,const Palette& p,float alpha){
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 float scale=1.f+.02f*Hover(id)-.03f*Press(id);
 float cx=(cell.left+cell.right)/2,cy=(cell.top+cell.bottom)/2;
 auto lift=D2D1::Matrix3x2F::Translation(-cx,-cy)*D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(cx,cy);
 Dc()->SetTransform(lift*Mat(previous));
 auto bmp=GridBitmap(photo.path);
 DrawCover(cell,12,bmp.Get(),p.sunk);
 Ink()->SetColor(Fade(p.glassEdge,alpha*.6f));Dc()->DrawRoundedRectangle(D2D1::RoundedRect(cell,12,12),Ink(),.8f);
 if(photo.variantCount>1){
  auto badge=D2D1::RectF(cell.right-34,cell.top+8,cell.right-8,cell.top+32);
  Glass(D2D1::RoundedRect(badge,12,12),p,.9f,D2D1::Matrix3x2F::Identity());Write(std::to_wstring(photo.variantCount),badge,F_Small,p.text);
 }
 if(FavouriteGet(photo.path)){
  // Bottom-left, so it never collides with the variant-count badge.
  auto heart=D2D1::RectF(cell.left+7,cell.bottom-29,cell.left+29,cell.bottom-7);
  Ink()->SetColor(Fade(D2D1::ColorF(0,0,0,.34f),alpha));
  Dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F((heart.left+heart.right)/2,(heart.top+heart.bottom)/2),13,13),Ink());
  Icon(IcHeart,Inset(heart,3),Fade(D2D1::ColorF(1.f,.36f,.44f,1.f),alpha),1.6f,true);
 }
 if(selectedPhotos.count(photo.id)){Ink()->SetColor(p.accent);Dc()->DrawRoundedRectangle(D2D1::RoundedRect(Inset(cell,2),10,10),Ink(),2.5f);}
 Dc()->SetTransform(previous);
}

GridPlacement LibraryGridFor(float w,float h,float& areaLeft,float& areaTop,float& areaW,bool album=false){
 (void)h;
 areaLeft=Margin+10;areaTop=LibTop;areaW=w-2*areaLeft;
 bool folders=!album&&libView==ViewFolders;
 float cellH=folders?LibCardH:AlbumCell;
 float cellW=folders?LibCard:AlbumCell;
 // The Favourites card occupies the first cell of the folder grid. It is a
 // virtual collection, not a directory, so it is counted here rather than
 // inserted into libFolders where the indexer would have to know about it.
 size_t count=folders?libFolders.size()+1:albumPhotos.size();
 return PlanGrid(areaW,cellW,cellH,LibGap,count);
}
void PaintLibraryScreen(float w,float h,const Palette& p){
 LayoutLibraryChrome(w,false);
 float areaLeft,areaTop,areaW;
 auto grid=LibraryGridFor(w,h,areaLeft,areaTop,areaW);
 float bottomBar=h-40;
 auto timeline=PlanTimeline(areaW);
 float contentH=libView==ViewPhotosFlat?timeline.contentH+FavouriteChipH:grid.contentH;
 libScrollExtent=(std::max)(0.f,contentH-(bottomBar-areaTop-12));
 libScroll=Clamp(libScroll,0,libScrollExtent);
 // The content plane runs behind the floating chrome. Clipping at LibTop
 // created a solid horizontal bar even though no bar was explicitly drawn.
 Dc()->PushAxisAlignedClip(D2D1::RectF(0,0,w,bottomBar),D2D1_ANTIALIAS_MODE_ALIASED);
 size_t count=libView==ViewFolders?libFolders.size():albumPhotos.size();
 if(libView==ViewPhotosFlat){
  // A shortcut, not a second copy of the pictures. The timeline stays the
  // chronological source of truth; Favourites is a filter over it, reached
  // from one compact chip that scrolls away with the content.
  float chipY=areaTop-libScroll;
  D2D1_RECT_F chip=D2D1::RectF(areaLeft,chipY,areaLeft+(std::min)(areaW,258.f),chipY+FavouriteChipH-10);
  if(chip.bottom>-20&&chip.top<bottomBar+20){
   Hotspot(IdFavouritesChip,chip);
   float lift=Hover(IdFavouritesChip)-.6f*Press(IdFavouritesChip);
   auto shape=D2D1::RoundedRect(Shift(chip,0,-2.f*lift),(chip.bottom-chip.top)/2,(chip.bottom-chip.top)/2);
   Glass(shape,p,1.f,D2D1::Matrix3x2F::Identity());
   Ink()->SetColor(Fade(Mix(p.cardEdge,p.accent,.25f+.5f*Hover(IdFavouritesChip)),.85f));
   Dc()->DrawRoundedRectangle(shape,Ink(),1.f);
   Icon(IcHeart,D2D1::RectF(shape.rect.left+14,shape.rect.top+11,shape.rect.left+36,shape.rect.bottom-11),
    D2D1::ColorF(1.f,.36f,.44f,1.f),1.7f,favouritePhotos.empty()?false:true);
   Write(T(S_Favourites),D2D1::RectF(shape.rect.left+44,shape.rect.top,shape.rect.right-58,shape.rect.bottom),F_Row,p.text);
   Write(std::to_wstring(favouritePhotos.size()),
    D2D1::RectF(shape.rect.right-52,shape.rect.top,shape.rect.right-16,shape.rect.bottom),F_Row,p.faint);
  }
  for(size_t g=0;g<timelineGroups.size();++g){
   float groupTop=areaTop-libScroll+FavouriteChipH+timeline.groupTops[g];
   auto& group=timelineGroups[g];
   int rows=int((group.photos.size()+size_t(timeline.columns)-1)/size_t(timeline.columns));
   float groupBottom=groupTop+TimelineHeaderH+rows*(AlbumCell+LibGap);
   if(groupBottom<0||groupTop>bottomBar+40)continue;
   Write(TimelineDateLabel(group.day),D2D1::RectF(areaLeft,groupTop,areaLeft+areaW,groupTop+30),F_Timeline,p.text);
   for(size_t local=0;local<group.photos.size();++local){
    int col=int(local)%timeline.columns,row=int(local)/timeline.columns;
    D2D1_RECT_F cell=D2D1::RectF(areaLeft+col*(AlbumCell+LibGap),groupTop+TimelineHeaderH+row*(AlbumCell+LibGap),0,0);
    cell.right=cell.left+AlbumCell;cell.bottom=cell.top+AlbumCell;
    if(cell.bottom<0||cell.top>bottomBar+40)continue;
    size_t i=group.photos[local];int id=IdCard0+int(i);Hotspot(id,cell);
    ThumbRequest(albumPhotos[i].path);
    if(!(hero.active&&albumPhotos[i].id==hero.photoId))PaintPhotoCell(id,albumPhotos[i],cell,p,1.f);
   }
  }
 }else{
  size_t total=count+1;   // cell 0 is the virtual Favourites collection
  size_t first=size_t((std::max)(0,int((libScroll-40)/(grid.cellH+LibGap))))*grid.columns;
  size_t last=(std::min)(total,first+size_t((h-areaTop+80)/(grid.cellH+LibGap)+3)*grid.columns);
  for(size_t slot=first;slot<last;slot++){
   auto cell=GridCell(grid,areaLeft,areaTop-libScroll,LibGap,slot);
   if(cell.bottom<0||cell.top>bottomBar+40)continue;
   if(slot==0){Hotspot(IdFavourites,cell);PaintFavouritesCard(IdFavourites,cell,p,1.f);continue;}
   size_t i=slot-1;
   int id=IdCard0+int(i);Hotspot(id,cell);PaintFolderCard(id,libFolders[i],cell,p,1.f);
  }
 }
 Dc()->PopAxisAlignedClip();
 if(count==0&&!IndexIsScanning()&&libView!=ViewFolders)
  Write(T(S_NoPhotosFound),D2D1::RectF(w/2-200,h/2-14,w/2+200,h/2+14),F_Row,p.dim);
 // status footer
 std::wstring left;
 if(IndexIsScanning())left=T(S_Indexing)+std::wstring(L" ")+std::to_wstring(IndexKnownPhotoCount())+L" "+T(S_Photos);
 else{
  auto n=libFolders.size();
  left=T(S_Found)+std::wstring(L" ")+std::to_wstring(n)+L" "+RuPlural(n,S_Folder1,S_Folder2to4,S_Folder5plus)+
   L"  •  "+std::to_wstring(IndexKnownPhotoCount())+L" "+T(S_Photos);
 }
 Write(left,D2D1::RectF(Margin,h-30,w/2,h-8),F_Meta,p.faint);
 std::wstring right=IndexIsScanning()?T(S_LibraryUpdating):T(S_LibraryUpdated);
 Write(right,D2D1::RectF(w/2,h-30,w-Margin,h-8),F_Meta,p.faint);

}
D2D1_RECT_F FindAlbumCellRect(uint64_t photoId,float w,float h){
 if(screen==ScrViewer)return D2D1::RectF(0,0,0,0);
 float areaLeft,areaTop,areaW;
 auto grid=LibraryGridFor(w,h,areaLeft,areaTop,areaW,true);
 auto& scroll=screen==ScrLibrary?libScroll:albumScroll;
 if(screen==ScrLibrary&&libView==ViewPhotosFlat){
  auto cell=TimelinePhotoRect(photoId,areaLeft,areaTop,areaW,scroll);
  float bottomBar=h-40;
  if(cell.bottom<0||cell.top>bottomBar+40)return D2D1::RectF(0,0,0,0);
  return cell;
 }
 for(size_t i=0;i<albumPhotos.size();i++)if(albumPhotos[i].id==photoId){
  auto cell=GridCell(grid,areaLeft,areaTop-scroll,LibGap,i);
  float bottomBar=h-40;
  if(cell.bottom<0||cell.top>bottomBar+40)return D2D1::RectF(0,0,0,0);
  return cell;
 }
 return D2D1::RectF(0,0,0,0);
}
void PrepareFolderTransition(const std::wstring& folder,bool closing){
 folderTx.photos.clear();folderTx.folder=folder;
 auto captures=folderPreviews.find(NormalisePath(folder));if(captures==folderPreviews.end())return;
 folderTx.front=folderFronts[NormalisePath(folder)];
 float w,h;Size(w,h);
 for(auto photo:captures->second){
  auto id=AlbumPhotoIdFor(photo.path);if(!id||!photo.texture)continue;
  auto cell=FindAlbumCellRect(id,w,h);if(Width(cell)<=0)continue;
  photo.to=cell;folderTx.photos.push_back(std::move(photo));
 }
 (void)closing;
}
void PaintFolderTransition(){
 if(!folderTx.active)return;
 float progress=Clamp(folderTx.blend.v,0,1);
 for(size_t i=0;i<folderTx.photos.size();i++){
  const auto& photo=folderTx.photos[i];
  float t=reducedMotion?progress:Clamp((progress-float(i)*.025f)/(1.f-float(i)*.025f),0,1);
  auto lerp=[&](float a,float b){return a+(b-a)*t;};
  auto r=D2D1::RectF(lerp(photo.from.left,photo.to.left),lerp(photo.from.top,photo.to.top),lerp(photo.from.right,photo.to.right),lerp(photo.from.bottom,photo.to.bottom));
  float rise=reducedMotion?0:-18.f*sinf(t*3.14159265f);r=Shift(r,0,rise);
  D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
  Dc()->SetTransform(D2D1::Matrix3x2F::Rotation(photo.angle*(1-t),D2D1::Point2F((r.left+r.right)/2,(r.top+r.bottom)/2))*Mat(previous));
  SoftShadow(D2D1::RoundedRect(r,8,8),.3f);
  DrawCover(r,8+4*t,photo.texture.Get(),D2D1::ColorF(0,0,0,0));
  Dc()->SetTransform(previous);
 }
 if(Width(folderTx.front)>0&&progress<1){
  auto r=folderTx.front;
  D2D1_GRADIENT_STOP stops[]={{0,D2D1::ColorF(.24f,.70f,1,1-progress)},{1,D2D1::ColorF(.08f,.46f,.94f,1-progress)}};
  ComPtr<ID2D1GradientStopCollection> colors;ComPtr<ID2D1LinearGradientBrush> brush;
  if(SUCCEEDED(Dc()->CreateGradientStopCollection(stops,2,&colors))&&SUCCEEDED(Dc()->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,r.top),D2D1::Point2F(0,r.bottom)),colors.Get(),&brush)))Dc()->FillRoundedRectangle(D2D1::RoundedRect(r,12,12),brush.Get());
 }
}
void PaintAlbumScreen(float w,float h,const Palette& p){
 LayoutLibraryChrome(w,true);
 float areaLeft,areaTop,areaW;
 auto grid=LibraryGridFor(w,h,areaLeft,areaTop,areaW,true);
 float bottomBar=h-30;
 albumScrollExtent=(std::max)(0.f,grid.contentH-(bottomBar-areaTop-12));
 albumScroll=Clamp(albumScroll,0,albumScrollExtent);
 Dc()->PushAxisAlignedClip(D2D1::RectF(0,0,w,bottomBar),D2D1_ANTIALIAS_MODE_ALIASED);
 size_t first=size_t((std::max)(0,int((albumScroll-40)/(grid.cellH+LibGap))))*grid.columns;
 size_t last=(std::min)(albumPhotos.size(),first+size_t((h-areaTop+80)/(grid.cellH+LibGap)+3)*grid.columns);
 for(size_t i=first;i<last;i++){
  auto cell=GridCell(grid,areaLeft,areaTop-albumScroll,LibGap,i);
  if(cell.bottom<0||cell.top>bottomBar+40)continue;
  if(hero.active&&albumPhotos[i].id==hero.photoId)continue; // the hero overlay is standing in for it
  bool flying=false;for(auto& photo:folderTx.photos)if(folderTx.active&&photo.path==albumPhotos[i].path)flying=true;
  if(flying)continue;
  int id=IdCard0+int(i);
  Hotspot(id,cell);
  PaintPhotoCell(id,albumPhotos[i],cell,p,1.f);
 }
 Dc()->PopAxisAlignedClip();
 if(albumPhotos.empty())Write(IndexFolderState(albumFolder)==FolderState::Indexing?T(S_Indexing):T(S_NoPhotos),D2D1::RectF(w/2-200,h/2-14,w/2+200,h/2+14),F_Row,p.dim);
 std::wstring left=std::to_wstring(albumPhotos.size())+L" "+T(S_Photos);
 Write(left,D2D1::RectF(Margin,h-24,w/2,h-4),F_Meta,p.faint);
 auto title=fs::path(albumFolder).filename().wstring();

}
void PaintHeroOverlay(float w,float h,const Palette& p){
 if(!hero.active)return;
 D2D1_RECT_F rect=D2D1::RectF(hero.left.v,hero.top.v,hero.right.v,hero.bottom.v);
 SoftShadow(D2D1::RoundedRect(rect,hero.radius.v,hero.radius.v),hero.shadow.v*.6f);
 ID2D1Bitmap* show=hero.fromBitmap.Get();
 unsigned iw=show==bitmap.Get()&&current?current->w:hero.fromW,ih=show==bitmap.Get()&&current?current->h:hero.fromH;
 if(show&&iw&&ih){
  auto rounded=D2D1::RoundedRect(rect,hero.radius.v,hero.radius.v);
  Dc()->PushAxisAlignedClip(rect,D2D1_ANTIALIAS_MODE_ALIASED);
  DrawCover(rect,hero.radius.v,show,p.sunk);
  if(!hero.closing&&!loading&&bitmap){
   Dc()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),Clamp(hero.crossfade.v,0,1)),nullptr);
   DrawCover(rect,hero.radius.v,bitmap.Get(),D2D1::ColorF(0,0,0,0));Dc()->PopLayer();
  }
  Dc()->PopAxisAlignedClip();
  Ink()->SetColor(Fade(p.glassEdge,.5f));Dc()->DrawRoundedRectangle(rounded,Ink(),1.f);
 }else{
  Ink()->SetColor(p.sunk);Dc()->FillRoundedRectangle(D2D1::RoundedRect(rect,hero.radius.v,hero.radius.v),Ink());
 }
 (void)w;(void)h;
 bool settled=!hero.left.Moving()&&!hero.top.Moving()&&!hero.right.Moving()&&!hero.bottom.Moving()&&!hero.radius.Moving();
 if(settled&&(hero.closing||(!loading&&!hero.crossfade.Moving())))hero.active=false;
}

void Frame(){
 if(!GfxReady())return;
 float w,h;Size(w,h);
 auto p=Pal();
 hots.clear();rects.clear();
 if(screen==ScrLibrary&&!folderTx.active){folderPreviews.clear();folderFronts.clear();}
 if(current&&!bitmap){
  auto slotKey=currentFrameKey.empty()?NormalisePath(currentPath)+L"|live":currentFrameKey;
  bitmap=GpuTextureFor(current,slotKey,slotKey);
  if(!bitmap)errorText=L"This image exceeds the renderer's bitmap limit.";
  // A thumbnail of this file is usually already in memory, and reducing 180
  // pixels to 22 costs nothing. Only when there is none does this fall back
  // to a strided average of the frame itself.
  auto reduced=ThumbLookup(currentPath);
  if(!reduced&&backdropSource&&backdropSource->w<=512)reduced=backdropSource;
  std::shared_ptr<Image> tiny=reduced?Downsample(reduced,22):std::shared_ptr<Image>();
  if(tiny&&tiny->w&&tiny->h){
   backdropIsAverage=false;
   Dc()->CreateBitmap(D2D1::SizeU(tiny->w,tiny->h),tiny->pixels.data(),tiny->w*4,
    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&backdrop);
  }else if(AverageColour(*current,backdropAverage)){
   backdropIsAverage=true;
   Dc()->CreateBitmap(D2D1::SizeU(1,1),backdropAverage,4,
    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&backdrop);
  }
 }
 bool browsing=!preview&&screen!=ScrViewer;
 if(!preview&&screen==ScrViewer)Layout();
 float radius=preview?32.f:(WindowMaximized()?0.f:26.f);
 auto shape=D2D1::RoundedRect(D2D1::RectF(0,0,w,h),radius,radius);

 GfxBeginScene(w,h);
 if(preview){
  // Preview shows the photograph and nothing else: no chrome, no plate, no border.
  if(bitmap&&current){
   auto matrix=ImageMatrix();
   Dc()->SetTransform(matrix);
   Dc()->DrawBitmap(bitmap.Get(),D2D1::RectF(0,0,float(current->w),float(current->h)),1.f,
    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr);
   Dc()->SetTransform(D2D1::Matrix3x2F::Identity());
  }
 }else if(browsing){
  Ink()->SetColor(Mix(D2D1::ColorF(.055f,.075f,.11f,1.f),D2D1::ColorF(.87f,.91f,.96f,1.f),themeMix.v));
  Dc()->FillRectangle(D2D1::RectF(0,0,w,h),Ink());
  // The destination is already present behind the shared photos. Fading an
  // entire duplicate screen here made the transition look like a dark veil;
  // only the folder previews and front flap need to animate.
  if(screen==ScrAlbum)PaintAlbumScreen(w,h,p);
  else{
   float reveal=Clamp(libContentIn.v,0,1);
   D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
   Dc()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
    D2D1::Matrix3x2F::Identity(),.35f+.65f*reveal),nullptr);
   Dc()->SetTransform(D2D1::Matrix3x2F::Translation(libContentDirection*(1-reveal)*18.f,0)*Mat(previous));
   PaintLibraryScreen(w,h,p);
   Dc()->SetTransform(previous);Dc()->PopLayer();
  }
 }else{
  PaintBackdrop(w,h,p);
  if(!hero.active)PaintImage(w,h,p);
  if(!current&&currentPath.empty())PaintEmpty(w,h,p);
  if(!errorText.empty())Write(errorText,D2D1::RectF(40,h/2+90,w-40,h/2+140),F_Row,p.dim);
 }

 // Chrome remains frosted while scrolling.  Falling back to a transparent
 // fill made the header and dock visibly "switch off" during manipulation.
 GfxEndScene(shape,!preview);
 PaintFolderTransition();
 if(hero.active&&!preview)PaintHeroOverlay(w,h,p);
 if(browsing){
  LayoutLibraryChrome(w,screen==ScrAlbum);
  PaintLibraryChrome(w,p,screen==ScrAlbum,fs::path(albumFolder).filename().wstring());
  if(sortOpen||sortReveal.v>.004f)PaintSortPopup(w,p,sortReveal.v);
  if(filterOpen||filterReveal.v>.004f)PaintFilterPopup(w,h,p,filterReveal.v);
 }else if(!preview){
  Dc()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),Clamp(chrome.v,0,1)),nullptr);
  PaintGallery(p);
  if(siblings.size()>1){
   for(int id:{IdPrevPhoto,IdNextPhoto}){
    auto r=R(id);Glass(D2D1::RoundedRect(r,22,22),p,.85f,D2D1::Matrix3x2F::Identity());
    IconButton(id,id==IdPrevPhoto?IcChevronL:IcChevron,p,1,p.text);
   }
  }
  PaintBack(p);
  PaintZoomBar(p);
  PaintTitle(w,p,R(IdDockBar).left,R(IdZoomBar).right>0?R(IdZoomBar).right:Margin+Bubble);
  PaintDock(p);
  PaintEditBubble(p);
  PaintWindowButtons(p);
  PaintPanel(p);
  PaintToast(w,h,p);
  if(loading&&!current)Write(T(S_Opening),D2D1::RectF(w/2-90,h-Margin-GalleryH-46,w/2+90,h-Margin-GalleryH-22),F_Meta,p.dim);
  Dc()->PopLayer();
 }
 GfxPresent(true);
 if(screen==ScrViewer&&current&&bitmap&&latencyId==latest&&currentGeneration==latencyId){
  double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-openStarted).count();
  if(!firstVisibleLogged){Log(L"first_visible_ms="+std::to_wstring(elapsed)+(showingPreview?L" stage=preview ":L" stage=full ")+currentPath);firstVisibleLogged=true;}
  if(!showingPreview&&!fullVisibleLogged){Log(L"full_visible_ms="+std::to_wstring(elapsed)+L" "+currentPath);fullVisibleLogged=true;}
 }
}

// ---------------------------------------------------------------- tick -----
bool Tick(float dt){
 bool busy=false;
 static double nextCachePoll=0;
 if(lowMemoryNotice&&WaitForSingleObject(lowMemoryNotice,0)==WAIT_OBJECT_0){
  if(!lowMemoryActive){lowMemoryActive=true;CacheHandlePressure(currentPath);}
 }else lowMemoryActive=false;
 if(Now()>=nextCachePoll){
  nextCachePoll=Now()+1.0;RefreshCacheBudget();
  {std::lock_guard lock(cacheMx);CacheTrimLocked(NormalisePath(currentPath),false);}
  CacheLogTelemetry();
 }
 if(fastNavigation&&Now()>=fastNavigationUntil)fastNavigation=false;
 busy|=chrome.Step(dt);
 busy|=StepWindow(dt);
 busy|=themeMix.Step(dt);
 busy|=wheelMix.Step(dt);
 busy|=zoomLog.Step(dt);
 if(zoomAnchored){
  // Derive translation from the same animated scale every frame. Independent
  // springs in log(scale) and pan would move the point underneath the cursor.
  panSX.v=zoomAnchorX-zoomPointX*Zoom();panSY.v=zoomAnchorY-zoomPointY*Zoom();
  panSX.vel=panSY.vel=0;
  if(!zoomLog.Moving())zoomAnchored=false;
 }else{busy|=panSX.Step(dt);busy|=panSY.Step(dt);}
 busy|=rotate.Step(dt);
 busy|=panelSlide.Step(dt);busy|=levelSlide.Step(dt);busy|=titleIn.Step(dt);
 busy|=toastIn.Step(dt);busy|=likePop.Step(dt);busy|=thumbLift.Step(dt);
 busy|=clipHighFade.Step(dt);busy|=clipLowFade.Step(dt);busy|=histRise.Step(dt);
 busy|=filterReveal.Step(dt);busy|=sortReveal.Step(dt);busy|=libViewSlide.Step(dt);busy|=libContentIn.Step(dt);
 for(auto& entry:buttons){busy|=entry.second.hover.Step(dt);busy|=entry.second.press.Step(dt);}
 for(auto& s:galleryWidth)busy|=s.Step(dt);
 // A closing panel must finish retreating before its replacement arrives.
 if(panelSlide.target==0&&panelSlide.v<.02f&&panel!=PanelNone){panel=PanelNone;busy=true;}
 if(panel==PanelNone&&pendingPanel!=PanelNone){panel=pendingPanel;pendingPanel=PanelNone;panelSlide.To(1);busy=true;}
 if(!toast.empty()&&Now()>toastUntil&&toastIn.target>0){toastIn.To(0);busy=true;}
 if(Now()<copyUntil+.05)busy=true;
 if(Now()<likeBurst+.55)busy=true;
 // Filmstrip: inertia when thrown, otherwise a spring toward the active item.
 if(!galleryDragging){
  if(fabsf(galleryVel)>4.f){
   galleryScroll+=galleryVel*dt;
   galleryVel*=powf(.015f,dt);
   galleryHasTarget=false;busy=true;
  }else galleryVel=0;
  if(galleryHasTarget){
   float delta=galleryTarget-galleryScroll;
   if(fabsf(delta)>.4f){galleryScroll+=delta*(1.f-powf(.0007f,dt));busy=true;}
   else{galleryScroll=galleryTarget;galleryHasTarget=false;}
  }
 }
 float w,h;Size(w,h);
 if(!galleryDragging&&!galleryHasTarget&&siblings.size()>1){
  float content=0;
  for(size_t i=0;i<galleryWidth.size();i++)content+=galleryWidth[i].v+(i?ThumbGap:0);
  float barWidth=(std::min)(w-2*Margin-40,content+20);
  float low=(std::min)(0.f,barWidth-20-content),high=0;
  if(galleryScroll>high){galleryScroll+=(high-galleryScroll)*(1.f-powf(.0004f,dt));busy=true;}
  else if(galleryScroll<low){galleryScroll+=(low-galleryScroll)*(1.f-powf(.0004f,dt));busy=true;}
 }
 if(fabsf(panelScrollVel)>2.f){
  panelScroll=Clamp(panelScroll+panelScrollVel*dt,0,(std::max)(0.f,panelExtent));
  panelScrollVel*=powf(.01f,dt);busy=true;
 }else panelScrollVel=0;
 if(!preview&&screen!=ScrViewer){
  if(IndexIsScanning()){scanSpin+=dt*220.f;busy=true;}
  if(fabsf(libScrollVel)>2.f){
   libScroll=Clamp(libScroll+libScrollVel*dt,0,(std::max)(0.f,libScrollExtent));
   libScrollVel*=powf(.01f,dt);busy=true;
  }else libScrollVel=0;
  if(fabsf(albumScrollVel)>2.f){
   albumScroll=Clamp(albumScroll+albumScrollVel*dt,0,(std::max)(0.f,albumScrollExtent));
   albumScrollVel*=powf(.01f,dt);busy=true;
  }else albumScrollVel=0;
  if(fabsf(filterScrollVel)>2.f){
   filterScroll=Clamp(filterScroll+filterScrollVel*dt,0,(std::max)(0.f,filterScrollExtent));
   filterScrollVel*=powf(.01f,dt);busy=true;
  }else filterScrollVel=0;
  if(libSearchFocused)busy=true; // keeps the caret blinking
 }
 busy|=folderTx.blend.Step(dt);
 if(folderTx.active&&(!folderTx.blend.Moving()||Now()-folderTx.started>1.2)){folderTx.blend.Reset(folderTx.blend.target);folderTx.active=false;}
 busy|=hero.left.Step(dt);busy|=hero.top.Step(dt);busy|=hero.right.Step(dt);busy|=hero.bottom.Step(dt);
 busy|=hero.radius.Step(dt);busy|=hero.shadow.Step(dt);busy|=hero.crossfade.Step(dt);
 if(hero.active)busy=true;
 // With nothing animating and nothing loading, put the frame the reader is
 // most likely to ask for next onto the GPU. Direct2D's device context is
 // single-threaded, so "background" here means "in the gaps", which is
 // exactly where a 20 MB upload is invisible.
 // Not gated on `busy`: the springs from the last navigation keep it true for
 // a few hundred milliseconds, which is the entire gap between two keypresses
 // at any realistic browsing speed, so an idle-only rule never fired. Gated
 // instead on not being mid-decode, and rate-limited so it cannot run twice
 // for the same neighbour.
 if(!loading&&!preview&&screen==ScrViewer&&siblings.size()>1&&GfxReady()){
  int index=CurrentIndex();
  if(index>=0){
   int direction=navigationDirection?navigationDirection:1;
   int next=int((index+direction+(int)siblings.size())%(int)siblings.size());
   auto& neighbour=siblings[size_t(next)];
   // Pre-upload whichever tier Open() would pick, which is the full frame when
   // one is cached. Measuring showed every navigation over 16 ms was a
   // full-resolution CreateBitmap: 145 MB for a 36 megapixel frame, 20-35 ms,
   // on the UI thread, in the frame meant to show the new photograph. The
   // work is unavoidable, but it does not have to happen while somebody is
   // waiting for it.
   // Screen tier only. Speculatively uploading a neighbour's full frame costs
   // 138 MB of transfer for a photograph nobody has asked to see yet, and at
   // fit it would be thrown away in favour of the screen frame anyway.
   auto key=FrameKey(neighbour,CacheScreen,requestEdge);
   auto frame=CacheGet(key);
   if(!frame&&NeedsFullResolution()){
    key=FrameKey(neighbour,CacheFull,0);frame=CacheGet(key);
   }
   if(frame)GpuPreUpload(frame,key,currentFrameKey);
  }
 }
 return busy;
}
void SyncGallery(){
 if(galleryWidth.size()!=siblings.size()){
  galleryWidth.assign(siblings.size(),Spring(ThumbNarrow,GalleryK,GalleryC));
 }
 int active=CurrentIndex();
 for(size_t i=0;i<galleryWidth.size();i++)galleryWidth[i].To(int(i)==active?ThumbWide:ThumbNarrow);
 if(active>=0){
  float w,h;Size(w,h);
  float before=0;
  for(int i=0;i<active;i++)before+=galleryWidth[size_t(i)].target+ThumbGap;
  float content=0;
  for(size_t i=0;i<galleryWidth.size();i++)content+=galleryWidth[i].target+(i?ThumbGap:0);
  float barWidth=(std::min)(w-2*Margin-40,content+20);
  float centre=before+ThumbWide/2;
  float wanted=Clamp(barWidth/2-10-centre,(std::min)(0.f,barWidth-20-content),0.f);
  galleryTarget=wanted;galleryHasTarget=true;galleryVel=0;
 }
 Wake();
}

// --------------------------------------------------------------- input -----
void SetTool(int which){
 tool=(tool==which)?ToolNone:which;
 painting=false;
 if(tool==ToolCrop&&!hasCrop&&current){
  crop=D2D1::RectF(DisplayW()*.1f,DisplayH()*.1f,DisplayW()*.9f,DisplayH()*.9f);
  hasCrop=true;
 }
 Wake();
}
void ApplyAspect(int which){
 cropAspect=which;
 if(which==0||!hasCrop)return;
 const float ratios[5]={0,1.f,4.f/3.f,3.f/2.f,16.f/9.f};
 float ratio=ratios[which];
 float cx=(crop.left+crop.right)/2,cy=(crop.top+crop.bottom)/2;
 float area=Width(crop)*Height(crop);
 float nh=sqrtf(area/ratio),nw=nh*ratio;
 nw=(std::min)(nw,float(DisplayW()));nh=(std::min)(nh,float(DisplayH()));
 crop=D2D1::RectF(cx-nw/2,cy-nh/2,cx+nw/2,cy+nh/2);
 if(crop.left<0){crop.right-=crop.left;crop.left=0;}
 if(crop.top<0){crop.bottom-=crop.top;crop.top=0;}
 if(crop.right>DisplayW()){crop.left-=crop.right-DisplayW();crop.right=float(DisplayW());}
 if(crop.bottom>DisplayH()){crop.top-=crop.bottom-DisplayH();crop.bottom=float(DisplayH());}
 Wake();
}
void Command(int id){
 if(id==IdPrevPhoto){Navigate(-1);return;}
 if(id==IdNextPhoto){Navigate(1);return;}
 if(id==IdShapeRect||id==IdShapeEllipse){shapeKind=id==IdShapeRect?2:3;Wake();return;}
 if(id==IdCopyPath){
  if(OpenClipboard(win)){
   auto bytes=(currentPath.size()+1)*sizeof(wchar_t);HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,bytes);
   if(data){void* target=GlobalLock(data);if(target){memcpy(target,currentPath.c_str(),bytes);GlobalUnlock(data);EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,data))GlobalFree(data);}else GlobalFree(data);}
   CloseClipboard();Notify(T(S_Copied));
  }
  return;
 }
 if(id==IdOpenMap){
  if(info.hasGps){
   wchar_t url[128];swprintf_s(url,L"https://www.google.com/maps?q=%.6f,%.6f",info.lat,info.lon);
   ShellExecuteW(win,L"open",url,nullptr,nullptr,SW_SHOWNORMAL);
  }
  return;
 }
 float w,h;Size(w,h);
 switch(id){
 case IdBack:if(!navStack.empty()){ViewerBack();return;}Close();return;
 case IdZoomOut:SetZoom(Zoom()/1.35f,0,0);return;
 case IdZoomIn:SetZoom(Zoom()*1.35f,0,0);return;
 case IdInfo:OpenPanel(PanelInfo);return;
 case IdCopy:CopyCurrent();return;
 case IdLike:ToggleLike();return;
 case IdRotate:Turn(1);return;
 // 1:1 is about the photograph, not about the tier: one original pixel to one
 // screen pixel. On a screen-tier frame that is a magnification, which is
 // exactly the condition that asks the worker for full resolution.
 case IdFit:if(fit){SetZoom(1.f/(dpi*(std::max)(0.0001f,FrameScale())),0,0);RequestFullResolution();}else Fit();return;
 case IdMore:OpenPanel(PanelMenu);return;
 case IdEdit:OpenPanel(PanelEdit);return;
 case IdWinMin:ShowWindow(win,SW_MINIMIZE);return;
 case IdWinMax:SendMessageW(win,WM_SYSCOMMAND,WindowMaximized()?SC_RESTORE:SC_MAXIMIZE,0);return;
 case IdWinClose:Close();return;
 case IdSend:if(current&&!loading){if(Dirty())Notify(T(S_SaveFirst));else ShareImage(win,currentPath);}ClosePanel();return;
 case IdSave:Save(false);ClosePanel();return;
 case IdSaveAs:Save(true);ClosePanel();return;
 case IdPrint:if(current&&!loading){auto flat=Composite();if(flat)PrintImage(win,*flat,0);}ClosePanel();return;
 case IdDelete:confirmDelete=!confirmDelete;Wake();return;
 case IdConfirmCancel:confirmDelete=false;Wake();return;
 case IdConfirmDelete:confirmDelete=false;ClosePanel();RemoveCurrent();return;
 case IdDefaultApp:{
  std::wstring failure;
  if(RegisterAsViewer(failure)){OpenDefaultAppsPage();Notify(T(S_DefaultDone));}
  else Notify(T(S_DefaultFailed));
  ClosePanel();return;
 }
 case IdSpacePreview:{
  // The Space hook only works while Vetro Look is running, so the switch is a
  // background autostart entry rather than a setting inside the app.
  bool turningOn=!autostart;
  autostart=!autostart;
  if(!SetAutostart(autostart))autostart=AutostartEnabled();
  if(autostart&&turningOn){
   // The Run entry only takes effect at the next sign-in; without this, the
   // toggle would show "On" immediately while Space silently did nothing
   // until Windows was restarted. This process (opened on a file, or the
   // library) never installs the hook itself — only a dedicated background
   // instance does — so start that instance right now instead of making the
   // user wait for a setting to actually apply.
   wchar_t exePath[MAX_PATH]{};
   if(GetModuleFileNameW(nullptr,exePath,MAX_PATH)){
    std::wstring cmd=L"\""+std::wstring(exePath)+L"\" --background";
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
   }
  }
  Wake();return;
 }
 case IdThemeDark:SetTheme(false);return;
 case IdThemeLight:SetTheme(true);return;
 case IdWheelZoom:SetWheelMode(0);return;
 case IdWheelNav:SetWheelMode(1);return;
 case IdTotalCmd:{
  std::wstring note;
  if(tcStatus==TcInstalled)TotalCommanderRemove(note);else TotalCommanderInstall(win,note);
  tcStatus=TotalCommanderStatus();
  if(!note.empty())Notify(note);
  Wake();return;
 }
 case IdLanguage:menuLevel=1;levelSlide.Reset(0);levelSlide.To(1);Wake();return;
 case IdLangBack:menuLevel=0;levelSlide.Reset(1);levelSlide.To(0);Wake();return;
 case IdLangRu:SetLanguage(0);return;
 case IdLangEn:SetLanguage(1);return;
 case IdToolCrop:SetTool(ToolCrop);return;
 case IdToolRotate:tool=(tool==ToolRotate)?ToolNone:ToolRotate;Wake();return;
 case IdToolDraw:SetTool(ToolDraw);return;
 case IdToolArrow:SetTool(ToolArrow);return;
 case IdToolSelect:SetTool(ToolSelect);return;
 case IdRotLeft:Turn(-1);return;
 case IdRotRight:Turn(1);return;
 case IdCropFree:case IdCrop11:case IdCrop43:case IdCrop32:case IdCrop169:ApplyAspect(id-IdCropFree);return;
 case IdCropCancel:hasCrop=false;tool=ToolNone;Fit();Wake();return;
 case IdCropApply:tool=ToolNone;Fit();Notify(T(S_Apply));Wake();return;
 case IdUndo:if(!strokes.empty())strokes.pop_back();Wake();return;
 case IdThin:thickness=2;Wake();return;
 case IdMedium:thickness=4;Wake();return;
 case IdThick:thickness=9;Wake();return;
 case IdHistRGB:case IdHistLuma:case IdHistR:case IdHistG:case IdHistB:histMode=id-IdHistRGB;Wake();return;
 case IdScopeHist:scopeMode=0;Wake();return;
 case IdScopeVector:scopeMode=1;Wake();return;
 case IdClipHigh:clipHigh=!clipHigh;clipHighFade.To(clipHigh?1.f:0.f);Wake();return;
 case IdClipLow:clipLow=!clipLow;clipLowFade.To(clipLow?1.f:0.f);Wake();return;
 default:break;
 }
 if(id>=IdSwatch0&&id<=IdSwatch5){swatch=id-IdSwatch0;Wake();return;}
 if(id>=IdGallery0){
  size_t index=size_t(id-IdGallery0);
  if(index<siblings.size()&&siblings[index]!=currentPath)Open(siblings[index]);
  return;
 }
 (void)w;(void)h;
}
int CropHandle(float x,float y){
 if(!hasCrop||tool!=ToolCrop)return -1;
 auto s=ToScreen(crop);
 const float positions[8][2]={{0,0},{.5f,0},{1,0},{1,.5f},{1,1},{.5f,1},{0,1},{0,.5f}};
 for(int i=0;i<8;i++){
  float hx=s.left+Width(s)*positions[i][0],hy=s.top+Height(s)*positions[i][1];
  if(fabsf(x-hx)<12&&fabsf(y-hy)<12)return i;
 }
 if(Inside(s,x,y))return 8;
 return -1;
}
void DragCrop(int handle,D2D1_POINT_2F point,D2D1_POINT_2F previous){
 float maxW=float(DisplayW()),maxH=float(DisplayH());
 if(handle==8){
  float dx=point.x-previous.x,dy=point.y-previous.y;
  dx=Clamp(dx,-crop.left,maxW-crop.right);
  dy=Clamp(dy,-crop.top,maxH-crop.bottom);
  crop.left+=dx;crop.right+=dx;crop.top+=dy;crop.bottom+=dy;
  return;
 }
 if(handle==0||handle==6||handle==7)crop.left=Clamp(point.x,0,crop.right-24);
 if(handle==2||handle==3||handle==4)crop.right=Clamp(point.x,crop.left+24,maxW);
 if(handle==0||handle==1||handle==2)crop.top=Clamp(point.y,0,crop.bottom-24);
 if(handle==4||handle==5||handle==6)crop.bottom=Clamp(point.y,crop.top+24,maxH);
 if(cropAspect){
  const float ratios[5]={0,1.f,4.f/3.f,3.f/2.f,16.f/9.f};
  float ratio=ratios[cropAspect];
  float width=Width(crop);
  float height=width/ratio;
  if(handle==0||handle==1||handle==2)crop.top=crop.bottom-height;else crop.bottom=crop.top+height;
  crop.top=Clamp(crop.top,0,maxH-24);crop.bottom=Clamp(crop.bottom,crop.top+24,maxH);
 }
}
void MouseDown(float x,float y){
 int id=HitTest(x,y);
 pressed=id;
 if(id!=IdNone)B(id).press.To(1);
 if(id==IdTrack){
  sliderGrab=true;thumbLift.To(1);
  auto track=R(IdTrack);
  SetZoom(SliderToZoom((x-track.left)/(std::max)(1.f,Width(track))),0,0);
  return;
 }
 if(id==IdGalleryBody||(id>=IdGallery0)){
  galleryDragging=true;galleryGrabX=x;galleryGrabScroll=galleryScroll;galleryGrabTime=Now();galleryVel=0;
  return;
 }
 if(id==IdPanelBody||id!=IdNone)return;
 // Nothing floating was hit: the click belongs to the picture. An armed
 // editing tool keeps its panel open so the controls stay to hand.
 if(panel!=PanelNone&&tool==ToolNone){ClosePanel();return;}
 auto point=ToDisplay(x,y);
 if(tool==ToolCrop){
  cropGrab=CropHandle(x,y);
  if(cropGrab>=0){painting=true;return;}
 }
 if(tool==ToolDraw||tool==ToolArrow||tool==ToolSelect){
  live=Stroke{};
  live.colour=swatches[swatch];
  live.width=(std::max)(.5f,thickness/(std::max)(.001f,Zoom()));
  live.kind=tool==ToolArrow?1:(tool==ToolSelect?shapeKind:0);
  live.pts.push_back(point);live.pts.push_back(point);
  painting=true;return;
 }
 zoomAnchored=false;Snap();dragImage=true;
 SetCapture(win);
}
void MouseMove(float x,float y){
 static D2D1_POINT_2F lastDisplay{};
 auto point=ToDisplay(x,y);
 if(sliderGrab){
  auto track=R(IdTrack);
  SetZoom(SliderToZoom((x-track.left)/(std::max)(1.f,Width(track))),0,0);
  lastDisplay=point;return;
 }
 if(galleryDragging){
  galleryScroll=galleryGrabScroll+(x-galleryGrabX);
  double now=Now();
  if(now-galleryGrabTime>.001)galleryVel=float((x-galleryGrabX)/(now-galleryGrabTime));
  Wake();lastDisplay=point;return;
 }
 if(painting){
  if(tool==ToolCrop&&cropGrab>=0)DragCrop(cropGrab,point,lastDisplay);
  else if(tool==ToolDraw){
   auto& last=live.pts.back();
   if(fabsf(point.x-last.x)+fabsf(point.y-last.y)>1.2f/(std::max)(.001f,Zoom()))live.pts.push_back(point);
   else last=point;
  }
  else if(tool==ToolArrow){
   if(GetKeyState(VK_SHIFT)&0x8000){auto a=live.pts.front();float dx=point.x-a.x,dy=point.y-a.y;
    float angle=roundf(atan2f(dy,dx)/(3.14159265f/4))*(3.14159265f/4),len=hypotf(dx,dy);
    point=D2D1::Point2F(a.x+cosf(angle)*len,a.y+sinf(angle)*len);}
   live.pts.back()=point;
  }
  else if(tool==ToolSelect){
   auto start=live.pts.front();float dx=point.x-start.x,dy=point.y-start.y;
   if(GetKeyState(VK_SHIFT)&0x8000){float side=(std::max)(fabsf(dx),fabsf(dy));dx=copysignf(side,dx);dy=copysignf(side,dy);}
   live.pts.back()=D2D1::Point2F(start.x+dx,start.y+dy);
  }
  Wake();lastDisplay=point;return;
 }
 if(dragImage){
  float dx=(x-mouse.x/dpi),dy=(y-mouse.y/dpi);
  panSX.Reset(panSX.v+dx);panSY.Reset(panSY.v+dy);
  fit=false;Wake();
 }
 lastDisplay=point;
 int id=HitTest(x,y);
 if(id!=hover){
  if(hover!=IdNone)B(hover).hover.To(0);
  hover=id;
  if(hover!=IdNone)B(hover).hover.To(1);
  Wake();
 }
}
void MouseUp(float x,float y){
 if(pressed!=IdNone)B(pressed).press.To(0);
 if(sliderGrab){sliderGrab=false;thumbLift.Reset(1.f);thumbLift.To(0);Wake();}
 if(galleryDragging){galleryDragging=false;Wake();}
 else if(painting){
  if(tool==ToolDraw||tool==ToolArrow||tool==ToolSelect){
   if(live.pts.size()>1)strokes.push_back(live);
   live=Stroke{};
  }
  painting=false;cropGrab=-1;Wake();
 }
 if(dragImage){dragImage=false;ReleaseCapture();}
 int id=HitTest(x,y);
 if(id!=IdNone&&id==pressed)Command(id);
 pressed=IdNone;
}

// -------------------------------------------------------- library input ---
bool sizeDragLo=false,sizeDragHi=false;
void CloseLibraryPopups(){
 sortOpen=false;filterOpen=false;sortReveal.To(0);filterReveal.To(0);filterScrollVel=0;
}
void ApplyFilterChanges(){
 filtersActive=!filterExtOn.empty()||filterRawOnly||filterRegularOnly||filterProfile||filterSizeLo>0||filterSizeHi!=~0ull;
 RefreshLibrary();Wake();
}
void KickProfileClassification(){
 // One-shot background sweep over whatever the filter panel can currently
 // see; harmless to call repeatedly, since already-classified paths return
 // immediately from the cache.
 std::vector<std::wstring> paths;
 if(libView==ViewFolders)for(auto& f:libFolders)for(uint8_t i=0;i<f.sampleCount;i++)paths.push_back(f.samples[i]);
 else for(auto& photo:albumPhotos)paths.push_back(photo.path);
 std::thread([paths=std::move(paths)]{for(auto& p:paths)ClassifyProfile(p);}).detach();
}
void LibraryCommand(int id){
 // Commands can interrupt a transition.  Keeping this input path live also
 // protects against a renderer/device-loss frame leaving an animation flag
 // behind after its visual has disappeared.
 hero.active=false;folderTx.active=false;
 if(id>=IdCard0){
  CloseLibraryPopups();
  size_t index=size_t(id-IdCard0);
  bool showingFolders=libView==ViewFolders&&screen==ScrLibrary;
  if(showingFolders){if(index<libFolders.size())OpenAlbum(libFolders[index].path);}
  else if(index<albumPhotos.size())OpenPhotoFromAlbum(albumPhotos[index].path,albumPhotos[index].id,R(id));
  return;
 }
 if(id>=IdFilterExt0&&id<IdFilterExt0+int(std::size(LibraryExtensions))){
  auto ext=LibraryExtensions[id-IdFilterExt0];
  if(filterExtOn.empty())filterExtOn[ext]=true;
  else if(filterExtOn.count(ext))filterExtOn.erase(ext);else filterExtOn[ext]=true;
  ApplyFilterChanges();return;
 }
 switch(id){
 case IdLibBack:GoToLibrary();return;
 case IdFavourites:case IdFavouritesChip:CloseLibraryPopups();OpenFavourites();return;
 case IdLibSearch:libSearchFocused=true;CloseLibraryPopups();Wake();return;
 case IdLibSort:{bool opening=!sortOpen;CloseLibraryPopups();sortOpen=opening;sortReveal.To(opening?1.f:0.f);Wake();return;}
 case IdLibFilter:{bool opening=!filterOpen;CloseLibraryPopups();filterOpen=opening;filterReveal.To(opening?1.f:0.f);if(opening)KickProfileClassification();Wake();return;}
 case IdLibViewFolders:
  if(screen!=ScrLibrary||libView!=ViewFolders){
   screen=ScrLibrary;albumFolder.clear();favouritesOpen=false;
   libView=ViewFolders;libViewSlide.To(0);libContentDirection=-1;libContentIn.Reset(0);libContentIn.To(1);
   SaveLibView();RefreshLibrary();
  }
  Wake();return;
 case IdLibViewPhotos:
  if(screen!=ScrLibrary||libView!=ViewPhotosFlat){
   screen=ScrLibrary;albumFolder.clear();favouritesOpen=false;
   libView=ViewPhotosFlat;libViewSlide.To(1);libContentDirection=1;libContentIn.Reset(0);libContentIn.To(1);
   SaveLibView();RefreshLibrary();
  }
  Wake();return;
 case IdLibRescan:IndexRescan();Wake();return;
 case IdSortName:if(libSortField==0)libSortDesc=!libSortDesc;else{libSortField=0;libSortDesc=false;}RefreshLibrary();Wake();return;
 case IdSortDate:if(libSortField==1)libSortDesc=!libSortDesc;else{libSortField=1;libSortDesc=true;}RefreshLibrary();Wake();return;
 case IdSortSize:if(libSortField==2)libSortDesc=!libSortDesc;else{libSortField=2;libSortDesc=true;}RefreshLibrary();Wake();return;
 case IdFilterRawOnly:filterRawOnly=!filterRawOnly;if(filterRawOnly)filterRegularOnly=false;ApplyFilterChanges();return;
 case IdFilterRegularOnly:filterRegularOnly=!filterRegularOnly;if(filterRegularOnly)filterRawOnly=false;ApplyFilterChanges();return;
 case IdFilterProfileAny:filterProfile=0;ApplyFilterChanges();return;
 case IdFilterProfileSRGB:filterProfile=1;ApplyFilterChanges();return;
 case IdFilterProfileP3:filterProfile=2;ApplyFilterChanges();return;
 case IdFilterProfileAdobe:filterProfile=3;ApplyFilterChanges();return;
 case IdFilterProfileNone:filterProfile=4;ApplyFilterChanges();return;
 case IdFilterClear:filterExtOn.clear();filterRawOnly=filterRegularOnly=false;filterProfile=0;filterSizeLo=0;filterSizeHi=~0ull;ApplyFilterChanges();return;
 case IdFilterApply:filterOpen=false;filterReveal.To(0);ApplyFilterChanges();return;
 case IdWinMin:ShowWindow(win,SW_MINIMIZE);return;
 case IdWinMax:SendMessageW(win,WM_SYSCOMMAND,WindowMaximized()?SC_RESTORE:SC_MAXIMIZE,0);return;
 case IdWinClose:Close();return;
 default:break;
 }
}
void LibraryMouseDown(float x,float y){
 selectionGesture=false;
 int id=HitTest(x,y);
 pressed=id;
 if(id!=IdNone)B(id).press.To(1);
 if(id==IdFilterSizeLo){sizeDragLo=true;return;}
 if(id==IdFilterSizeHi){sizeDragHi=true;return;}
 if(id>=IdCard0){
  size_t index=size_t(id-IdCard0);
  bool showingFolders=libView==ViewFolders&&screen==ScrLibrary;
  if(!showingFolders&&index<albumPhotos.size()){
   bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
   auto photoId=albumPhotos[index].id;selectionGesture=control||shift;
   if(shift&&selectionAnchor){
    auto anchor=std::find_if(albumPhotos.begin(),albumPhotos.end(),[](auto& p){return p.id==selectionAnchor;});
    if(anchor!=albumPhotos.end()){if(!control)selectedPhotos.clear();size_t from=size_t(anchor-albumPhotos.begin());for(size_t i=(std::min)(from,index);i<=(std::max)(from,index);++i)selectedPhotos.insert(albumPhotos[i].id);}
   }else if(control){if(!selectedPhotos.erase(photoId))selectedPhotos.insert(photoId);selectionAnchor=photoId;}
   else if(!selectedPhotos.count(photoId)){selectedPhotos.clear();selectedPhotos.insert(photoId);selectionAnchor=photoId;}
   // A card click may turn into a native file drag; both start the same way,
   // so the distinction is only made once the mouse actually moves.
   dragArmed=true;dragActive=false;dragOrigin={LONG(x*dpi),LONG(y*dpi)};dragPath=albumPhotos[index].path;
  }
  return;
 }
 if(id!=IdLibSearch)libSearchFocused=false;
 if(id==IdNone)CloseLibraryPopups();
}
void LibraryMouseMove(float x,float y){
 if(sizeDragLo||sizeDragHi){
  auto body=R(IdFilterPopup);
  float trackLeft=body.left+20,trackRight=body.right-20;
  float t=Clamp((x-trackLeft)/(std::max)(1.f,trackRight-trackLeft),0,1);
  uint64_t v=TToSize(t);
  if(sizeDragLo)filterSizeLo=(std::min)(v,filterSizeHi==~0ull?v:filterSizeHi);
  else filterSizeHi=(std::max)(v,filterSizeLo);
  Wake();return;
 }
 if(dragArmed&&!dragActive){
  int threshX=GetSystemMetrics(SM_CXDRAG),threshY=GetSystemMetrics(SM_CYDRAG);
  if(abs(int(x*dpi)-dragOrigin.x)>threshX||abs(int(y*dpi)-dragOrigin.y)>threshY){
   dragActive=true;dragArmed=false;
   std::vector<std::wstring> paths;for(auto& photo:albumPhotos)if(selectedPhotos.count(photo.id))paths.push_back(photo.path);
   if(paths.empty())paths.push_back(dragPath);
   if(pressed!=IdNone)B(pressed).press.To(0);
   ReleaseCapture();BeginFileDrag(paths);
   dragActive=false;pressed=IdNone;selectionGesture=false;Wake();
  }
  return;
 }
 int id=HitTest(x,y);
 if(id!=hover){if(hover!=IdNone)B(hover).hover.To(0);hover=id;if(hover!=IdNone)B(hover).hover.To(1);Wake();}
}
void LibraryMouseUp(float x,float y){
 if(pressed!=IdNone)B(pressed).press.To(0);
 sizeDragLo=sizeDragHi=false;
 dragArmed=false;
 int id=HitTest(x,y);
 if(!dragActive&&!selectionGesture&&id!=IdNone&&id==pressed)LibraryCommand(id);
 pressed=IdNone;
}

// ---------------------------------------------------------------- shell ----
LRESULT CALLBACK Keyboard(int code,WPARAM wp,LPARAM lp){
 if(code==HC_ACTION){
  auto key=(KBDLLHOOKSTRUCT*)lp;static bool held=false;
  if(key->vkCode==VK_SPACE){
   if(wp==WM_KEYUP||wp==WM_SYSKEYUP){bool consumed=held;held=false;if(consumed)return 1;}
   else if(held)return 1;
   else if(wp==WM_KEYDOWN&&!held&&!(GetAsyncKeyState(VK_CONTROL)&0x8000)&&!(GetAsyncKeyState(VK_MENU)&0x8000)&&!(GetAsyncKeyState(VK_SHIFT)&0x8000)){
    HWND fg=GetForegroundWindow();
    bool canPreview=ExplorerCanPreview(fg);QuickLog(canPreview?L"Space accepted from Explorer":L"Space ignored: foreground is not a previewable Explorer view");
    if(canPreview){held=true;PostMessageW(win,Preview,0,(LPARAM)fg);return 1;}
   }
  }
 }
 return CallNextHookEx(hook,code,wp,lp);
}
// Three passes over a real folder, through the real renderer.
//
//  1 cold      — every frame decoded for the first time, prefetch running
//  2 warm      — the same walk again, now that the cache holds screen frames
//  3 back/forth— N to N+1 and back, twenty times, which is the motion that
//                should be indistinguishable from an already-open gallery
// Three passes over a real folder, through the real renderer.
//
//  0 cold      — every frame decoded for the first time, prefetch running
//  1 warm      — the same walk again, now that the cache holds screen frames
//  2 steady    — a long randomised walk of `navBenchSteps` transitions, which
//                is where rare stalls live: a median says nothing about the one
//                transition in two hundred that the reader actually notices
//
// Every transition carries an attribution, so a slow one can name its cause
// instead of leaving it to be guessed at.
void NavBenchTick(){
 if(navFiles.empty()){DestroyWindow(win);return;}
 // The cold pass waits for each frame to finish; the later passes do not, since
 // waiting would hide exactly the latency they exist to measure.
 if(navPhase==0&&loading&&++navSettle<400)return;
 navSettle=0;

 auto record=[&](int phase){
  NavSample s;
  s.ms=navLastOpenMs;
  s.phase=phase;
  s.file=fs::path(currentPath).filename().wstring();
  s.tier=navLastTrace.tier;
  s.gpuResident=navLastTrace.gpuResident;
  s.gpuReused=navLastTrace.gpuReused;
  s.gpuCreated=navLastTrace.gpuCreated;
  s.cacheLookupMs=navLastTrace.cacheLookupMs;
  s.gpuMs=navLastTrace.gpuMs;
  s.lockWaitMs=navLastTrace.lockWaitMs;
  s.evictions=navLastTrace.evictionsDuring;
  s.directionChanged=navLastTrace.directionChanged;
  s.viewportChanged=navLastTrace.viewportChanged;
  navSamples.push_back(std::move(s));
 };
 auto timedOpen=[&](const std::wstring& path){
  auto start=std::chrono::steady_clock::now();
  Open(path);
  // Present the frame too: a swap nobody drew is not a navigation.
  if(GfxReady())Frame();
  navLastOpenMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 };

 if(navPhase==0){
  if(navIndex>0)record(0);
  if(navIndex>=int(navFiles.size())){navPhase=1;navIndex=0;}
  else{timedOpen(navFiles[size_t(navIndex++)]);return;}
 }
 if(navPhase==1){
  if(navIndex>0)record(1);
  if(navIndex>=int(navFiles.size())){navPhase=2;navIndex=0;navRepeat=0;}
  else{timedOpen(navFiles[size_t(navIndex++)]);return;}
 }
 if(navPhase==2){
  if(navRepeat>0)record(2);
  if(navRepeat>=navBenchSteps||navFiles.size()<2){navPhase=3;navIndex=0;navRepeat=0;}
  else{
   // A walk with occasional reversals and occasional jumps: pure back-and-forth
   // between two files only ever exercises the two resident textures, and pure
   // forward motion never tests a direction change.
   static uint32_t seed=12345;
   seed=seed*1664525u+1013904223u;
   unsigned roll=(seed>>16)%100;
   if(roll<70)navIndex+=1;                                   // keep going
   else if(roll<90)navIndex-=1;                              // change of mind
   else navIndex+=int((seed>>8)%17)-8;                       // a jump
   int count=int(navFiles.size());
   navIndex=((navIndex%count)+count)%count;
   navRepeat++;
   timedOpen(navFiles[size_t(navIndex)]);
   return;
  }
 }
 if(navPhase==3){
  // Sequential, wrapping. This is the motion the prefetcher is built for and
  // the one a reader actually performs; the randomised pass above is the
  // adversarial case, kept because that is where rare stalls show up.
  if(navRepeat>0)record(3);
  if(navRepeat<navBenchSteps&&navFiles.size()>1){
   navIndex=(navIndex+1)%int(navFiles.size());
   navRepeat++;
   timedOpen(navFiles[size_t(navIndex)]);
   return;
  }
 }

 // ---- report ------------------------------------------------------------
 auto percentile=[](std::vector<double>& sorted,double p)->double{
  if(sorted.empty())return 0;
  size_t index=size_t(p*double(sorted.size()-1)+.5);
  return sorted[(std::min)(index,sorted.size()-1)];
 };
 auto summarise=[&](int phase,const wchar_t* name){
  std::vector<double> values;
  size_t gpuReady=0,screenHit=0,fullNav=0,total=0;
  for(auto& s:navSamples)if(s.phase==phase){
   values.push_back(s.ms);total++;
   if(s.gpuResident)gpuReady++;
   if(s.tier==TierScreen)screenHit++;
   if(s.tier==TierFull)fullNav++;
  }
  if(values.empty())return;
  std::sort(values.begin(),values.end());
  double sum=0;for(double v:values)sum+=v;
  Log(std::wstring(L"NAVBENCH ")+name+
      L" samples="+std::to_wstring(total)+
      L" p50="+std::to_wstring(percentile(values,.50))+
      L" p95="+std::to_wstring(percentile(values,.95))+
      L" p99="+std::to_wstring(percentile(values,.99))+
      L" max="+std::to_wstring(values.back())+
      L" mean="+std::to_wstring(sum/double(values.size()))+
      L" gpu_ready="+std::to_wstring(gpuReady)+L"/"+std::to_wstring(total)+
      L" screen_cache_hit="+std::to_wstring(screenHit)+L"/"+std::to_wstring(total)+
      L" full_tier_navigations="+std::to_wstring(fullNav)+L"/"+std::to_wstring(total));
 };
 summarise(0,L"cold");
 summarise(1,L"warm");
 summarise(2,L"random");
 summarise(3,L"sequential");

 // Every transition over one frame at 60 Hz, with its cause.
 size_t slow=0;
 for(auto& s:navSamples){
  if(s.ms<=16.0)continue;
  slow++;
  std::wstring cause;
  if(s.tier==TierMiss)cause=L"screen-cache-miss";
  else if(s.tier==TierWarm||s.tier==TierThumb)cause=L"low-tier-only";
  else if(s.gpuCreated)cause=L"gpu-texture-created";
  else if(s.gpuReused)cause=L"gpu-upload";
  else if(s.evictions)cause=L"eviction";
  else if(s.viewportChanged)cause=L"viewport-resize";
  else if(s.lockWaitMs>1.0)cause=L"lock-contention";
  else if(s.directionChanged)cause=L"direction-change";
  else cause=L"other";
  Log(L"NAVSLOW phase="+std::to_wstring(s.phase)+
      L" ms="+std::to_wstring(s.ms)+
      L" cause="+cause+
      L" tier="+std::to_wstring(s.tier)+
      L" lookupMs="+std::to_wstring(s.cacheLookupMs)+
      L" gpuMs="+std::to_wstring(s.gpuMs)+
      L" lockMs="+std::to_wstring(s.lockWaitMs)+
      L" evict="+std::to_wstring(s.evictions)+
      L" dirChange="+std::to_wstring(s.directionChanged?1:0)+
      L" resize="+std::to_wstring(s.viewportChanged?1:0)+
      L" file="+s.file);
 }
 Log(L"NAVBENCH slow_transitions="+std::to_wstring(slow)+L"/"+std::to_wstring(navSamples.size()));
 CacheLogTelemetry();
 Log(L"NAVBENCH peak_working_set_mb="+std::to_wstring(ProcessWorkingSet()/(1024*1024)));
 KillTimer(win,7);
 DestroyWindow(win);
}
void TestTick(){
 if(++testWait>200){Log(L"FAIL timeout");testFailures++;DestroyWindow(win);return;}
 // A preview makes the window responsive but is not completion. Runtime tests
 // must wait for the atomic full-resolution replacement or they can cancel it
 // by opening the next fixture and report a false PASS on the JPEG preview.
 if(loading||showingPreview)return;
 // Loaded resets the device bitmap; give the render loop one frame to upload
 // the final pixels before asserting that the image was actually rendered.
 if(current&&!bitmap){Wake();return;}
 static bool assetModelTested=false;
 if(!assetModelTested){
  assetModelTested=true;auto saved=albumPhotos;auto savedVariants=assetVariants;
  albumPhotos={{L"D:\\album\\IMG_1.NEF",L"IMG_1.NEF",L"nef",1,1,1},{L"D:\\album\\IMG_1.JPG",L"IMG_1.JPG",L"jpg",1,1,2},
   {L"D:\\album\\IMG_2.JPG",L"IMG_2.JPG",L"jpg",1,1,3},{L"D:\\album\\IMG_2_edit.JPG",L"IMG_2_edit.JPG",L"jpg",1,1,4},
   {L"D:\\album\\IMG_3.JPG",L"IMG_3.JPG",L"jpg",1,1,5}};
  BuildAssetStacks();bool ok=albumPhotos.size()==3&&assetVariants.size()==2;
  if(ok)Log(L"PASS conservative RAW/JPEG and edit Asset stacks");else{Log(L"FAIL Asset stacks");testFailures++;}
  albumPhotos=std::move(saved);assetVariants=std::move(savedVariants);
 }
 static bool timelineTested=false;
 if(!timelineTested){
  timelineTested=true;auto saved=albumPhotos;auto savedGroups=timelineGroups;
  SYSTEMTIME a{};a.wYear=2026;a.wMonth=9;a.wDay=7;a.wHour=12;
  SYSTEMTIME b=a;b.wDay=6;FILETIME fa{},fb{};SystemTimeToFileTime(&a,&fa);SystemTimeToFileTime(&b,&fb);
  auto ticks=[](const FILETIME& value){return uint64_t(value.dwLowDateTime)|(uint64_t(value.dwHighDateTime)<<32);};
  albumPhotos={{L"a",L"a",L"jpg",1,ticks(fa),1},{L"b",L"b",L"jpg",1,ticks(fb),2},{L"c",L"c",L"jpg",1,ticks(fa),3}};
  RebuildTimeline();
  bool ok=timelineGroups.size()==2&&timelineGroups[0].photos.size()==2&&timelineGroups[1].photos.size()==1;
  if(ok)Log(L"PASS date timeline groups photos by local calendar day");else{Log(L"FAIL date timeline grouping");testFailures++;}
  albumPhotos=std::move(saved);timelineGroups=std::move(savedGroups);
 }
 // The moment a preview is replaced by a better tier is the one a reader
 // notices: the photograph must not jump, rescale, rotate or blink. This
 // drives the same code the Loaded handler runs, at fit and while zoomed, and
 // compares the rectangle the image actually occupies on screen.
 // Source and display are two different things, and the places that must not
 // confuse them are the header (which reports the photograph) and Save (which
 // writes it). This drives both against a real 36 megapixel fixture.
 static bool tierSplitTested=false;
 if(!tierSplitTested){
  tierSplitTested=true;
  auto fixture=fs::path(L"tests/fixtures/bench-36mp.jpg");
  std::error_code ec;
  if(!fs::exists(fixture,ec)){
   wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
   fixture=fs::path(exe).parent_path()/L".."/L"VetroView"/L"tests"/L"fixtures"/L"bench-36mp.jpg";
  }
  if(!fs::exists(fixture,ec))Log(L"SKIP tier split (bench-36mp.jpg not found)");
  else{
   auto savedCurrent=current;auto savedPath=currentPath;auto savedFit=fit;
   bool savedCrop=hasCrop;auto savedStrokes=strokes;float savedRotate=rotate.target;
   currentPath=fixture.wstring();hasCrop=false;strokes.clear();rotate.Reset(0);

   auto screen=DecodeScreen(currentPath,2560,{});
   if(!screen){Log(L"FAIL tier split: screen decode");testFailures++;}
   else{
    current=screen;fit=true;
    bool smaller=current->w<current->SourceW();
    if(smaller)Log(L"PASS the displayed frame is smaller than the asset");
    else{Log(L"FAIL screen tier was not smaller than the source");testFailures++;}

    // The header must report the photograph, not the tier.
    if(SourceW()==7360&&SourceH()==4912)
     Log(L"PASS header reports original dimensions from a screen-tier frame");
    else{Log(L"FAIL header dimensions "+std::to_wstring(SourceW())+L"x"+std::to_wstring(SourceH()));testFailures++;}
    if(DisplayW()==current->w&&DisplayW()!=SourceW())
     Log(L"PASS display space still describes the decoded frame");
    else{Log(L"FAIL display space");testFailures++;}

    // At fit, a screen frame must not be asking for full resolution.
    if(!NeedsFullResolution())Log(L"PASS fit does not require the full tier");
    else{Log(L"FAIL fit asked for full resolution");testFailures++;}
    // Merely enlarging the window raises Zoom() without wanting more detail
    // than the asset holds, so it must NOT drag in a full decode.
    fit=false;zoomLog.Reset(logf(1.6f));
    if(!NeedsFullResolution())Log(L"PASS enlarging the view does not demand full resolution");
    else{Log(L"FAIL a window-sized zoom asked for full resolution");testFailures++;}
    // 1:1 against the asset does.
    zoomLog.Reset(logf(1.f/(std::max)(0.0001f,FrameScale())));
    if(NeedsFullResolution())Log(L"PASS 1:1 against the original asks for full resolution");
    else{Log(L"FAIL 1:1 did not ask for full resolution");testFailures++;}
    fit=true;zoomLog.Reset(0);

    // Save must write the photograph, whatever is on screen.
    auto flattened=Composite();
    if(flattened&&flattened->w==7360&&flattened->h==4912)
     Log(L"PASS Save composites at original resolution from a screen-tier view");
    else{
     Log(L"FAIL Save resolution "+(flattened?std::to_wstring(flattened->w)+L"x"+std::to_wstring(flattened->h):L"none"));
     testFailures++;
    }

    // ... and a crop authored on the displayed frame must land on the asset
    // at the same place, scaled up.
    hasCrop=true;tool=ToolNone;
    crop=D2D1::RectF(0,0,float(current->w)/2.f,float(current->h)/2.f);
    auto cropped=Composite();
    float ratio=float(current->SourceW())/float(current->w);
    unsigned wantW=unsigned(float(current->w)/2.f*ratio+.5f);
    if(cropped&&abs(int(cropped->w)-int(wantW))<=2)
     Log(L"PASS crop coordinates scale from display space to the asset");
    else{
     Log(L"FAIL crop scaling "+(cropped?std::to_wstring(cropped->w):L"none")+L" wanted "+std::to_wstring(wantW));
     testFailures++;
    }
    hasCrop=false;

    // The other half of the contract: browsing must not develop full
    // resolution, but asking for it must actually deliver it. This drives the
    // real request through the real worker and waits for the swap.
    fit=true;zoomLog.Reset(0);
    siblings.clear();siblings.push_back(currentPath);
    currentFrameKey.clear();
    RequestFullResolution();
    bool arrived=false;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    MSG pump{};
    while(std::chrono::steady_clock::now()<deadline){
     while(PeekMessageW(&pump,nullptr,0,0,PM_REMOVE)){
      if(pump.message==WM_QUIT)break;
      TranslateMessage(&pump);DispatchMessageW(&pump);
     }
     if(current&&current->tier==TierFullRes&&current->w==7360){arrived=true;break;}
     Sleep(20);
    }
    if(arrived)Log(L"PASS asking for full resolution delivers the original pixels");
    else{
     Log(L"FAIL full resolution never arrived (tier="+std::to_wstring(current?current->tier:-1)+
         L" w="+std::to_wstring(current?current->w:0)+L")");
     testFailures++;
    }
   }
   current=savedCurrent;currentPath=savedPath;fit=savedFit;
   hasCrop=savedCrop;strokes=savedStrokes;rotate.Reset(savedRotate);
   bitmap.Reset();
  }
 }
 static bool refineTested=false;
 if(!refineTested&&GfxReady()){
  refineTested=true;
  auto savedCurrent=current;auto savedFit=fit;
  float savedZoom=zoomLog.target,savedPanX=panSX.target,savedPanY=panSY.target;
  float savedRotate=rotate.target;

  auto frame=[](unsigned w,unsigned h){
   auto image=std::make_shared<Image>();image->w=w;image->h=h;image->pixels.assign(size_t(w)*h*4,128);
   return image;
  };
  // A 2760x1842 screen frame replaced by the 7360x4912 original: the same
  // photograph at two tiers, which is exactly what the worker delivers.
  auto preview=frame(2760,1842),full=frame(7360,4912);
  auto rectOf=[&]{
   auto m=ImageMatrix();
   auto a=m.TransformPoint(D2D1::Point2F(0,0));
   auto b=m.TransformPoint(D2D1::Point2F(float(current->w),float(current->h)));
   return D2D1::RectF((std::min)(a.x,b.x),(std::min)(a.y,b.y),(std::max)(a.x,b.x),(std::max)(a.y,b.y));
  };
  // This mirrors the Loaded handler exactly; if that changes, this must too.
  auto swapIn=[&](const std::shared_ptr<Image>& arriving){
   auto shown=current;
   current=arriving;
   bool refined=shown&&current&&shown!=current&&shown->w&&current->w&&!fit;
   if(refined)zoomLog.Reset(zoomLog.target+logf(float(shown->w)/float(current->w)));
   else if(fit){Fit();Snap();}
  };
  auto compare=[&](const D2D1_RECT_F& a,const D2D1_RECT_F& b,float tolerance){
   return fabsf(a.left-b.left)<tolerance&&fabsf(a.top-b.top)<tolerance&&
          fabsf(a.right-b.right)<tolerance&&fabsf(a.bottom-b.bottom)<tolerance;
  };

  // 1. At fit.
  rotate.Reset(0);fit=true;current=preview;Fit();Snap();
  auto before=rectOf();
  swapIn(full);
  auto after=rectOf();
  if(compare(before,after,1.5f))Log(L"PASS preview to full does not move the image at fit");
  else{Log(L"FAIL preview to full moved the image at fit");testFailures++;}

  // 2. Zoomed in and panned, which is where a naive swap rescales.
  fit=false;current=preview;
  zoomLog.Reset(logf(1.6f));panSX.Reset(83);panSY.Reset(-47);Snap();
  before=rectOf();
  swapIn(full);
  after=rectOf();
  if(compare(before,after,2.0f))Log(L"PASS preview to full holds zoom and pan");
  else{Log(L"FAIL preview to full changed zoom or pan");testFailures++;}

  // 3. Orientation must survive the swap: both tiers arrive already oriented,
  // so the displayed aspect cannot change.
  bool aspectHeld=fabsf((float(preview->w)/preview->h)-(float(full->w)/full->h))<0.01f;
  if(aspectHeld)Log(L"PASS both tiers carry the same orientation and aspect");
  else{Log(L"FAIL tier aspect mismatch");testFailures++;}

  // 4. No blank frame: a swap must never leave the renderer without pixels
  // before the next present.
  current=preview;bitmap.Reset();
  auto key=NormalisePath(currentPath)+L"|refine-test";
  auto texture=GpuTextureFor(current,key,key);
  if(texture)Log(L"PASS the arriving tier has a texture before it is drawn");
  else{Log(L"FAIL arriving tier had no texture");testFailures++;}

  current=savedCurrent;fit=savedFit;
  zoomLog.Reset(savedZoom);panSX.Reset(savedPanX);panSY.Reset(savedPanY);rotate.Reset(savedRotate);
  bitmap.Reset();
 }
 static bool cachePolicyTested=false;
 if(!cachePolicyTested){
  cachePolicyTested=true;
  auto b8=CacheBudgetFor(6*CacheGB,8*CacheGB,300*CacheMB);
  auto b16=CacheBudgetFor(12*CacheGB,16*CacheGB,500*CacheMB);
  auto b32=CacheBudgetFor(30*CacheGB,32*CacheGB,700*CacheMB);
  auto low=CacheBudgetFor(1*CacheGB,8*CacheGB,200*CacheMB);
  bool policy=low>=CacheLowMin&&low<=CacheLowMax&&b8>=256*CacheMB&&b8<=1*CacheGB&&
   b16>=1*CacheGB&&b16<=2*CacheGB&&b32>=2*CacheGB&&b32<=CacheHardCap;
  auto before=prefetchEpoch.load();CacheHandlePressure(currentPath);
  policy&=prefetchEpoch.load()>before;
  if(policy)Log(L"PASS RAM cache policy simulated 8/16/32 GB and low-memory cancellation");
  else{Log(L"FAIL RAM cache policy");testFailures++;}
 }
 if(testStage<int(testFiles.size())){
  if(testStage>0){
   if(!current||!bitmap){Log(L"FAIL decode/render");testFailures++;}
   else Log(L"PASS rendered "+currentPath+L" codec="+current->codec+L" decode_ms="+std::to_wstring(current->ms));
  }
  Open(testFiles[testStage++]);return;
 }
 if(testStage==int(testFiles.size())){
  if(!current||!bitmap){Log(L"FAIL final decode/render");testFailures++;}
 else Log(L"PASS rendered "+currentPath+L" codec="+current->codec);
  Fit();Snap();
  float canvasW,canvasH;Size(canvasW,canvasH);
  float fillError=(std::min)(fabsf(EffW()*Zoom()-canvasW),fabsf(EffH()*Zoom()-canvasH));
  // Tiny synthetic fixtures can legitimately hit the user-facing maximum
  // zoom before their short edge reaches the canvas.
  if(fillError<.1f||fabsf(Zoom()-MaxZoom)<.001f)Log(L"PASS fit reaches canvas edge or zoom limit");
  else{Log(L"FAIL canvas fit");testFailures++;}
  float renderDpiX=0,renderDpiY=0;Dc()->GetDpi(&renderDpiX,&renderDpiY);
  if(fabsf(renderDpiX-96*dpi)<.01f&&fabsf(renderDpiY-96*dpi)<.01f)Log(L"PASS renderer DPI matches input");
  else{Log(L"FAIL renderer DPI");testFailures++;}
  // Keep the same image point under a non-central cursor during every frame,
  // including a second wheel event before the first spring has settled.
  zoomLog.Reset(logf(1.f));panSX.Reset(31);panSY.Reset(-17);
  const float ax=123,ay=-79,ix=ax-panSX.v,iy=ay-panSY.v;
  SetZoom(1.7f,ax,ay);float anchorError=0;
  for(int frame=0;frame<160;frame++){
   if(frame==4)SetZoom(2.2f,ax,ay);
   Tick(1.f/120.f);
   anchorError=(std::max)(anchorError,fabsf(ix*Zoom()+panSX.v-ax)+fabsf(iy*Zoom()+panSY.v-ay));
  }
  if(anchorError<.002f)Log(L"PASS animated cursor anchor, repeated wheel");
  else{Log(L"FAIL animated cursor anchor");testFailures++;}
  Fit();Snap();
  // The interactive suite asserts zoom semantics, independently of the
  // user's persisted scrolling preference.
  wheelMode=0;wheelMix.Reset(0);wheelCarry=0;
  float before=Zoom();
  // If a tiny fixture is already at MaxZoom, use the opposite direction so
  // this still verifies that the wheel changes zoom rather than clamping.
  short wheelDelta=before>=MaxZoom-.001f?short(-WHEEL_DELTA):short(WHEEL_DELTA);
  SendMessageW(win,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(wheelDelta)),MAKELPARAM(300,300));
  if(fabsf(zoomLog.target-logf(before))>.0001f)Log(L"PASS wheel zoom");else{testFailures++;Log(L"FAIL zoom");}
  float px=panSX.v,py=panSY.v;
  SendMessageW(win,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(400,300));
  SendMessageW(win,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(450,340));
  SendMessageW(win,WM_LBUTTONUP,0,MAKELPARAM(450,340));
  if(panSX.v!=px&&panSY.v!=py)Log(L"PASS pan changed both axes");else{Log(L"FAIL pan");testFailures++;}
  SetWindowPos(win,nullptr,0,0,1100,760,SWP_NOMOVE|SWP_NOZORDER);
  Log(L"PASS resize requested");testStage++;return;
 }
 if(testStage==int(testFiles.size())+1){
  RECT rect;GetClientRect(win,&rect);
  if(rect.right==1100&&rect.bottom==760)Log(L"PASS resized client dimensions");
  else{Log(L"FAIL resize dimensions");testFailures++;}
  auto old=currentPath;
  SendMessageW(win,WM_KEYDOWN,VK_RIGHT,0);
  if(currentPath!=old)Log(L"PASS sibling navigation");else{Log(L"FAIL navigation");testFailures++;}
  testStage++;return;
 }
 if(testStage==int(testFiles.size())+2){
  AnimateWindow(true);testStage++;return;
 }
 if(testStage==int(testFiles.size())+3){
  if(windowAnimating)return;
  if(WindowMaximized())Log(L"PASS animated maximise");else{Log(L"FAIL maximise");testFailures++;}
  AnimateWindow(false);testStage++;return;
 }
 if(testStage==int(testFiles.size())+4){
  if(windowAnimating)return;
  RECT rect{};GetClientRect(win,&rect);
  if(!WindowMaximized()&&rect.right==1100&&rect.bottom==760)Log(L"PASS animated restore preserves bounds");
  else{Log(L"FAIL restore bounds");testFailures++;}
  testStage++;return;
 }
 if(testStage==int(testFiles.size())+5){
  // Regression: a stale shared-element transition used to make every
  // library control ignore clicks.  Commands must cancel decoration and run.
  folderTx.active=true;hero.active=true;libSearchFocused=false;
  LibraryCommand(IdLibSearch);
  if(!folderTx.active&&!hero.active&&libSearchFocused)Log(L"PASS library input interrupts transitions");
  else{Log(L"FAIL transition blocked library input");testFailures++;}
  testStage++;return;
 }
 Log(L"DONE failures="+std::to_wstring(testFailures));
 DestroyWindow(win);
}
LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
 if(msg==WM_KEYDOWN||msg==WM_MOUSEWHEEL||msg==WM_LBUTTONDOWN||
  (msg==WM_MOUSEMOVE&&(GET_X_LPARAM(lp)!=mouse.x||GET_Y_LPARAM(lp)!=mouse.y))){
  lastInteraction=Now();chrome.To(1);Wake();
 }
 switch(msg){
 case WM_CREATE:win=hwnd;return 0;
 case WM_SYSCOMMAND:
  if((wp&0xfff0)==SC_MAXIMIZE){AnimateWindow(true);return 0;}
  if((wp&0xfff0)==SC_RESTORE&&WindowMaximized()){AnimateWindow(false);return 0;}
  break;
 case WM_NCACTIVATE:return TRUE;
 case WM_NCLBUTTONDOWN:
  if(wp==HTCAPTION&&customMax){customMax=false;SetWindowPos(win,nullptr,customRestore.left,customRestore.top,customRestore.right-customRestore.left,customRestore.bottom-customRestore.top,SWP_NOZORDER);}
  break;
 case WM_NCPAINT:return 0;
 case WM_NCCALCSIZE:
  if(wp){
   auto params=(NCCALCSIZE_PARAMS*)lp;
   if(WindowMaximized()){
    MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))params->rgrc[0]=monitor.rcWork;
   }
   return 0;
  }
  break;
 case WM_NCHITTEST:{
  if(preview)return HTCLIENT;
  POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
  ScreenToClient(hwnd,&p);
  RECT r;GetClientRect(hwnd,&r);
  int edge=int(7*dpi);
  if(!WindowMaximized()){
   if(p.y<edge)return p.x<edge?HTTOPLEFT:p.x>r.right-edge?HTTOPRIGHT:HTTOP;
   if(p.y>r.bottom-edge)return p.x<edge?HTBOTTOMLEFT:p.x>r.right-edge?HTBOTTOMRIGHT:HTBOTTOM;
   if(p.x<edge)return HTLEFT;
   if(p.x>r.right-edge)return HTRIGHT;
  }
  if(p.y<(Margin+Bubble)*dpi&&HitTest(p.x/dpi,p.y/dpi)==IdNone)return HTCAPTION;
  return HTCLIENT;
 }
 case WM_GETMINMAXINFO:{
  auto mm=(MINMAXINFO*)lp;
  mm->ptMinTrackSize=preview?POINT{60,60}:POINT{LONG(760*dpi),LONG(500*dpi)};
  MONITORINFO monitor{sizeof(monitor)};
  if(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)){
   mm->ptMaxPosition={monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top};
   mm->ptMaxSize={monitor.rcWork.right-monitor.rcWork.left,monitor.rcWork.bottom-monitor.rcWork.top};
  }
  return 0;
 }
 case WM_DPICHANGED:{
  dpi=HIWORD(wp)/96.f;
  auto r=(RECT*)lp;
  SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);
  RECT client;GetClientRect(hwnd,&client);
  GfxResize(client.right,client.bottom,dpi);
  if(fit)Fit();
  Wake();return 0;
 }
 case WM_SIZE:{
  RECT r;GetClientRect(hwnd,&r);
  GfxResize(r.right,r.bottom,dpi);
  ApplyCorners();
  // While our own maximise/restore animation is interpolating the window
  // rect, let the fit-zoom spring chase the moving target smoothly; snapping
  // it on every intermediate WM_SIZE is what produced the old jump-cut.
  if(fit){Fit();if(!windowAnimating&&(wp==SIZE_MAXIMIZED||wp==SIZE_RESTORED))Snap();}
  Wake();return 0;
 }
 case WM_SETTINGCHANGE:{
  BOOL animations=TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
  reducedMotion=!animations;
  Wake();return 0;
 }
 case WM_ERASEBKGND:return 1;
 case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(hwnd,&ps);Frame();EndPaint(hwnd,&ps);return 0;}
 case Loaded:{
  std::unique_ptr<Result> result;
  {
   std::lock_guard lock(mx);
   if(!ready.empty()){result=std::move(ready.front());ready.pop_front();}
  }
  if(result&&result->id==latest){
   auto shown=current;
   current=result->image;currentGeneration=result->id;errorText=result->error;
   currentFrameKey=result->key;
   if(current&&current->w<=1024)backdropSource=current;
   if(!result->files.empty()||!preview)siblings=std::move(result->files);
   loading=result->partial;
   bitmap=current?GpuTextureFor(current,result->key.empty()?NormalisePath(result->path)+L"|live":result->key,
                                currentFrameKey):ComPtr<ID2D1Bitmap>();
   backdrop.Reset();
   if(preview){if(current)PreviewBounds();else Close();}
   SyncGallery();
   ThumbTrim(siblings);
   // Metadata and the histogram are read from the final frame, so the panel
   // never shows figures taken from a downscaled preview.
   if(current&&!result->partial){
    metaId=result->id;metaPending=true;
    MetaRequest(currentPath,metaId,current,win,MetaReady);
   }
   // The full frame arriving behind a preview must not move the photograph: at
   // fit that is a no-op, and a reader who zoomed in keeps the same framing.
   bool refined=shown&&current&&shown!=current&&shown->w&&current->w&&!fit;
   if(refined)zoomLog.Reset(zoomLog.target+logf(float(shown->w)/float(current->w)));
   else if(fit){Fit();Snap();}
   showingPreview=result->partial;
   if(hero.active&&!hero.closing){
    hero.crossfade.Reset(0);hero.crossfade.To(1);
    float w,h;Size(w,h);
    if(current){float scale=(std::min)(w/current->w,h/current->h);
     hero.left.To((w-current->w*scale)*.5f);hero.right.To((w+current->w*scale)*.5f);
     hero.top.To((h-current->h*scale)*.5f);hero.bottom.To((h+current->h*scale)*.5f);}
   }
   Log(current?L"Opened "+currentPath:L"Open failed "+currentPath);
   Wake();
  }
  return 0;
 }
 case MetaReady:{
  Meta fresh;
  // The quick pass answers with rating and label only; it must not overwrite
  // a full record that has already arrived for the same photograph.
  if(MetaCollect(metaQuickId,fresh)){
   if(SamePath(fresh.path,currentPath)&&!info.ready){
    info.hasRating=fresh.hasRating;info.rating=fresh.rating;
    info.label=fresh.label;info.xmpSource=fresh.xmpSource;
    Wake();
   }
   return 0;
  }
  if(MetaCollect(metaId,fresh)){info=std::move(fresh);metaPending=false;for(auto& g:histPath)g.Reset();scopeBitmap.Reset();
   histRise.Reset(0);histRise.To(1);Wake();}
  return 0;
 }
 case ThumbReady:{
  if(screen==ScrViewer&&(!current||(showingPreview&&current->w<=180))){
   auto thumb=ThumbLookup(currentPath);
   if(thumb&&thumb!=current){
    current=thumb;currentFrameKey=NormalisePath(currentPath)+L"|thumb";backdropSource=thumb;
    bitmap.Reset();backdrop.Reset();
    showingPreview=true;
    if(fit){Fit();Snap();}
   }
  }
  Wake();return 0;
 }
 case IndexFolders:if(screen!=ScrViewer)RefreshLibrary();else Wake();return 0;
 case IndexProgress:Wake();return 0;
 case WM_DEVICECHANGE:
  if(wp==DBT_DEVICEARRIVAL||wp==DBT_DEVICEREMOVECOMPLETE)IndexDrivesChanged();
  return TRUE;
 case WM_COPYDATA:{
  auto cd=(COPYDATASTRUCT*)lp;
  if(cd->dwData==1&&cd->cbData>=sizeof(wchar_t)&&cd->cbData<65536&&((wchar_t*)cd->lpData)[cd->cbData/2-1]==0){
   NormalWindow();Open((wchar_t*)cd->lpData);return TRUE;
  }
  break;
 }
 case ActivateNormal:NormalWindow();return 0;
 case Preview:
  if(preview){Close();return 0;}
  else{
   auto path=ExplorerSelection((HWND)lp);
   if(Supported(path)){
    if(!compactWindow){
     GetWindowRect(win,&normalBounds);
     beforePreviewValid=GetWindowPlacement(win,&beforePreview)!=FALSE;
    }
    explorer=(HWND)lp;preview=true;customMax=false;ClosePanel();
    windowAnimating=false;
    ShowWindow(win,SW_HIDE);
    SetWindowLongPtrW(win,GWL_STYLE,GetWindowLongPtrW(win,GWL_STYLE)&~(WS_MAXIMIZE|WS_MINIMIZE));
    SetWindowLongPtrW(win,GWLP_HWNDPARENT,0);
    ApplyCorners();
    if(current&&currentPath==path){rotate.Reset(0);ResetEdits();PreviewBounds();}
    else{current.reset();ReleaseBitmaps();Open(path);}
   }
  }
  return 0;
 case Saved:{
  std::unique_ptr<SaveResult> result;
  {std::lock_guard lock(mx);result=std::move(saveResult);}
  actionBusy=false;
  if(actionWorker.joinable())actionWorker.join();
  if(result){
   if(result->ok){if(result->imageId==latest)Open(result->path,true);Notify(T(S_Saved));}
   else Notify(result->error);
  }
  return 0;
 }
 case WM_KEYDOWN:{
  bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
  if(!preview&&screen!=ScrViewer){
   if(wp==VK_ESCAPE){
    if(sortOpen||filterOpen){CloseLibraryPopups();Wake();return 0;}
    if(libSearchFocused){libSearchFocused=false;Wake();return 0;}
    if(screen==ScrAlbum){GoToLibrary();return 0;}
    return 0;
   }
   if(libSearchFocused){
    if(wp==VK_BACK&&!libSearch.empty()){libSearch.pop_back();RefreshLibrary();Wake();return 0;}
    if(wp==VK_RETURN){libSearchFocused=false;Wake();return 0;}
   }
   return 0;
  }
  // A command-line Quick Look window is explicitly dismissed with Esc even
  // when an incidental panel is open.
  if(wp==VK_ESCAPE&&quickLookInvocation){Close();return 0;}
  if(wp==VK_ESCAPE&&panel!=PanelNone){ClosePanel();return 0;}
  if(wp==VK_ESCAPE&&tool!=ToolNone){tool=ToolNone;painting=false;Wake();return 0;}
  if(wp==VK_ESCAPE||(wp==VK_SPACE&&preview)){if(!preview&&screen==ScrViewer&&!navStack.empty()){ViewerBack();return 0;}Close();return 0;}
  if(preview)return 0;
  if(control&&wp=='S'){Save(shift);return 0;}
  if(control&&wp=='C'){CopyCurrent();return 0;}
  if(control&&wp=='O'){Choose();return 0;}
  if(control&&wp=='Z'){Command(IdUndo);return 0;}
  if(wp==VK_RIGHT)Navigate(1);
  else if(wp==VK_LEFT)Navigate(-1);
  else if(wp=='0')Fit();
  else if(wp=='1'){SetZoom(1.f/(dpi*(std::max)(0.0001f,FrameScale())),0,0);RequestFullResolution();}
  else if(wp==VK_ADD||wp==VK_OEM_PLUS)SetZoom(Zoom()*1.35f,0,0);
  else if(wp==VK_SUBTRACT||wp==VK_OEM_MINUS)SetZoom(Zoom()/1.35f,0,0);
  else if(wp=='R')Turn(shift?-1:1);
  else if(wp=='F')ToggleLike();
  else if(wp=='I')OpenPanel(PanelInfo);
  else if(wp==VK_DELETE)OpenPanel(PanelMenu),confirmDelete=true;
  return 0;
 }
 case WM_CHAR:
  if(!preview&&screen!=ScrViewer&&libSearchFocused&&wp>=32&&wp!=127){
   libSearch+=wchar_t(wp);RefreshLibrary();Wake();return 0;
  }
  return 0;
 case WM_MOUSEWHEEL:{
  if(preview)return 0;
  POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
  ScreenToClient(hwnd,&p);
  float x=p.x/dpi,y=p.y/dpi,w,h;Size(w,h);
  float delta=GET_WHEEL_DELTA_WPARAM(wp)/120.f;
  if(screen!=ScrViewer){
   // Scrolling is direct manipulation, so a decorative fly-back must yield
   // immediately instead of leaving its photo pinned over the moving grid.
   hero.active=false;folderTx.active=false;
   if((filterOpen||filterReveal.v>.1f)&&Inside(R(IdFilterPopup),x,y)){
    filterScrollVel-=delta*700.f;Wake();return 0;
   }
   if(screen==ScrLibrary){libScrollVel-=delta*900.f;Wake();}
   else{albumScrollVel-=delta*900.f;Wake();}
   return 0;
  }
  // Once the decoded image is available, wheel input owns the frame. Keeping
  // the opening overlay alive here made zoom work internally but look frozen.
  if(hero.active&&current)hero.active=false;
  int over=HitTest(x,y);
  if(over==IdGalleryBody||over>=IdGallery0){galleryVel-=delta*900.f;galleryHasTarget=false;Wake();return 0;}
  if(panel!=PanelNone&&Inside(R(IdPanelBody),x,y)){
   panelScrollVel-=delta*900.f;Wake();return 0;
  }
  if(wheelMode==1&&!siblings.empty()){
   // Touchpads and free-spinning wheels send fractions of a notch; carrying the
   // remainder keeps one physical click at exactly one photograph.
   if(wheelCarry!=0&&(wheelCarry>0)!=(delta>0))wheelCarry=0;
   wheelCarry+=delta;
   int steps=int(wheelCarry);
   if(steps){wheelCarry-=float(steps);Navigate(-steps);}
   return 0;
  }
  SetZoom(expf(zoomLog.target)*powf(1.22f,delta),x-w/2,y-h/2);
  return 0;
 }
 case WM_LBUTTONDBLCLK:
  if(preview||screen!=ScrViewer)return 0;
  if(HitTest(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi)==IdNone){
   if(fit){SetZoom(1.f/(dpi*(std::max)(0.0001f,FrameScale())),0,0);RequestFullResolution();}else Fit();
  }
  return 0;
 case WM_LBUTTONDOWN:
  if(preview){SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0);return 0;}
  mouse={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
  if(screen!=ScrViewer)LibraryMouseDown(mouse.x/dpi,mouse.y/dpi);else MouseDown(mouse.x/dpi,mouse.y/dpi);
  Wake();return 0;
 case WM_MOUSEMOVE:{
  if(preview)return 0;
  POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
  if(screen!=ScrViewer)LibraryMouseMove(p.x/dpi,p.y/dpi);else MouseMove(p.x/dpi,p.y/dpi);
  mouse=p;
  TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};
  TrackMouseEvent(&track);
  return 0;
 }
 case WM_MOUSELEAVE:
  if(hover!=IdNone){B(hover).hover.To(0);hover=IdNone;Wake();}
  return 0;
 case WM_LBUTTONUP:
  if(preview)return 0;
  if(screen!=ScrViewer)LibraryMouseUp(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);else MouseUp(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);
  Wake();return 0;
 case WM_CAPTURECHANGED:dragImage=false;galleryDragging=false;sliderGrab=false;return 0;
 case WM_CONTEXTMENU:{
  POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(point.x==-1||point.y==-1)GetCursorPos(&point);
  POINT client=point;ScreenToClient(hwnd,&client);int hit=HitTest(client.x/dpi,client.y/dpi);
  if(screen!=ScrViewer&&!(screen==ScrLibrary&&libView==ViewFolders)&&hit>=IdCard0&&size_t(hit-IdCard0)<albumPhotos.size()){
   auto& primary=albumPhotos[size_t(hit-IdCard0)];auto variants=assetVariants.find(primary.id);if(variants==assetVariants.end())return 0;
   HMENU menu=CreatePopupMenu();for(size_t i=0;i<variants->second.size()&&i<30;++i)AppendMenuW(menu,MF_STRING,UINT(i+1),variants->second[i].name.c_str());
   int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);
   if(choice>0&&size_t(choice)<=variants->second.size()){auto& selected=variants->second[size_t(choice-1)];OpenPhotoFromAlbum(selected.path,primary.id,R(hit));}
   return 0;
  }
  if(screen!=ScrLibrary||libView!=ViewFolders)return 0;
  if(hit<IdCard0||size_t(hit-IdCard0)>=libFolders.size())return 0;
  auto folder=libFolders[size_t(hit-IdCard0)];
  auto parent=folder.members.empty()?fs::path(folder.path).parent_path().wstring():folder.path;
  std::wstring value=std::to_wstring(PhysicalId(parent));
  HMENU menu=CreatePopupMenu();
  AppendMenuW(menu,MF_STRING,1,folder.members.empty()?L"Разрешить объединение соседних технических папок":L"Всегда показывать эти папки отдельно");
  int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);
  if(choice==1){DWORD separate=folder.members.empty()?0:1;RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\LibrarySeparate",value.c_str(),REG_DWORD,&separate,sizeof(separate));RefreshLibrary();}
  return 0;
 }
 case WM_DROPFILES:{
  HDROP drop=(HDROP)wp;
  UINT n=DragQueryFileW(drop,0,nullptr,0);
  std::wstring p(n+1,0);
  DragQueryFileW(drop,0,p.data(),n+1);
  DragFinish(drop);p.resize(n);
  OpenExternal(p);return 0;
 }
 case WM_TIMER:
  if(wp==2){TestTick();return 0;}
  if(wp==7){NavBenchTick();return 0;}
  if(wp==5){
   KillTimer(hwnd,5);
   BOOL restore=FALSE;DwmSetWindowAttribute(hwnd,DWMWA_TRANSITIONS_FORCEDISABLED,&restore,sizeof(restore));
   return 0;
  }
  if(wp==6){
   // Explorer occasionally repaints its own z-order right as the preview
   // appears; a second topmost assertion a frame later wins that race
   // instead of leaving the preview to surface on its own after a delay.
   KillTimer(hwnd,6);
   if(preview)SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
   return 0;
  }
  if(wp==3){
   if(!preview&&screen==ScrViewer){chrome.To(current&&panel==PanelNone&&tool==ToolNone&&!dragImage&&!loading&&Now()-lastInteraction>3.0?0.f:1.f);if(chrome.Moving())Wake();}
   else if(!preview&&chrome.target<1.f){chrome.To(1.f);}
   if(preview&&GetForegroundWindow()==explorer&&ExplorerCanPreview(explorer)){
    auto p=ExplorerSelection(explorer);
    if(Supported(p)&&p!=currentPath)Open(p);
   }
   return 0;
  }
  return 0;
 case Tray:
  if(lp==WM_LBUTTONUP)NormalWindow();
  else if(lp==WM_RBUTTONUP){
   HMENU menu=CreatePopupMenu();
   AppendMenuW(menu,MF_STRING,1,L"Open Vetro Look");
   AppendMenuW(menu,MF_STRING,2,L"Quit Vetro Look");
   POINT p;GetCursorPos(&p);SetForegroundWindow(hwnd);
   int action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,p.x,p.y,0,hwnd,nullptr);
   DestroyMenu(menu);
   if(action==2)DestroyWindow(hwnd);
   if(action==1)NormalWindow();
  }
  return 0;
 case WM_QUERYENDSESSION:return TRUE;
 case WM_ENDSESSION:
  // An installer/shutdown is different from the user's ordinary Close: an
  // autostart viewer normally hides, but it must release VetroLook.exe here.
  if(wp){autostart=false;DestroyWindow(hwnd);}return 0;
 case WM_CLOSE:Close();return 0;
 case WM_DESTROY:PostQuitMessage(testFailures?1:0);return 0;
 }
 return DefWindowProcW(hwnd,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 RoInitialize(RO_INIT_SINGLETHREADED);
 bool ole=SUCCEEDED(OleInitialize(nullptr)); // needed for DoDragDrop, on top of the WinRT apartment above
 DWORD stored=0,size=sizeof(stored);
 LONG themeRead=RegGetValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"LightTheme",RRF_RT_REG_DWORD,nullptr,&stored,&size);
 bool light=false;
 if(themeRead==ERROR_SUCCESS)light=stored!=0;
 else{
  // First launch follows the Windows app theme, then persist that initial
  // choice so a later manual change remains the user's choice.
  DWORD systemLight=0;size=sizeof(systemLight);
  if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                  L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&systemLight,&size)==ERROR_SUCCESS)
   light=systemLight!=0;
  DWORD initial=light?1u:0u;
  RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"LightTheme",REG_DWORD,&initial,sizeof(initial));
 }
 stored=0;size=sizeof(stored);
 RegGetValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"Language",RRF_RT_REG_DWORD,nullptr,&stored,&size);
 language=int(stored)?1:0;
 stored=0;size=sizeof(stored);
 RegGetValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"WheelMode",RRF_RT_REG_DWORD,nullptr,&stored,&size);
 wheelMode=int(stored)?1:0;wheelMix.Reset(float(wheelMode));
 // Library view. Absent the setting — a first run, or a reset profile — this
 // is the timeline; a value only exists once somebody chose Folders (or chose
 // to go back to the timeline) for themselves.
 stored=DWORD(DefaultLibView);size=sizeof(stored);
 if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"LibraryView",RRF_RT_REG_DWORD,nullptr,&stored,&size)!=ERROR_SUCCESS)
  stored=DWORD(DefaultLibView);
 libView=(stored==DWORD(ViewFolders))?ViewFolders:ViewPhotosFlat;
 libViewSlide.Reset(libView==ViewPhotosFlat?1.f:0.f);
 BOOL animations=TRUE;
 SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
 reducedMotion=!animations;
 autostart=AutostartEnabled();

 int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 testing=argc>2&&!wcscmp(argv[1],L"--self-test");
 // --nav-bench <log> <folder>: walk a real folder three times and report the
 // navigation latency of each pass. Uses the ordinary window and renderer.
 navBench=argc>3&&!wcscmp(argv[1],L"--nav-bench");
 if(navBench){
  testing=true;
  logFile.open(fs::path(argv[2]));
  std::error_code ec;
  for(fs::directory_iterator it(fs::path(argv[3]),fs::directory_options::skip_permission_denied,ec),end;
      it!=end&&!ec;it.increment(ec)){
   if(it->is_regular_file(ec)&&Supported(it->path().wstring()))navFiles.push_back(it->path().wstring());
  }
  std::sort(navFiles.begin(),navFiles.end(),[](auto&a,auto&b){return StrCmpLogicalW(a.c_str(),b.c_str())<0;});
  size_t limit=argc>4?size_t(_wtoi(argv[4])):50;
  if(navFiles.size()>limit)navFiles.resize(limit);
  if(argc>5)navBenchSteps=(std::max)(1,_wtoi(argv[5]));
  // Interval between navigations. The default of one frame at 60 Hz is
  // faster than any hand; a larger value models a reader who pauses, which
  // is what lets idle pre-upload do its job.
  if(argc>6)navBenchInterval=(std::max)(1u,unsigned(_wtoi(argv[6])));
 }
 // --quicklook is the public entry point any file manager can call; --preview is
 // the older spelling the Explorer path still uses.
 bool quickCLI=argc>2&&(!wcscmp(argv[1],L"--preview")||!wcscmp(argv[1],L"--quicklook"));
 quickLookInvocation=quickCLI;
 bool background=argc>1&&!wcscmp(argv[1],L"--background");
 backgroundMode=background;
 bool registerCLI=argc>1&&!wcscmp(argv[1],L"--register");
 if(registerCLI){std::wstring failure;bool ok=RegisterAsViewer(failure);LocalFree(argv);return ok?0:4;}
 if(argc>1&&!wcscmp(argv[1],L"--unregister")){UnregisterViewer();LocalFree(argv);return 0;}
 if(!testing)RepairRegistrationIfStale();
 // --nav-bench has already opened the log; reopening an open wofstream sets
 // failbit and silently discards every line that follows.
 if(testing&&!navBench){logFile.open(fs::path(argv[2]));for(int i=3;i<argc;i++)testFiles.push_back(argv[i]);}
 // Opening a file always gets its own window: launching a second photo while
 // one is already showing must not disturb it, so that case falls through
 // and lets this process create a window of its own instead of forwarding
 // to whichever instance FindWindow happens to turn up.
 HWND existing=FindWindowW(L"VetroLook.Window",nullptr);
#ifdef VETRO_REVIEW_BUILD
 existing=nullptr;autostart=false;
#endif
 bool openingFile=argc>1&&!background&&!testing&&!navBench;
 if(existing&&!testing&&!quickCLI&&!openingFile&&!background){
  PostMessageW(existing,ActivateNormal,0,0);
  LocalFree(argv);return 0;
 }
 WNDCLASSW wc{};
 wc.lpfnWndProc=Proc;wc.hInstance=instance;wc.lpszClassName=L"VetroLook.Window";
#ifdef VETRO_REVIEW_BUILD
 wc.lpszClassName=L"VetroLook.Review.Window";
#endif
 wc.style=CS_DBLCLKS;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
 wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
 if(!wc.hIcon)wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
 RegisterClassW(&wc);
 win=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"Vetro Look",
  WS_OVERLAPPEDWINDOW,150,100,1180,800,nullptr,nullptr,instance,nullptr);
 if(!win)return 3;
 dpi=GetDpiForWindow(win)/96.f;
 themeMix.Reset(light?1.f:0.f);
 BOOL dark=!light;DwmSetWindowAttribute(win,20,&dark,sizeof(dark));
 // DWM's own "window open" animation runs before our first composed frame
 // reaches the screen and, with the redirection bitmap disabled, falls back
 // to the plain rectangular frame instead of our rounded region — the flash
 // of square corners the very first paint shows. Holding transitions off
 // until just after that first frame is presented skips the flash entirely;
 // a one-shot timer restores them shortly after so later state changes
 // (minimise, etc.) still get the system's own animation.
 BOOL noTransition=TRUE;DwmSetWindowAttribute(win,DWMWA_TRANSITIONS_FORCEDISABLED,&noTransition,sizeof(noTransition));
 SetTimer(win,5,180,nullptr);
 ApplyCorners();
 if(!GfxCreate(win,dpi))return 2;
 DragAcceptFiles(win,TRUE);
 lowMemoryNotice=CreateMemoryResourceNotification(LowMemoryResourceNotification);
 RefreshCacheBudget();
 FavouritesLoad();
 MetaCacheLoad();
 // 5 MB of lens XML, parsed once on a low-priority thread. Nothing waits for
 // it: LensLookup reports "not ready" until it lands, and the Info panel asks
 // again the next time it needs a record.
 LensDbStart();
 worker=std::thread(Worker);
 ThumbStart(win,ThumbReady);
 if(!testing&&!quickCLI)IndexStart(win,IndexFolders,IndexProgress);
 SetTimer(win,3,350,nullptr);
 NOTIFYICONDATAW tray{sizeof(tray)};
 tray.hWnd=win;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
 tray.uCallbackMessage=Tray;tray.hIcon=wc.hIcon;
 wcscpy_s(tray.szTip,L"Vetro Look - Space in Explorer. Right-click to quit.");
 if(!testing)Shell_NotifyIconW(NIM_ADD,&tray);
 // A busy render or Shell dialog must never time out the low-level hook.
 DWORD keyboardThreadId=0;
 HANDLE keyboardReady=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 std::thread keyboardThread;
 bool installHook=!testing&&!openingFile;
#ifdef VETRO_REVIEW_BUILD
 installHook=false;
#endif
 if(installHook)keyboardThread=std::thread([&]{
  MSG event{};PeekMessageW(&event,nullptr,0,0,PM_NOREMOVE);
  keyboardThreadId=GetCurrentThreadId();
  hook=SetWindowsHookExW(WH_KEYBOARD_LL,Keyboard,instance,0);
  QuickLog(hook?L"keyboard hook installed":L"keyboard hook failed error="+std::to_wstring(GetLastError()));
  SetEvent(keyboardReady);
  while(GetMessageW(&event,nullptr,0,0)>0){TranslateMessage(&event);DispatchMessageW(&event);}
  if(hook)UnhookWindowsHookEx(hook);
 });
 if(keyboardThread.joinable())WaitForSingleObject(keyboardReady,INFINITE);
 CloseHandle(keyboardReady);
 if(!background&&!quickCLI){Frame();ShowWindow(win,SW_SHOW);UpdateWindow(win);Log(L"Window first paint");}
 if(navBench){ShowWindow(win,SW_SHOW);SetTimer(win,7,navBenchInterval,nullptr);}
 else if(testing){ShowWindow(win,SW_SHOW);SetTimer(win,2,250,nullptr);}
 else if(quickCLI){preview=true;explorer=GetDesktopWindow();ApplyCorners();Open(argv[2]);}
 else if(argc>1&&!background){
  // Launched straight onto a file (Explorer, "Open with", a shortcut): Back
  // should still walk Viewer -> that file's folder -> the library, so the
  // two frames underneath the viewer are synthesised up front.
  std::error_code ec;auto absolute=fs::absolute(fs::path(argv[1]),ec);
  auto parent=(ec?fs::path(argv[1]):absolute).parent_path().wstring();
  navStack.clear();
  navStack.push_back({ScrLibrary,L"",0,0});
  navStack.push_back({ScrAlbum,parent,0,0});
  IndexTouchFolder(parent);
  Open(argv[1]);
 }
 LocalFree(argv);

 // Presenting with vsync paces the loop to the display, so springs advance at
 // whatever refresh rate the monitor runs and idle frames cost nothing.
 MSG msg{};
 bool running=true;
 auto previous=std::chrono::steady_clock::now();
 while(running){
  while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
   if(msg.message==WM_QUIT){running=false;break;}
   TranslateMessage(&msg);DispatchMessageW(&msg);
  }
  if(!running)break;
  auto now=std::chrono::steady_clock::now();
  float dt=std::chrono::duration<float>(now-previous).count();
  previous=now;
  bool animating=Tick((std::min)(dt,.05f));
  if(animating||needFrame){
   needFrame=false;
   if(IsWindowVisible(win))Frame();
   else Sleep(8);
  }else if(!PeekMessageW(&msg,nullptr,0,0,PM_NOREMOVE))WaitMessage();
 }
 if(keyboardThread.joinable()){PostThreadMessageW(keyboardThreadId,WM_QUIT,0,0);keyboardThread.join();}
 Shell_NotifyIconW(NIM_DELETE,&tray);
 {std::lock_guard lock(mx);stopping=true;latest=++generation;}
 cv.notify_one();
 if(worker.joinable())worker.join();
 if(actionWorker.joinable())actionWorker.join();
 if(!testing&&!quickCLI)IndexStop();
 MetaStop();ThumbStop();ShutdownSharing();
 FavouritesFlush();MetaCacheFlush();
 if(lowMemoryNotice){CloseHandle(lowMemoryNotice);lowMemoryNotice=nullptr;}
 ReleaseBitmaps();GfxDestroy();if(ole)OleUninitialize();RoUninitialize();
 return int(msg.wParam);
}
