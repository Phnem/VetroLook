// Vetro Look, GPL-3.0-or-later. See capability.h for the split this file keeps.
#include "capability.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <psapi.h>
#include <powerbase.h>
#include <algorithm>
#include <cmath>
using Microsoft::WRL::ComPtr;

namespace{
Capabilities machine;

// Documented for CallNtPowerInformation, but declared only in the kernel-mode
// headers, so it is spelled out here exactly as the documentation gives it.
struct ProcessorPower{
 ULONG Number,MaxMhz,CurrentMhz,MhzLimit,MaxIdleState,CurrentIdleState;
};

double Clamp01(double v){return v<0?0:(v>1?1:v);}

std::wstring VendorName(unsigned id){
 switch(id){
  case 0x10DE:return L"NVIDIA";
  case 0x1002:case 0x1022:return L"AMD";
  case 0x8086:return L"Intel";
  case 0x5143:return L"Qualcomm";
  case 0x1414:return L"Microsoft";
  default:return {};
 }
}

void ProbeAdapter(ID3D11Device* device){
 if(!device)return;
 ComPtr<IDXGIDevice> dxgi;
 if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))))return;
 ComPtr<IDXGIAdapter> adapter;
 if(FAILED(dxgi->GetAdapter(&adapter)))return;
 ComPtr<IDXGIAdapter1> adapter1;
 DXGI_ADAPTER_DESC1 desc{};
 if(SUCCEEDED(adapter.As(&adapter1))&&SUCCEEDED(adapter1->GetDesc1(&desc))){
  machine.adapter.description=desc.Description;
  machine.adapter.vendorId=desc.VendorId;machine.adapter.deviceId=desc.DeviceId;
  machine.adapter.vendor=VendorName(desc.VendorId);
  machine.adapter.dedicatedVideoMemory=desc.DedicatedVideoMemory;
  machine.adapter.sharedSystemMemory=desc.SharedSystemMemory;
  // Half a gigabyte of its own is the line between a part with memory and a
  // part borrowing the system's. It decides what a VRAM budget even means.
  machine.adapter.integrated=desc.DedicatedVideoMemory<(512ull<<20);
 }
}

// The decode profiles the adapter publishes. Asked of Direct3D, not of the
// playback engine: this answer has to exist before any engine is created.
void ProbeDecode(ID3D11Device* device){
 if(!device)return;
 ComPtr<ID3D11VideoDevice> video;
 if(FAILED(device->QueryInterface(IID_PPV_ARGS(&video))))return;
 UINT count=video->GetVideoDecoderProfileCount();
 for(UINT i=0;i<count;i++){
  GUID profile{};
  if(FAILED(video->GetVideoDecoderProfile(i,&profile)))continue;
  auto& d=machine.decode;
  if(profile==D3D11_DECODER_PROFILE_H264_VLD_NOFGT)d.h264=true;
  else if(profile==D3D11_DECODER_PROFILE_HEVC_VLD_MAIN)d.hevc8=true;
  else if(profile==D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10)d.hevc10=true;
  else if(profile==D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0)d.vp9=true;
  else if(profile==D3D11_DECODER_PROFILE_MPEG2_VLD)d.mpeg2=true;
  else if(profile==D3D11_DECODER_PROFILE_VC1_VLD)d.vc1=true;
  // AV1 in two rungs: eight bit and ten. The second one is what matters, because
  // almost every AV1 file worth playing is ten bit.
  else if(profile==D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0)d.av1=true;
 }
}

