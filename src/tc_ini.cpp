#include "tc_ini.h"

#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
enum class Encoding { Ansi,Utf8,Utf8Bom,Utf16Le,Utf16Be };
struct Document { Encoding encoding=Encoding::Ansi;std::wstring text; };
struct Line { std::wstring body,eol; };

bool EqualNoCase(const std::wstring& a,const std::wstring& b){
 return CompareStringOrdinal(a.c_str(),int(a.size()),b.c_str(),int(b.size()),TRUE)==CSTR_EQUAL;
}
std::wstring Trim(const std::wstring& s){
 size_t a=0,b=s.size();while(a<b&&iswspace(s[a]))a++;while(b>a&&iswspace(s[b-1]))b--;
 return s.substr(a,b-a);
}
bool Utf8(const uint8_t* p,size_t n,std::wstring& out){
 if(!n){out.clear();return true;}
 int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char*)p,int(n),nullptr,0);
 if(!count)return false;
 out.resize(size_t(count));
 return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char*)p,int(n),out.data(),count)==count;
}
bool Decode(const std::vector<uint8_t>& bytes,Document& doc,std::wstring& error){
 const uint8_t* p=bytes.data();size_t n=bytes.size();
 // A file that has no bytes has no encoding to preserve. Choose an explicit
 // Unicode encoding so a freshly-created usercmd.ini can contain the actual
 // installed path without a lossy 8.3 alias.
 if(!n){doc.encoding=Encoding::Utf8Bom;doc.text.clear();return true;}
 if(n>=2&&p[0]==0xff&&p[1]==0xfe){
  if((n-2)&1){error=L"Malformed UTF-16LE Total Commander INI";return false;}
  doc.encoding=Encoding::Utf16Le;doc.text.resize((n-2)/2);
  for(size_t i=0;i<doc.text.size();i++)doc.text[i]=wchar_t(p[2+i*2]|unsigned(p[3+i*2])<<8);
  return true;
 }
 if(n>=2&&p[0]==0xfe&&p[1]==0xff){
  if((n-2)&1){error=L"Malformed UTF-16BE Total Commander INI";return false;}
  doc.encoding=Encoding::Utf16Be;doc.text.resize((n-2)/2);
  for(size_t i=0;i<doc.text.size();i++)doc.text[i]=wchar_t(unsigned(p[2+i*2])<<8|p[3+i*2]);
  return true;
 }
 if(n>=3&&p[0]==0xef&&p[1]==0xbb&&p[2]==0xbf){
  doc.encoding=Encoding::Utf8Bom;
  if(!Utf8(p+3,n-3,doc.text)){error=L"Malformed UTF-8 Total Commander INI";return false;}
  return true;
 }
 // A BOM-less Total Commander INI is traditionally in the active Windows
 // code page.  ASCII is identical in both encodings; non-ASCII UTF-8 is only
 // selected when it is valid and cannot be a byte-for-byte ACP round trip.
 std::wstring candidate;
 bool validUtf8=Utf8(p,n,candidate),hasHigh=std::any_of(bytes.begin(),bytes.end(),[](uint8_t c){return c>=0x80;});
 if(validUtf8&&hasHigh){
  BOOL lossy=FALSE;int count=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,candidate.c_str(),int(candidate.size()),nullptr,0,nullptr,&lossy);
  std::string round(size_t((std::max)(0,count)),'\0');
  if(count)WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,candidate.c_str(),int(candidate.size()),round.data(),count,nullptr,&lossy);
  if(lossy||round.size()!=n||memcmp(round.data(),p,n)!=0){doc.encoding=Encoding::Utf8;doc.text=std::move(candidate);return true;}
 }
 doc.encoding=Encoding::Ansi;
 int count=MultiByteToWideChar(CP_ACP,0,(const char*)p,int(n),nullptr,0);
 if(!count){error=L"Could not decode Total Commander INI";return false;}
 doc.text.resize(size_t(count));
 MultiByteToWideChar(CP_ACP,0,(const char*)p,int(n),doc.text.data(),count);
 return true;
}
bool Encode(const Document& doc,std::vector<uint8_t>& bytes,std::wstring& error){
 bytes.clear();
 if(doc.encoding==Encoding::Utf16Le||doc.encoding==Encoding::Utf16Be){
  bool le=doc.encoding==Encoding::Utf16Le;bytes={uint8_t(le?0xff:0xfe),uint8_t(le?0xfe:0xff)};
  bytes.reserve(2+doc.text.size()*2);
  for(wchar_t c:doc.text){uint16_t v=uint16_t(c);bytes.push_back(uint8_t(le?v:v>>8));bytes.push_back(uint8_t(le?v>>8:v));}
  return true;
 }
 UINT cp=doc.encoding==Encoding::Ansi?CP_ACP:CP_UTF8;
 DWORD flags=doc.encoding==Encoding::Ansi?WC_NO_BEST_FIT_CHARS:0;BOOL lossy=FALSE;
 int count=WideCharToMultiByte(cp,flags,doc.text.c_str(),int(doc.text.size()),nullptr,0,nullptr,cp==CP_ACP?&lossy:nullptr);
 if(!count&&!doc.text.empty()){error=L"Could not encode Total Commander INI";return false;}
 size_t bom=doc.encoding==Encoding::Utf8Bom?3:0;bytes.resize(bom+size_t(count));
 if(bom){bytes[0]=0xef;bytes[1]=0xbb;bytes[2]=0xbf;}
 if(count)WideCharToMultiByte(cp,flags,doc.text.c_str(),int(doc.text.size()),(char*)bytes.data()+bom,count,nullptr,cp==CP_ACP?&lossy:nullptr);
 if(lossy){error=L"The Total Commander INI encoding cannot represent the Unicode VetroLook path";bytes.clear();return false;}
 return true;
}
std::vector<Line> Lines(const std::wstring& text){
 std::vector<Line> lines;
 for(size_t at=0;at<text.size();){
  size_t end=text.find_first_of(L"\r\n",at);if(end==std::wstring::npos){lines.push_back({text.substr(at),L""});break;}
  size_t after=end+1;if(text[end]==L'\r'&&after<text.size()&&text[after]==L'\n')after++;
  lines.push_back({text.substr(at,end-at),text.substr(end,after-end)});at=after;
 }
 return lines;
}
std::wstring Join(const std::vector<Line>& lines){std::wstring out;for(auto& l:lines){out+=l.body;out+=l.eol;}return out;}
bool SectionName(const std::wstring& body,std::wstring& name){
 auto t=Trim(body);if(t.size()<2||t.front()!=L'['||t.back()!=L']')return false;
 name=Trim(t.substr(1,t.size()-2));return true;
}
bool KeyName(const std::wstring& body,std::wstring& name,size_t& equals){
 size_t first=0;while(first<body.size()&&iswspace(body[first]))first++;
 if(first==body.size()||body[first]==L';'||body[first]==L'#')return false;
 equals=body.find(L'=',first);if(equals==std::wstring::npos)return false;
 name=Trim(body.substr(first,equals-first));return !name.empty();
}
std::wstring PreferredEol(const std::vector<Line>& lines){for(auto& l:lines)if(!l.eol.empty())return l.eol;return L"\r\n";}
bool Locate(const std::vector<Line>& lines,const std::wstring& section,size_t& begin,size_t& end){
 for(size_t i=0;i<lines.size();i++){std::wstring name;if(SectionName(lines[i].body,name)&&EqualNoCase(name,section)){
   begin=i+1;end=lines.size();for(size_t j=begin;j<lines.size();j++){if(SectionName(lines[j].body,name)){end=j;break;}}return true;
  }}return false;
}
std::optional<std::wstring> Value(const Document& doc,const std::wstring& section,const std::wstring& key){
 auto lines=Lines(doc.text);size_t begin=0,end=0;if(!Locate(lines,section,begin,end))return std::nullopt;
 for(size_t i=begin;i<end;i++){std::wstring name;size_t eq=0;if(KeyName(lines[i].body,name,eq)&&EqualNoCase(name,key))return Trim(lines[i].body.substr(eq+1));}
 return std::nullopt;
}
void EnsurePreviousEnds(std::vector<Line>& lines,size_t index,const std::wstring& eol){if(index&&lines[index-1].eol.empty())lines[index-1].eol=eol;}
void Set(Document& doc,const std::wstring& section,const std::wstring& key,const std::optional<std::wstring>& value){
 auto lines=Lines(doc.text);auto eol=PreferredEol(lines);size_t begin=0,end=0;
 if(!Locate(lines,section,begin,end)){
  if(!value)return;
  EnsurePreviousEnds(lines,lines.size(),eol);lines.push_back({L"["+section+L"]",eol});lines.push_back({key+L"="+*value,L""});doc.text=Join(lines);return;
 }
 bool changed=false;
 for(size_t i=begin;i<end;){
  std::wstring name;size_t eq=0;if(!KeyName(lines[i].body,name,eq)||!EqualNoCase(name,key)){i++;continue;}
  if(!value){lines.erase(lines.begin()+ptrdiff_t(i));end--;changed=true;continue;}
  if(!changed){size_t valueAt=eq+1;while(valueAt<lines[i].body.size()&&(lines[i].body[valueAt]==L' '||lines[i].body[valueAt]==L'\t'))valueAt++;
   lines[i].body=lines[i].body.substr(0,valueAt)+*value;changed=true;i++;
  }else{lines.erase(lines.begin()+ptrdiff_t(i));end--;}
 }
 if(value&&!changed){EnsurePreviousEnds(lines,end,eol);lines.insert(lines.begin()+ptrdiff_t(end),Line{key+L"="+*value,end<lines.size()?eol:L""});}
 doc.text=Join(lines);
}
}

