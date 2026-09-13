// Vetro Look, GPL-3.0-or-later. See mediastate.h.
#include "mediastate.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fs=std::filesystem;

namespace{
// Enough to cover a season of something and a year of films, small enough that
// the whole store is one short file read at startup.
constexpr size_t MaxEntries=600;
// A film being watched moves its position thirty times a second. The disk hears
// about it once every few seconds, and once more when the film closes.
constexpr double WriteEvery=5.0;

std::mutex mx;
std::unordered_map<std::wstring,MediaState> entries;
bool loaded=false,dirty=false;
double lastWrite=0;

double Now(){return double(GetTickCount64())/1000.0;}

// Not `Folder`: the shell headers already have one, and an ambiguous name in
// a file that includes them is a compile error waiting for the next include.
std::wstring StoreFolder(){
 PWSTR path=nullptr;
 std::wstring out;
 if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path))&&path){
  out=std::wstring(path)+L"\\VetroLook";
  CoTaskMemFree(path);
 }
 return out;
}

// One line per film, tab separated, signature last because it is the only field
// that can contain anything surprising. A line that does not parse is skipped
// rather than aborting the read: a damaged store loses convenience, not films.
void LoadLocked(){
 if(loaded)return;
 loaded=true;
 auto path=fs::path(MediaStatePath());
 std::wifstream file(path);
 if(!file.is_open())return;
 std::wstring line;
 while(std::getline(file,line)){
  std::wistringstream parts(line);
  MediaState state;
  std::wstring signature;
  wchar_t tab=0;
  parts>>state.position>>state.duration>>state.subtitleDelay>>state.audioDelay
       >>state.audioTrack>>state.subtitleTrack>>state.seen;
  parts.get(tab);
  std::getline(parts,signature);
  if(signature.empty()||state.duration<=0)continue;
  state.signature=signature;
  entries[signature]=state;
 }
}

void SaveLocked(){
 auto folder=StoreFolder();
 if(folder.empty())return;
 std::error_code ec;
 fs::create_directories(folder,ec);
 // Oldest first out: the store is a convenience with a budget, not an archive.
 std::vector<const MediaState*> ordered;
 ordered.reserve(entries.size());
 for(auto& entry:entries)ordered.push_back(&entry.second);
 std::sort(ordered.begin(),ordered.end(),
           [](const MediaState* a,const MediaState* b){return a->seen>b->seen;});
 if(ordered.size()>MaxEntries)ordered.resize(MaxEntries);
 // Written beside the real file and moved into place, so an interrupted write
 // cannot leave half a store behind.
 auto target=fs::path(MediaStatePath());
 auto temporary=target;temporary+=L".new";
 {
  std::wofstream file(temporary,std::ios::trunc);
  if(!file.is_open())return;
  for(const MediaState* state:ordered)
   file<<state->position<<L" "<<state->duration<<L" "<<state->subtitleDelay<<L" "
       <<state->audioDelay<<L" "<<state->audioTrack<<L" "<<state->subtitleTrack<<L" "
       <<state->seen<<L"\t"<<state->signature<<L"\n";
 }
 fs::rename(temporary,target,ec);
 if(ec)fs::remove(temporary,ec);
 dirty=false;lastWrite=Now();
}
}

std::wstring MediaStatePath(){
 auto folder=StoreFolder();
 return folder.empty()?std::wstring():folder+L"\\media-state.txt";
}

const MediaState* MediaStateFind(const std::wstring& signature){
 if(signature.empty())return nullptr;
 std::lock_guard lock(mx);
 LoadLocked();
 auto found=entries.find(signature);
 return found==entries.end()?nullptr:&found->second;
}

void MediaStateRemember(const MediaState& state){
 if(state.signature.empty()||state.duration<=0)return;
 std::lock_guard lock(mx);
 LoadLocked();
 auto& slot=entries[state.signature];
 slot=state;
 FILETIME now{};GetSystemTimeAsFileTime(&now);
 slot.seen=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
 dirty=true;
 if(Now()-lastWrite>=WriteEvery)SaveLocked();
}

void MediaStateFlush(){
 std::lock_guard lock(mx);
 if(dirty)SaveLocked();
}

void MediaStateClear(){
 std::lock_guard lock(mx);
 LoadLocked();
 entries.clear();dirty=true;
 SaveLocked();
}

bool MediaStateResumable(const MediaState& state){
 if(state.duration<=0)return false;
 // Nothing to come back to in the first half minute, and nothing left at the
 // end: a film watched to its credits is a film to start again.
 if(state.position<30)return false;
 if(state.position>state.duration-30)return false;
 return state.position<state.duration*0.98;
}
