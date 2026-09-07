#include "../src/dnd.h"
#include <shellapi.h>
#include <iostream>
int wmain(){
 OleInitialize(nullptr);int failures=0;auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";failures+=!ok;};
 std::vector<std::wstring> paths={L"C:\\Vetro test\\one.jpg",L"D:\\two.png"};
 IDataObject* object=CreateFileDataObject(paths);check(object!=nullptr,"create multi-file data object");
 FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
 check(object&&object->QueryGetData(&format)==S_OK,"offers CF_HDROP");
 STGMEDIUM medium{};check(object&&SUCCEEDED(object->GetData(&format,&medium)),"renders CF_HDROP");
 if(medium.hGlobal){
  auto drop=(HDROP)medium.hGlobal;check(DragQueryFileW(drop,0xFFFFFFFF,nullptr,0)==paths.size(),"contains both selected files");
  for(UINT i=0;i<paths.size();++i){UINT n=DragQueryFileW(drop,i,nullptr,0);std::wstring value(n+1,0);DragQueryFileW(drop,i,value.data(),n+1);value.resize(n);check(value==paths[i],"path round trip");}
  ReleaseStgMedium(&medium);
 }
 if(object)object->Release();check(BeginFileDrag({})==E_INVALIDARG,"empty drag rejected cleanly");
 OleUninitialize();std::cout<<"failures="<<failures<<"\n";return failures?1:0;
}
