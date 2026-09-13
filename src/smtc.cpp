// Vetro Look, GPL-3.0-or-later. See smtc.h.
//
// Written against the ABI headers rather than C++/WinRT: this application is
// plain COM throughout, the surface needed here is four interfaces wide, and
// adding a language projection for it would be the largest dependency in the
// build for the smallest reason.
#include "smtc.h"
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using Microsoft::WRL::Wrappers::HStringReference;
namespace media=ABI::Windows::Media;

namespace{
ComPtr<media::ISystemMediaTransportControls> controls;
ComPtr<media::ISystemMediaTransportControls2> controls2;
ComPtr<media::ISystemMediaTransportControlsDisplayUpdater> updater;
ComPtr<media::IVideoDisplayProperties> videoProperties;
EventRegistrationToken buttonToken{};
HWND notifyWindow=nullptr;
UINT notifyMessage=0;
bool attached=false;

// The properties last pushed, so an unchanged update is not sent: every Update()
// is a cross-process call, and the position moves thirty times a second.
std::wstring shownTitle,shownSubtitle;
bool shownPlaying=false,shownHasMedia=false;
double shownPosition=-1,shownDuration=-1;
bool shownPrevious=false,shownNext=false;

SmtcCommand FromButton(media::SystemMediaTransportControlsButton button){
 switch(button){
  case media::SystemMediaTransportControlsButton_Play:return SmtcPlay;
  case media::SystemMediaTransportControlsButton_Pause:return SmtcPause;
  case media::SystemMediaTransportControlsButton_Next:return SmtcNext;
  case media::SystemMediaTransportControlsButton_Previous:return SmtcPrevious;
  case media::SystemMediaTransportControlsButton_Stop:return SmtcStop;
  default:return SmtcToggle;
 }
}
}

void SmtcAttach(HWND window,UINT message){
 if(attached)return;
 attached=true;
 notifyWindow=window;notifyMessage=message;
 ComPtr<ISystemMediaTransportControlsInterop> interop;
 if(FAILED(Windows::Foundation::GetActivationFactory(
     HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(),&interop)))
  return;
 if(FAILED(interop->GetForWindow(window,IID_PPV_ARGS(&controls)))||!controls)return;
 controls.As(&controls2);
 controls->get_DisplayUpdater(&updater);

 // The four buttons this application can honestly answer. Windows greys out
 // anything left disabled, which is better than a button that does nothing.
 controls->put_IsPlayEnabled(true);
 controls->put_IsPauseEnabled(true);
 controls->put_IsStopEnabled(true);
 controls->put_IsEnabled(false);        // until something is actually playing

 auto handler=Callback<ABI::Windows::Foundation::ITypedEventHandler<
   media::SystemMediaTransportControls*,media::SystemMediaTransportControlsButtonPressedEventArgs*>>(
  [](media::ISystemMediaTransportControls*,
     media::ISystemMediaTransportControlsButtonPressedEventArgs* args)->HRESULT{
   media::SystemMediaTransportControlsButton button{};
   if(args&&SUCCEEDED(args->get_Button(&button))&&notifyWindow)
    PostMessageW(notifyWindow,notifyMessage,WPARAM(FromButton(button)),0);
   return S_OK;
  });
 if(handler)controls->add_ButtonPressed(handler.Get(),&buttonToken);
}

void SmtcDetach(){
 if(!attached)return;
 if(controls&&buttonToken.value)controls->remove_ButtonPressed(buttonToken);
 if(controls){
  controls->put_IsEnabled(false);
  controls->put_PlaybackStatus(media::MediaPlaybackStatus_Closed);
 }
 videoProperties.Reset();updater.Reset();controls2.Reset();controls.Reset();
 attached=false;notifyWindow=nullptr;
 shownTitle.clear();shownSubtitle.clear();
 shownPosition=shownDuration=-1;
}

bool SmtcAvailable(){return controls!=nullptr;}

void SmtcSetMedia(const std::wstring& title,const std::wstring& subtitle){
 if(!updater)return;
 if(title==shownTitle&&subtitle==shownSubtitle)return;
 shownTitle=title;shownSubtitle=subtitle;
 if(title.empty()){updater->ClearAll();return;}
 updater->put_Type(media::MediaPlaybackType_Video);
 // Asked for after the type is set, not before: the properties object belongs
 // to the kind of media the updater currently describes, and one taken while it
 // was still unset accepts a title that nothing ever reads.
 videoProperties.Reset();
 updater->get_VideoProperties(&videoProperties);
 if(videoProperties){
  videoProperties->put_Title(HStringReference(title.c_str()).Get());
  videoProperties->put_Subtitle(HStringReference(subtitle.c_str()).Get());
 }
 updater->Update();
}

void SmtcSetPlaying(bool playing,bool hasMedia){
 if(!controls)return;
 if(playing==shownPlaying&&hasMedia==shownHasMedia)return;
 shownPlaying=playing;shownHasMedia=hasMedia;
 controls->put_IsEnabled(hasMedia);
 controls->put_PlaybackStatus(!hasMedia?media::MediaPlaybackStatus_Closed:
                              playing?media::MediaPlaybackStatus_Playing:
                                      media::MediaPlaybackStatus_Paused);
}

void SmtcSetNeighbours(bool previous,bool next){
 if(!controls)return;
 if(previous==shownPrevious&&next==shownNext)return;
 shownPrevious=previous;shownNext=next;
 controls->put_IsPreviousEnabled(previous);
 controls->put_IsNextEnabled(next);
}

void SmtcSetTimeline(double position,double duration){
 if(!controls2||duration<=0)return;
 // A second's resolution is what the system's own panel shows; sending more
 // would be a cross-process call per frame for a number nobody can read.
 if(shownDuration==duration&&shownPosition>=0&&fabs(position-shownPosition)<1.0)return;
 shownPosition=position;shownDuration=duration;
 ComPtr<media::ISystemMediaTransportControlsTimelineProperties> timeline;
 if(FAILED(Windows::Foundation::ActivateInstance(
     HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties).Get(),&timeline))||
    !timeline)return;
 auto span=[](double seconds){
  ABI::Windows::Foundation::TimeSpan value{};
  value.Duration=int64_t(seconds*10'000'000.0);
  return value;
 };
 timeline->put_StartTime(span(0));
 timeline->put_MinSeekTime(span(0));
 timeline->put_Position(span(position));
 timeline->put_MaxSeekTime(span(duration));
 timeline->put_EndTime(span(duration));
 controls2->UpdateTimelineProperties(timeline.Get());
}
