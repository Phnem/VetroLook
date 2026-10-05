// Local TinyNeXt inference and conservative gallery decisions. No network activity.
#include "smartgallery.h"
#include "pipeline.h"
#include "metaread.h"
#include "media.h"
#include "ui.h"
#include "metacache.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <stdexcept>
#include <winrt/Windows.AI.MachineLearning.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <windows.ai.machinelearning.native.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <cmath>
namespace fs=std::filesystem;
using namespace winrt::Windows::AI::MachineLearning;
namespace{
constexpr uint32_t Algorithm=2,CacheMagic=0x53474c32,LegacyMagic=0x53474c31;
struct LegacyRecord{uint64_t id=0,size=0,modified=0,classifiedAt=0;uint32_t algorithm=0,modelVersion=0,reasons=0;SmartClass category=SmartClass::Photo;SmartVisibility visibility=SmartVisibility::Uncertain;float confidence=0,score=0;std::array<float,6> probabilities{};int32_t overrideValue=-1;};
std::mutex saveMx,mx;std::condition_variable cv;std::thread worker;
bool stopping=false,paused=false,dirty=false;HWND window=nullptr;UINT message=0;
std::unordered_map<uint64_t,SmartRecord> records;
std::deque<PhotoEntry> queue;std::unordered_set<uint64_t> queued;
std::deque<std::pair<PhotoEntry,int>> corrections;
SmartStatus status;
uint32_t activeModelVersion=0;
std::array<uint8_t,32> activeIdentity{};
std::wstring testCacheFolder,testModelPath,testPolicyPath;
struct ReviewStamp{uint64_t size,modified;std::wstring path;};
std::unordered_map<uint64_t,ReviewStamp> reviewItems;
std::unordered_set<uint64_t> reviewPending;
bool Current(const SmartRecord& r,uint64_t size,uint64_t modified){return r.algorithm==Algorithm&&r.size==size&&r.modified==modified&&r.identity==activeIdentity;}
void ReviewReady(){status.reviewReady=status.modelReady&&status.inventoryKnown&&status.discoveryComplete&&status.reviewTotal>0&&reviewPending.empty();}
void RestoreApplication();
std::wstring Lower(std::wstring s){for(auto& c:s)c=c==L'/'?L'\\':towlower(c);return s;}
fs::path CachePath(){if(!testCacheFolder.empty())return fs::path(testCacheFolder)/L"smart-gallery.bin";PWSTR raw=nullptr;fs::path path;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw))){path=fs::path(raw)/L"VetroLook"/L"smart-gallery.bin";CoTaskMemFree(raw);}return path;}
fs::path ExecutableFolder(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return fs::path(path).parent_path();}
bool Technical(const std::wstring& path){auto p=Lower(path);for(auto key:{L"\\icons\\",L"\\assets\\",L"\\resources\\",L"\\appdata\\",L"\\cache\\",L"\\temp\\",L"\\.gemini\\",L"\\.cursor\\",L"\\node_modules\\",L"\\sysfiles\\",L"\\analogefex\\",L"\\render\\",L"\\renders\\",L"\\final_frames\\",L"\\playblast_frames\\"})if(p.find(key)!=std::wstring::npos)return true;return false;}
bool ScreenshotPath(const std::wstring& path){auto p=Lower(path);for(auto key:{L"\\screenshots\\",L"screenshot",L"снимок экрана",L"screen_shot",L"screen shot"})if(p.find(key)!=std::wstring::npos)return true;return false;}
bool DocumentPath(const std::wstring& path){auto p=Lower(path);for(auto key:{L"\\scans\\",L"\\documents\\",L"\\receipts\\"})if(p.find(key)!=std::wstring::npos)return true;return false;}
std::vector<uint8_t> ReadBytes(const fs::path& path,size_t limit){std::ifstream in(path,std::ios::binary|std::ios::ate);auto size=in.tellg();if(!in||size<0||uint64_t(size)>limit)throw std::runtime_error("invalid model/policy file size");std::vector<uint8_t> bytes(size_t(size),uint8_t(0));in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),bytes.size());if(!in)throw std::runtime_error("model/policy read failed");return bytes;}
std::array<uint8_t,32> Hash(const std::vector<uint8_t>& bytes){
 struct Handles{BCRYPT_ALG_HANDLE a=nullptr;BCRYPT_HASH_HANDLE h=nullptr;~Handles(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}} handles;
 std::array<uint8_t,32> result{};
 if(BCryptOpenAlgorithmProvider(&handles.a,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||BCryptCreateHash(handles.a,&handles.h,nullptr,0,nullptr,0,0)<0||BCryptHashData(handles.h,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)<0||BCryptFinishHash(handles.h,result.data(),ULONG(result.size()),0)<0)throw std::runtime_error("SHA256 failure");return result;
}
std::wstring Hex(const std::array<uint8_t,32>& bytes){const wchar_t* digits=L"0123456789abcdef";std::wstring text;for(auto byte:bytes){text+=digits[byte>>4];text+=digits[byte&15];}return text;}
winrt::Windows::Data::Json::JsonObject ReadJson(const fs::path& path){auto bytes=ReadBytes(path,65536);return winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(std::string(bytes.begin(),bytes.end())));}
void RestoreApplication(){
 try{auto value=ReadJson(CachePath().parent_path()/L"gallery-application.json");status.autoApply=value.GetNamedBoolean(L"applied",false)&&value.GetNamedString(L"identity",L"")==Hex(activeIdentity);}catch(...){status.autoApply=false;}
}
SmartPolicy ReadPolicy(const fs::path& model,const fs::path& path){
 SmartPolicy p;auto value=ReadJson(path);const wchar_t* classes[]={L"photo",L"screenshot",L"document",L"icon_ui_asset",L"artwork",L"other"};
 auto names=value.GetNamedArray(L"classes"),thresholds=value.GetNamedArray(L"hide_thresholds");
 if(value.GetNamedNumber(L"schema")!=1||names.Size()!=6||thresholds.Size()!=6||!value.GetNamedBoolean(L"calibrated")||!value.GetNamedBoolean(L"uncertain_is_visible")||!value.GetNamedBoolean(L"user_override_wins"))throw std::runtime_error("invalid calibrated policy");
 for(unsigned i=0;i<6;i++){p.thresholds[i]=thresholds.GetNumberAt(i);if(names.GetStringAt(i)!=classes[i]||!std::isfinite(p.thresholds[i])||p.thresholds[i]<0||p.thresholds[i]>1.01)throw std::runtime_error("invalid policy classes/thresholds");}
 p.photoCeiling=value.GetNamedNumber(L"photo_ceiling");p.minimumMargin=value.GetNamedNumber(L"minimum_margin");
 if(!std::isfinite(p.photoCeiling)||!std::isfinite(p.minimumMargin)||p.photoCeiling<0||p.photoCeiling>1||p.minimumMargin<0||p.minimumMargin>1||p.thresholds[0]<=1)throw std::runtime_error("invalid photo protection policy");
 p.modelHash=Hex(Hash(ReadBytes(model,64*1024*1024)));p.policyHash=Hex(Hash(ReadBytes(path,65536)));
 if(value.GetNamedString(L"model_sha256")!=p.modelHash)throw std::runtime_error("policy/model SHA256 mismatch");p.enabled=true;return p;
}
void Load(){auto path=CachePath();std::ifstream in(path,std::ios::binary);uint32_t magic=0,count=0;in.read((char*)&magic,4);in.read((char*)&count,4);if(!in||(magic!=CacheMagic&&magic!=LegacyMagic)||count>2000000)return;std::unordered_map<uint64_t,SmartRecord> loaded;for(uint32_t i=0;i<count;i++){SmartRecord r{};if(magic==LegacyMagic){LegacyRecord old;in.read((char*)&old,sizeof(old));r.id=old.id;r.size=old.size;r.modified=old.modified;r.overrideValue=old.overrideValue;}else in.read((char*)&r,sizeof(r));if(!in||uint32_t(r.category)>5||uint32_t(r.visibility)>2||r.overrideValue<-1||r.overrideValue>1)return;loaded[r.id]=r;}std::lock_guard lock(mx);for(auto& [id,r]:loaded)if(!records.count(id))records[id]=r;}
void Save(){std::lock_guard saveLock(saveMx);std::vector<SmartRecord> snapshot;{std::lock_guard lock(mx);if(!dirty)return;snapshot.reserve(records.size());for(auto& [id,r]:records)snapshot.push_back(r);dirty=false;}
 auto path=CachePath();std::error_code ec;fs::create_directories(path.parent_path(),ec);auto tmp=path;tmp+=L".tmp";std::ofstream out(tmp,std::ios::binary|std::ios::trunc);uint32_t count=uint32_t(snapshot.size()),magic=CacheMagic;out.write((char*)&magic,4);out.write((char*)&count,4);for(auto& r:snapshot)out.write((char*)&r,sizeof(r));out.close();if(!out||!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){std::lock_guard lock(mx);dirty=true;}}