// Exact refresh, HDR state and paper white come from the display configuration
// rather than from the adapter: EnumDisplaySettings rounds 59.94 to 59, and the
// difference between those two numbers is the whole of frame pacing.
bool ConfigFacts(const std::wstring& gdiDevice,DisplayFacts& out){
 UINT32 paths=0,modes=0;
 if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes)!=ERROR_SUCCESS)return false;
 std::vector<DISPLAYCONFIG_PATH_INFO> pathList(paths);
 std::vector<DISPLAYCONFIG_MODE_INFO> modeList(modes);
 if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&paths,pathList.data(),&modes,modeList.data(),nullptr)!=ERROR_SUCCESS)
  return false;
 pathList.resize(paths);
 for(const auto& path:pathList){
  DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
  source.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
  source.header.size=sizeof source;
  source.header.adapterId=path.sourceInfo.adapterId;
  source.header.id=path.sourceInfo.id;
  if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS)continue;
  if(!gdiDevice.empty()&&gdiDevice!=source.viewGdiDeviceName)continue;
  const auto& rate=path.targetInfo.refreshRate;
  if(rate.Denominator)out.refreshHz=double(rate.Numerator)/double(rate.Denominator);
  // Windows reports the path's rate as zero for some virtual outputs; the mode
  // table still carries the signal's own timing.
  if(out.refreshHz<=0&&path.targetInfo.modeInfoIdx<modeList.size()){
   const auto& mode=modeList[path.targetInfo.modeInfoIdx];
   if(mode.infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET){
    const auto& signal=mode.targetMode.targetVideoSignalInfo;
    if(signal.vSyncFreq.Denominator)
     out.refreshHz=double(signal.vSyncFreq.Numerator)/double(signal.vSyncFreq.Denominator);
   }
  }
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colour{};
  colour.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  colour.header.size=sizeof colour;
  colour.header.adapterId=path.targetInfo.adapterId;
  colour.header.id=path.targetInfo.id;
  if(DisplayConfigGetDeviceInfo(&colour.header)==ERROR_SUCCESS){
   out.hdr=colour.advancedColorEnabled!=0;
   out.wideGamut=colour.wideColorEnforced!=0||colour.advancedColorEnabled!=0;
  }
  DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
  white.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
  white.header.size=sizeof white;
  white.header.adapterId=path.targetInfo.adapterId;
  white.header.id=path.targetInfo.id;
  if(DisplayConfigGetDeviceInfo(&white.header)==ERROR_SUCCESS&&white.SDRWhiteLevel)
   out.sdrWhiteNits=double(white.SDRWhiteLevel)*80.0/1000.0;
  return true;
 }
 return false;
}

// The panel's own luminance claim, which only the adapter knows.
void OutputFacts(HMONITOR monitor,DisplayFacts& out){
 ComPtr<IDXGIFactory1> factory;
 if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return;
 for(UINT a=0;;a++){
  ComPtr<IDXGIAdapter1> adapter;
  if(FAILED(factory->EnumAdapters1(a,&adapter)))break;
  for(UINT o=0;;o++){
   ComPtr<IDXGIOutput> output;
   if(FAILED(adapter->EnumOutputs(o,&output)))break;
   DXGI_OUTPUT_DESC desc{};
   if(FAILED(output->GetDesc(&desc))||desc.Monitor!=monitor)continue;
   out.name=desc.DeviceName;
   out.width=unsigned(desc.DesktopCoordinates.right-desc.DesktopCoordinates.left);
   out.height=unsigned(desc.DesktopCoordinates.bottom-desc.DesktopCoordinates.top);
   ComPtr<IDXGIOutput6> output6;
   DXGI_OUTPUT_DESC1 desc1{};
   if(SUCCEEDED(output.As(&output6))&&SUCCEEDED(output6->GetDesc1(&desc1))){
    out.maxNits=desc1.MaxLuminance;out.minNits=desc1.MinLuminance;
    if(desc1.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)out.hdr=true;
    // Rec.709's green primary sits at x=0.30. A panel appreciably outside it has
    // a wider gamut than sRGB -- but that is recorded, not acted on: unless
    // Windows is managing the colour, the display is still being sent sRGB
    // numbers, and grading for P3 on the strength of an EDID claim would
    // desaturate every film on a display that merely exaggerates.
    if(desc1.GreenPrimary[0]<0.28f||desc1.RedPrimary[0]>0.66f)out.panelWideGamut=true;
   }
   return;
  }
 }
}
}

DisplayFacts ProbeDisplay(HWND window){
 DisplayFacts facts;
 HMONITOR monitor=window?MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)
                        :MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
 MONITORINFOEXW info{};info.cbSize=sizeof info;
 std::wstring gdiDevice;
 if(monitor&&GetMonitorInfoW(monitor,&info)){
  gdiDevice=info.szDevice;
  facts.width=unsigned(info.rcMonitor.right-info.rcMonitor.left);
  facts.height=unsigned(info.rcMonitor.bottom-info.rcMonitor.top);
 }
 ConfigFacts(gdiDevice,facts);
 if(monitor)OutputFacts(monitor,facts);
 if(facts.refreshHz<=0){
  // Last resort, and rounded: better a whole number than nothing, because a
  // pacing plan with no refresh at all falls back to the audio clock.
  DEVMODEW mode{};mode.dmSize=sizeof mode;
  if(EnumDisplaySettingsW(gdiDevice.empty()?nullptr:gdiDevice.c_str(),ENUM_CURRENT_SETTINGS,&mode))
   facts.refreshHz=double(mode.dmDisplayFrequency);
 }
 if(facts.sdrWhiteNits<=0)facts.sdrWhiteNits=80;   // the sRGB reference
 if(facts.name.empty())facts.name=gdiDevice;
 return facts;
}

