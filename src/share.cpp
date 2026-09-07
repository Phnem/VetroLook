// Vetro Look, GPL-3.0-or-later. Native Windows share sheet, no automatic sending.
#include "actions.h"
#include <shlobj.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <filesystem>
using namespace winrt;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Storage;
static DataTransferManager manager{nullptr};
static event_token shareToken{};
static fire_and_forget SupplyFile(DataRequestedEventArgs args,std::wstring path){
 auto request=args.Request();auto deferral=request.GetDeferral();
 try{auto file=co_await StorageFile::GetFileFromPathAsync(path);auto items=single_threaded_vector<IStorageItem>();items.Append(file);request.Data().Properties().Title(file.Name());request.Data().SetStorageItems(items);}catch(...){request.FailWithDisplayText(L"Не удалось открыть файл для отправки.");}
 deferral.Complete();
}
void ShutdownSharing(){if(manager){manager.DataRequested(shareToken);manager=nullptr;}}
void ShareImage(HWND owner,const std::wstring& path){
 try{ShutdownSharing();auto interop=get_activation_factory<DataTransferManager,IDataTransferManagerInterop>();check_hresult(interop->GetForWindow(owner,guid_of<DataTransferManager>(),put_abi(manager)));shareToken=manager.DataRequested([path](auto const&,DataRequestedEventArgs const& args){SupplyFile(args,path);});check_hresult(interop->ShowShareUIForWindow(owner));}
 catch(...){MessageBoxW(owner,L"Системное окно отправки недоступно.",L"Vetro Look",MB_OK|MB_ICONERROR);}
}