std::optional<std::wstring> TcIniValue(const std::vector<uint8_t>& bytes,const std::wstring& section,const std::wstring& key){
 Document doc;std::wstring error;if(!Decode(bytes,doc,error))return std::nullopt;return Value(doc,section,key);
}
bool TcIniSet(std::vector<uint8_t>& bytes,const std::wstring& section,const std::wstring& key,const std::optional<std::wstring>& value,std::wstring& error){
 Document doc;if(!Decode(bytes,doc,error))return false;Set(doc,section,key,value);return Encode(doc,bytes,error);
}
bool TcIniReadFile(const std::wstring& path,std::vector<uint8_t>& bytes,std::wstring& error){
 std::ifstream in(std::filesystem::path(path),std::ios::binary|std::ios::ate);if(!in){error=L"Could not read "+path;return false;}
 auto n=in.tellg();if(n<0||n>64*1024*1024){error=L"Total Commander INI is unexpectedly large";return false;}
 bytes.resize(size_t(n));in.seekg(0);if(n&&!in.read((char*)bytes.data(),n)){error=L"Could not read "+path;return false;}return true;
}
bool TcIniWriteFileAtomic(const std::wstring& path,const std::vector<uint8_t>& bytes,std::wstring& error){
 auto temp=path+L".vetrolook-tmp-"+std::to_wstring(GetCurrentProcessId());
 HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE){error=L"Could not create a temporary INI beside "+path;return false;}
 DWORD written=0;bool ok=bytes.size()<=MAXDWORD&&WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size();
 if(ok)ok=FlushFileBuffers(file)!=0;CloseHandle(file);
 if(ok)ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
 if(!ok){DeleteFileW(temp.c_str());error=L"Could not atomically update "+path;}return ok;
}