const Capabilities& CapabilityProbe(ID3D11Device* device,HWND window){
 if(machine.probed)return machine;
 ProbeAdapter(device);
 ProbeDecode(device);
 machine.display=ProbeDisplay(window);
 MEMORYSTATUSEX state{};state.dwLength=sizeof state;
 if(GlobalMemoryStatusEx(&state))machine.totalRam=state.ullTotalPhys;
 SYSTEM_INFO system{};GetNativeSystemInfo(&system);
 machine.cores=system.dwNumberOfProcessors;
 // The build number, read where Windows keeps it rather than from a version
 // API that lies to unmanifested processes.
 wchar_t text[32]{};DWORD size=sizeof text;DWORD build=0;
 if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
    L"CurrentBuildNumber",RRF_RT_REG_SZ,nullptr,text,&size)==ERROR_SUCCESS)
  build=wcstoul(text,nullptr,10);
 machine.os=L"Windows build "+std::to_wstring(build);
 machine.probed=true;
 return machine;
}
const Capabilities& CapabilitiesNow(){return machine;}
void CapabilityDisplayChanged(const DisplayFacts& display){machine.display=display;}

PowerFacts ReadPower(){
 PowerFacts power;
 SYSTEM_POWER_STATUS status{};
 if(GetSystemPowerStatus(&status)){
  power.onBattery=status.ACLineStatus==0;
  power.batterySaver=status.SystemStatusFlag!=0;
  power.batteryPercent=status.BatteryLifePercent==255?-1:int(status.BatteryLifePercent);
 }
 // One record per logical processor; the ceiling is the lowest of them, because
 // the core being held back is the one that decides whether a frame is late.
 unsigned cores=machine.cores?machine.cores:GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
 std::vector<ProcessorPower> info(cores?cores:1);
 if(CallNtPowerInformation(ProcessorInformation,nullptr,0,info.data(),
    ULONG(info.size()*sizeof(ProcessorPower)))==0){
  double worst=1;
  for(const auto& cpu:info){
   if(!cpu.MaxMhz)continue;
   double ratio=double(cpu.MhzLimit?cpu.MhzLimit:cpu.MaxMhz)/double(cpu.MaxMhz);
   worst=(std::min)(worst,ratio);
  }
  power.clockHeadroom=worst;
  // A few percent below the maximum is ordinary firmware behaviour; a fifth
  // below it is the machine protecting itself.
  power.throttled=worst<0.8;
 }
 return power;
}

MemoryFacts ReadMemory(ID3D11Device* device){
 MemoryFacts facts;
 MEMORYSTATUSEX state{};state.dwLength=sizeof state;
 if(GlobalMemoryStatusEx(&state)){
  facts.totalRam=state.ullTotalPhys;facts.availableRam=state.ullAvailPhys;
 }
 PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof counters;
 if(GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof counters))
  facts.workingSet=counters.WorkingSetSize;
 // Comfortable means a fifth of the machine, or two gigabytes, whichever is
 // more: a 32 GB machine with 1 GB left is under pressure even though the
 // percentage still looks survivable.
 if(facts.totalRam){
  double comfort=(std::max)(double(facts.totalRam)*0.2,2.0*double(1ull<<30));
  facts.ramPressure=Clamp01((comfort-double(facts.availableRam))/comfort);
 }
 if(device){
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIAdapter3> adapter3;
  if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))&&
     SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter.As(&adapter3))){
   DXGI_QUERY_VIDEO_MEMORY_INFO local{};
   if(SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local))&&local.Budget){
    facts.vramBudget=local.Budget;facts.vramUsage=local.CurrentUsage;
    facts.vramKnown=true;
    // Pressure starts at four fifths of the budget. DXGI's budget already
    // accounts for everything else running, so reaching it means eviction
    // rather than merely a full card.
    double headroom=double(local.Budget)*0.2;
    facts.vramPressure=Clamp01((double(local.CurrentUsage)-double(local.Budget)*0.8)/(headroom>0?headroom:1));
   }
  }
 }
 return facts;
}

bool DecodeSupported(const DecodeFacts& decode,const std::wstring& codec,unsigned bitDepth){
 auto has=[&](const wchar_t* name){return codec.find(name)!=std::wstring::npos;};
 if(has(L"h264")||has(L"avc"))return decode.h264&&bitDepth<=8;
 if(has(L"hevc")||has(L"h265"))return bitDepth>8?decode.hevc10:(decode.hevc8||decode.hevc10);
 if(has(L"vp9"))return decode.vp9;
 if(has(L"av1"))return decode.av1;
 if(has(L"mpeg2"))return decode.mpeg2;
 if(has(L"vc1")||has(L"wmv"))return decode.vc1;
 return false;
}

