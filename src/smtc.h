#pragma once
// Vetro Look, GPL-3.0-or-later.
// The Windows media controls: the overlay a keyboard's play key raises, the
// panel on the lock screen, the thing another application asks to pause when it
// needs the speakers.
//
// A media player that does not appear there is a media player Windows does not
// know is playing, and the viewer's own transport is not the only place a
// person presses pause.
#include <windows.h>
#include <string>

// Attaches the controls to `window`. The transport keys arrive as `message`,
// with the command in wParam as one of SmtcCommand. Safe to call once; later
// calls do nothing.
void SmtcAttach(HWND window,UINT message);
void SmtcDetach();

enum SmtcCommand{SmtcPlay=1,SmtcPause,SmtcToggle,SmtcNext,SmtcPrevious,SmtcStop};

// What Windows should be showing. `title` is what a person would call this
// film; empty `title` means nothing is playing and the controls go quiet.
void SmtcSetMedia(const std::wstring& title,const std::wstring& subtitle);
void SmtcSetPlaying(bool playing,bool hasMedia);
// Where the film has got to, so the system's own timeline agrees with ours.
void SmtcSetTimeline(double position,double duration);
// Whether the neighbour buttons do anything, which depends on the folder rather
// than on the film.
void SmtcSetNeighbours(bool previous,bool next);
bool SmtcAvailable();
