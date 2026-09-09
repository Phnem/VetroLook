// Vetro Look, GPL-3.0-or-later.
// Total Commander integration: a public --quicklook command plus the smallest
// possible textual edits to TC's own INI files after explicit user consent.
#include "ui.h"
#include "tc_ini.h"
#include <filesystem>
#include <tlhelp32.h>

namespace{
namespace fs=std::filesystem;
const wchar_t* Command=L"em_VetroLookQuickLook";
// This spelling must remain NOT VERIFIED until captured from a real Total
// Commander UI assignment; textual patch tests alone do not establish it.
const wchar_t* SpaceKey=L"SPACE";
const wchar_t* Shortcuts=L"Shortcuts";
const wchar_t* StateKey=L"Software\\VetroLook\\Integrations";

std::wstring RegString(HKEY root,const wchar_t* key,const wchar_t* name){
 wchar_t value[32768]{};DWORD size=sizeof(value);
 if(RegGetValueW(root,key,name,RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,nullptr,value,&size)!=ERROR_SUCCESS)return {};
 wchar_t expanded[32768]{};
 DWORD n=ExpandEnvironmentStringsW(value,expanded,DWORD(std::size(expanded)));
 return n&&n<std::size(expanded)?expanded:value;
}
void RegWrite(const wchar_t* name,const std::wstring& value){
 RegSetKeyValueW(HKEY_CURRENT_USER,StateKey,name,REG_SZ,value.c_str(),DWORD((value.size()+1)*sizeof(wchar_t)));
}
void RegWriteDword(const wchar_t* name,DWORD value){RegSetKeyValueW(HKEY_CURRENT_USER,StateKey,name,REG_DWORD,&value,sizeof(value));}
bool RegDword(const wchar_t* name){DWORD value=0,size=sizeof(value);return RegGetValueW(HKEY_CURRENT_USER,StateKey,name,RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS&&value!=0;}

std::wstring FindIni(){
 std::vector<std::wstring> candidates;wchar_t env[32768]{};
 if(GetEnvironmentVariableW(L"COMMANDER_INI",env,DWORD(std::size(env))))candidates.push_back(env);
 for(HKEY root:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}){
  auto ini=RegString(root,L"Software\\Ghisler\\Total Commander",L"IniFileName");if(!ini.empty())candidates.push_back(ini);
  auto dir=RegString(root,L"Software\\Ghisler\\Total Commander",L"InstallDir");if(!dir.empty())candidates.push_back((fs::path(dir)/L"wincmd.ini").wstring());
 }
 if(GetEnvironmentVariableW(L"APPDATA",env,DWORD(std::size(env))))candidates.push_back((fs::path(env)/L"GHISLER"/L"wincmd.ini").wstring());
 std::error_code ec;for(auto& candidate:candidates)if(!candidate.empty()&&fs::is_regular_file(fs::path(candidate),ec))return candidate;
 return {};
}
std::wstring UserCommands(const std::wstring& ini){return (fs::path(ini).parent_path()/L"usercmd.ini").wstring();}
bool Backup(const std::wstring& file){
 std::error_code ec;if(!fs::is_regular_file(fs::path(file),ec))return true;
 auto backup=file+L".vetrolook-backup";if(fs::exists(fs::path(backup),ec))return true;
 return CopyFileW(file.c_str(),backup.c_str(),TRUE)!=0;
}
std::wstring Executable(){wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));return n&&n<std::size(path)?path:L"";}
bool TotalCommanderRunning(){
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return false;
 PROCESSENTRY32W entry{sizeof(entry)};bool found=false;
 if(Process32FirstW(snapshot,&entry))do{
  if(!_wcsicmp(entry.szExeFile,L"TOTALCMD.EXE")||!_wcsicmp(entry.szExeFile,L"TOTALCMD64.EXE")){found=true;break;}
 }while(Process32NextW(snapshot,&entry));
 CloseHandle(snapshot);return found;
}
bool WaitForTotalCommander(HWND owner,std::wstring& note){
 while(TotalCommanderRunning()){
  if(MessageBoxW(owner,T(S_TcRunning),L"Vetro Look",MB_RETRYCANCEL|MB_ICONWARNING)!=IDRETRY){note=T(S_TcCancelled);return false;}
 }
 return true;
}
bool LoadOptional(const std::wstring& path,std::vector<uint8_t>& bytes,bool& existed,std::wstring& error){
 std::error_code ec;existed=fs::is_regular_file(fs::path(path),ec);if(!existed){bytes.clear();return true;}return TcIniReadFile(path,bytes,error);
}
std::optional<std::wstring> Value(const std::vector<uint8_t>& bytes,const wchar_t* section,const wchar_t* key){return TcIniValue(bytes,section,key);}
bool WritePair(const std::wstring& commands,const std::vector<uint8_t>& oldCommands,bool commandsExisted,const std::vector<uint8_t>& newCommands,
               const std::wstring& ini,const std::vector<uint8_t>& newIni,std::wstring& error){
 if(!TcIniWriteFileAtomic(commands,newCommands,error))return false;
 if(TcIniWriteFileAtomic(ini,newIni,error))return true;
 std::wstring ignored;
 if(commandsExisted)TcIniWriteFileAtomic(commands,oldCommands,ignored);else DeleteFileW(commands.c_str());
 return false;
}
void ClearState(){
 RegDeleteKeyValueW(HKEY_CURRENT_USER,StateKey,L"TotalCommanderPreviousSpace");
 RegDeleteKeyValueW(HKEY_CURRENT_USER,StateKey,L"TotalCommanderReplacedSpace");
 RegDeleteKeyValueW(HKEY_CURRENT_USER,StateKey,L"TotalCommanderIni");
}
}

