// Vetro Look, GPL-3.0-or-later.
// Viewer-pipeline benchmark. Prints one TSV row per stage so before/after runs
// diff cleanly; every number here is wall-clock on this machine, not a model.
#include "image.h"
#include "pipeline.h"
#include <turbojpeg.h>
#include <libraw/libraw.h>
#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <algorithm>
#include <functional>
#include <thread>
#include <atomic>

namespace{
using Clock=std::chrono::steady_clock;
double Since(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}

// Median of a few runs: a single timing on Windows is mostly scheduler noise.
double Best(int runs,const std::function<void()>& body){
 std::vector<double> samples;
 for(int i=0;i<runs;i++){auto t=Clock::now();body();samples.push_back(Since(t));}
 std::sort(samples.begin(),samples.end());
 return samples[samples.size()/2];
}
void Row(const std::wstring& file,const wchar_t* stage,double ms,const std::wstring& note){
 std::wcout<<file<<L"\t"<<stage<<L"\t"<<std::fixed<<std::setprecision(2)<<ms<<L"\t"<<note<<L"\n";
}
}

// Decode a JPEG with nothing else attached, so the number belongs to
// libjpeg-turbo and not to the viewer. `edge` 0 means full resolution.
static double PureJpegMs(const std::vector<uint8_t>& bytes,unsigned edge,
                         unsigned& outW,unsigned& outH,int runs){
 std::vector<double> samples;
 std::vector<uint8_t> pixels;
 for(int i=0;i<runs;i++){
  auto start=Clock::now();
  auto tj=tj3Init(TJINIT_DECOMPRESS);
  if(!tj)return -1;
  if(tj3DecompressHeader(tj,bytes.data(),bytes.size())!=0){tj3Destroy(tj);return -1;}
  int fullW=tj3Get(tj,TJPARAM_JPEGWIDTH),fullH=tj3Get(tj,TJPARAM_JPEGHEIGHT);
  int w=fullW,h=fullH;
  if(edge&&unsigned((std::max)(fullW,fullH))>edge){
   int count=0;auto factors=tj3GetScalingFactors(&count);
   for(int f=0;f<count;f++){
    int sw=TJSCALED(fullW,factors[f]),sh=TJSCALED(fullH,factors[f]);
    if(unsigned((std::max)(sw,sh))<edge||int64_t(sw)*sh>=int64_t(w)*h)continue;
    if(tj3SetScalingFactor(tj,factors[f])==0){w=sw;h=sh;}
   }
  }
  pixels.assign(size_t(w)*size_t(h)*4,0);
  bool ok=tj3Decompress8(tj,bytes.data(),bytes.size(),pixels.data(),0,TJPF_BGRA)==0;
  tj3Destroy(tj);
  if(!ok)return -1;
  outW=unsigned(w);outH=unsigned(h);
  samples.push_back(Since(start));
 }
 std::sort(samples.begin(),samples.end());
 return samples[samples.size()/2];
}

// Where the time between "file on disk" and "pixels ready for the GPU" goes.
static int Stages(int argc,wchar_t** argv){
 std::wcout<<L"file\tstage\tms\tnote\n";
 for(int i=2;i<argc;i++){
  std::wstring path=argv[i];
  auto name=std::filesystem::path(path).filename().wstring();
  std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);
  if(!file){Row(name,L"file_read",-1,L"open failed");continue;}
  auto length=file.tellg();
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  file.seekg(0);
  auto readStart=Clock::now();
  file.read((char*)bytes.data(),length);
  Row(name,L"file_read",Since(readStart),std::to_wstring(bytes.size())+L" bytes");

  // Argument evaluation order is unspecified, so each timing runs on its own
  // line and the note is read afterwards.
  unsigned w=0,h=0,sw=0,sh=0;
  double fullMs=PureJpegMs(bytes,0,w,h,3);
  Row(name,L"jpeg_decode_full",fullMs,std::to_wstring(w)+L"x"+std::to_wstring(h));
  double screenMs=PureJpegMs(bytes,2560,sw,sh,3);
  Row(name,L"jpeg_decode_screen_dct",screenMs,std::to_wstring(sw)+L"x"+std::to_wstring(sh));

  std::wstring error;
  auto full=Decode(path,error);
  if(full){
   std::shared_ptr<Image> scratch;
   double resize=Best(3,[&]{scratch=Downsample(full,2560);});
   Row(name,L"cpu_resize_full_to_screen",resize,
       scratch?std::to_wstring(scratch->w)+L"x"+std::to_wstring(scratch->h):L"none");
   Row(name,L"cpu_orientation",Best(3,[&]{scratch=OrientPixels(full,6);}),L"");
   uint8_t average[4]{};
   Row(name,L"backdrop_average",Best(3,[&]{AverageColour(*full,average);}),L"");
   Row(name,L"full_frame_mb",double(full->pixels.size())/(1024*1024),L"bytes the GPU must take");
  }
  if(auto screen=DecodeScreen(path,2560,{}))
   Row(name,L"screen_frame_mb",double(screen->pixels.size())/(1024*1024),L"bytes the GPU must take");
  Row(name,L"time_to_first_visible_cpu",screenMs,L"decode only, GPU excluded");
 }
 return 0;
}

