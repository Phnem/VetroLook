// Vetro Look, GPL-3.0-or-later. See subtitles.h.
#include "subtitles.h"
#include <algorithm>
#include <cwctype>

namespace{
bool Contains(const std::wstring& text,const wchar_t* needle){
 return text.find(needle)!=std::wstring::npos;
}
// The override tags of 23.1, the ones that mean a human placed this somewhere on
// purpose. Anything here and the cue belongs to the renderer that was designed
// for it.
const wchar_t* authoredTags[]={
 L"\\pos(",L"\\move(",L"\\clip(",L"\\iclip(",L"\\org(",
 L"\\p1",L"\\p2",L"\\p3",L"\\p4",       // vector drawing
 L"\\t(",L"\\fad(",L"\\fade(",          // transforms and timed fades
 L"\\frx",L"\\fry",L"\\frz",L"\\fax",L"\\fay",
 L"\\k",L"\\K",L"\\kf",L"\\ko",         // karaoke
 L"\\be",L"\\blur",
};
}

CueKind ClassifyCue(const std::wstring& assText){
 if(assText.empty())return CueKind::None;
 // A cue that is only whitespace is nothing on screen, whatever it contains.
 bool anyVisible=false;
 for(wchar_t ch:assText)if(!iswspace(ch)&&ch!=L'\\'&&ch!=L'N'&&ch!=L'n'){anyVisible=true;break;}
 if(!anyVisible)return CueKind::None;
 for(const wchar_t* tag:authoredTags)if(Contains(assText,tag))return CueKind::ComplexAss;
 // An alignment other than the bottom three is a placement decision: a caption
 // pinned to the top of the frame is answering something on screen, and moving
 // it to the bottom would break the answer.
 size_t an=assText.find(L"\\an");
 if(an!=std::wstring::npos&&an+3<assText.size()){
  wchar_t digit=assText[an+3];
  if(digit>=L'4'&&digit<=L'9')return CueKind::ComplexAss;
 }
 size_t a=assText.find(L"\\a");
 if(a!=std::wstring::npos&&a+2<assText.size()&&iswdigit(assText[a+2])){
  // Legacy \a alignment: 1/2/3 are the bottom row, everything else is placed.
  wchar_t digit=assText[a+2];
  if(digit!=L'1'&&digit!=L'2'&&digit!=L'3')return CueKind::ComplexAss;
 }
 return CueKind::Simple;
}

bool CodecIsBitmap(const std::wstring& codec){
 return codec==L"hdmv_pgs_subtitle"||codec==L"pgs"||
        codec==L"dvd_subtitle"||codec==L"vobsub"||
        codec==L"dvb_subtitle"||codec==L"dvb_teletext"||
        codec==L"xsub"||codec==L"bitmap";
}

std::wstring PlainFromAss(const std::wstring& assText){
 std::wstring out;
 out.reserve(assText.size());
 for(size_t i=0;i<assText.size();i++){
  wchar_t ch=assText[i];
  if(ch==L'{'){
   // An override block. Skipped whole: by the time a cue reaches the bubble it
   // has already been classified as dialogue, so what is inside is styling the
   // bubble does not use.
   size_t close=assText.find(L'}',i);
   if(close==std::wstring::npos)break;
   i=close;
   continue;
  }
  if(ch==L'\\'&&i+1<assText.size()){
   wchar_t next=assText[i+1];
   if(next==L'N'||next==L'n'){out.push_back(L'\n');i++;continue;}
   if(next==L'h'){out.push_back(L' ');i++;continue;}   // hard space
  }
  out.push_back(ch);
 }
 // Trailing blank lines would measure as height the bubble does not need.
 while(!out.empty()&&(out.back()==L'\n'||out.back()==L' '))out.pop_back();
 size_t start=0;
 while(start<out.size()&&(out[start]==L'\n'||out[start]==L' '))start++;
 return out.substr(start);
}

BubbleAdvance AdvanceBubble(BubbleState& state,const std::wstring& text,double now,
                            double collapseAfter,bool reducedMotion){
 BubbleAdvance result;
 // Reduce Motion is not a slower animation, it is no animation: the shape is
 // simply there or not (24.4).
 const double openTime=reducedMotion?0.0:0.22;
 const double collapseTime=reducedMotion?0.0:0.34;
 auto enter=[&](BubblePhase phase){
  if(state.phase!=phase){state.phase=phase;state.since=now;}
 };

 if(!text.empty()){
  bool changed=text!=state.text;
  bool wasGone=state.phase==BubblePhase::Hidden;
  bool wasLeaving=state.phase==BubblePhase::Collapsing;
  state.text=text;
  if(wasGone){
   enter(BubblePhase::Opening);
  }else if(wasLeaving){
   // A cue during a collapse retargets the same object; finishing the collapse
   // and then opening a second one would break the illusion that this is one
   // piece of glass (25.5).
   state.phase=BubblePhase::Opening;
   state.since=now-openTime*0.5;     // it is already partly open; do not restart
   result.retarget=true;
  }else{
   if(changed)result.retarget=true;
   enter(BubblePhase::Reading);
  }
  if(state.phase==BubblePhase::Opening){
   double progress=openTime<=0?1.0:(now-state.since)/openTime;
   if(progress>=1){enter(BubblePhase::Reading);result.scale=1;}
   else result.scale=float(0.15+0.85*(progress<0?0:progress));
  }
  result.phase=state.phase;
  result.textAlpha=state.phase==BubblePhase::Opening?float((std::min)(1.0,(now-state.since)/(openTime>0?openTime:1)*1.6)):1.f;
  if(reducedMotion){result.scale=1;result.textAlpha=1;}
  return result;
 }

 // Nothing on screen now.
 switch(state.phase){
  case BubblePhase::Hidden:
   break;
  case BubblePhase::Opening:
  case BubblePhase::Reading:
   enter(BubblePhase::Holding);
   break;
  case BubblePhase::Holding:
   // The bubble holds its shape through the ordinary gaps of a conversation.
   // Blinking out between two lines a second apart is the single most restless
   // thing a subtitle can do (25.2).
   if(now-state.since>=collapseAfter)enter(BubblePhase::Collapsing);
   break;
  case BubblePhase::Collapsing:{
   double progress=collapseTime<=0?1.0:(now-state.since)/collapseTime;
   if(progress>=1){
    state.phase=BubblePhase::Hidden;state.since=now;state.text.clear();
   }
   break;
  }
 }
 result.phase=state.phase;
 if(state.phase==BubblePhase::Collapsing){
  double progress=collapseTime<=0?1.0:(now-state.since)/collapseTime;
  progress=progress<0?0:(progress>1?1:progress);
  // The shape shrinks towards a point; the text leaves first, early enough to
  // stay readable while it is still large enough to read (25.3).
  result.scale=float(1.0-0.92*progress);
  result.textAlpha=float((std::max)(0.0,1.0-progress*2.2));
 }else if(state.phase==BubblePhase::Hidden){
  result.scale=0;result.textAlpha=0;
 }
 return result;
}
