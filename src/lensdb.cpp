// Vetro Look, GPL-3.0-or-later.
#include "lensdb.h"
#include <windows.h>
#include <expat.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
namespace fs=std::filesystem;

namespace{

struct Distortion{std::string model;double focal=0,a=0,b=0,c=0;};
struct Vignetting{double focal=0,aperture=0,distance=0,k1=0,k2=0,k3=0;};
struct Tca{std::string model;double focal=0,red=1,blue=1;};

struct Lens{
 std::string maker,model;
 std::vector<std::string> aliases;      // every <model> spelling, including lang variants
 std::string mount,type;
 double cropFactor=0;
 std::vector<Distortion> distortion;
 std::vector<Vignetting> vignetting;
 std::vector<Tca> tca;
 std::string normalised;                // aliases folded for matching
};
struct Camera{
 std::string maker,model;
 std::vector<std::string> makerAliases,modelAliases;
 std::string mount;
 double cropFactor=1;
 std::string normalised;
};

std::mutex mx;
std::vector<Lens> lenses;
std::vector<Camera> cameras;
std::atomic<bool> ready{false},startedLoad{false};
std::thread loader;

std::string Narrow(const std::wstring& s){
 if(s.empty())return {};
 int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),int(s.size()),nullptr,0,nullptr,nullptr);
 if(n<=0)return {};
 std::string out(size_t(n),'\0');
 WideCharToMultiByte(CP_UTF8,0,s.c_str(),int(s.size()),out.data(),n,nullptr,nullptr);
 return out;
}
std::wstring Widen(const std::string& s){
 if(s.empty())return {};
 int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),nullptr,0);
 if(n<=0)return {};
 std::wstring out(size_t(n),L'\0');
 MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),out.data(),n);
 return out;
}

// Matching key: lower case, punctuation and separators collapsed to single
// spaces. "NIKKOR Z 50mm f/1.8 S" and "Nikon Nikkor Z 50 mm f/1.8 S" have to
// look alike, because cameras and catalogues do not agree on either.
std::string Fold(const std::string& s){
 std::string out;
 bool space=true;
 for(char raw:s){
  unsigned char c=(unsigned char)raw;
  if(c>=128){out+=char(c);space=false;continue;}
  if(isalnum(c)){out+=char(tolower(c));space=false;continue;}
  if(!space){out+=' ';space=true;}
 }
 while(!out.empty()&&out.back()==' ')out.pop_back();
 return out;
}
std::vector<std::string> Tokens(const std::string& folded){
 std::vector<std::string> out;
 size_t at=0;
 while(at<folded.size()){
  size_t end=folded.find(' ',at);
  if(end==std::string::npos)end=folded.size();
  if(end>at)out.push_back(folded.substr(at,end-at));
  at=end+1;
 }
 return out;
}
// Token overlap, weighted so that a distinctive token ("105mm", "sigma")
// counts for more than a filler one ("f", "ed", "af").
double Score(const std::vector<std::string>& want,const std::vector<std::string>& have){
 if(want.empty()||have.empty())return 0;
 double hit=0,total=0;
 for(auto& token:want){
  double weight=token.size()<=2?0.35:(token.size()<=3?0.7:1.0);
  total+=weight;
  if(std::find(have.begin(),have.end(),token)!=have.end())hit+=weight;
 }
 if(total<=0)return 0;
 // Extra tokens on the database side are mild evidence against: "50mm f/1.8"
 // must not beat "50mm f/1.8 S" when the file says the latter, and must not
 // lose badly to a 24-70 whose name happens to contain "50".
 double surplus=double(have.size()>want.size()?have.size()-want.size():0);
 return (hit/total)-0.02*surplus;
}

double Attr(const char** atts,const char* name,double fallback=0){
 for(int i=0;atts[i];i+=2)if(!strcmp(atts[i],name))return atof(atts[i+1]);
 return fallback;
}
const char* AttrText(const char** atts,const char* name){
 for(int i=0;atts[i];i+=2)if(!strcmp(atts[i],name))return atts[i+1];
 return nullptr;
}

struct Parser{
 std::vector<Lens> lenses;
 std::vector<Camera> cameras;
 Lens lens;Camera camera;
 int depth=0;
 bool inLens=false,inCamera=false;
 std::string element,text;
};