// Peak working set reached while `body` runs, over the value before it started.
// Process-wide peak is monotonic and so useless for a second measurement in the
// same process; sampling gives a per-stage figure instead.
static double PeakDeltaMB(const std::function<void()>& body){
 auto working=[]{
  PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters));
  return double(counters.WorkingSetSize);
 };
 double before=working();
 std::atomic<double> peak{before};
 std::atomic<bool> done{false};
 std::thread sampler([&]{
  while(!done.load(std::memory_order_relaxed)){
   double now=working();
   double seen=peak.load(std::memory_order_relaxed);
   while(now>seen&&!peak.compare_exchange_weak(seen,now)){}
   std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
 });
 body();
 done.store(true);
 sampler.join();
 return (peak.load()-before)/(1024*1024);
}

// The tier ladder, one row per rung, in the shape the brief asks for: what the
// decoder naturally produces, what the viewport actually needs, and what a full
// decode costs. The gap between the middle two rows is the waste this pass is
// about.
static int Tiers(int argc,wchar_t** argv){
 if(argc<3){std::wcerr<<L"usage: bench --tiers <edge> <file>...\n";return 2;}
 unsigned edge=unsigned(_wtoi(argv[2]));
 std::wcout<<L"file\tstage\tms\tdims\tmb\tpeak_mb\tcodec\n";
 auto row=[&](const std::wstring& name,const wchar_t* stage,double ms,
              const std::shared_ptr<Image>& image,double peak){
  std::wcout<<name<<L"\t"<<stage<<L"\t"<<std::fixed<<std::setprecision(2)<<ms<<L"\t"
   <<(image?std::to_wstring(image->w)+L"x"+std::to_wstring(image->h):std::wstring(L"none"))<<L"\t"
   <<std::setprecision(1)<<(image?double(image->pixels.size())/(1024*1024):0.0)<<L"\t"
   <<std::setprecision(1)<<peak<<L"\t"<<(image?image->codec:L"-")<<L"\n";
 };
 for(int i=3;i<argc;i++){
  std::wstring path=argv[i];
  auto name=std::filesystem::path(path).filename().wstring();
  std::error_code ec;
  auto bytes=std::filesystem::file_size(path,ec);
  std::wcout<<name<<L"\tsource\t0.00\t-\t"<<std::fixed<<std::setprecision(1)
   <<double(ec?0:bytes)/(1024*1024)<<L"\t0.0\t-\n";

  std::shared_ptr<Image> preview,screen,full;
  double ms=Best(3,[&]{preview=DecodeThumb(path,edge);});
  row(name,L"preview",ms,preview,0);

  double peak=0;
  ms=Best(3,[&]{screen=DecodeScreen(path,edge,{});});
  peak=PeakDeltaMB([&]{screen=DecodeScreen(path,edge,{});});
  row(name,L"screen",ms,screen,peak);

  // What the viewport actually wants: the bucket, not whatever the decoder
  // happened to hand back.
  if(screen){
   unsigned longEdge=(std::max)(screen->w,screen->h);
   std::wcout<<name<<L"\toverdraw\t0.00\t"<<longEdge<<L"/"<<edge<<L"\t"
    <<std::setprecision(2)<<(edge?double(longEdge)/double(edge):0.0)<<L"\t0.0\t-\n";
  }

  std::wstring error;
  ms=Best(1,[&]{full=Decode(path,error);});
  peak=PeakDeltaMB([&]{std::wstring e;full=Decode(path,e);});
  row(name,L"full",ms,full,peak);
  if(!full)std::wcout<<name<<L"\tfull_error\t0.00\t"<<error<<L"\t0.0\t0.0\t-\n";
 }
 return 0;
}

