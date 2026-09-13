// Vetro Look, GPL-3.0-or-later.
// The rules of AI subtitles that have no model in them (transcript.h).
#include "../src/transcript.h"
#include <windows.h>
#include <iostream>

namespace{
int failures=0;
void Check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;}
AiCue Cue(double start,double end,const wchar_t* text,float confidence=0.9f,float noSpeech=0.05f){
 AiCue cue;cue.start=start;cue.end=end;cue.text=text;cue.confidence=confidence;cue.noSpeech=noSpeech;
 return cue;
}
bool Near(double a,double b){return a>b-1e-6&&a<b+1e-6;}
}

int main(){
 SetConsoleOutputCP(CP_UTF8);

 // ------------------------------------------------------------- coverage --
 {
  std::vector<Span> spans;
  CoverageAdd(spans,{60,120});CoverageAdd(spans,{0,60.02});CoverageAdd(spans,{300,360});
  Check(spans.size()==2&&Near(spans[0].start,0)&&Near(spans[0].end,120),"coverage: chunks that meet become one span");
  Check(CoverageContains(spans,59.9)&&!CoverageContains(spans,200)&&CoverageContains(spans,300),"coverage: containment");
  Check(Near(CoverageFirstGap(spans,10,500),120),"coverage: first gap after a covered start");
  Check(Near(CoverageFirstGap(spans,130,500),130),"coverage: a gap start is its own answer");
  Check(CoverageFirstGap(spans,310,355)<0,"coverage: a fully covered range has no gap");
  Check(Near(CoverageSeconds(spans),180),"coverage: seconds");
  CoverageAdd(spans,{50,40});
  Check(spans.size()==2,"coverage: an empty span changes nothing");
 }

 // ------------------------------------------------------------- planning --
 {
  std::vector<Span> covered={{0,120}};
  auto plan=PlanChunk(covered,100,3600,300,60);
  Check(plan.work&&Near(plan.start,120)&&Near(plan.length,60),"plan: the next gap ahead of the playhead");
  plan=PlanChunk({{0,420}},100,3600,300,60);
  Check(!plan.work,"plan: nothing to do once the lookahead is covered");
  plan=PlanChunk({{0,420}},5400,7200,300,60);
  Check(plan.work&&Near(plan.start,5398),"plan: a far seek starts a little before the landing point");
  plan=PlanChunk({},3560,3600,300,60);
  Check(plan.work&&Near(plan.start,3558)&&Near(plan.start+plan.length,3600),"plan: the end of a film is one chunk");
  plan=PlanChunk({},0,3600,300,58.5);
  Check(plan.work&&Near(plan.length,58.5),"plan: a normal chunk has the requested length");
  plan=PlanChunk({},10,0,300,60);
  Check(!plan.work,"plan: no length, no plan");
  plan=PlanChunk({{0,53.5}},40,53.5625,300,60);
  Check(!plan.work,"plan: the sliver past the last sample is not a chunk");
  Check(ChunkAbandoned(600,60,5400,300)&&!ChunkAbandoned(600,60,620,300)&&ChunkAbandoned(9000,60,600,300),
        "plan: work far from the playhead is abandoned, work near it is not");
 }

 // ------------------------------------------------------------- settling --
 {
  std::vector<AiCue> cues={Cue(121,124,L"Первая"),Cue(150,155,L"Вторая"),Cue(176,179.8,L"Обрезанная на краю")};
  auto settled=SettleChunk(cues,120,180,false);
  Check(settled.kept.size()==2&&Near(settled.coveredUntil,176),"settle: a line cut by the chunk's end is heard whole next time");
  settled=SettleChunk(cues,120,180,true);
  Check(settled.kept.size()==3&&Near(settled.coveredUntil,180),"settle: the film's last chunk keeps everything");
  settled=SettleChunk({Cue(121,124,L"Раз"),Cue(140,150,L"Два")},120,180,false);
  Check(settled.kept.size()==2&&Near(settled.coveredUntil,180),"settle: a line that ended well before the edge is kept");
  settled=SettleChunk({Cue(125,179.9,L"Долгий монолог")},120,180,false);
  Check(settled.kept.size()==1&&Near(settled.coveredUntil,180),"settle: work always moves forward");
  settled=SettleChunk({},120,180,false);
  Check(settled.kept.empty()&&Near(settled.coveredUntil,180),"settle: a silent chunk is still covered");
 }

 // -------------------------------------------------------- hallucination --
 {
  std::vector<AiCue> none;
  Check(!CueSuppressed(Cue(10,12,L"Где ты был вчера?"),none),"filter: ordinary dialogue passes");
  Check(CueSuppressed(Cue(10,12,L"  ... "),none),"filter: punctuation alone is nothing");
  Check(CueSuppressed(Cue(10,12,L"Субтитры сделал DimaTorzok"),none),"filter: subtitle credits are not in the film");
  Check(CueSuppressed(Cue(10,12,L"Thank you for watching!"),none),"filter: English sign-off");
  Check(CueSuppressed(Cue(10,12,L"Угу.",0.3f,0.8f),none),"filter: doubtful words over non-speech");
  Check(!CueSuppressed(Cue(10,12,L"Угу.",0.8f,0.8f),none),"filter: a confident short answer stays");
  Check(CueSuppressed(Cue(10,30,L"А",0.4f,0.2f),none),"filter: twenty seconds of one letter");
  std::vector<AiCue> stuck={Cue(10,12,L"Я не знаю."),Cue(12,14,L"Я не знаю.")};
  Check(CueSuppressed(Cue(14,16,L"я не знаю"),stuck),"filter: the same line a third time running");
  std::vector<AiCue> once={Cue(10,12,L"Я не знаю.")};
  Check(!CueSuppressed(Cue(14,16,L"Я не знаю."),once),"filter: a line said twice is still a line");
  Check(CueSuppressed(Cue(10,15,L"да да да да да да да да да да да да да"),none),"filter: a phrase looping inside one cue");
 }

 // ---------------------------------------------------------------- store --
 {
  std::vector<AiCue> store;
  InsertCues(store,{Cue(20,22,L"b"),Cue(10,12,L"a")});
  InsertCues(store,{Cue(30,32,L"c")});
  Check(store.size()==3&&store[0].text==L"a"&&store[2].text==L"c","store: kept in time order");
  InsertCues(store,{Cue(21,25,L"b2")});
  Check(store.size()==3&&store[1].text==L"b2","store: a newer cue replaces the one it overlaps");
  Check(CueAt(store,11)&&CueAt(store,11)->text==L"a"&&!CueAt(store,15)&&!CueAt(store,5)&&CueAt(store,31),"store: the cue on screen");
 }

 // --------------------------------------------------------------- export --
 {
  std::vector<AiCue> cues={Cue(1.5,3.25,L"Привет"),Cue(3661.001,3662,L"a --> b")};
  auto srt=ExportSrt(cues);
  Check(srt.find("1\r\n00:00:01,500 --> 00:00:03,250\r\n\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\r\n")==0,"export: SRT timing and UTF-8 text");
  Check(srt.find("2\r\n01:01:01,001 --> 01:01:02,000")!=std::string::npos,"export: SRT hours");
  auto vtt=ExportVtt(cues);
  Check(vtt.rfind("WEBVTT\n\n00:00:01.500 --> 00:00:03.250\n",0)==0,"export: VTT header and full stops");
  Check(vtt.find("a -> b")!=std::string::npos,"export: an arrow in the text cannot end a VTT header");
 }

 // ---------------------------------------------------------------- cache --
 {
  auto key=AiCacheKey(L"C:\\Films\\A.mkv|100|200",L"large-v3-turbo-q5_0",L"394221709cd5",L"",2,1);
  auto other=AiCacheKey(L"C:\\Films\\A.mkv|100|200",L"small",L"1be3a9b2",L"",2,1);
  Check(key!=other&&AiCacheFileName(key)!=AiCacheFileName(other),"cache: another model is another transcript");
  Check(key.find(L"|auto|")!=std::wstring::npos,"cache: no language means auto");
  Check(AiCacheFileName(key)==AiCacheFileName(key)&&AiCacheFileName(key).size()==43,"cache: a stable file name");
  Transcript t;
  t.key=key;t.language=L"ru";
  CoverageAdd(t.covered,{0,176});CoverageAdd(t.analysed,{0,180});
  t.speech.push_back({1,3});t.speech.push_back({150,155});
  AiCue cue=Cue(1.5,3.25,L"Строка\nвторая \\ обратная");
  cue.words.push_back({1.5,2.0,0.9f,L"Строка"});
  t.cues.push_back(cue);
  Transcript back;
  Check(ParseTranscript(SerializeTranscript(t),back),"cache: a transcript reads back");
  Check(back.key==key&&back.language==L"ru"&&back.covered.size()==1&&back.speech.size()==2&&
        back.cues.size()==1&&back.cues[0].text==cue.text&&back.cues[0].words.size()==1&&
        back.cues[0].words[0].text==L"Строка"&&Near(back.cues[0].end,3.25),"cache: everything survives, newlines and backslashes too");
  Check(!ParseTranscript("not a transcript\n",back),"cache: a foreign file is refused");
  Check(!ParseTranscript("VETRO-TRANSCRIPT 1\ncovered x y\n",back),"cache: a damaged line is refused");
 }

 // --------------------------------------------------------- silence skip --
 {
  std::vector<Span> speech={{10,20},{30,40},{41,50}};
  std::vector<Span> analysed={{0,60}};
  Check(SilenceSkipTarget(speech,analysed,22,SilenceSkip::Off)<0,"skip: off is off");
  Check(Near(SilenceSkipTarget(speech,analysed,22,SilenceSkip::Gentle),29.6),"skip: gentle lands a breath before the voice");
  Check(Near(SilenceSkipTarget(speech,analysed,22,SilenceSkip::Aggressive),29.85),"skip: aggressive lands closer");
  Check(SilenceSkipTarget(speech,analysed,15,SilenceSkip::Aggressive)<0,"skip: never while someone is talking");
  Check(SilenceSkipTarget(speech,analysed,40.2,SilenceSkip::Gentle)<0,"skip: a one-second pause is not skipped gently");
  Check(SilenceSkipTarget(speech,analysed,28.5,SilenceSkip::Gentle)<0,"skip: too close to the next line to bother");
  Check(SilenceSkipTarget(speech,{{0,25}},22,SilenceSkip::Aggressive)<0,"skip: unknown is not silence");
  Check(SilenceSkipTarget(speech,analysed,52,SilenceSkip::Aggressive)<0,"skip: no speech ahead known, stay");
 }

 std::cout<<(failures?"FAILURES: ":"all passed: ")<<failures<<"\n";
 return failures?1:0;
}