void XMLCALL Start(void* user,const char* name,const char** atts){
 auto& p=*static_cast<Parser*>(user);
 p.element=name;p.text.clear();
 if(!strcmp(name,"lens")){p.inLens=true;p.lens=Lens{};return;}
 if(!strcmp(name,"camera")){p.inCamera=true;p.camera=Camera{};return;}
 if(p.inLens){
  if(!strcmp(name,"distortion")){
   Distortion d;
   if(auto m=AttrText(atts,"model"))d.model=m;
   d.focal=Attr(atts,"focal");d.a=Attr(atts,"a");d.b=Attr(atts,"b");d.c=Attr(atts,"c");
   // poly3 spells its single term "k1"; poly5 uses k1/k2.
   if(d.model=="poly3"){d.a=0;d.b=Attr(atts,"k1");d.c=0;}
   else if(d.model=="poly5"){d.a=0;d.b=Attr(atts,"k1");d.c=Attr(atts,"k2");}
   if(d.focal>0)p.lens.distortion.push_back(std::move(d));
  }else if(!strcmp(name,"vignetting")){
   Vignetting v;
   v.focal=Attr(atts,"focal");v.aperture=Attr(atts,"aperture");v.distance=Attr(atts,"distance",1000);
   v.k1=Attr(atts,"k1");v.k2=Attr(atts,"k2");v.k3=Attr(atts,"k3");
   if(v.focal>0)p.lens.vignetting.push_back(v);
  }else if(!strcmp(name,"tca")){
   Tca t;
   if(auto m=AttrText(atts,"model"))t.model=m;
   t.focal=Attr(atts,"focal");t.red=Attr(atts,"vr",1);t.blue=Attr(atts,"vb",1);
   if(t.focal>0)p.lens.tca.push_back(std::move(t));
  }
 }
}
void XMLCALL Text(void* user,const char* s,int len){
 auto& p=*static_cast<Parser*>(user);
 p.text.append(s,size_t(len));
}
void XMLCALL End(void* user,const char* name){
 auto& p=*static_cast<Parser*>(user);
 std::string value=p.text;
 while(!value.empty()&&(value.back()==' '||value.back()=='\n'||value.back()=='\r'||value.back()=='\t'))value.pop_back();
 size_t first=value.find_first_not_of(" \n\r\t");
 value=first==std::string::npos?std::string():value.substr(first);
 p.text.clear();
 if(!strcmp(name,"lens")){
  if(!p.lens.model.empty()){
   std::string folded;
   for(auto& alias:p.lens.aliases)folded+=Fold(alias)+" ";
   p.lens.normalised=Fold(p.lens.maker)+" "+folded;
   p.lenses.push_back(std::move(p.lens));
  }
  p.inLens=false;return;
 }
 if(!strcmp(name,"camera")){
  if(!p.camera.model.empty()){
   std::string folded;
   for(auto& alias:p.camera.modelAliases)folded+=Fold(alias)+" ";
   for(auto& alias:p.camera.makerAliases)folded+=Fold(alias)+" ";
   p.camera.normalised=folded;
   p.cameras.push_back(std::move(p.camera));
  }
  p.inCamera=false;return;
 }
 if(value.empty())return;
 if(p.inLens){
  if(!strcmp(name,"maker")){if(p.lens.maker.empty())p.lens.maker=value;}
  else if(!strcmp(name,"model")){if(p.lens.model.empty())p.lens.model=value;p.lens.aliases.push_back(value);}
  else if(!strcmp(name,"mount")){if(p.lens.mount.empty())p.lens.mount=value;}
  else if(!strcmp(name,"type"))p.lens.type=value;
  else if(!strcmp(name,"cropfactor"))p.lens.cropFactor=atof(value.c_str());
 }else if(p.inCamera){
  if(!strcmp(name,"maker")){if(p.camera.maker.empty())p.camera.maker=value;p.camera.makerAliases.push_back(value);}
  else if(!strcmp(name,"model")){if(p.camera.model.empty())p.camera.model=value;p.camera.modelAliases.push_back(value);}
  else if(!strcmp(name,"mount"))p.camera.mount=value;
  else if(!strcmp(name,"cropfactor"))p.camera.cropFactor=atof(value.c_str());
 }
}

