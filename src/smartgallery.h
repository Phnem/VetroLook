#pragma once
#include "index.h"
#include "image.h"
#include <array>
#include <memory>
enum class SmartClass:uint32_t{Photo,Screenshot,Document,Icon,Artwork,Other};
enum class SmartVisibility:uint32_t{Visible,Hidden,Uncertain};
enum SmartReason:uint32_t{SmartCamera=1,SmartSmallAlpha=2,SmartTechnicalPath=4,SmartNamedScreenshot=8,SmartModel=16,SmartNoThumbnail=32};
struct SmartRecord{
 uint64_t id=0,size=0,modified=0,classifiedAt=0;
 uint32_t algorithm=0,modelVersion=0,reasons=0;
 SmartClass category=SmartClass::Photo;SmartVisibility visibility=SmartVisibility::Uncertain;
 float confidence=0,score=0;std::array<float,6> probabilities{};
 // -1 is automatic, 0 is Hide, 1 is Show; survives file/model changes.
 int32_t overrideValue=-1;
 std::array<uint8_t,32> identity{}; // model + policy + algorithm fingerprint
};
struct SmartPolicy{
 bool enabled=false;std::array<double,6> thresholds{1.01,1.01,1.01,1.01,1.01,1.01};
 double photoCeiling=0,minimumMargin=1;std::wstring modelHash,policyHash;
};
struct SmartSettings{bool enabled=true;uint32_t categories=1;bool uncertainOnly=false;bool applyAutomatic=true;};
struct SmartStatus{
 bool modelReady=false,paused=false;uint64_t completed=0,pending=0;double initMs=0,inferenceMs=0;std::wstring error;
 bool inventoryKnown=false,discoveryComplete=false,autoApply=false,reviewReady=false;
 uint64_t reviewTotal=0,reviewCompleted=0,reviewUnreadable=0;
 double secondsPerFile=.5;
};
struct SmartTestOptions{std::wstring cacheFolder,modelPath,policyPath;};
void SmartStart(HWND notify,UINT message,const SmartTestOptions* testOptions=nullptr);
void SmartStop();
void SmartRequest(const std::vector<PhotoEntry>& photos);
// A complete, unfiltered index snapshot; collection happens off the UI thread.
void SmartSyncLibrary(const std::vector<PhotoEntry>& photos,bool discoveryComplete);
bool SmartApplyReviewed();
void SmartPause(bool paused);
SmartRecord SmartLookup(const PhotoEntry& photo);
bool SmartPasses(const PhotoEntry& photo,const SmartSettings& settings);
SmartRecord SmartDecide(const PhotoEntry& photo,const Image* thumbnail,const std::array<float,6>* probabilities,bool camera,uint32_t modelVersion,unsigned sourceWidth=0,unsigned sourceHeight=0,const SmartPolicy* policy=nullptr);
bool SmartReadPolicy(const std::wstring& modelPath,const std::wstring& policyPath,SmartPolicy& policy,std::wstring& error);
void SmartSetOverride(const PhotoEntry& photo,int value);
SmartSettings SmartPreferences();
void SmartSavePreferences(const SmartSettings& settings);
SmartStatus SmartStatusNow();
// Developer/QA entry point. An original ImageNet checkpoint is never loaded here.
bool SmartProbeModel(const std::wstring& path,const std::shared_ptr<Image>& thumbnail,std::array<float,6>& probabilities,std::wstring& error,bool gpu=false);
bool SmartBenchmarkModel(const std::wstring& path,const std::shared_ptr<Image>& thumbnail,std::wstring& report,bool gpu,unsigned iterations);
