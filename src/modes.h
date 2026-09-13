#pragma once
// Vetro Look, GPL-3.0-or-later.
// The presentation-mode lifecycle.
//
// A photograph and a film are two modes of one viewer, not two applications, so
// the window, the filmstrip, the theme and the navigation are never rebuilt
// when the kind of media changes. What does change is which mode owns the media
// surface, and that handover is a state machine rather than a pile of `if`.
//
// The machine is pure: it holds no Direct2D, no decoder and no file. It says
// what state the viewer is in and what has to be released before the next mode
// prepares; the shell does the releasing. That keeps the one interesting rule
// testable -- returning from a film to a photograph must free the film's
// resources exactly once, and must never free the shell.
#include "media.h"
#include <cstdint>

enum class ModeState{
 Empty,                // nothing open
 Probing,              // kind decided, mode not yet chosen
 ImagePreparing,       // decode in flight
 ImageActive,
 VideoPreparing,       // playback session opening, first frame not yet shown
 VideoActive,
 Error,
};
// The plan's lifecycle names an explicit exit state between one mode and the
// next. There is no state here for it on purpose: a departure is reported to the
// shell as the `releaseVideo`/`releaseImage` work below, inside the same call
// that starts the next mode, so there is no window in which the machine is
// between modes and no way for a release to be skipped or run twice. A mode that
// needs to keep the old surface on screen while the next one prepares will hold
// that in the transition, not in a state.

// What the shell may offer for the media in front of it. The shell asks for a
// capability; it never asks which engine is behind the surface.
struct ModeCaps{
 bool canZoom=false;
 bool canPlay=false;
 bool canSeek=false;
 bool canFrameStep=false;
 bool canCopyFrame=false;
 bool canShowInfo=false;
 bool canEdit=false;
};
ModeCaps CapsFor(MediaKind kind);

// The work a transition hands back to the shell. `releaseVideo` is the only
// expensive one, and it is true exactly once per departure from a video state.
struct ModeTransition{
 ModeState from=ModeState::Empty;
 ModeState to=ModeState::Empty;
 bool releaseVideo=false;     // detach presentation, cancel preview/AI, stop playback
 bool releaseImage=false;     // drop the decoded frame and its GPU texture
 bool checkpoint=false;       // persist the playback position before leaving
 uint64_t generation=0;       // the media generation this transition belongs to
 bool Changed()const{return from!=to;}
};

// One per window. Written only from the thread that owns the window: every
// worker result carries the generation it was started for and is dropped when
// that generation is no longer current.
struct ModeMachine{
 ModeState state=ModeState::Empty;
 MediaKind kind=MediaKind::Unsupported;      // what the active surface holds
 MediaKind incoming=MediaKind::Unsupported;  // what the pending open holds
 uint64_t generation=0;

 // A new media item was requested. Bumps the generation, so anything already
 // in flight for the previous item can be recognised as stale, and reports what
 // the outgoing mode must release.
 ModeTransition Open(MediaKind next);
 // The first frame -- a decoded photograph, or a film's first presented frame
 // -- is on screen.
 ModeTransition Ready();
 // The open failed. The surface shows the error, not the previous media.
 ModeTransition Failed();
 // The viewer left the media surface entirely: library, album, or shutdown.
 ModeTransition Close();

 bool InVideo()const{return state==ModeState::VideoPreparing||state==ModeState::VideoActive;}
 bool InImage()const{return state==ModeState::ImagePreparing||state==ModeState::ImageActive;}
 ModeCaps Caps()const{return CapsFor(kind);}
};

// Diagnostics only.
const wchar_t* ModeStateName(ModeState state);
