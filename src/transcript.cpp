// Vetro Look, GPL-3.0-or-later.
// See transcript.h.
#include "transcript.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <windows.h>

namespace{

std::string Narrow(const std::wstring& text){
 if(text.empty())return {};
 int length=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
 std::string out(size_t(length),'\0');
 WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),out.data(),length,nullptr,nullptr);
 return out;
}
std::wstring Widen(const std::string& text){
 if(text.empty())return {};
 int length=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
 std::wstring out(size_t(length),L'\0');
 MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),out.data(),length);
 return out;
}
// The C library's towlower and iswalnum answer for ASCII only in the "C"
// locale, which would leave "Субтитры" capitalised and every Russian filter
// blind. Windows' own case and class tables are Unicode whatever the locale.
std::wstring Lower(std::wstring text){
 if(!text.empty())CharLowerBuffW(text.data(),DWORD(text.size()));
 return text;
}
// Letters and digits only, lower case: "Спасибо за просмотр!" and "спасибо за
// просмотр" are the same line to a person and should be to the filters.
std::wstring Essence(const std::wstring& text){
 std::wstring out;
 for(wchar_t c:text)if(IsCharAlphaNumericW(c))out.push_back(c);
 return Lower(std::move(out));
}
std::wstring Trim(const std::wstring& text){
 size_t a=0,b=text.size();
 while(a<b&&iswspace(text[a]))a++;
 while(b>a&&iswspace(text[b-1]))b--;
 return text.substr(a,b-a);
}

std::string Stamp(double seconds,char separator){
 if(seconds<0)seconds=0;
 long long ms=llround(seconds*1000.0);
 char text[32];
 snprintf(text,sizeof text,"%02lld:%02lld:%02lld%c%03lld",ms/3600000,(ms/60000)%60,(ms/1000)%60,separator,ms%1000);
 return text;
}

}

// ------------------------------------------------------------- coverage ----
void CoverageAdd(std::vector<Span>& spans,Span span){
 if(span.end<=span.start)return;
 spans.push_back(span);
 std::sort(spans.begin(),spans.end(),[](const Span& a,const Span& b){return a.start<b.start;});
 std::vector<Span> merged;
 for(const auto& s:spans){
  // A hair's gap is float noise between two chunks that met, not a hole.
  if(!merged.empty()&&s.start<=merged.back().end+0.05)merged.back().end=(std::max)(merged.back().end,s.end);
  else merged.push_back(s);
 }
 spans.swap(merged);
}
bool CoverageContains(const std::vector<Span>& spans,double t){
 for(const auto& s:spans)if(t>=s.start&&t<s.end)return true;
 return false;
}
double CoverageFirstGap(const std::vector<Span>& spans,double from,double until){
 double t=from;
 for(const auto& s:spans){
  if(s.end<=t)continue;
  if(s.start>t)break;
  t=s.end;
 }
 return t<until?t:-1;
}
double CoverageSeconds(const std::vector<Span>& spans){
 double total=0;
 for(const auto& s:spans)total+=s.end-s.start;
 return total;
}

// -------------------------------------------------------------- planning ---
ChunkPlan PlanChunk(const std::vector<Span>& covered,double playhead,double duration,
                    double lookahead,double chunkSeconds){
 ChunkPlan plan;
 if(duration<=0||chunkSeconds<=0)return plan;
 // A little behind the playhead as well: the line being spoken as a seek lands
 // started before the landing point.
 double from=(std::max)(0.0,playhead-2.0);
 double until=(std::min)(duration,playhead+lookahead);
 if(until<=from)return plan;
 double gap=CoverageFirstGap(covered,from,until);
 if(gap<0)return plan;
 // A sliver between the last audio sample and the container's stated length
 // is not audio. Asking the decoder for it only ever fails.
 if(duration-gap<0.5)return plan;
 plan.work=true;
 plan.start=gap;
 plan.length=(std::min)(chunkSeconds,duration-gap);
 // A sliver at the very end of a film is folded into this chunk rather than
 // left as a two-second job of its own.
 if(duration-(gap+plan.length)<3.0)plan.length=duration-gap;
 return plan;
}
bool ChunkAbandoned(double chunkStart,double chunkLength,double playhead,double lookahead){
 double end=chunkStart+chunkLength;
 return end<playhead-30.0||chunkStart>playhead+lookahead+30.0;
}

SettledChunk SettleChunk(std::vector<AiCue> cues,double chunkStart,double chunkEnd,bool finalChunk){
 SettledChunk settled;
 std::sort(cues.begin(),cues.end(),[](const AiCue& a,const AiCue& b){return a.start<b.start;});
 if(finalChunk||cues.empty()){
  settled.kept=std::move(cues);
  settled.coveredUntil=chunkEnd;
  return settled;
 }
 const AiCue& last=cues.back();
 // Only a line that reaches the chunk's end can have been cut by it.
 bool cut=last.end>=chunkEnd-1.5;
 double resume=last.start;
 // Dropping it must still move the work forward by a meaningful stretch, or a
 // single long monologue would be re-transcribed forever.
 if(!cut||resume<chunkStart+(chunkEnd-chunkStart)*0.5){
  settled.kept=std::move(cues);
  settled.coveredUntil=chunkEnd;
  return settled;
 }
 cues.pop_back();
 settled.kept=std::move(cues);
 settled.coveredUntil=resume;
 return settled;
}