static int RawStages(int argc,wchar_t** argv){
 std::wcout<<L"file\tbackend\tparse_ms\tunpack_ms\tbayer_mb\tbayer_upload_ms\t"
              L"demosaic_color_ms\tcolor_ms\trgb_copy_ms\tscreen_total_ms\t"
              L"roi_1to1_ms\tcpu_peak_mb\tvram_mb\toutput\tnote\n";
 for(int i=2;i<argc;i++)for(bool half:{true,false}){
  RawStageMetrics metrics;
  double peak=PeakDeltaMB([&]{metrics=MeasureRawStages(argv[i],half);});
  double total=metrics.parseMs+metrics.unpackMs+metrics.demosaicAndColorMs+metrics.rgbCopyMs;
  std::wcout<<std::filesystem::path(argv[i]).filename().wstring()<<L"\tLibRaw-"<<(half?L"half":L"full")
   <<L"\t"<<std::fixed<<std::setprecision(2)<<metrics.parseMs
   <<L"\t"<<metrics.unpackMs<<L"\t"<<std::setprecision(1)<<double(metrics.bayerBytes)/(1024*1024)
   <<L"\tNA\t"<<std::setprecision(2)<<metrics.demosaicAndColorMs
   <<L"\tFUSED\t"<<metrics.rgbCopyMs<<L"\t"<<total
   <<L"\tNA\t"<<std::setprecision(1)<<peak<<L"\tNA\t"
   <<metrics.outputWidth<<L"x"<<metrics.outputHeight<<L"\t"
   <<(metrics.success?L"ok":metrics.error)<<L"\n";
 }
 return 0;
}

static int RawPreviews(int argc,wchar_t** argv){
 if(argc<4)return 2;unsigned edge=unsigned(_wtoi(argv[2]));
 std::wcout<<L"file\tcount\tindex\tdims\tbytes\tformat\tselected_for_edge\n";
 for(int a=3;a<argc;a++){
  LibRaw raw;if(raw.open_file(argv[a])!=LIBRAW_SUCCESS)continue;
  int count=(std::min)(raw.imgdata.thumbs_list.thumbcount,int(LIBRAW_THUMBNAIL_MAXCOUNT));
  int selected=0;uint64_t selectedArea=0;bool selectedCovers=false;
  for(int i=0;i<count;i++){
   const auto& item=raw.imgdata.thumbs_list.thumblist[i];uint64_t area=uint64_t(item.twidth)*item.theight;
   bool covers=(std::max)(unsigned(item.twidth),unsigned(item.theight))>=edge;
   if(area&&((covers&&!selectedCovers)||(covers==selectedCovers&&
      ((covers&&(!selectedArea||area<selectedArea))||(!covers&&area>selectedArea))))){
    selected=i;selectedArea=area;selectedCovers=covers;
   }
  }
  for(int i=0;i<count;i++){
   const auto& item=raw.imgdata.thumbs_list.thumblist[i];
   std::wcout<<std::filesystem::path(argv[a]).filename().wstring()<<L"\t"<<count<<L"\t"<<i<<L"\t"
    <<item.twidth<<L"x"<<item.theight<<L"\t"<<item.tlength<<L"\t"<<int(item.tformat)
    <<L"\t"<<(i==selected?L"yes":L"no")<<L"\n";
  }
 }
 return 0;
}

static std::shared_ptr<Image> ReferenceBox(const std::shared_ptr<Image>& src,unsigned edge){
 if(!src||!edge)return {};
 unsigned longEdge=(std::max)(src->w,src->h);
 unsigned w=(std::max)(1u,unsigned((uint64_t(src->w)*edge+longEdge/2)/longEdge));
 unsigned h=(std::max)(1u,unsigned((uint64_t(src->h)*edge+longEdge/2)/longEdge));
 auto out=std::make_shared<Image>();out->w=w;out->h=h;out->pixels.resize(size_t(w)*h*4);
 for(unsigned y=0;y<h;y++){
  unsigned y0=unsigned(uint64_t(y)*src->h/h),y1=unsigned(uint64_t(y+1)*src->h/h);if(y1<=y0)y1=y0+1;
  for(unsigned x=0;x<w;x++){
   unsigned x0=unsigned(uint64_t(x)*src->w/w),x1=unsigned(uint64_t(x+1)*src->w/w);if(x1<=x0)x1=x0+1;
   uint64_t sum[4]={};unsigned n=0;
   for(unsigned sy=y0;sy<y1;sy++)for(unsigned sx=x0;sx<x1;sx++){
    const uint8_t* p=&src->pixels[(size_t(sy)*src->w+sx)*4];
    for(int c=0;c<4;c++)sum[c]+=p[c];n++;
   }
   uint8_t* d=&out->pixels[(size_t(y)*w+x)*4];for(int c=0;c<4;c++)d[c]=uint8_t(sum[c]/n);
  }
 }
 return out;
}

