// Vetro Look, GPL-3.0-or-later.
#include "actions.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
struct WindowsError{HRESULT code;}; static void Check(HRESULT hr){if(FAILED(hr))throw WindowsError{hr};}
Image RotatedPixels(const Image& image,int degrees,bool white){
 int turn=((degrees/90)%4+4)%4;Image out;out.w=turn%2?image.h:image.w;out.h=turn%2?image.w:image.h;
 out.pixels.resize(size_t(out.w)*out.h*4);
 for(unsigned y=0;y<image.h;y++)for(unsigned x=0;x<image.w;x++){
  unsigned dx=x,dy=y;
  if(turn==1){dx=image.h-1-y;dy=x;}else if(turn==2){dx=image.w-1-x;dy=image.h-1-y;}else if(turn==3){dx=y;dy=image.w-1-x;}
  auto src=&image.pixels[(size_t(y)*image.w+x)*4];auto dst=&out.pixels[(size_t(dy)*out.w+dx)*4];unsigned a=src[3];
  for(int c=0;c<3;c++)dst[c]=white?uint8_t(std::min(255u,unsigned(src[c])+255-a)):(a?uint8_t(std::min(255u,(unsigned(src[c])*255+a/2)/a)):0);
  dst[3]=white?255:uint8_t(a);
 }return out;
}
bool WriteImage(const Image& image,int degrees,const std::wstring& path,std::wstring& error){
 std::wstring temp;
 try{
  auto ext=fs::path(path).extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),towlower);
  GUID container=GUID_ContainerFormatPng;bool white=false;
  if(ext==L".jpg"||ext==L".jpeg"||ext==L".jfif"){container=GUID_ContainerFormatJpeg;white=true;}
  else if(ext==L".bmp"){container=GUID_ContainerFormatBmp;white=true;}
  else if(ext==L".tif"||ext==L".tiff")container=GUID_ContainerFormatTiff;
  else if(ext!=L".png"){error=L"Для сохранения выберите PNG, JPEG, BMP или TIFF.";return false;}
  auto pixels=RotatedPixels(image,degrees,white);
  GUID id;Check(CoCreateGuid(&id));wchar_t unique[40];StringFromGUID2(id,unique,40);
  temp=path+L"."+unique+L".tmp";
  {
   ComPtr<IWICImagingFactory> factory;Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
   ComPtr<IWICStream> stream;Check(factory->CreateStream(&stream));Check(stream->InitializeFromFilename(temp.c_str(),GENERIC_WRITE));
   ComPtr<IWICBitmapEncoder> encoder;Check(factory->CreateEncoder(container,nullptr,&encoder));Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
   ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> options;Check(encoder->CreateNewFrame(&frame,&options));
   if(container==GUID_ContainerFormatJpeg){PROPBAG2 prop{};prop.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=.95f;Check(options->Write(1,&prop,&quality));}
   Check(frame->Initialize(options.Get()));Check(frame->SetSize(pixels.w,pixels.h));Check(frame->SetResolution(96,96));
   WICPixelFormatGUID format=white?GUID_WICPixelFormat24bppBGR:GUID_WICPixelFormat32bppBGRA;Check(frame->SetPixelFormat(&format));
   ComPtr<IWICBitmap> bitmap;Check(factory->CreateBitmapFromMemory(pixels.w,pixels.h,GUID_WICPixelFormat32bppBGRA,pixels.w*4,UINT(pixels.pixels.size()),pixels.pixels.data(),&bitmap));
   ComPtr<IWICFormatConverter> converted;Check(factory->CreateFormatConverter(&converted));Check(converted->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
   Check(frame->WriteSource(converted.Get(),nullptr));Check(frame->Commit());Check(encoder->Commit());
  }
  // Encode completely before replacing the destination; failed writes preserve the original.
  if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES){if(!ReplaceFileW(path.c_str(),temp.c_str(),nullptr,0,nullptr,nullptr))throw std::runtime_error("replace");}
  else if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("move");
  return true;
 }catch(...){if(!temp.empty())DeleteFileW(temp.c_str());error=L"Не удалось сохранить файл. Проверьте доступ к папке и свободное место.";return false;}
}
bool RecycleImage(HWND owner,const std::wstring& path,std::wstring& error){
 try{ComPtr<IFileOperation> operation;Check(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation)));if(owner)Check(operation->SetOwnerWindow(owner));Check(operation->SetOperationFlags(FOF_ALLOWUNDO|FOFX_RECYCLEONDELETE|FOF_NOCONFIRMATION|FOF_NOERRORUI));ComPtr<IShellItem> item;Check(SHCreateItemFromParsingName(fs::path(path).make_preferred().c_str(),nullptr,IID_PPV_ARGS(&item)));Check(operation->DeleteItem(item.Get(),nullptr));Check(operation->PerformOperations());BOOL aborted=FALSE;Check(operation->GetAnyOperationsAborted(&aborted));if(aborted){error=L"Удаление отменено.";return false;}return true;}catch(const WindowsError& failure){error=L"Не удалось переместить файл в корзину. HRESULT="+std::to_wstring((unsigned long)failure.code);return false;}catch(...){error=L"Не удалось переместить файл в корзину.";return false;}
}
void PrintImage(HWND owner,const Image& image,int degrees){
 PRINTDLGW dialog{sizeof(dialog)};dialog.hwndOwner=owner;dialog.Flags=PD_RETURNDC|PD_USEDEVMODECOPIESANDCOLLATE|PD_NOSELECTION|PD_NOPAGENUMS;
 if(PrintDlgW(&dialog)){
  if(dialog.hDC){bool started=false;try{auto pixels=RotatedPixels(image,degrees,true);DOCINFOW doc{sizeof(doc)};doc.lpszDocName=L"Vetro Look";if(StartDocW(dialog.hDC,&doc)<=0)throw std::runtime_error("print");started=true;if(StartPage(dialog.hDC)<=0)throw std::runtime_error("page");
   int w=GetDeviceCaps(dialog.hDC,HORZRES),h=GetDeviceCaps(dialog.hDC,VERTRES);double scale=std::min(double(w)/pixels.w,double(h)/pixels.h);int dw=int(pixels.w*scale),dh=int(pixels.h*scale);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=pixels.w;info.bmiHeader.biHeight=-int(pixels.h);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;SetStretchBltMode(dialog.hDC,HALFTONE);SetBrushOrgEx(dialog.hDC,0,0,nullptr);if(StretchDIBits(dialog.hDC,(w-dw)/2,(h-dh)/2,dw,dh,0,0,pixels.w,pixels.h,pixels.pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY)==GDI_ERROR)throw std::runtime_error("pixels");if(EndPage(dialog.hDC)<=0||EndDoc(dialog.hDC)<=0)throw std::runtime_error("spool");
  }catch(...){if(started)AbortDoc(dialog.hDC);MessageBoxW(owner,L"Не удалось отправить изображение на печать.",L"Vetro Look",MB_OK|MB_ICONERROR);}DeleteDC(dialog.hDC);}
 }
 if(dialog.hDevMode)GlobalFree(dialog.hDevMode);if(dialog.hDevNames)GlobalFree(dialog.hDevNames);
}