// ------------------------------------------------------- hallucination ----
bool CueSuppressed(const AiCue& cue,const std::vector<AiCue>& previous){
 auto text=Trim(cue.text);
 auto essence=Essence(text);
 if(essence.empty())return true;
 // Speech the model itself doubts, over what it thinks was not speech.
 if(cue.noSpeech>0.6f&&cue.confidence<0.5f)return true;
 // A long stretch with almost nothing said, said without confidence.
 double length=cue.end-cue.start;
 if(length>12.0&&double(essence.size())/length<0.6&&cue.confidence<0.6f)return true;
 // Lines learned from subtitle credits rather than heard in the film.
 const wchar_t* credits[]={
  L"субтитрысделал",L"субтитрысоздавал",L"редакторсубтитров",L"корректорсубтитров",
  L"продолжениеследует",L"спасибозапросмотр",L"подписывайтесьнаканал",L"ставьтелайки",
  L"thankyouforwatching",L"subtitlesby",L"amaraorg",L"pleasesubscribe",L"transcribedby",
 };
 for(auto credit:credits)if(essence.find(credit)!=std::wstring::npos)return true;
 // A line the model is stuck on: the same words for the third time running.
 if(previous.size()>=2){
  const auto& a=previous[previous.size()-1];
  const auto& b=previous[previous.size()-2];
  if(Essence(a.text)==essence&&Essence(b.text)==essence&&cue.start-b.start<30.0)return true;
 }
 // A short phrase repeated inside one cue many times over.
 if(essence.size()>=24){
  for(size_t unit=2;unit<=essence.size()/4;unit++){
   auto piece=essence.substr(0,unit);
   size_t repeats=0,at=0;
   while(at+unit<=essence.size()&&essence.compare(at,unit,piece)==0){repeats++;at+=unit;}
   if(repeats>=4&&at>=essence.size()*9/10)return true;
  }
 }
 return false;
}

void InsertCues(std::vector<AiCue>& store,const std::vector<AiCue>& fresh){
 for(const auto& cue:fresh){
  store.erase(std::remove_if(store.begin(),store.end(),[&](const AiCue& old){
   return old.start<cue.end&&cue.start<old.end;
  }),store.end());
  auto at=std::lower_bound(store.begin(),store.end(),cue,[](const AiCue& a,const AiCue& b){return a.start<b.start;});
  store.insert(at,cue);
 }
}

const AiCue* CueAt(const std::vector<AiCue>& store,double t){
 auto it=std::upper_bound(store.begin(),store.end(),t,[](double value,const AiCue& cue){return value<cue.start;});
 if(it==store.begin())return nullptr;
 --it;
 return t<it->end?&*it:nullptr;
}

// ----------------------------------------------------------------- export ---
std::string ExportSrt(const std::vector<AiCue>& cues){
 std::string out;
 int index=1;
 for(const auto& cue:cues){
  auto text=Narrow(Trim(cue.text));
  if(text.empty())continue;
  out+=std::to_string(index++)+"\r\n"+Stamp(cue.start,',')+" --> "+Stamp(cue.end,',')+"\r\n"+text+"\r\n\r\n";
 }
 return out;
}
std::string ExportVtt(const std::vector<AiCue>& cues){
 std::string out="WEBVTT\n\n";
 for(const auto& cue:cues){
  auto text=Narrow(Trim(cue.text));
  if(text.empty())continue;
  // "-->" inside the text would end the cue header for a VTT reader.
  for(size_t at;(at=text.find("-->"))!=std::string::npos;)text.replace(at,3,"->");
  out+=Stamp(cue.start,'.')+" --> "+Stamp(cue.end,'.')+"\n"+text+"\n\n";
 }
 return out;
}

// ------------------------------------------------------------------ cache ---
std::wstring AiCacheKey(const std::wstring& mediaSignature,const std::wstring& modelId,
                        const std::wstring& modelHash,const std::wstring& language,
                        long long audioTrack,int optionsVersion){
 return Lower(mediaSignature)+L"|"+modelId+L"|"+modelHash+L"|"+(language.empty()?L"auto":language)+
        L"|aid"+std::to_wstring(audioTrack)+L"|v"+std::to_wstring(optionsVersion);
}
std::wstring AiCacheFileName(const std::wstring& key){
 // FNV-1a over the key's UTF-8, twice with different bases: a name, not a secret.
 auto bytes=Narrow(key);
 unsigned long long a=1469598103934665603ull,b=1099511628211ull*31ull;
 for(unsigned char c:bytes){a=(a^c)*1099511628211ull;b=(b^c)*1099511628211ull+0x9E3779B97F4A7C15ull;}
 wchar_t name[48];
 swprintf_s(name,L"%016llx%016llx.transcript",a,b);
 return name;
}

