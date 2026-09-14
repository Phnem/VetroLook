#pragma once
// Vetro Look, GPL-3.0-or-later.
// Video Mode: the presentation mode that owns the media surface when the file in
// front of the viewer is a film rather than a photograph.
//
// The mode owns three things and borrows the rest from the shell: a poster frame
// for the moment before playback exists, a playback engine behind
// `IPlaybackEngine`, and the transport controls drawn over the picture. The
// window, the theme, the filmstrip and the navigation are the shell's, exactly
// as they are for a photograph.
//
// Everything expensive happens off the window thread and is tagged with the
// shell's media generation: a film the user has already navigated away from can
// never paint over the one in front of them.
#include "media.h"
#include "playback.h"
#include "shellmedia.h"
#include "streaming.h"
#include "transcript.h"
#include "ui.h"
#include <memory>
#include <string>

// Asks for the first look at `path`, and starts playback when an engine is
// available. Returns immediately; the poster frame and the shell's facts arrive
// on Video Mode's worker and are posted to `notify` as `message`.
void VideoModeEnter(const std::wstring& path,const MediaRoute& route,uint64_t generation,
                    HWND notify,UINT message);

// Takes the worker's result for `generation`, if one has landed. True when the
// surface has something new to draw.
bool VideoModeCollect(uint64_t generation);

// Leaves the mode: stops playback, detaches the engine's content from the
// window, drops the poster and its texture, and marks anything still in flight
// as stale. Cheap, and safe to call when not in video.
void VideoModeLeave();

// Releases GPU-side resources only, keeping the facts and the poster pixels. For
// device loss, where every texture must be recreated but nothing has changed
// about the media.
void VideoModeReleaseTextures();

// Joins the worker and destroys the engine. Called once, at shutdown.
void VideoModeStop();

// ------------------------------------------------------------- playback -----
// The engine had something to say. Drains it, and hands its presentation content
// to the shell the first time that content exists.
void VideoModePump();
// The window's client size in device pixels and its corner radius, so the film
// is rendered at the size it is shown and has the shape of the window it is in.
void VideoModeSurface(unsigned widthPixels,unsigned heightPixels,float cornerRadiusPixels);
// The colour the engine paints around the film, kept in step with the theme.
void VideoModeBackground(uint8_t r,uint8_t g,uint8_t b);

// ---------------------------------------------------------------- quality ----
// The three things the shell knows and the engine cannot: what the output is,
// when frames should be shown, and how much memory the engine's queues may hold.
// Set before the first film when possible -- the colour of the output is fixed
// when the engine is created -- and safe to repeat with an unchanged value.
void VideoModeSetTarget(const PresentationTarget& target);
void VideoModeSetPacing(const PresentationPlan& plan);
void VideoModeSetStreamBudget(const StreamBudget& budget);
// Video enhancement, as decided by the shell (enhance.h). Kept across engines.
void VideoModeSetEnhancement(const EnhancementPlan& plan);
const PresentationTarget& VideoModeTarget();

void VideoModeTogglePlay();
void VideoModeSeekBy(double seconds);
// One displayed frame forward (+1) or back (-1). Pauses the film first.
void VideoModeStepFrame(int direction);

// ---------------------------------------------------------------- product ----
// Nudging sound or text against the picture, for a film mastered slightly out
// of step. The amount belongs to the film and is remembered with it; a delta of
// zero puts it back to nothing.
void VideoModeAdjustSubtitleDelay(double delta);
void VideoModeAdjustAudioDelay(double delta);
double VideoModeSubtitleDelay();
double VideoModeAudioDelay();
// The next or previous chapter, with its title. False when the film has none,
// or when there is nowhere further to go in that direction.
bool VideoModeJumpChapter(int direction,std::wstring& title);
size_t VideoModeChapterCount();
// Writes the frame on screen to the pictures folder. Returns where it went.
std::wstring VideoModeScreenshot();
// The A-B loop, one press at a time: A, then B, then off. Returns which state
// it has just entered.
int VideoModeToggleLoopPoint();
bool VideoModeLoop(double& from,double& to);
// True while both ends of the loop are set, which is what stops the queue from
// carrying a looping film away to the next one.
bool VideoModeLoopActive();
// True once per film, when playback was resumed where it was left off.
bool VideoModeResumed(double& position);
// Bumped by every committed seek. Work begun for an older position must not
// come back as current (20.2).
uint64_t VideoModeSeekGeneration();
// Volume is the engine's, in percent, and mute is separate from it: a film
// silenced and a film at zero are different states to come back from.
void VideoModeSetVolume(double percent);
void VideoModeToggleMute();
void VideoModeSetZoom(double scale);
double VideoModeZoom();
void VideoModeResetZoom();
// The shell owns the window, so the expand control asks it rather than resizing
// anything itself.
void VideoModeSetExpandHandler(void(*toggle)(),bool(*expanded)());
// PiP is the same top-level window changing shape. Video Mode only requests
// that transition; ownership of the HWND and its restore geometry stays with
// the shell.
void VideoModeSetPipHandler(void(*toggle)(),bool(*active)());
bool VideoModeShowingVideo();          // the engine's picture is on screen
const PlaybackSnapshot& VideoModeSnapshot();

