#pragma once
// Vetro Look, GPL-3.0-or-later.
// Subtitles: what to draw ourselves, and what to leave alone.
//
// Two kinds of subtitle arrive through the same track and they deserve opposite
// treatment. Plain dialogue is ours: it is text with timings, and the viewer
// draws it in its own material, on its own layer, where it is legible over any
// picture. Authored typesetting -- a sign positioned on a shop front, karaoke
// timed to a syllable, a rotated caption clipped to a window -- is somebody's
// deliberate work, and redrawing it as a rounded bubble would destroy it (23.1).
// So it is handed to the engine's own renderer untouched. Bitmap subtitles are
// pictures; they have no text to re-lay-out at all (26.2).
//
// The classifier below decides, per cue rather than per file, because one ASS
// track routinely carries both (23.2). Everything here is pure: the decision is
// made from the cue's own text, and the bubble's motion is arithmetic on a state
// and a clock, so both can be tested without a film.
#include <cstdint>
#include <string>

enum class CueKind{
 None,          // nothing on screen
 Simple,        // plain dialogue: the viewer's own bubble
 ComplexAss,    // authored typesetting: the engine's renderer, untouched
 Bitmap,        // PGS, VobSub, DVB: a picture, drawn by the engine
};

// Reads one cue's ASS text -- everything between the commas of a Dialogue line,
// as the engine hands it over -- and decides who should draw it. Text with no
// override tags at all is dialogue; the tags that mean authorship are listed in
// 23.1 and are what this looks for.
CueKind ClassifyCue(const std::wstring& assText);
// True for a subtitle codec that carries pictures rather than text.
bool CodecIsBitmap(const std::wstring& codec);

// The dialogue itself, with ASS override tags, drawing commands and hard line
// breaks resolved into something a text layout can take.
std::wstring PlainFromAss(const std::wstring& assText);

// ---------------------------------------------------------------- motion ----
// The bubble is a physical object, not a label that switches on and off (25).
// Between two lines close together it holds its shape and morphs; after a real
// silence it collapses into a point and disappears from there; a line arriving
// mid-collapse retargets the same object rather than starting a second one.
enum class BubblePhase{
 Hidden,
 Opening,     // a point expanding into the measured shape
 Reading,     // a cue is on screen
 Holding,     // the cue ended, another may be along shortly
 Collapsing,  // the silence has lasted; shrinking towards a point
};

struct BubbleState{
 BubblePhase phase=BubblePhase::Hidden;
 double since=0;        // when the current phase began, in seconds
 std::wstring text;     // what is being shown, which outlives the cue in Holding
 double cueEnd=0;
};

// What the bubble should be doing now. `text` empty means no cue at this moment.
// `collapseAfter` is the silence threshold of 25.7, three seconds by default.
// `readingTime` is how long the cue that just ended was on screen: a cue shorter
// than the animation gets a shorter animation rather than less reading time
// (25.6).
struct BubbleAdvance{
 BubblePhase phase=BubblePhase::Hidden;
 bool retarget=false;   // the geometry must be re-aimed at new text
 float scale=1;         // 0 at the point, 1 at the measured shape
 float textAlpha=1;     // the text leaves before the shape does
};
BubbleAdvance AdvanceBubble(BubbleState& state,const std::wstring& text,double now,
                            double collapseAfter,bool reducedMotion);
