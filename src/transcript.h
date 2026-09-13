#pragma once
// Vetro Look, GPL-3.0-or-later.
// AI subtitles, the part with no model in it (27, 28, 72). What a generated cue
// is, which stretch of the film to transcribe next, where a chunk may be cut,
// which lines are a model talking to itself, how a transcript is exported and
// kept, and where silence may be skipped. Pure: the rules are tested against
// literal cues, not against a GPU that happens to be free.
#include <string>
#include <vector>

// A word with its own timing (27.8). V1 does not highlight words, but the data
// model has them from the first transcript, so a later feature needs no rerun.
struct AiWord{
 double start=0,end=0;
 float p=1;
 std::wstring text;
};
struct AiCue{
 double start=0,end=0;
 std::wstring text;
 float confidence=1;          // mean token probability
 float noSpeech=0;            // the model's own "this was not speech" probability
 std::vector<AiWord> words;
};
struct Span{double start=0,end=0;};

// ------------------------------------------------------------- coverage ----
// Which parts of the film have been analysed. Spans are kept sorted and merged.
void CoverageAdd(std::vector<Span>& spans,Span span);
bool CoverageContains(const std::vector<Span>& spans,double t);
// The first point at or after `from`, before `until`, that is not covered; or a
// negative number when the whole range is.
double CoverageFirstGap(const std::vector<Span>& spans,double from,double until);
double CoverageSeconds(const std::vector<Span>& spans);

// -------------------------------------------------------------- planning ---
// What to transcribe next (27.3): the first gap in a bounded window around the
// playhead. Work behind the viewer or far ahead of them waits; a seek moves the
// window, and what is already transcribed stays.
struct ChunkPlan{bool work=false;double start=0,length=0;};
ChunkPlan PlanChunk(const std::vector<Span>& covered,double playhead,double duration,
                    double lookahead,double chunkSeconds);
// True when a chunk in flight is no longer worth finishing: the viewer has
// moved so far that it lies outside the window they are watching.
bool ChunkAbandoned(double chunkStart,double chunkLength,double playhead,double lookahead);

// How a finished chunk is kept (27.7). The last line of a chunk may have been
// cut by the chunk's end, so it is dropped and the next chunk starts where that
// line began -- the line is heard whole next time, never rewritten on screen.
// The final chunk of a film keeps everything.
struct SettledChunk{std::vector<AiCue> kept;double coveredUntil=0;};
SettledChunk SettleChunk(std::vector<AiCue> cues,double chunkStart,double chunkEnd,bool finalChunk);

// ------------------------------------------------------- hallucination ----
// 27.6: text over silence, a line repeated as if stuck, a long low-confidence
// stretch, and the credits a model learned from subtitle files rather than from
// the film. `previous` is what has been kept so far, oldest first.
bool CueSuppressed(const AiCue& cue,const std::vector<AiCue>& previous);
// Adds cues to a sorted store, replacing anything they overlap.
void InsertCues(std::vector<AiCue>& store,const std::vector<AiCue>& fresh);
// The cue on screen at `t`, or null.
const AiCue* CueAt(const std::vector<AiCue>& store,double t);

// ----------------------------------------------------------------- export ---
std::string ExportSrt(const std::vector<AiCue>& cues);   // UTF-8
std::string ExportVtt(const std::vector<AiCue>& cues);   // UTF-8

// ------------------------------------------------------------------ cache ---
// 27.10: the media, the model, its exact file, the language and the decoding
// options that change the text. A different model is a different transcript.
std::wstring AiCacheKey(const std::wstring& mediaSignature,const std::wstring& modelId,
                        const std::wstring& modelHash,const std::wstring& language,
                        long long audioTrack,int optionsVersion);
// The key reduced to a file name.
std::wstring AiCacheFileName(const std::wstring& key);
struct Transcript{
 std::wstring key;
 std::wstring language;        // detected or chosen
 std::vector<Span> covered;    // transcribed
 std::vector<Span> analysed;   // speech detection has looked here
 std::vector<Span> speech;     // where it heard speech
 std::vector<AiCue> cues;
};
std::string SerializeTranscript(const Transcript& transcript);
bool ParseTranscript(const std::string& utf8,Transcript& out);

// --------------------------------------------------------- silence skip ---
// 28.2. Off by default and never automatic for films; Gentle skips only real
// pauses, Aggressive trims the gaps in a lecture. Returns where to jump, or a
// negative number when the playhead should stay where it is -- including when
// the speech ahead has not been analysed yet.
enum class SilenceSkip{Off,Gentle,Aggressive};
double SilenceSkipTarget(const std::vector<Span>& speech,const std::vector<Span>& analysed,
                         double position,SilenceSkip mode);