PipelineChoice ChoosePipeline(const Capabilities& facts,const MediaProfile& media,const PowerFacts& power){
 PipelineChoice choice;
 choice.hardwareDecode=DecodeSupported(facts.decode,media.codec,media.bitDepth);
 choice.hdrOutput=media.hdr&&facts.display.hdr;
 choice.toneMap=media.hdr&&!facts.display.hdr;
 // Software decode of a large frame is the one case where the scaler gives way:
 // the cores are already the bottleneck, and a cheaper scaler is a better trade
 // than a late frame. Battery saver asks for the same thing.
 bool heavy=!choice.hardwareDecode&&media.width*media.height>1920u*1080u;
 choice.highQualityScaler=!heavy&&!power.batterySaver;
 // §12.6: enhancement is never a candidate on battery, and never on a part with
 // no memory of its own.
 choice.enhancementCandidate=!power.onBattery&&!power.batterySaver&&
  !facts.adapter.integrated&&facts.adapter.vendor==L"NVIDIA"&&choice.hardwareDecode;
 choice.note=PipelineNote(choice);
 return choice;
}

const wchar_t* PipelineNote(const PipelineChoice& choice){
 if(!choice.hardwareDecode)
  return choice.toneMap?L"software decode, tone mapped to SDR":L"software decode";
 if(choice.hdrOutput)return L"hardware decode, HDR output";
 if(choice.toneMap)return L"hardware decode, tone mapped to SDR";
 return L"hardware decode";
}

std::wstring CapabilityReport(const Capabilities& facts,const PowerFacts& power,const MemoryFacts& memory){
 auto mb=[](uint64_t bytes){return std::to_wstring(bytes>>20)+L" MB";};
 auto number=[](double value,const wchar_t* format){
  wchar_t text[32];swprintf_s(text,format,value);return std::wstring(text);
 };
 std::wstring out;
 out+=L"Adapter: "+(facts.adapter.description.empty()?L"unknown":facts.adapter.description);
 out+=L" ("+mb(facts.adapter.dedicatedVideoMemory)+(facts.adapter.integrated?L", integrated)":L")")+L"\n";
 out+=L"Hardware decode:";
 const std::pair<const wchar_t*,bool> codecs[]={
  {L" H.264",facts.decode.h264},{L" HEVC",facts.decode.hevc8},{L" HEVC10",facts.decode.hevc10},
  {L" VP9",facts.decode.vp9},{L" AV1",facts.decode.av1},
  {L" MPEG-2",facts.decode.mpeg2},{L" VC-1",facts.decode.vc1}};
 bool any=false;
 for(const auto& codec:codecs)if(codec.second){out+=codec.first;any=true;}
 if(!any)out+=L" none";
 out+=L"\n";
 out+=L"Display: "+number(facts.display.refreshHz,L"%.3f")+L" Hz";
 out+=facts.display.hdr?L", HDR on":(facts.display.wideGamut?L", wide gamut":L", SDR");
 if(!facts.display.hdr&&!facts.display.wideGamut&&facts.display.panelWideGamut)
  out+=L" (panel claims a wider gamut)";
 if(facts.display.maxNits>0)out+=L", peak "+number(facts.display.maxNits,L"%.0f")+L" nits";
 out+=L", paper white "+number(facts.display.sdrWhiteNits,L"%.0f")+L" nits\n";
 out+=L"Memory: "+mb(memory.availableRam)+L" free of "+mb(memory.totalRam)+
  L", this process "+mb(memory.workingSet)+L"\n";
 if(memory.vramKnown)
  out+=L"Video memory: "+mb(memory.vramUsage)+L" of "+mb(memory.vramBudget)+L" budget\n";
 out+=L"Power: ";
 out+=power.onBattery?L"battery":L"mains";
 if(power.batterySaver)out+=L", battery saver";
 if(power.batteryPercent>=0)out+=L", "+std::to_wstring(power.batteryPercent)+L"%";
 out+=L", clock ceiling "+number(power.clockHeadroom*100,L"%.0f")+L"%";
 if(power.throttled)out+=L" (throttled)";
 out+=L"\n"+facts.os+L", "+std::to_wstring(facts.cores)+L" logical cores\n";
 return out;
}