std::wstring DatabaseDirectory(){
 wchar_t exe[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,exe,MAX_PATH))return {};
 auto here=fs::path(exe).parent_path();
 std::error_code ec;
 // Installed next to the executable; in a development tree, still in source.
 for(auto candidate:{here/L"lensfun-db",here/L".."/L"third_party"/L"lensfun-db",
                     here/L".."/L".."/L"third_party"/L"lensfun-db",
                     here/L".."/L".."/L".."/L"third_party"/L"lensfun-db"}){
  if(fs::exists(candidate,ec)&&fs::is_directory(candidate,ec))
   return fs::weakly_canonical(candidate,ec).wstring();
 }
 return {};
}

void Load(){
 auto directory=DatabaseDirectory();
 Parser parser;
 if(!directory.empty()){
  std::error_code ec;
  for(fs::directory_iterator it(directory,ec),end;it!=end&&!ec;it.increment(ec)){
   if(!it->is_regular_file(ec))continue;
   auto extension=it->path().extension().wstring();
   for(auto& c:extension)c=towlower(c);
   if(extension!=L".xml")continue;
   HANDLE file=CreateFileW(it->path().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
   if(file==INVALID_HANDLE_VALUE)continue;
   LARGE_INTEGER size{};
   std::vector<char> bytes;
   if(GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<32ll*1024*1024){
    bytes.resize(size_t(size.QuadPart));
    DWORD got=0;
    if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&got,nullptr)||got!=bytes.size())bytes.clear();
   }
   CloseHandle(file);
   if(bytes.empty())continue;
   XML_Parser xml=XML_ParserCreate("UTF-8");
   if(!xml)continue;
   XML_SetUserData(xml,&parser);
   XML_SetElementHandler(xml,Start,End);
   XML_SetCharacterDataHandler(xml,Text);
   // The files carry a DOCTYPE pointing at a .dtd that is not shipped; expat
   // must not try to resolve it.
   XML_SetParamEntityParsing(xml,XML_PARAM_ENTITY_PARSING_NEVER);
   XML_Parse(xml,bytes.data(),int(bytes.size()),1);
   XML_ParserFree(xml);
   parser.inLens=parser.inCamera=false;
  }
 }
 {
  std::lock_guard lock(mx);
  lenses=std::move(parser.lenses);
  cameras=std::move(parser.cameras);
 }
 ready=true;
}

// Linear interpolation between the two nearest calibrated focal lengths.
// Outside the calibrated range, the nearest end is used unchanged; Lensfun
// does the same, and extrapolating a polynomial distortion model is a good
// way to bend a picture the wrong way.
template<class T,class Get>
bool Bracket(const std::vector<T>& list,double focal,Get get,const T*& low,const T*& high,double& blend){
 low=high=nullptr;blend=0;
 for(const auto& item:list){
  double f=get(item);
  if(f<=focal&&(!low||f>get(*low)))low=&item;
  if(f>=focal&&(!high||f<get(*high)))high=&item;
 }
 if(!low&&!high)return false;
 if(!low)low=high;
 if(!high)high=low;
 double a=get(*low),b=get(*high);
 blend=(b>a)?(focal-a)/(b-a):0;
 blend=(std::max)(0.0,(std::min)(1.0,blend));
 return true;
}

}   // namespace

void LensDbStart(){
 bool expected=false;
 if(!startedLoad.compare_exchange_strong(expected,true))return;
 loader=std::thread([]{
  // Never on the UI thread: 5 MB of XML is fast, but not free.
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  try{Load();}catch(...){ready=true;}
 });
 loader.detach();
}
bool LensDbReady(){return ready.load();}
size_t LensDbLensCount(){std::lock_guard lock(mx);return lenses.size();}
size_t LensDbCameraCount(){std::lock_guard lock(mx);return cameras.size();}

