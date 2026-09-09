#include "../src/tc_ini.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace{
int failures=0;
void Check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";if(!ok)failures++;}
std::vector<uint8_t> Bytes(const char* s){return {(const uint8_t*)s,(const uint8_t*)s+strlen(s)};}
std::vector<uint8_t> Utf16(const std::wstring& s){std::vector<uint8_t> b{0xff,0xfe};for(auto c:s){b.push_back(uint8_t(c));b.push_back(uint8_t(c>>8));}return b;}
}
int main(){
 std::wstring error;
 auto ansi=Bytes("; before\r\n[Configuration]\r\nfoo=bar\r\n\r\n[Shortcuts]\r\n  SPACE = old_command\r\n; keep\r\nF2=cm_RenameOnly\r\n[After]\r\nx=y\r\n");
 auto original=ansi;
 Check(TcIniSet(ansi,L"Shortcuts",L"SPACE",L"em_VetroLookQuickLook",error),"patch ANSI/CRLF");
 Check(TcIniValue(ansi,L"Shortcuts",L"SPACE")==L"em_VetroLookQuickLook","read patched shortcut");
 std::string text((char*)ansi.data(),ansi.size());
 Check(text.find("; before\r\n[Configuration]\r\nfoo=bar")!=std::string::npos&&text.find("; keep\r\nF2=cm_RenameOnly\r\n[After]")!=std::string::npos,"preserve comments, order and unknown keys");
 Check(text.find("  SPACE = em_VetroLookQuickLook\r\n")!=std::string::npos,"preserve key whitespace and CRLF");
 Check(TcIniSet(ansi,L"Shortcuts",L"SPACE",L"old_command",error)&&ansi==original,"round-trip exact bytes");

 auto utf8=Bytes("\xef\xbb\xbf[em_VetroLookQuickLook]\nmenu=Старое\nunknown=stay\n");
 Check(TcIniSet(utf8,L"em_VetroLookQuickLook",L"menu",L"VetroLook быстрый просмотр",error),"patch UTF-8 BOM/LF Unicode");
 Check(utf8.size()>3&&utf8[0]==0xef&&utf8[1]==0xbb&&utf8[2]==0xbf,"preserve UTF-8 BOM");
 Check(std::find(utf8.begin(),utf8.end(),uint8_t('\r'))==utf8.end(),"preserve LF newlines");
 Check(TcIniValue(utf8,L"em_VetroLookQuickLook",L"unknown")==L"stay","preserve unknown command key");

 auto u16=Utf16(L"# note\r\n[Shortcuts]\r\nF3=cm_View\r\n");
 Check(TcIniSet(u16,L"Shortcuts",L"SPACE",L"em_VetroLookQuickLook",error),"patch UTF-16LE");
 Check(u16[0]==0xff&&u16[1]==0xfe&&TcIniValue(u16,L"Shortcuts",L"SPACE")==L"em_VetroLookQuickLook","preserve UTF-16LE BOM and value");
 Check(TcIniSet(u16,L"Shortcuts",L"SPACE",std::nullopt,error),"remove owned shortcut");
 Check(!TcIniValue(u16,L"Shortcuts",L"SPACE").has_value()&&TcIniValue(u16,L"Shortcuts",L"F3")==L"cm_View","remove only requested key");

 auto empty=std::vector<uint8_t>{};
 Check(TcIniSet(empty,L"em_VetroLookQuickLook",L"cmd",L"C:\\Программы\\VetroLook.exe",error),"create Unicode command section");
 Check(empty.size()>3&&empty[0]==0xef&&empty[1]==0xbb&&empty[2]==0xbf,"new command file receives explicit UTF-8 BOM");
 Check(TcIniValue(empty,L"em_VetroLookQuickLook",L"cmd")==L"C:\\Программы\\VetroLook.exe","full Unicode executable path round-trips");
 auto beforeRemove=empty;
 Check(TcIniSet(empty,L"em_VetroLookQuickLook",L"cmd",std::nullopt,error),"remove command key");
 Check(!TcIniValue(empty,L"em_VetroLookQuickLook",L"cmd").has_value(),"command key absent after removal");
 Check(beforeRemove!=empty,"removal changes only owned entry");
 return failures?1:0;
}