Image CropPixels(const Image& image,int x,int y,unsigned w,unsigned h){
 Image out;
 if(!image.w||!image.h)return out;
 x=std::clamp(x,0,int(image.w)-1);y=std::clamp(y,0,int(image.h)-1);
 out.w=std::min(w?w:1u,image.w-unsigned(x));out.h=std::min(h?h:1u,image.h-unsigned(y));
 out.pixels.resize(size_t(out.w)*out.h*4);
 for(unsigned row=0;row<out.h;row++)
  memcpy(out.pixels.data()+size_t(row)*out.w*4,image.pixels.data()+((size_t(y+row)*image.w)+x)*4,size_t(out.w)*4);
 return out;
}
bool CopyToClipboard(HWND owner,const Image& image,const std::wstring& path){
 if(!image.w||!image.h)return false;
 // A device independent bitmap for editors, plus the original file so that a
 // paste into Explorer or a mail client keeps the untouched original.
 auto flat=RotatedPixels(image,0,true);
 size_t rowBytes=size_t(flat.w)*4,pixelBytes=rowBytes*flat.h;
 HGLOBAL dib=GlobalAlloc(GMEM_MOVEABLE,sizeof(BITMAPV5HEADER)+pixelBytes);
 if(!dib)return false;
 if(auto raw=GlobalLock(dib)){
  auto header=(BITMAPV5HEADER*)raw;
  ZeroMemory(header,sizeof(BITMAPV5HEADER));
  header->bV5Size=sizeof(BITMAPV5HEADER);header->bV5Width=LONG(flat.w);header->bV5Height=-LONG(flat.h);
  header->bV5Planes=1;header->bV5BitCount=32;header->bV5Compression=BI_BITFIELDS;
  header->bV5RedMask=0x00FF0000;header->bV5GreenMask=0x0000FF00;header->bV5BlueMask=0x000000FF;header->bV5AlphaMask=0xFF000000;
  header->bV5CSType=LCS_sRGB;header->bV5SizeImage=DWORD(pixelBytes);
  memcpy((uint8_t*)raw+sizeof(BITMAPV5HEADER),flat.pixels.data(),pixelBytes);
  GlobalUnlock(dib);
 }else{GlobalFree(dib);return false;}
 HGLOBAL drop=nullptr;
 if(!path.empty()){
  size_t chars=path.size()+2;
  drop=GlobalAlloc(GMEM_MOVEABLE,sizeof(DROPFILES)+chars*sizeof(wchar_t));
  if(drop){
   if(auto raw=GlobalLock(drop)){
    auto files=(DROPFILES*)raw;ZeroMemory(files,sizeof(DROPFILES));
    files->pFiles=sizeof(DROPFILES);files->fWide=TRUE;
    auto text=(wchar_t*)((uint8_t*)raw+sizeof(DROPFILES));
    memcpy(text,path.c_str(),path.size()*sizeof(wchar_t));
    text[path.size()]=0;text[path.size()+1]=0;
    GlobalUnlock(drop);
   }else{GlobalFree(drop);drop=nullptr;}
  }
 }
 if(!OpenClipboard(owner)){GlobalFree(dib);if(drop)GlobalFree(drop);return false;}
 EmptyClipboard();
 bool ok=SetClipboardData(CF_DIBV5,dib)!=nullptr;
 if(!ok)GlobalFree(dib);
 if(drop&&!SetClipboardData(CF_HDROP,drop))GlobalFree(drop);
 CloseClipboard();
 return ok;
}