// Pointer interaction with the transport controls, in window DIPs. `Down`
// returns true when it took the event, which is how the shell knows not to
// treat the click as something else.
bool VideoModePointerDown(float x,float y);
void VideoModePointerMove(float x,float y);
void VideoModePointerUp(float x,float y);
bool VideoModeOverControls(float x,float y);

// ---------------------------------------------------------------- paint -----
// Draws the poster frame into `viewport`. Called inside the scene, where the
// photograph would be drawn, because that is what it stands in for. Draws
// nothing once the engine's own picture is composited underneath the interface:
// painting over it is exactly what the composition layer exists to avoid.
// `base` is the opaque colour the window falls back to around the frame.
void VideoModePaint(D2D1_RECT_F viewport,const Palette& palette,D2D1_COLOR_F base,float alpha);

// Draws the transport controls, or the file's facts while there is nothing to
// transport. Called after the scene is composited, with the rest of the chrome.
// `dpi` is needed because the frosted backdrop lives in the compositor, which
// measures in device pixels while everything drawn here is in DIPs.
void VideoModePaintOverlay(D2D1_RECT_F viewport,const Palette& palette,float alpha,float dpi);

// Paints the opaque layers of Video Mode's frosted material. The compositor
// supplies the live blurred film below it; the shell calls this for its own
// chrome too so every floating element is made from the same material.
void VideoModePaintMatteSurface(const D2D1_ROUNDED_RECT& shape,float alpha);

// Where the transport will be drawn in `viewport`, so the shell can have the
// film frosted under it first. Empty while there is nothing to transport.
D2D1_RECT_F VideoModeControlPanel(D2D1_RECT_F viewport);
// The small speed / stream card grows from the transport instead of opening a
// separate window. The shell frosts it alongside the main transport.
D2D1_RECT_F VideoModePopupPanel(D2D1_RECT_F viewport);
// The compositor fades its blurred source at exactly the same rate as the
// Direct2D card, preventing a clear after-image while a popup closes.
float VideoModePopupOpacity();
// The timeline's preview card, when the pointer is asking. Frosted by the
// compositor like everything else that floats over the film.
D2D1_RECT_F VideoModePreviewPanel(D2D1_RECT_F viewport);
// The subtitle bubble's shape, so the compositor frosts the film under it -- the
// local blur of 24.3, rather than blurring a whole 4K frame for one small plate.
// Empty while nothing is being said.
D2D1_RECT_F VideoModeSubtitlePanel();
// The subtitle bubble is drawn by the shell after the chrome's fading layer, at
// its own strength: a line of dialogue does not fade out with the controls.
// `chromeAlpha` only decides how far above the transport it sits.
void VideoModePaintSubtitles(D2D1_RECT_F viewport,float chromeAlpha);
// How strongly the bubble is drawn right now, for its glass to match.
float VideoModeSubtitleOpacity();
// The transport's glass as it is drawn at this chrome alpha: it shrinks towards
// its bottom centre while it fades. `radius` receives the matching corner.
D2D1_RECT_F VideoModeControlGlass(D2D1_RECT_F viewport,float alpha,float& radius);
// One line for Diagnostics: which kind of cue is on screen, who is drawing it,
// and the beginning of its text.
std::wstring VideoModeCueDescription();
// How long a silence lasts before the bubble collapses (25.7), and the language
// preferences the engine picks tracks with (22.3).
void VideoModeSetSubtitlePolicy(double collapseAfterSeconds,const std::wstring& subtitleLanguages,
                                const std::wstring& audioLanguages);

// Advances the controls' own springs. Returns true while anything is still
// moving, so the shell keeps drawing frames.
bool VideoModeStep(float seconds);

// What the surface knows about the file, for the title bar and the Info panel.
const MediaFacts& VideoModeFacts();
const MediaRoute& VideoModeRoute();
bool VideoModeHasPoster();
// The engine's error, when it could not play this file. Empty otherwise.
std::wstring VideoModeError();

// ------------------------------------------------------------- streams -----
// What the next `VideoModeEnter` of an address should tell the engine: the
// resolver's separate audio, headers and title. Resets the reconnect state.
void VideoModeSetStreamOptions(const OpenOptions& options);
// Fires a scheduled reconnect when its time has come. Called on a timer; returns
// true when it started one.
bool VideoModeNetworkTick();
// One status line for the shell to show -- reconnecting, restored -- taken once.
bool VideoModeTakeStreamNotice(std::wstring& text);
// The sentence for a failure category.
std::wstring VideoModeFailureText(StreamFailure failure);
// Back to the live edge. False when this is not a live stream.
bool VideoModeJumpToLive();
// The network block of Diagnostics, or empty for a local file.
std::wstring VideoModeNetworkReport();

// -------------------------------------------------------- AI subtitles -----
// On and off for this film; true when on. Generated cues take the bubble.
bool VideoModeToggleAiSubtitles();
bool VideoModeAiSubtitles();
// Local films only: a stream's audio is not fetched a second time.
bool VideoModeAiAvailableHere();
void VideoModeSetSilenceSkip(SilenceSkip mode);
SilenceSkip VideoModeSilenceSkip();
// Writes the generated track beside the film as movie.<lang>.ai.srt. Returns
// where it went, or empty when there was nothing to write.
std::wstring VideoModeAiExport();
std::wstring VideoModeAiReport();