LensCorrectionInfo LensLookup(const std::wstring& cameraMake,const std::wstring& cameraModel,
                              const std::wstring& lensName,double focal,double aperture){
 LensCorrectionInfo out;
 LensDbStart();
 if(!ready.load())return out;                 // caller retries once loading finishes
 out.ready=true;
 std::lock_guard lock(mx);
 if(lenses.empty()){out.note=L"The lens database is not installed beside the application.";return out;}

 auto cameraTokens=Tokens(Fold(Narrow(cameraMake))+" "+Fold(Narrow(cameraModel)));
 const Camera* bestCamera=nullptr;double bestCameraScore=0;
 for(auto& camera:cameras){
  double score=Score(cameraTokens,Tokens(camera.normalised));
  if(score>bestCameraScore){bestCameraScore=score;bestCamera=&camera;}
 }
 if(bestCamera&&bestCameraScore>=0.6){
  out.cameraMatch=Widen(bestCamera->maker)+L" "+Widen(bestCamera->model);
  out.cropFactor=bestCamera->cropFactor;
  out.mount=Widen(bestCamera->mount);
 }

 if(lensName.empty()){
  out.note=L"The file records no lens.";
  return out;
 }
 auto wanted=Tokens(Fold(Narrow(lensName)));
 const Lens* best=nullptr;double bestScore=0;
 for(auto& lens:lenses){
  // A mount mismatch is a hard exclusion when the camera is known: a Canon
  // 50mm must never be offered as the profile for a Nikon body.
  if(!out.mount.empty()&&!lens.mount.empty()&&Widen(lens.mount)!=out.mount)continue;
  double score=Score(wanted,Tokens(lens.normalised));
  if(score>bestScore){bestScore=score;best=&lens;}
 }
 if(!best||bestScore<0.62){
  // Try again without the mount filter: Lensfun's mount compatibility lists
  // are richer than the single mount name a camera record carries.
  best=nullptr;bestScore=0;
  for(auto& lens:lenses){
   double score=Score(wanted,Tokens(lens.normalised));
   if(score>bestScore){bestScore=score;best=&lens;}
  }
 }
 if(!best||bestScore<0.62){
  out.note=L"No calibration profile matches this lens.";
  return out;
 }
 out.matched=true;
 out.lensMatch=Widen(best->model);
 if(out.mount.empty())out.mount=Widen(best->mount);
 if(out.cropFactor<=0)out.cropFactor=best->cropFactor;
 // A projection change needs only the lens type, which every record has by
 // implication (rectilinear unless it says otherwise).
 out.geometry=true;

 if(focal>0&&!best->distortion.empty()){
  const Distortion *low=nullptr,*high=nullptr;double blend=0;
  if(Bracket(best->distortion,focal,[](const Distortion& d){return d.focal;},low,high,blend)){
   out.distortion=true;
   out.distortionModel=Widen(low->model.empty()?high->model:low->model);
   out.distortionA=low->a+(high->a-low->a)*blend;
   out.distortionB=low->b+(high->b-low->b)*blend;
   out.distortionC=low->c+(high->c-low->c)*blend;
  }
 }
 if(focal>0&&!best->tca.empty()){
  const Tca *low=nullptr,*high=nullptr;double blend=0;
  if(Bracket(best->tca,focal,[](const Tca& t){return t.focal;},low,high,blend)){
   out.tca=true;
   out.tcaModel=Widen(low->model.empty()?high->model:low->model);
   out.tcaRed=low->red+(high->red-low->red)*blend;
   out.tcaBlue=low->blue+(high->blue-low->blue)*blend;
  }
 }
 if(focal>0&&!best->vignetting.empty()){
  // Vignetting is calibrated per (focal, aperture, distance). Pick the entry
  // closest in all three rather than interpolating a three-dimensional grid
  // that the database only sparsely populates.
  const Vignetting* nearest=nullptr;double nearestCost=1e18;
  for(auto& v:best->vignetting){
   double cost=std::fabs(std::log((std::max)(1.0,v.focal))-std::log((std::max)(1.0,focal)))*3.0;
   if(aperture>0&&v.aperture>0)cost+=std::fabs(std::log(v.aperture)-std::log(aperture));
   else if(aperture<=0)cost+=0.25;
   if(cost<nearestCost){nearestCost=cost;nearest=&v;}
  }
  if(nearest){
   out.vignetting=true;out.vignettingModel=L"pa";
   out.vignetteK1=nearest->k1;out.vignetteK2=nearest->k2;out.vignetteK3=nearest->k3;
  }
 }
 return out;
}