static int PsdFidelity(int argc,wchar_t** argv){
 if(argc<4)return 2;unsigned edge=unsigned(_wtoi(argv[2]));int failures=0;
 std::wcout<<L"file\tdims\tmean_abs\tb_abs\tg_abs\tr_abs\ta_abs\tmax_abs\tresult\n";
 for(int i=3;i<argc;i++){
  std::wstring error;auto full=Decode(argv[i],error);auto screen=DecodeScreen(argv[i],edge,{});
  auto reference=ReferenceBox(full,edge);double mean=0,channel[4]={};unsigned maximum=0;size_t count=0;
  if(screen&&reference&&screen->w==reference->w&&screen->h==reference->h){
   count=screen->pixels.size();
   for(size_t p=0;p<count;p++){unsigned delta=unsigned(abs(int(screen->pixels[p])-int(reference->pixels[p])));mean+=delta;channel[p&3]+=delta;maximum=(std::max)(maximum,delta);}
   mean/=count;
   for(double& value:channel)value/=count/4;
  }else maximum=UINT_MAX;
  bool pass=count&&mean<=1.0&&maximum<=2;if(!pass)failures++;
  std::wcout<<std::filesystem::path(argv[i]).filename().wstring()<<L"\t"
   <<(screen?std::to_wstring(screen->w)+L"x"+std::to_wstring(screen->h):L"none")
   <<L"\t"<<std::fixed<<std::setprecision(4)<<mean;
  for(double value:channel)std::wcout<<L"\t"<<value;
  std::wcout<<L"\t"<<maximum<<L"\t"<<(pass?L"PASS":L"FAIL")<<L"\n";
 }
 return failures?1:0;
}

// RAW ladder, timed rung by rung, repeated so the spread is visible.
// Percentiles matter more than a mean here: an occasional slow unpack is what
// a reader notices, not the average.
static int RawLadder(int argc,wchar_t** argv){
 std::wcout<<L"file\tstage\tms\tnote\n";
 const int runs=5;
 for(int i=2;i<argc;i++){
  std::wstring path=argv[i];
  auto name=std::filesystem::path(path).filename().wstring();
  std::error_code ec;
  auto size=std::filesystem::file_size(path,ec);
  Row(name,L"bytes",double(ec?0:size),std::to_wstring((ec?0:size)/(1024*1024))+L" MB");

  auto percentiles=[&](std::vector<double> v,const wchar_t* stage,const std::wstring& note){
   if(v.empty())return;
   std::sort(v.begin(),v.end());
   Row(name,stage,v[v.size()/2],note+L" p50");
   Row(name,stage,v[(std::min)(v.size()-1,size_t(v.size()*95/100))],note+L" p95");
   Row(name,stage,v.back(),note+L" max");
  };

  std::vector<double> preview,screen;
  std::shared_ptr<Image> p1,p2;
  for(int r=0;r<runs;r++){
   auto t0=Clock::now();p1=DecodeThumb(path,2560);preview.push_back(Since(t0));
   auto t1=Clock::now();p2=DecodeScreen(path,2560,{});screen.push_back(Since(t1));
  }
  percentiles(preview,L"first_preview",p1?std::to_wstring(p1->w)+L"x"+std::to_wstring(p1->h):L"none");
  percentiles(screen,L"screen_ready",p2?std::to_wstring(p2->w)+L"x"+std::to_wstring(p2->h):L"none");

  std::vector<double> half;
  std::shared_ptr<Image> p3;
  for(int r=0;r<3;r++){auto t=Clock::now();p3=DecodeRawHalf(path,{});half.push_back(Since(t));}
  percentiles(half,L"raw_half_develop",p3?std::to_wstring(p3->w)+L"x"+std::to_wstring(p3->h):L"none");

  std::wstring error;
  std::vector<double> full;
  std::shared_ptr<Image> p4;
  for(int r=0;r<2;r++){auto t=Clock::now();p4=Decode(path,error);full.push_back(Since(t));}
  percentiles(full,L"full_ready",p4?std::to_wstring(p4->w)+L"x"+std::to_wstring(p4->h):L"none:"+error);

  PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters));
  Row(name,L"peak_working_set_mb",double(counters.PeakWorkingSetSize)/(1024*1024),L"");
 }
 return 0;
}