TcStatus TotalCommanderStatus(){
 auto ini=FindIni();if(ini.empty())return TcMissing;
 std::vector<uint8_t> bytes;std::wstring error;if(!TcIniReadFile(ini,bytes,error))return TcAvailable;
 auto value=Value(bytes,Shortcuts,SpaceKey);return value&&_wcsicmp(value->c_str(),Command)==0?TcInstalled:TcAvailable;
}

bool TotalCommanderInstall(HWND owner,std::wstring& note){
 auto ini=FindIni();if(ini.empty()){note=T(S_TcNotFound);return false;}
 if(!WaitForTotalCommander(owner,note))return false;
 auto commands=UserCommands(ini);std::vector<uint8_t> iniBytes,commandBytes;bool iniExisted=false,commandsExisted=false;std::wstring error;
 if(!LoadOptional(ini,iniBytes,iniExisted,error)||!LoadOptional(commands,commandBytes,commandsExisted,error)){note=error;return false;}
 auto existing=Value(iniBytes,Shortcuts,SpaceKey).value_or(L"");
 if(!existing.empty()&&_wcsicmp(existing.c_str(),Command)!=0){
  std::wstring question=std::wstring(T(S_TcReplace))+L"\n\n"+SpaceKey+L" = "+existing;
  if(MessageBoxW(owner,question.c_str(),L"Vetro Look",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK){note=T(S_TcCancelled);return false;}
 }
 if(!Backup(ini)||!Backup(commands)){note=T(S_TcFailed);return false;}
 auto newIni=iniBytes,newCommands=commandBytes;auto exe=Executable();
 bool ok=!exe.empty()&&TcIniSet(newCommands,Command,L"cmd",exe,error)&&
  TcIniSet(newCommands,Command,L"param",L"--quicklook \"%P%N\"",error)&&
  TcIniSet(newCommands,Command,L"menu",L"VetroLook Quick Look",error)&&
  TcIniSet(newIni,Shortcuts,SpaceKey,Command,error);
 if(!ok||!WritePair(commands,commandBytes,commandsExisted,newCommands,ini,newIni,error)){note=error.empty()?T(S_TcFailed):error;return false;}
 RegWrite(L"TotalCommanderIni",ini);
 bool replaced=!existing.empty()&&_wcsicmp(existing.c_str(),Command)!=0;
 RegWriteDword(L"TotalCommanderReplacedSpace",replaced?1:0);
 if(replaced)RegWrite(L"TotalCommanderPreviousSpace",existing);else RegDeleteKeyValueW(HKEY_CURRENT_USER,StateKey,L"TotalCommanderPreviousSpace");
 note=T(S_TcInstalled);return true;
}

bool TotalCommanderRemove(std::wstring& note){
 auto ini=FindIni();if(ini.empty())ini=RegString(HKEY_CURRENT_USER,StateKey,L"TotalCommanderIni");
 if(ini.empty()){note=T(S_TcNotFound);return false;}
 if(!WaitForTotalCommander(nullptr,note))return false;
 auto commands=UserCommands(ini);std::vector<uint8_t> iniBytes,commandBytes;bool iniExisted=false,commandsExisted=false;std::wstring error;
 if(!LoadOptional(ini,iniBytes,iniExisted,error)||!LoadOptional(commands,commandBytes,commandsExisted,error)){note=error;return false;}
 auto newIni=iniBytes,newCommands=commandBytes;
 for(auto key:{L"cmd",L"param",L"menu"})if(!TcIniSet(newCommands,Command,key,std::nullopt,error)){note=error;return false;}
 auto current=Value(newIni,Shortcuts,SpaceKey);
 if(current&&_wcsicmp(current->c_str(),Command)==0){
  if(RegDword(L"TotalCommanderReplacedSpace")){
   auto previous=RegString(HKEY_CURRENT_USER,StateKey,L"TotalCommanderPreviousSpace");
   if(!previous.empty()&&!TcIniSet(newIni,Shortcuts,SpaceKey,previous,error)){note=error;return false;}
  }else if(!TcIniSet(newIni,Shortcuts,SpaceKey,std::nullopt,error)){note=error;return false;}
 }
 if(!Backup(ini)||!Backup(commands)||!WritePair(commands,commandBytes,commandsExisted,newCommands,ini,newIni,error)){note=error.empty()?T(S_TcFailed):error;return false;}
 ClearState();note=T(S_TcRemoved);return true;
}
