#pragma once
// Vetro Look, GPL-3.0-or-later.
// AI subtitles and speech detection (27, 28, 72, Appendix U).
//
// The runtime is whisper.cpp, loaded by name from `whisper\` beside the
// application the first time it is wanted -- never linked, never loaded for a
// photograph, and a machine without it is a complete player. The model is a
// separate download the viewer asks for once and can remove again.
//
// Audio never leaves the device (27.12). It is taken from the film by a second,
// output-less engine that writes 16 kHz mono far faster than real time, a bounded
// stretch at a time around the playhead, and transcribed on a worker thread that
// the Resource Governor can hold (12). Playback never waits for any of it.
#include <windows.h>
#include <string>
#include "transcript.h"

enum class AiState{
 Unavailable,   // no runtime beside the application
 NeedsModel,    // runtime present, model not downloaded
 Downloading,   // the model is arriving
 Idle,          // nothing wanted, or everything wanted is done
 Loading,       // the model is being put on the device
 Working,       // transcribing or listening for speech
 Held,          // the governor has asked it to wait
 Failed,
};

struct AiStatus{
 AiState state=AiState::Unavailable;
 std::wstring runtimeVersion;
 std::wstring modelName;
 std::wstring device;          // "CUDA", "CPU", as the runtime reports
 double covered=0;             // seconds transcribed in this film
 double analysed=0;            // seconds listened to for speech
 double duration=0;
 double lastChunkSeconds=0;    // audio length of the last chunk
 double lastChunkWall=0;       // and how long it took
 double downloadFraction=0;
 int cues=0;
 std::wstring language;
 std::wstring error;
 bool fromCache=false;
};

// ---------------------------------------------------------- the runtime ----
bool AiRuntimePresent();
// The model this build uses: large-v3-turbo, 5-bit (Appendix U: one balanced
// default, measured rather than guessed).
std::wstring AiModelName();
uint64_t AiModelBytes();
bool AiModelPresent();
// The runtime, the speech detector and the model are all here.
bool AiInstalled();
// What one press of the download row would fetch, and what is on disk now.
uint64_t AiDownloadBytes();
uint64_t AiInstalledBytes();
// Downloads the model from its published location and checks its SHA-256
// before it is used. `message` is posted to `notify` on progress and completion.
void AiModelDownload(HWND notify,UINT message);
void AiModelDownloadCancel();
// Removes the model file. Transcripts already made stay.
bool AiModelRemove();

// -------------------------------------------------------- per film ----------
// A local film with a known length. `audioTrack` is the engine's id for the
// track being heard; a different track is a different transcript.
void AiOpen(const std::wstring& path,const std::wstring& signature,double duration,
            long long audioTrack,HWND notify,UINT message);
void AiClose();
// What is wanted: subtitles, and speech detection for silence skip. Nothing
// runs unless one of them is.
void AiSetWanted(bool subtitles,bool speech);
// "" for automatic, otherwise a Whisper language code such as "ru" or "en".
void AiSetLanguage(const std::wstring& language);
void AiSetPlayhead(double seconds);
// The governor's word (GovernorPolicy::aiTranscription).
void AiSetAllowed(bool allowed);

// The generated cue on screen at `seconds`.
bool AiCueAt(double seconds,AiCue& cue);
bool AiHasCues();
// Where silence skip would jump from `position`, or negative to stay.
double AiSilenceTarget(double position,SilenceSkip mode);
AiStatus AiStatusNow();
// Writes what has been transcribed so far. The film's folder is written to only
// when the viewer asks (27.11).
bool AiExport(const std::wstring& path,bool vtt);
// Every stored transcript and speech map, for "clear all AI data" (72).
bool AiClearCache();

void AiStop();                 // process exit
