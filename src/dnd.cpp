// Vetro Look, GPL-3.0-or-later.
// A minimal IDataObject that only ever offers CF_HDROP, and a trivial
// IDropSource, so a thumbnail can be dragged out to Explorer, a browser or
// any other app as the real file(s) rather than a copied bitmap.
#include "dnd.h"
#include <shlobj.h>
#include <ole2.h>
#include <vector>

namespace{

HGLOBAL MakeHDrop(const std::vector<std::wstring>& paths){
 size_t chars=1; // final extra terminator
 for(auto& p:paths)chars+=p.size()+1;
 size_t bytes=sizeof(DROPFILES)+chars*sizeof(wchar_t);
 HGLOBAL global=GlobalAlloc(GHND|GMEM_SHARE,bytes);
 if(!global)return nullptr;
 auto raw=(uint8_t*)GlobalLock(global);
 if(!raw){GlobalFree(global);return nullptr;}
 auto drop=(DROPFILES*)raw;
 drop->pFiles=sizeof(DROPFILES);drop->fWide=TRUE;
 auto text=(wchar_t*)(raw+sizeof(DROPFILES));
 for(auto& p:paths){memcpy(text,p.c_str(),(p.size()+1)*sizeof(wchar_t));text+=p.size()+1;}
 *text=0;
 GlobalUnlock(global);
 return global;
}

class DropSource:public IDropSource{
 LONG refs=1;
public:
 HRESULT __stdcall QueryInterface(REFIID riid,void** out) override{
  if(riid==IID_IUnknown||riid==IID_IDropSource){*out=this;AddRef();return S_OK;}
  *out=nullptr;return E_NOINTERFACE;
 }
 ULONG __stdcall AddRef() override{return InterlockedIncrement(&refs);}
 ULONG __stdcall Release() override{auto left=InterlockedDecrement(&refs);if(!left)delete this;return left;}
 HRESULT __stdcall QueryContinueDrag(BOOL escapePressed,DWORD keyState) override{
  if(escapePressed)return DRAGDROP_S_CANCEL;
  if(!(keyState&MK_LBUTTON))return DRAGDROP_S_DROP;
  return S_OK;
 }
 HRESULT __stdcall GiveFeedback(DWORD) override{return DRAGDROP_S_USEDEFAULTCURSORS;}
};

class DataObject:public IDataObject{
 LONG refs=1;
 std::vector<std::wstring> paths;
public:
 explicit DataObject(std::vector<std::wstring> p):paths(std::move(p)){}
 HRESULT __stdcall QueryInterface(REFIID riid,void** out) override{
  if(riid==IID_IUnknown||riid==IID_IDataObject){*out=this;AddRef();return S_OK;}
  *out=nullptr;return E_NOINTERFACE;
 }
 ULONG __stdcall AddRef() override{return InterlockedIncrement(&refs);}
 ULONG __stdcall Release() override{auto left=InterlockedDecrement(&refs);if(!left)delete this;return left;}
 static bool IsHdrop(const FORMATETC* fmt){
  return fmt&&fmt->cfFormat==CF_HDROP&&(fmt->tymed&TYMED_HGLOBAL)&&fmt->dwAspect==DVASPECT_CONTENT;
 }
 HRESULT __stdcall GetData(FORMATETC* fmt,STGMEDIUM* medium) override{
  if(!IsHdrop(fmt))return DV_E_FORMATETC;
  HGLOBAL global=MakeHDrop(paths);
  if(!global)return E_OUTOFMEMORY;
  medium->tymed=TYMED_HGLOBAL;medium->hGlobal=global;medium->pUnkForRelease=nullptr;
  return S_OK;
 }
 HRESULT __stdcall GetDataHere(FORMATETC*,STGMEDIUM*) override{return E_NOTIMPL;}
 HRESULT __stdcall QueryGetData(FORMATETC* fmt) override{return IsHdrop(fmt)?S_OK:DV_E_FORMATETC;}
 HRESULT __stdcall GetCanonicalFormatEtc(FORMATETC*,FORMATETC* out) override{if(out)out->ptd=nullptr;return DATA_S_SAMEFORMATETC;}
 HRESULT __stdcall SetData(FORMATETC*,STGMEDIUM*,BOOL) override{return E_NOTIMPL;}
 HRESULT __stdcall EnumFormatEtc(DWORD direction,IEnumFORMATETC** out) override{
  if(direction!=DATADIR_GET){*out=nullptr;return E_NOTIMPL;}
  FORMATETC fmt{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
  return SHCreateStdEnumFmtEtc(1,&fmt,out);
 }
 HRESULT __stdcall DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*) override{return OLE_E_ADVISENOTSUPPORTED;}
 HRESULT __stdcall DUnadvise(DWORD) override{return OLE_E_ADVISENOTSUPPORTED;}
 HRESULT __stdcall EnumDAdvise(IEnumSTATDATA**) override{return OLE_E_ADVISENOTSUPPORTED;}
};
}

IDataObject* CreateFileDataObject(const std::vector<std::wstring>& paths){return paths.empty()?nullptr:new DataObject(paths);}

HRESULT BeginFileDrag(const std::vector<std::wstring>& paths,DWORD* performedEffect){
 if(paths.empty())return E_INVALIDARG;
 auto* data=CreateFileDataObject(paths);
 auto* source=new DropSource();
 DWORD effect=0;
 // Blocks, pumping its own loop, until the drop lands or the drag is
 // cancelled — exactly the same call Explorer itself uses.
 HRESULT result=DoDragDrop(data,source,DROPEFFECT_COPY|DROPEFFECT_LINK,&effect);
 data->Release();source->Release();
 if(performedEffect)*performedEffect=effect;
 return result;
}