std::vector<float> Prepare(const Image& image){
 if(!image.w||!image.h||image.pixels.size()!=size_t(image.w)*image.h*4)throw std::runtime_error("invalid thumbnail");
 std::vector<float> rgb(3*224*224);const float means[]={.485f,.456f,.406f},deviations[]={.229f,.224f,.225f};
 // Separable antialiased bilinear RGB resize, rounded between passes like PIL.
 // Coefficient precision/rounding follows Pillow's documented 8-bit resampler.
 constexpr int Bits=22,One=1<<Bits;
 struct Weights{unsigned first=0;std::vector<int> values;};
 auto weights=[](unsigned size){std::vector<Weights> all(224);double scale=double(size)/224,filter=(std::max)(1.,scale);
  for(unsigned x=0;x<224;x++){double center=(x+.5)*scale;int first=(std::max)(0,int(center-filter+.5)),last=(std::min)(int(size),int(center+filter+.5));all[x].first=unsigned(first);std::vector<double> values;double sum=0;
   for(int i=first;i<last;i++){double weight=(std::max)(0.,1.-std::abs((i-center+.5)/filter));values.push_back(weight);sum+=weight;}
   for(double weight:values)all[x].values.push_back(int(.5+weight/sum*One));
  }return all;};
 auto wx=weights(image.w),wy=weights(image.h);std::vector<uint8_t> source(size_t(image.w)*image.h*3),horizontal(size_t(224)*image.h*3);
 const unsigned background[]={248,240,222};for(size_t i=0;i<size_t(image.w)*image.h;i++)for(unsigned c=0;c<3;c++)source[i*3+c]=uint8_t((std::min)(255u,unsigned(image.pixels[i*4+2-c])+(background[c]*(255-image.pixels[i*4+3])+127)/255));
 for(unsigned y=0;y<image.h;y++)for(unsigned x=0;x<224;x++)for(unsigned c=0;c<3;c++){int64_t sum=One/2;for(size_t i=0;i<wx[x].values.size();i++)sum+=int64_t(source[(size_t(y)*image.w+wx[x].first+i)*3+c])*wx[x].values[i];horizontal[(size_t(y)*224+x)*3+c]=uint8_t((std::min)(int64_t(255),sum>>Bits));}
 for(unsigned y=0;y<224;y++)for(unsigned x=0;x<224;x++)for(unsigned c=0;c<3;c++){int64_t sum=One/2;for(size_t i=0;i<wy[y].values.size();i++)sum+=int64_t(horizontal[(size_t(wy[y].first+i)*224+x)*3+c])*wy[y].values[i];float value=float((std::min)(int64_t(255),sum>>Bits))/255.f;rgb[size_t(c)*224*224+y*224+x]=(value-means[c])/deviations[c];}return rgb;
}
struct Model{
 LearningModel model{nullptr};LearningModelSession session{nullptr};std::wstring input,output;uint32_t version=0;SmartPolicy policy;std::array<uint8_t,32> identity{};
 void Open(const fs::path& path,bool gpu=false,bool requireValidated=false){
  auto policyPath=testPolicyPath.empty()?path.parent_path()/L"gallery-policy.json":fs::path(testPolicyPath);
  if(testPolicyPath.empty()&&!fs::exists(policyPath)&&fs::exists(path.parent_path()/L"calibration"/L"gallery-policy.json"))policyPath=path.parent_path()/L"calibration"/L"gallery-policy.json";
  if(fs::exists(policyPath))policy=ReadPolicy(path,policyPath);
  if(requireValidated){
   if(!policy.enabled)throw std::runtime_error("missing calibrated policy");
   auto gate=ReadJson(path.parent_path()/L"gallery-validated.txt");
   if(gate.GetNamedString(L"model_sha256")!=policy.modelHash||gate.GetNamedString(L"policy_sha256")!=policy.policyHash)throw std::runtime_error("unvalidated model/policy");
   bool approved=gate.GetNamedBoolean(L"quality_gate_passed");
   if(!approved){
    // Explicit local risk acceptance is separate from the unchanged scientific result.
    auto authorization=ReadJson(path.parent_path()/L"gallery-user-authorization.json");
    auto evaluation=gate.GetNamedString(L"evaluation_sha256");
    approved=authorization.GetNamedNumber(L"schema")==1&&authorization.GetNamedBoolean(L"deployment_authorized")&&
     authorization.GetNamedString(L"authorization_kind")==L"explicit_user_risk_acceptance"&&
     authorization.GetNamedString(L"model_sha256")==policy.modelHash&&authorization.GetNamedString(L"policy_sha256")==policy.policyHash&&
     evaluation.size()==64&&authorization.GetNamedString(L"evaluation_sha256")==evaluation;
   }
   if(!approved)throw std::runtime_error("unvalidated model/policy");
  }
  std::wstring modelHash=policy.enabled?policy.modelHash:Hex(Hash(ReadBytes(path,64*1024*1024)));auto signature=winrt::to_string(modelHash+L"\n"+policy.policyHash+L"\nalgorithm:2");identity=Hash(std::vector<uint8_t>(signature.begin(),signature.end()));for(unsigned i=0;i<4;i++)version|=uint32_t(identity[i])<<(8*i);
  model=LearningModel::LoadFromFilePath(path.wstring());
  if(model.InputFeatures().Size()!=1||model.OutputFeatures().Size()!=1)throw std::runtime_error("model features");
  auto inDescriptor=model.InputFeatures().GetAt(0).as<TensorFeatureDescriptor>(),outDescriptor=model.OutputFeatures().GetAt(0).as<TensorFeatureDescriptor>();auto inShape=inDescriptor.Shape(),shape=outDescriptor.Shape();
  if(inDescriptor.TensorKind()!=TensorKind::Float||outDescriptor.TensorKind()!=TensorKind::Float||inShape.Size()!=4||inShape.GetAt(0)!=1||inShape.GetAt(1)!=3||inShape.GetAt(2)!=224||inShape.GetAt(3)!=224||shape.Size()!=2||shape.GetAt(0)!=1||shape.GetAt(1)!=6)throw std::runtime_error("expected RGB NCHW six-class FP32 model");
  input=model.InputFeatures().GetAt(0).Name().c_str();output=model.OutputFeatures().GetAt(0).Name().c_str();
  LearningModelSessionOptions options;options.BatchSizeOverride(1);
  if(!gpu){auto native=options.as<ILearningModelSessionOptionsNative>();winrt::check_hresult(native->SetIntraOpNumThreadsOverride(1));}
  session=LearningModelSession(model,LearningModelDevice(gpu?LearningModelDeviceKind::DirectXHighPerformance:LearningModelDeviceKind::Cpu),options);
 }
 std::array<float,6> Run(const Image& image){auto values=Prepare(image);std::vector<int64_t> dims{1,3,224,224};auto tensor=TensorFloat::CreateFromArray(dims,values);LearningModelBinding binding(session);binding.Bind(input,tensor);auto result=session.Evaluate(binding,L"gallery");auto logits=result.Outputs().Lookup(output).as<TensorFloat>().GetAsVectorView();if(logits.Size()!=6)throw std::runtime_error("expected six scores");std::array<float,6> p{};float maximum=logits.GetAt(0),sum=0;for(int i=0;i<6;i++){if(!std::isfinite(logits.GetAt(i)))throw std::runtime_error("nonfinite model output");maximum=(std::max)(maximum,logits.GetAt(i));}for(int i=0;i<6;i++){p[i]=expf(logits.GetAt(i)-maximum);sum+=p[i];}for(auto& value:p)value/=sum;return p;}
};
}
bool SmartReadPolicy(const std::wstring& model,const std::wstring& path,SmartPolicy& policy,std::wstring& error){try{policy=ReadPolicy(model,path);return true;}catch(const winrt::hresult_error& e){error=e.message().c_str();}catch(const std::exception& e){error=winrt::to_hstring(e.what()).c_str();}policy={};return false;}
SmartRecord SmartDecide(const PhotoEntry& photo,const Image* thumb,const std::array<float,6>* probabilities,bool camera,uint32_t modelVersion,unsigned sourceWidth,unsigned sourceHeight,const SmartPolicy* policy){
 SmartRecord r;r.id=photo.id;r.size=photo.size;r.modified=photo.modified;r.algorithm=Algorithm;r.modelVersion=modelVersion;r.classifiedAt=GetTickCount64();
 if(camera||IsRawPath(photo.path)||KindFromExtension(photo.path)==MediaKind::Video){r.category=SmartClass::Photo;r.visibility=SmartVisibility::Visible;r.confidence=1;r.score=1;r.reasons|=SmartCamera;return r;}
 bool technical=Technical(photo.path),screenshot=ScreenshotPath(photo.path);if(technical)r.reasons|=SmartTechnicalPath;
 if(screenshot)r.reasons|=SmartNamedScreenshot;
 // These two metadata decisions are independent of a statistical model. A path alone is insufficient.
 if(!sourceWidth&&thumb)sourceWidth=thumb->sourceW;
 if(!sourceHeight&&thumb)sourceHeight=thumb->sourceH;
 if((!policy||!policy->enabled)&&thumb&&technical&&thumb->hasAlpha&&sourceWidth&&sourceHeight&&sourceWidth<=512&&sourceHeight<=512){r.category=SmartClass::Icon;r.visibility=SmartVisibility::Hidden;r.confidence=.999f;r.reasons|=SmartSmallAlpha;return r;}
 if(!thumb){r.reasons|=SmartNoThumbnail;return r;}
 if(probabilities){
  double sum=0;for(float value:*probabilities){if(!std::isfinite(value)||value<0)return r;sum+=value;}if(std::abs(sum-1)>1e-3)return r;
  r.probabilities=*probabilities;r.reasons|=SmartModel;auto best=std::max_element(probabilities->begin(),probabilities->end());r.category=SmartClass(best-probabilities->begin());r.confidence=*best;r.score=(*probabilities)[0];
  if(r.category==SmartClass::Photo&&r.score>=.5f){r.visibility=SmartVisibility::Visible;return r;}
  bool document=DocumentPath(photo.path),neutral=!(technical||screenshot||document);
  bool supported=(r.category==SmartClass::Screenshot&&(screenshot||neutral))||(r.category==SmartClass::Document&&(document||neutral))||(uint32_t(r.category)>=3&&(technical||neutral));
  float second=0;for(unsigned i=0;i<6;i++)if(i!=uint32_t(r.category))second=(std::max)(second,(*probabilities)[i]);
  if(policy&&policy->enabled&&supported&&r.confidence>=policy->thresholds[uint32_t(r.category)]&&r.score<=policy->photoCeiling&&double(r.confidence)-second>=policy->minimumMargin)r.visibility=SmartVisibility::Hidden;
 }
 return r;
}
void SmartStart(HWND notify,UINT msg,const SmartTestOptions* testOptions){
 if(worker.joinable())return;
 {std::lock_guard lock(mx);records.clear();queue.clear();queued.clear();corrections.clear();reviewItems.clear();reviewPending.clear();status={};activeModelVersion=0;activeIdentity={};dirty=false;paused=false;testCacheFolder=testOptions?testOptions->cacheFolder:L"";testModelPath=testOptions?testOptions->modelPath:L"";testPolicyPath=testOptions?testOptions->policyPath:L"";}
 window=notify;message=msg;stopping=false;worker=std::thread([]{
  winrt::init_apartment(winrt::apartment_type::multi_threaded);SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN);Load();
  Model model;auto path=testModelPath.empty()?ExecutableFolder()/L"models"/L"gallery-tinynext.onnx":fs::path(testModelPath);
  // A scientific gate or explicit local user acceptance accompanies a deployable model.
  auto began=std::chrono::steady_clock::now();try{if(fs::exists(path)&&(!testModelPath.empty()||fs::exists(path.parent_path()/L"gallery-validated.txt"))){model.Open(path,false,testModelPath.empty());std::lock_guard lock(mx);status.modelReady=true;activeModelVersion=model.version;activeIdentity=model.identity;RestoreApplication();}else{std::lock_guard lock(mx);status.error=L"No validated six-class model";}}catch(const winrt::hresult_error& e){std::lock_guard lock(mx);status.error=e.message().c_str();}catch(...){std::lock_guard lock(mx);status.error=L"Invalid gallery model or calibrated policy";}
  {std::lock_guard lock(mx);status.initMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();}PostMessageW(window,message,0,0);
  uint64_t saved=0,lastNotice=0;
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
  for(;;){PhotoEntry photo;bool saveOnly=false;std::deque<std::pair<PhotoEntry,int>> edits;{std::unique_lock lock(mx);cv.wait(lock,[]{return stopping||dirty||!corrections.empty()||(!paused&&!queue.empty());});if(stopping)break;edits.swap(corrections);if(paused||queue.empty())saveOnly=true;else{photo=std::move(queue.front());queue.pop_front();queued.erase(photo.id);status.pending=queue.size();}}
   if(!edits.empty()){auto file=CachePath().parent_path()/L"gallery-corrections.tsv";std::ofstream out(file,std::ios::app);for(auto& [p,value]:edits)out<<p.id<<'\t'<<value<<'\t'<<winrt::to_string(p.path)<<'\n';}
   if(saveOnly){Save();continue;}
   auto cached=SmartLookup(photo);if(cached.algorithm==Algorithm&&cached.size==photo.size&&cached.modified==photo.modified&&cached.identity==model.identity&&(!(cached.reasons&SmartNoThumbnail)||!ThumbLookup(photo.path)))continue;
   auto itemBegan=std::chrono::steady_clock::now();
   // Header/cached metadata only. Never read a whole RAW/image merely to classify it.
   auto meta=MetaCacheGet(photo.path);bool camera=meta.valid&&!meta.camera.empty();unsigned width=meta.width,height=meta.height;
   if(factory&&!IsRawPath(photo.path)&&KindFromExtension(photo.path)==MediaKind::Image){
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if(SUCCEEDED(factory->CreateDecoderFromFilename(photo.path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))&&SUCCEEDED(decoder->GetFrame(0,&frame))){
     frame->GetSize(&width,&height);Microsoft::WRL::ComPtr<IWICMetadataQueryReader> reader;
     if(SUCCEEDED(frame->GetMetadataQueryReader(&reader))){PROPVARIANT make{},name{};PropVariantInit(&make);PropVariantInit(&name);camera=camera||(SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=271}",&make))&&SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=272}",&name)));PropVariantClear(&make);PropVariantClear(&name);}
    }
   }
   std::shared_ptr<Image> thumb;
   bool needPixels=bool(model.session)||(Technical(photo.path)&&width&&height&&width<=512&&height<=512);
   if(needPixels&&!camera&&!IsRawPath(photo.path)&&KindFromExtension(photo.path)!=MediaKind::Video){
    thumb=ThumbLookup(photo.path);if(!thumb){ThumbRequest(photo.path);for(int retry=0;retry<20&&!thumb;retry++){std::unique_lock lock(mx);if(cv.wait_for(lock,std::chrono::milliseconds(25),[]{return stopping||paused;}))break;lock.unlock();thumb=ThumbLookup(photo.path);}}
   }
   if(needPixels&&!thumb){std::lock_guard lock(mx);if(stopping)break;if(paused){if(queued.insert(photo.id).second)queue.push_front(photo);status.pending=queue.size();continue;}}
   std::array<float,6> probabilities{};bool evaluated=false;
   if(thumb&&model.session){try{auto start=std::chrono::steady_clock::now();probabilities=model.Run(*thumb);evaluated=true;std::lock_guard lock(mx);status.inferenceMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}catch(...){}}
   auto decision=SmartDecide(photo,thumb.get(),evaluated?&probabilities:nullptr,camera,model.version,width,height,&model.policy);decision.identity=model.identity;
   if(!needPixels)decision.reasons&=~SmartNoThumbnail;
   {std::lock_guard lock(mx);auto it=records.find(photo.id);if(it!=records.end())decision.overrideValue=it->second.overrideValue;records[photo.id]=decision;dirty=true;status.completed++;
    auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-itemBegan).count();
    status.secondsPerFile=.95*status.secondsPerFile+.05*(std::max)(.01,elapsed);
    auto member=reviewItems.find(photo.id);if(member!=reviewItems.end()&&member->second.size==photo.size&&member->second.modified==photo.modified&&reviewPending.erase(photo.id)){
     status.reviewCompleted++;if(decision.reasons&SmartNoThumbnail)status.reviewUnreadable++;
    }else if(member!=reviewItems.end()&&(member->second.size!=photo.size||member->second.modified!=photo.modified)){
     // An edit/rename during the first pass must not strand the progress bar.
     // Finish the current attempt, then queue the inventory's latest file stamp.
     auto updated=photo;updated.size=member->second.size;updated.modified=member->second.modified;updated.path=member->second.path;
     if(queued.insert(updated.id).second)queue.push_front(std::move(updated));status.pending=queue.size();
    }ReviewReady();}
   if(++saved%64==0)Save();if(GetTickCount64()-lastNotice>600){lastNotice=GetTickCount64();PostMessageW(window,message,0,0);}
   {std::unique_lock lock(mx);cv.wait_for(lock,std::chrono::milliseconds(6),[]{return stopping||paused;});}
  }Save();winrt::uninit_apartment();
 });
}
void SmartStop(){{std::lock_guard lock(mx);stopping=true;}cv.notify_all();if(worker.joinable())worker.join();Save();}
void SmartRequest(const std::vector<PhotoEntry>& photos){std::lock_guard lock(mx);for(auto& p:photos){auto it=records.find(p.id);if(it!=records.end()&&it->second.algorithm==Algorithm&&it->second.size==p.size&&it->second.modified==p.modified&&it->second.identity==activeIdentity&&(!(it->second.reasons&SmartNoThumbnail)||!ThumbLookup(p.path)))continue;if(queued.insert(p.id).second)queue.push_back(p);}status.pending=queue.size();cv.notify_one();}
void SmartSyncLibrary(const std::vector<PhotoEntry>& photos,bool discoveryComplete){
 {
  std::lock_guard lock(mx);if(stopping)return;
  reviewItems.clear();reviewPending.clear();status.reviewUnreadable=0;
  for(auto& photo:photos){
   reviewItems[photo.id]={photo.size,photo.modified,photo.path};auto cached=records.find(photo.id);
   if(cached==records.end()||!Current(cached->second,photo.size,photo.modified))reviewPending.insert(photo.id);
   else if(cached->second.reasons&SmartNoThumbnail)status.reviewUnreadable++;
  }
  status.inventoryKnown=true;status.discoveryComplete=discoveryComplete;
  status.reviewTotal=reviewItems.size();status.reviewCompleted=status.reviewTotal-reviewPending.size();ReviewReady();
 }
 SmartRequest(photos);if(window)PostMessageW(window,message,0,0);
}
bool SmartApplyReviewed(){
 Save();std::lock_guard lock(mx);if(!status.reviewReady)return false;
 try{
  winrt::Windows::Data::Json::JsonObject value;
  value.SetNamedValue(L"schema",winrt::Windows::Data::Json::JsonValue::CreateNumberValue(1));
  value.SetNamedValue(L"applied",winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
  value.SetNamedValue(L"identity",winrt::Windows::Data::Json::JsonValue::CreateStringValue(Hex(activeIdentity)));
  auto file=CachePath().parent_path()/L"gallery-application.json",tmp=file;tmp+=L".tmp";
  std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out<<winrt::to_string(value.Stringify());out.close();
  if(!out||!MoveFileExW(tmp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("application state write failed");
  status.autoApply=true;return true;
 }catch(...){status.error=L"Could not save gallery application choice";return false;}
}
void SmartPause(bool value){{std::lock_guard lock(mx);paused=value;status.paused=value;}cv.notify_all();}
SmartRecord SmartLookup(const PhotoEntry& photo){std::lock_guard lock(mx);auto it=records.find(photo.id);if(it==records.end())return {};auto r=it->second;if(r.size!=photo.size||r.modified!=photo.modified||r.algorithm!=Algorithm||r.identity!=activeIdentity){r.visibility=SmartVisibility::Uncertain;r.algorithm=0;}return r;}
bool SmartPasses(const PhotoEntry& photo,const SmartSettings& settings){if(!settings.enabled)return true;auto record=SmartLookup(photo);if(record.overrideValue>=0)return record.overrideValue==1;if(!settings.applyAutomatic)return true;if(settings.uncertainOnly)return record.visibility==SmartVisibility::Uncertain;if(record.visibility==SmartVisibility::Uncertain)return true;return (settings.categories&(1u<<uint32_t(record.category)))!=0;}
void SmartSetOverride(const PhotoEntry& photo,int value){if(value<-1||value>1)return;{std::lock_guard lock(mx);auto& r=records[photo.id];r.id=photo.id;r.overrideValue=value;dirty=true;corrections.push_back({photo,value});}cv.notify_one();
 if(window)PostMessageW(window,message,0,0);
}
SmartSettings SmartPreferences(){SmartSettings s;DWORD value=1,bytes=4;RegGetValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"SmartGallery",RRF_RT_REG_DWORD,nullptr,&value,&bytes);s.enabled=value!=0;value=1;bytes=4;RegGetValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"GalleryCategories",RRF_RT_REG_DWORD,nullptr,&value,&bytes);s.categories=value&63;return s;}
void SmartSavePreferences(const SmartSettings& s){DWORD enabled=s.enabled?1:0,categories=s.categories;RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"SmartGallery",REG_DWORD,&enabled,4);RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\VetroLook\\Settings",L"GalleryCategories",REG_DWORD,&categories,4);}
SmartStatus SmartStatusNow(){std::lock_guard lock(mx);return status;}
bool SmartProbeModel(const std::wstring& path,const std::shared_ptr<Image>& image,std::array<float,6>& probabilities,std::wstring& error,bool gpu){try{if(!image)return false;Model model;model.Open(path,gpu);probabilities=model.Run(*image);return true;}catch(const winrt::hresult_error& e){error=e.message().c_str();return false;}catch(...){error=L"Invalid model";return false;}}
bool SmartBenchmarkModel(const std::wstring& path,const std::shared_ptr<Image>& image,std::wstring& report,bool gpu,unsigned iterations){
 try{if(!image)throw std::runtime_error("thumbnail missing");auto began=std::chrono::steady_clock::now();Model model;model.Open(path,gpu);double init=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
  auto p=model.Run(*image);std::vector<double> samples;for(unsigned i=0;i<iterations;i++){auto start=std::chrono::steady_clock::now();model.Run(*image);samples.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
  double sum=0;for(auto ms:samples)sum+=ms;std::sort(samples.begin(),samples.end());report=L"device="+std::wstring(gpu?L"DirectX":L"CPU")+L" init_ms="+std::to_wstring(init)+L" count="+std::to_wstring(iterations)+L" total_ms="+std::to_wstring(sum)+L" mean_ms="+std::to_wstring(sum/(std::max)(1u,iterations))+L" p95_ms="+std::to_wstring(samples[size_t((samples.size()-1)*.95)])+L" probabilities=";
  for(float value:p)report+=std::to_wstring(value)+L",";return true;
 }catch(const winrt::hresult_error& e){report=e.message().c_str();return false;}catch(const std::exception& e){report=winrt::to_hstring(e.what()).c_str();return false;}
}