std::string SerializeTranscript(const Transcript& t){
 std::string out="VETRO-TRANSCRIPT 1\n";
 auto escape=[](const std::wstring& text){
  std::string s=Narrow(text),r;
  for(char c:s){
   if(c=='\n')r+="\\n";
   else if(c=='\r')continue;
   else if(c=='\\')r+="\\\\";
   else r.push_back(c);
  }
  return r;
 };
 out+="key "+escape(t.key)+"\n";
 out+="language "+escape(t.language)+"\n";
 char line[96];
 for(const auto& s:t.covered){snprintf(line,sizeof line,"covered %.3f %.3f\n",s.start,s.end);out+=line;}
 for(const auto& s:t.analysed){snprintf(line,sizeof line,"analysed %.3f %.3f\n",s.start,s.end);out+=line;}
 for(const auto& s:t.speech){snprintf(line,sizeof line,"speech %.3f %.3f\n",s.start,s.end);out+=line;}
 for(const auto& cue:t.cues){
  snprintf(line,sizeof line,"cue %.3f %.3f %.3f %.3f ",cue.start,cue.end,cue.confidence,cue.noSpeech);
  out+=line;out+=escape(cue.text);out+="\n";
  for(const auto& word:cue.words){
   snprintf(line,sizeof line,"word %.3f %.3f %.3f ",word.start,word.end,word.p);
   out+=line;out+=escape(word.text);out+="\n";
  }
 }
 return out;
}

bool ParseTranscript(const std::string& utf8,Transcript& out){
 out=Transcript{};
 size_t at=0;
 bool header=false;
 auto unescape=[](const std::string& s){
  std::string r;
  for(size_t i=0;i<s.size();i++){
   if(s[i]=='\\'&&i+1<s.size()){r.push_back(s[i+1]=='n'?'\n':s[i+1]);i++;}
   else r.push_back(s[i]);
  }
  return Widen(r);
 };
 while(at<utf8.size()){
  auto end=utf8.find('\n',at);
  if(end==std::string::npos)end=utf8.size();
  std::string line=utf8.substr(at,end-at);
  at=end+1;
  if(!header){if(line!="VETRO-TRANSCRIPT 1")return false;header=true;continue;}
  auto space=line.find(' ');
  std::string tag=line.substr(0,space);
  std::string rest=space==std::string::npos?std::string():line.substr(space+1);
  if(tag=="key")out.key=unescape(rest);
  else if(tag=="language")out.language=unescape(rest);
  else if(tag=="covered"||tag=="analysed"||tag=="speech"){
   Span s;
   if(sscanf_s(rest.c_str(),"%lf %lf",&s.start,&s.end)!=2)return false;
   (tag=="covered"?out.covered:tag=="analysed"?out.analysed:out.speech).push_back(s);
  }else if(tag=="cue"||tag=="word"){
   int consumed=0;
   if(tag=="cue"){
    AiCue cue;
    if(sscanf_s(rest.c_str(),"%lf %lf %f %f %n",&cue.start,&cue.end,&cue.confidence,&cue.noSpeech,&consumed)<4)return false;
    cue.text=unescape(rest.substr(size_t(consumed)));
    out.cues.push_back(std::move(cue));
   }else{
    if(out.cues.empty())return false;
    AiWord word;
    if(sscanf_s(rest.c_str(),"%lf %lf %f %n",&word.start,&word.end,&word.p,&consumed)<3)return false;
    word.text=unescape(rest.substr(size_t(consumed)));
    out.cues.back().words.push_back(std::move(word));
   }
  }
 }
 return header;
}

// --------------------------------------------------------- silence skip ---
double SilenceSkipTarget(const std::vector<Span>& speech,const std::vector<Span>& analysed,
                         double position,SilenceSkip mode){
 if(mode==SilenceSkip::Off)return -1;
 // Gentle waits out ordinary pauses and lands with a breath before the voice;
 // Aggressive trims anything a listener would call a gap.
 double minimumGap=mode==SilenceSkip::Gentle?2.0:0.8;
 double pad=mode==SilenceSkip::Gentle?0.4:0.15;
 if(!CoverageContains(analysed,position))return -1;
 for(const auto& s:speech)if(position>=s.start-pad&&position<s.end+pad)return -1;   // someone is talking
 double next=-1;
 for(const auto& s:speech)if(s.start>position){next=s.start;break;}
 // Nothing heard ahead yet: skip only if the silence has been analysed all the
 // way to where speech would have to start. Unknown is not silence.
 if(next<0)return -1;
 if(!CoverageContains(analysed,next-0.01))return -1;
 double target=next-pad;
 if(target-position<minimumGap)return -1;
 return target;
}
