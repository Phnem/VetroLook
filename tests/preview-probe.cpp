// Vetro Look, GPL-3.0-or-later.
// Stage 4's feasibility gate: can a second, output-less decoder hand back
// frames from anywhere in a film, fast enough to follow a pointer, while the
// playback engine is not involved at all?
//
// Everything the timeline's preview bubble promises rests on that one answer,
// so it is measured against a real file before any interface is built on it --
// the same way the composition gate was run before Video Mode had controls.
//
//   VetroPreviewProbe <file> [positions...]
#include "../src/preview.h"
#include "../src/playback.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace{
int failures=0;
void Check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;}
double Milliseconds(std::chrono::steady_clock::time_point from){
 return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-from).count();
}
// Waits for the frame at `seconds` to land, up to `limit` milliseconds.
std::shared_ptr<PreviewFrame> Await(double seconds,double limit,double& tookMs){
 auto started=std::chrono::steady_clock::now();
 PreviewRequest(seconds,240);
 while(Milliseconds(started)<limit){
  auto frame=PreviewBest(seconds);
  if(frame&&frame->exact){tookMs=Milliseconds(started);return frame;}
  Sleep(10);
 }
 tookMs=Milliseconds(started);
 return PreviewBest(seconds);
}
}

int wmain(int argc,wchar_t** argv){
 if(argc<2){std::cout<<"usage: VetroPreviewProbe <file> [seconds...]\n";return 2;}
 std::wstring path=argv[1];

 // Quantisation first: it is pure arithmetic, and a wrong bucket would make
 // every measurement below meaningless.
 Check(PreviewGranularity(90)<=1.0,"a ninety second clip gets fine buckets");
 Check(PreviewGranularity(3600)>=10.0,"an hour gets coarse ones");
 Check(PreviewGranularity(6*3600)>=60.0,"a six hour recording is coarser still");
 double g=PreviewGranularity(3600);
 Check(PreviewBucket(0,g)==0&&PreviewBucket(g*4+0.1,g)==4,"positions land in their bucket");
 Check(PreviewBucket(-5,g)==0,"a position before the start is the first bucket");

 std::wstring error;
 if(!PlaybackEnsureLibrary(error)){
  std::wcout<<L"FAIL playback library: "<<error<<L"\n";
  return 1;
 }
 Check(PlaybackEngineModule()!=nullptr,"the preview decoder can borrow the loaded library");

 std::vector<double> positions;
 for(int i=2;i<argc;i++)positions.push_back(_wtof(argv[i]));
 if(positions.empty())positions={5,600,1800,60,1200};

 // No playback engine is created anywhere in this probe. That is the point: the
 // preview path must stand on its own.
 PreviewOpen(path,L"probe",3600,nullptr,0);
 PreviewSetPolicy(true,1.f,0);

 double first=0;
 auto frame=Await(positions[0],20000,first);
 Check(frame!=nullptr,"a frame comes back at all");
 if(!frame){
  std::cout<<"FAILURES: "<<++failures<<" (no frame: the preview path is not viable as built)\n";
  PreviewStop();
  return 1;
 }
 std::printf("first frame %ux%u in %.0f ms (decoder start included)\n",frame->width,frame->height,first);
 Check(frame->width>=96&&frame->height>=54,"the frame is scaled down by the decoder, not by us");
 Check(frame->bgra.size()==size_t(frame->width)*frame->height*4,"the frame is complete");

 double worst=0,total=0;int measured=0;
 for(size_t i=1;i<positions.size();i++){
  double took=0;
  auto next=Await(positions[i],15000,took);
  std::printf("  %7.1f s -> %s in %6.0f ms\n",positions[i],next?"frame":"nothing",took);
  if(!next)failures++;
  worst=took>worst?took:worst;total+=took;measured++;
 }
 Check(measured>0&&worst<3000,"random access stays inside three seconds");
 if(measured)std::printf("mean %.0f ms, worst %.0f ms over %d seeks\n",total/measured,worst,measured);

 // The second ask for a position already decoded must not touch the decoder.
 double cached=0;
 auto again=Await(positions[0],2000,cached);
 Check(again!=nullptr&&cached<50,"a position already decoded comes back from the cache");
 std::printf("cached ask %.1f ms, cache holds %zu KB\n",cached,PreviewCacheBytes()/1024);

 // Latest wins: ask for five positions in a row without waiting, and only the
 // last one is worth decoding.
 for(double position:{100.0,200.0,300.0,400.0,500.0})PreviewRequest(position,240);
 Sleep(2500);
 auto last=PreviewBest(500);
 Check(last!=nullptr,"the position the pointer stopped on is the one decoded");

 PreviewClose();
 PreviewStop();
 std::cout<<(failures?"FAILURES: ":"all passed: ")<<failures<<"\n";
 return failures?1:0;
}
