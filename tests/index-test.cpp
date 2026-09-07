#include "../src/index.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
namespace fs=std::filesystem;
bool Supported(const std::wstring& path){auto ext=fs::path(path).extension().wstring();for(auto& c:ext)c=towlower(c);return ext==L".jpg"||ext==L".png";}
int wmain(int argc,wchar_t** argv){
 if(argc<2||argc>3)return 2;
 auto root=fs::absolute(argv[1]);if(fs::exists(root)){std::cerr<<"Fixture directory must be new\n";return 2;}
 fs::create_directories(root/L"album");fs::create_directories(root/L"moved");
 int failures=0;auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";failures+=!ok;};
 auto write=[](const fs::path& p){std::ofstream f(p,std::ios::binary);f<<"owned identity fixture";};
 auto original=root/L"album"/L"original.jpg";write(original);
 auto id=PhysicalId(original.wstring());check(id==PhysicalId(original.wstring()),"same file stable identity");
 auto slash=original.wstring();for(auto& c:slash)if(c==L'\\')c=L'/';
 check(id==PhysicalId(slash),"separator normalization");
 auto renamed=root/L"album"/L"renamed.jpg";fs::rename(original,renamed);check(id==PhysicalId(renamed.wstring()),"rename preserves identity");
 auto moved=root/L"moved"/L"renamed.jpg";fs::rename(renamed,moved);check(id==PhysicalId(moved.wstring()),"move preserves identity");
 write(original);check(id!=PhysicalId(original.wstring()),"different file different identity");
 auto replacement=PhysicalId(original.wstring());fs::remove(original);write(original);check(replacement!=PhysicalId(original.wstring()),"delete recreate changes identity");
 fs::path secondRoot;
 if(argc==3){secondRoot=fs::absolute(argv[2]);if(fs::exists(secondRoot)){std::cerr<<"Second fixture directory must be new\n";return 2;}
  fs::create_directories(secondRoot/L"album");auto twin=secondRoot/L"album"/L"original.jpg";write(twin);
  check(PhysicalId(original.wstring())!=PhysicalId(twin.wstring()),"same relative path on another volume differs");}
 auto folderId=PhysicalId((root/L"album").wstring());fs::rename(root/L"album",root/L"renamed-album");check(folderId==PhysicalId((root/L"renamed-album").wstring()),"folder rename preserves identity");
 constexpr int bulkCount=10000;
 for(int i=0;i<bulkCount;++i)write(root/L"renamed-album"/(std::to_wstring(i)+L".jpg"));
 FolderEntry folder;auto start=std::chrono::steady_clock::now();auto photos=IndexInspectDirectory((root/L"renamed-album").wstring(),folder);
 auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 check(photos.size()==bulkCount+1&&folder.photoCount==bulkCount+1,"targeted directory count 10001");
 check(folder.id==folderId,"scan uses physical folder identity");
 std::cout<<"targeted_scan_ms="<<ms<<"\n";
 write(root/L"renamed-album"/L".nomedia");photos=IndexInspectDirectory((root/L"renamed-album").wstring(),folder);check(photos.empty(),"nomedia excludes directory");
 fs::create_directories(root/L"empty");photos=IndexInspectDirectory((root/L"empty").wstring(),folder);check(photos.empty()&&!folder.path.empty()&&folder.id!=0,"empty folder keeps identity for removal");
 std::cout<<"failures="<<failures<<"\n";
 std::error_code cleanupError;fs::remove_all(root,cleanupError);check(!cleanupError,"owned fixtures cleaned");
 if(!secondRoot.empty()){cleanupError.clear();fs::remove_all(secondRoot,cleanupError);check(!cleanupError,"second-volume fixtures cleaned");}
 return failures?1:0;
}
