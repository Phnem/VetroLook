// Vetro Look, GPL-3.0-or-later.
// The presentation-mode lifecycle. See modes.h for why this is a state machine.
#include "modes.h"

ModeCaps CapsFor(MediaKind kind){
 ModeCaps caps;
 switch(kind){
  case MediaKind::Image:
  case MediaKind::AnimatedImage:
   caps.canZoom=caps.canCopyFrame=caps.canShowInfo=caps.canEdit=true;
   // An animated image plays in the sense that its frames advance, but nothing
   // in the viewer drives that yet, and claiming the capability would put a
   // timeline under a still photograph.
   break;
  case MediaKind::Video:
   caps.canPlay=caps.canSeek=caps.canFrameStep=caps.canCopyFrame=caps.canShowInfo=true;
   // Zoom and edit are the photograph's tools. Aspect, rotation and fit arrive
   // with the playback core; they are not the image zoom pipeline reused.
   break;
  case MediaKind::Audio:
   caps.canPlay=caps.canSeek=caps.canShowInfo=true;
   break;
  case MediaKind::Unsupported:
   break;
 }
 return caps;
}

namespace{
ModeState PreparingFor(MediaKind kind){
 switch(kind){
  case MediaKind::Image:
  case MediaKind::AnimatedImage: return ModeState::ImagePreparing;
  case MediaKind::Video:
  case MediaKind::Audio:         return ModeState::VideoPreparing;
  default:                       return ModeState::Error;
 }
}
ModeState ActiveFor(MediaKind kind){
 switch(kind){
  case MediaKind::Image:
  case MediaKind::AnimatedImage: return ModeState::ImageActive;
  case MediaKind::Video:
  case MediaKind::Audio:         return ModeState::VideoActive;
  default:                       return ModeState::Error;
 }
}
}

ModeTransition ModeMachine::Open(MediaKind next){
 ModeTransition t;
 t.from=state;
 // Leaving a film is the expensive direction, and it is also the one that must
 // not be skipped when the user walks quickly through a mixed folder: the
 // release is reported on the departure itself, not on arrival.
 if(InVideo()){t.releaseVideo=true;t.checkpoint=true;}
 // A photograph's frame is cheap to drop and expensive to re-decode, so it is
 // only released when the next item is not a photograph: the image cache is
 // what makes neighbour navigation instant.
 if(InImage()&&PreparingFor(next)!=ModeState::ImagePreparing)t.releaseImage=true;
 generation++;
 incoming=next;
 state=next==MediaKind::Unsupported?ModeState::Error:PreparingFor(next);
 if(state!=ModeState::Error)kind=next;
 t.to=state;t.generation=generation;
 return t;
}

ModeTransition ModeMachine::Ready(){
 ModeTransition t;
 t.from=state;t.generation=generation;
 if(state==ModeState::ImagePreparing)state=ModeState::ImageActive;
 else if(state==ModeState::VideoPreparing)state=ModeState::VideoActive;
 // Ready for a mode that is already active is not an error: a photograph
 // refined from its screen tier to full resolution reports readiness again.
 else if(state==ModeState::ImageActive||state==ModeState::VideoActive)state=ActiveFor(kind);
 t.to=state;
 return t;
}

ModeTransition ModeMachine::Failed(){
 ModeTransition t;
 t.from=state;t.generation=generation;
 if(InVideo()){t.releaseVideo=true;}
 state=ModeState::Error;
 t.to=state;
 return t;
}

ModeTransition ModeMachine::Close(){
 ModeTransition t;
 t.from=state;t.generation=generation;
 if(InVideo()){t.releaseVideo=true;t.checkpoint=true;}
 if(InImage())t.releaseImage=true;
 generation++;
 state=ModeState::Empty;
 kind=incoming=MediaKind::Unsupported;
 t.to=state;t.generation=generation;
 return t;
}

const wchar_t* ModeStateName(ModeState state){
 switch(state){
  case ModeState::Empty:              return L"empty";
  case ModeState::Probing:            return L"probing";
  case ModeState::ImagePreparing:     return L"image-preparing";
  case ModeState::ImageActive:        return L"image-active";
  case ModeState::VideoPreparing:     return L"video-preparing";
  case ModeState::VideoActive:        return L"video-active";
  case ModeState::Error:              return L"error";
 }
 return L"?";
}