int wmain(int argc,wchar_t** argv){
 if(argc<2){std::wcerr<<L"usage: bench [--stages|--raw|--raw-stages|--raw-previews <edge>|--psd-fidelity <edge>|--tiers <edge>] <file>...\n";return 2;}
 if(!wcscmp(argv[1],L"--stages")){
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  int rc=Stages(argc,argv);
  CoUninitialize();
  return rc;
 }
 if(!wcscmp(argv[1],L"--tiers")){
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  int rc=Tiers(argc,argv);
  CoUninitialize();
  return rc;
 }
 if(!wcscmp(argv[1],L"--raw")){
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  int rc=RawLadder(argc,argv);
  CoUninitialize();
  return rc;
 }
 if(!wcscmp(argv[1],L"--raw-stages")){
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  int rc=RawStages(argc,argv);
  CoUninitialize();
  return rc;
 }
 if(!wcscmp(argv[1],L"--raw-previews"))return RawPreviews(argc,argv);
 if(!wcscmp(argv[1],L"--psd-fidelity")){
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  int rc=PsdFidelity(argc,argv);
  CoUninitialize();
  return rc;
 }
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 std::wcout<<L"file\tstage\tms\tnote\n";
 for(int i=1;i<argc;i++){
  std::wstring path=argv[i];
  auto name=std::filesystem::path(path).filename().wstring();
  std::error_code ec;auto bytes=std::filesystem::file_size(path,ec);
  Row(name,L"bytes",double(ec?0:bytes),L"");

  std::shared_ptr<Image> thumb,screen,full;
  auto size=[](const std::shared_ptr<Image>& i){
   return i?std::to_wstring(i->w)+L"x"+std::to_wstring(i->h):std::wstring(L"none");};
  // Argument evaluation order is unspecified, so the timing runs first and the
  // note is read afterwards; inlining both into one Row() call reported "none"
  // for every frame that had in fact decoded.
  double ms=Best(3,[&]{thumb=DecodeThumb(path,180);});Row(name,L"thumb",ms,size(thumb));
  // VETRO_BENCH_EDGE forces a larger viewport than the fixtures' embedded
  // previews can satisfy, which is what exercises the RAW half-size rung.
  static unsigned edge=[]{
   wchar_t value[16]{};
   return GetEnvironmentVariableW(L"VETRO_BENCH_EDGE",value,16)?unsigned(_wtoi(value)):2560u;
  }();
  ms=Best(3,[&]{screen=DecodeScreen(path,edge,{});});Row(name,L"screen",ms,size(screen));
  std::wstring error;
  ms=Best(1,[&]{full=Decode(path,error);});Row(name,L"full",ms,full?size(full):L"none:"+error);

  std::shared_ptr<Image> scratch;uint8_t average[4]{};
  if(full){
   ms=Best(3,[&]{scratch=Downsample(full,22);});Row(name,L"backdrop_from_full",ms,L"22px");
   ms=Best(3,[&]{AverageColour(*full,average);});Row(name,L"backdrop_average_full",ms,L"1px");
   ms=Best(1,[&]{scratch=OrientPixels(full,6);});Row(name,L"orient90_full",ms,size(scratch));
  }
  if(screen){
   ms=Best(3,[&]{scratch=Downsample(screen,22);});Row(name,L"backdrop_from_screen",ms,L"22px");
   ms=Best(3,[&]{scratch=OrientPixels(screen,6);});Row(name,L"orient90_screen",ms,size(scratch));
  }
  PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters));
  Row(name,L"peak_working_set_mb",double(counters.PeakWorkingSetSize)/(1024*1024),L"");
 }
 CoUninitialize();
 return 0;
}
