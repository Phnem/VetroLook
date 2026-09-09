// Independent GUI benchmark harness.  It deliberately knows nothing about
// VetroLook's decoder/pipeline: all timing is measured from injected input to
// a changed and subsequently quiet desktop frame captured through DXGI Desktop
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

struct Options {
  std::wstring exe, args, file, csv;
  std::wstring app, version, edition, benchmark, notes;
  std::wstring mode = L"navigate"; // navigate = Right/Left; type = literal UTF-16 text; cold-open = launch-only, no input sent.
  double minChangedFraction = 0.04; // cold-open only: fraction of the sampled grid that must cross perCellChangeThreshold.
  double perCellChangeThreshold = 15.0;
  std::wstring launch = L"create"; // create = CreateProcess, shell = ShellExecuteEx, appx = ShellExecuteEx ProgID.
  std::wstring progid;
  std::wstring text = L"TEST";
  std::wstring sequenceFile;
  std::vector<WORD> sequenceKeys;
  std::wstring attachProcessName; // manual-trigger mode: attach to an already-running app instead of launching.
  std::wstring dumpDir; // manual-trigger mode: where diagnostic BMP frames are saved.
  int armTimeoutMs = 120000; // manual-trigger mode: how long to wait for the user's click.
  int samples = 1;
  int startIndex = -1; // purely informational, recorded in the CSV for reproducibility.
  int timeoutMs = 10000;
  int settleMs = 180;
  int startupDelayMs = 0;
  double changeThreshold = 2.0;
  bool keepOpen = false;
  int maxConsecutiveTimeouts = 5; // Hard stop: never grind through hundreds of dead samples.
  bool verbose = false;
};

static std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }
static uint64_t Ft64(const FILETIME& t) {
  ULARGE_INTEGER v{}; v.LowPart = t.dwLowDateTime; v.HighPart = t.dwHighDateTime; return v.QuadPart;
}
static uint64_t QpcNow() { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return uint64_t(q.QuadPart); }
static double QpcMs(uint64_t a, uint64_t b, uint64_t freq) { return (double(b - a) * 1000.0) / double(freq); }

static std::wstring CsvEscape(std::wstring s) {
  std::wstring out = L"\"";
  for (wchar_t c : s) { if (c == L'\"') out += L"\"\""; else out += c; }
  return out + L"\"";
}
static std::string Narrow(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string r(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr); return r;
}
static std::wstring NowIso() {
  SYSTEMTIME t{}; GetSystemTime(&t); wchar_t b[64]{};
  swprintf_s(b, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
  return b;
}

static std::wstring ResolvePhotosProgId() {
  HKEY hkcr = nullptr;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, nullptr, 0, KEY_READ, &hkcr) == ERROR_SUCCESS) {
    wchar_t subkey[256]{};
    DWORD idx = 0;
    DWORD len = 256;
    while (RegEnumKeyExW(hkcr, idx++, subkey, &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
      len = 256;
      if (wcsncmp(subkey, L"AppX", 4) == 0) {
        std::wstring appKeyPath = std::wstring(subkey) + L"\\Application";
        HKEY hApp = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, appKeyPath.c_str(), 0, KEY_QUERY_VALUE, &hApp) == ERROR_SUCCESS) {
          wchar_t aumid[512]{};
          DWORD type = 0, size = sizeof(aumid);
          if (RegQueryValueExW(hApp, L"ApplicationUserModelId", nullptr, &type, (LPBYTE)aumid, &size) != ERROR_SUCCESS) {
            size = sizeof(aumid);
            RegQueryValueExW(hApp, L"AppUserModelID", nullptr, &type, (LPBYTE)aumid, &size);
          }
          if (wcsstr(aumid, L"Photos") != nullptr) {
            RegCloseKey(hApp);
            RegCloseKey(hkcr);
            return subkey;
          }
          RegCloseKey(hApp);
        }
      }
    }
    RegCloseKey(hkcr);
  }
  return L"AppX43hnxtbyyps62jhe9sqpdzxn1790zetc"; // fallback
}

struct WindowSearch { DWORD pid; HWND hwnd = nullptr; };
static BOOL CALLBACK FindWindowForPid(HWND h, LPARAM l) {
  auto* s = reinterpret_cast<WindowSearch*>(l); DWORD p = 0; GetWindowThreadProcessId(h, &p);
  if (p == s->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
    RECT r{};
    GetWindowRect(h, &r);
    if ((r.right - r.left) >= 200 && (r.bottom - r.top) >= 200) {
      s->hwnd = h;
      return FALSE;
    }
  }
  return TRUE;
}
static HWND WaitForWindow(DWORD pid, int timeoutMs) {
  const auto end = GetTickCount64() + uint64_t(timeoutMs);
  while (GetTickCount64() < end) { WindowSearch s{pid}; EnumWindows(FindWindowForPid, LPARAM(&s)); if (s.hwnd) return s.hwnd; Sleep(40); }
  return nullptr;
}

struct FindByProcessNameCtx { std::wstring processName; HWND result=nullptr; };
static BOOL CALLBACK FindWindowByProcessNameCb(HWND h, LPARAM l) {
  auto* ctx = reinterpret_cast<FindByProcessNameCtx*>(l);
  if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr) return TRUE;
  RECT r{}; GetWindowRect(h, &r);
  if ((r.right-r.left) < 200 || (r.bottom-r.top) < 200) return TRUE;
  wchar_t cls[128]{}; GetClassNameW(h, cls, 128);
  if (wcscmp(cls, L"#32770") == 0) return TRUE; // standard Open/Save/etc dialog - never the viewer surface.
  DWORD pid=0; GetWindowThreadProcessId(h, &pid);
  HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!ph) return TRUE;
  wchar_t path[MAX_PATH]{}; DWORD sz = MAX_PATH;
  bool ok = QueryFullProcessImageNameW(ph, 0, path, &sz);
  CloseHandle(ph);
  if (!ok) return TRUE;
  std::wstring lowerP(path); for (auto& c : lowerP) c = towlower(c);
  std::wstring lowerName = ctx->processName; for (auto& c : lowerName) c = towlower(c);
  if (lowerP.find(lowerName) != std::wstring::npos) { ctx->result = h; return FALSE; }
  return TRUE;
}
// Attaches to an already-running app's window instead of launching one - for cases (ImageGlass)
// where automated launch does not reliably reach a real viewer state, and the user drives the app
// by hand up to the point we need to start timing from.
static HWND FindWindowByProcessName(const std::wstring& processName) {
  FindByProcessNameCtx ctx{processName};
  EnumWindows(FindWindowByProcessNameCb, LPARAM(&ctx));
  return ctx.result;
}

// Global low-level mouse hook: the only reliable way to time "the user's actual click", since a
// manual-trigger measurement is defined from the real mouse-down, not from an approximate
// human-pressed hotkey. Requires the installing thread to pump messages (the hook is delivered via
// the thread's message queue), hence the explicit loop in WaitForGlobalMouseClick.
static HHOOK g_mouseHook = nullptr;
static volatile LONG g_clicked = 0;
static uint64_t g_clickQpc = 0;
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
    if (InterlockedCompareExchange(&g_clicked, 1, 0) == 0) {
      g_clickQpc = QpcNow();
    }
  }
  return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}
static bool WaitForGlobalMouseClick(int timeoutMs, uint64_t& outQpc) {
  g_clicked = 0; g_clickQpc = 0;
  g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
  if (!g_mouseHook) return false;
  // A blocking GetMessage() only returns when an actual message is queued for THIS thread; a
  // WH_MOUSE_LL hook fires as a sent-message side effect while the queue is being checked, but
  // that alone does not guarantee GetMessage returns promptly (or at all, if nothing else ever
  // posts to this thread). Polling with PeekMessage re-enters the "checking the queue" state
  // (which is what actually lets the hook fire) on a short fixed interval, so our own loop
  // reliably re-checks g_clicked instead of depending on GetMessage's return value.
  const uint64_t deadline = GetTickCount64() + uint64_t(timeoutMs);
  MSG msg{};
  while (!g_clicked && GetTickCount64() < deadline) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_clicked) break;
    Sleep(5);
  }
  UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr;
  if (g_clicked) { outQpc = g_clickQpc; return true; }
  return false;
}

static std::vector<HWND> EnumVisibleWindows() {
  std::vector<HWND> v;
  EnumWindows([](HWND h, LPARAM l) -> BOOL {
    if (IsWindowVisible(h)) reinterpret_cast<std::vector<HWND>*>(l)->push_back(h);
    return TRUE;
  }, LPARAM(&v));
  return v;
}

static HWND WaitForAppxWindow(const std::vector<HWND>& before, int timeoutMs, DWORD& outPid) {
  const auto end = GetTickCount64() + uint64_t(timeoutMs);
  while (GetTickCount64() < end) {
    auto current = EnumVisibleWindows();
    for (HWND h : current) {
      bool existed = false;
      for (HWND b : before) { if (b == h) { existed = true; break; } }
      if (!existed) {
        wchar_t cls[128]{};
        GetClassNameW(h, cls, 128);
        if (wcscmp(cls, L"WinUIDesktopWin32WindowClass") == 0 || wcscmp(cls, L"ApplicationFrameWindow") == 0) {
          RECT r{};
          GetWindowRect(h, &r);
          if ((r.right - r.left) >= 200 && (r.bottom - r.top) >= 200) {
            GetWindowThreadProcessId(h, &outPid);
            return h;
          }
        }
      }
    }
    Sleep(50);
  }
  return nullptr;
}

struct Frame { std::vector<uint8_t> luma; RECT source{}; };
struct ColorFrame { std::vector<uint8_t> bgr; int width=0, height=0; }; // diagnostic dumps only.

static bool SaveBmp(const std::wstring& path, const ColorFrame& f) {
  if (f.width <= 0 || f.height <= 0) return false;
  int rowSize = ((f.width*3+3)/4)*4;
  uint32_t dataSize = uint32_t(rowSize) * uint32_t(f.height);
  std::ofstream out(fs::path(path), std::ios::binary);
  if (!out) return false;
  uint32_t fileSize = 14+40+dataSize;
  auto w16=[&](uint16_t v){ out.write(reinterpret_cast<const char*>(&v),2); };
  auto w32=[&](uint32_t v){ out.write(reinterpret_cast<const char*>(&v),4); };
  out.write("BM",2); w32(fileSize); w32(0); w32(14+40);
  w32(40); w32(uint32_t(f.width)); w32(uint32_t(f.height)); w16(1); w16(24); w32(0); w32(dataSize); w32(2835); w32(2835); w32(0); w32(0);
  std::vector<uint8_t> pad(size_t(rowSize) - size_t(f.width)*3, 0);
  for (int y=f.height-1; y>=0; --y) { // BMP rows are bottom-up
    out.write(reinterpret_cast<const char*>(&f.bgr[size_t(y)*size_t(f.width)*3]), size_t(f.width)*3);
    if (!pad.empty()) out.write(reinterpret_cast<const char*>(pad.data()), std::streamsize(pad.size()));
  }
  return true;
}
static double Difference(const Frame& a, const Frame& b) {
  if (a.luma.size() != b.luma.size() || a.luma.empty()) return 255.0;
  uint64_t total = 0; for (size_t i=0;i<a.luma.size();++i) total += uint64_t(std::abs(int(a.luma[i])-int(b.luma[i])));
  return double(total) / double(a.luma.size());
}
// Mean delta alone can be tripped by a small, localized change (a spinner, a
// blinking cursor, a toolbar highlight) - real "a photo appeared" transitions
// change a large, spatially-distributed fraction of the sampled grid. Used to
// gate cold-open detection specifically, where a small localized false
// positive (splash screen glow, loading spinner) is a real risk that
// navigate-mode's whole-image swap doesn't have.
static double ChangedFraction(const Frame& a, const Frame& b, uint8_t perCellThreshold) {
  if (a.luma.size() != b.luma.size() || a.luma.empty()) return 1.0;
  size_t changed = 0;
  for (size_t i = 0; i < a.luma.size(); ++i) {
    if (std::abs(int(a.luma[i]) - int(b.luma[i])) >= perCellThreshold) ++changed;
  }
  return double(changed) / double(a.luma.size());
}

class DesktopCapture {
 public:
  bool Init(HWND window, std::wstring& error) {
    // Primary capture backend: DXGI Desktop Duplication
    HMONITOR mon = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST); MONITORINFOEXW mi{}; mi.cbSize=sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
      ComPtr<IDXGIFactory1> factory;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT ai=0;;++ai) {
          ComPtr<IDXGIAdapter1> a; if (factory->EnumAdapters1(ai,&a)==DXGI_ERROR_NOT_FOUND) break;
          for (UINT oi=0;;++oi) {
            ComPtr<IDXGIOutput> o; if (a->EnumOutputs(oi,&o)==DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{}; o->GetDesc(&od);
            if (wcscmp(od.DeviceName, mi.szDevice) != 0) continue;
            UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT; D3D_FEATURE_LEVEL fl{};
            if (SUCCEEDED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr,0,D3D11_SDK_VERSION,&dev_,&fl,&ctx_))) {
              ComPtr<IDXGIOutput1> o1;
              if (SUCCEEDED(o.As(&o1)) && SUCCEEDED(o1->DuplicateOutput(dev_.Get(), &dup_))) {
                desktop_ = od.DesktopCoordinates;
                isDxgi_ = true;
                return true;
              }
            }
          }
        }
      }
    }
    // Diagnostic fallback backend: PrintWindow
    isDxgi_ = false;
    RECT cr{};
    if (!GetClientRect(window, &cr) || (cr.right - cr.left < 64) || (cr.bottom - cr.top < 64)) {
      error = L"Both DXGI Duplication and PrintWindow client rect check failed";
      return false;
    }
    return true;
  }

  bool IsDxgi() const { return isDxgi_; }

  std::optional<Frame> Capture(HWND window, DWORD waitMs, std::wstring& error) {
    if (isDxgi_) {
      return CaptureDxgi(window, waitMs, error);
    }
    return CapturePrintWindow(window, error);
  }

  // Full-color capture for diagnostic frame dumps (manual-trigger mode). DXGI only.
  std::optional<ColorFrame> CaptureColor(HWND window, DWORD waitMs, std::wstring& error, int maxWidth = 900) {
    if (!isDxgi_) { error = L"Color capture requires DXGI backend"; return std::nullopt; }
    DXGI_OUTDUPL_FRAME_INFO fi{}; ComPtr<IDXGIResource> resource;
    HRESULT hr = dup_->AcquireNextFrame(waitMs, &fi, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return std::nullopt;
    if (FAILED(hr)) { error = L"AcquireNextFrame failed"; return std::nullopt; }
    struct Release { IDXGIOutputDuplication* d; ~Release(){d->ReleaseFrame();} } release{dup_.Get()};
    ComPtr<ID3D11Texture2D> texture; if (FAILED(resource.As(&texture))) { error=L"Frame is not a texture"; return std::nullopt; }
    D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
    if (!staging_ || d.Width!=width_ || d.Height!=height_) {
      D3D11_TEXTURE2D_DESC s=d; s.BindFlags=0; s.MiscFlags=0; s.CPUAccessFlags=D3D11_CPU_ACCESS_READ; s.Usage=D3D11_USAGE_STAGING;
      if (FAILED(dev_->CreateTexture2D(&s,nullptr,&staging_))) { error=L"Create staging texture failed"; return std::nullopt; }
      width_=d.Width; height_=d.Height;
    }
    ctx_->CopyResource(staging_.Get(), texture.Get()); D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&map))) { error=L"Map desktop frame failed"; return std::nullopt; }
    struct Unmap { ID3D11DeviceContext* c; ID3D11Texture2D* t; ~Unmap(){c->Unmap(t,0);} } unmap{ctx_.Get(),staging_.Get()};
    RECT cr{}; GetClientRect(window,&cr); POINT p{cr.left,cr.top}; ClientToScreen(window,&p);
    RECT r{p.x,p.y,p.x+(cr.right-cr.left),p.y+(cr.bottom-cr.top)};
    r.left=std::max(r.left,desktop_.left); r.top=std::max(r.top,desktop_.top); r.right=std::min(r.right,desktop_.right); r.bottom=std::min(r.bottom,desktop_.bottom);
    int srcW=r.right-r.left, srcH=r.bottom-r.top;
    if (srcW<16 || srcH<16) { error=L"Viewer region too small"; return std::nullopt; }
    int dstW = std::min(srcW, maxWidth);
    int dstH = int((int64_t(srcH) * dstW) / srcW);
    if (dstW<1) dstW=1; if (dstH<1) dstH=1;
    ColorFrame out; out.width=dstW; out.height=dstH; out.bgr.resize(size_t(dstW)*size_t(dstH)*3);
    for (int y=0;y<dstH;++y) for (int x=0;x<dstW;++x) {
      int sx = r.left + (srcW*x)/dstW - desktop_.left;
      int sy = r.top + (srcH*y)/dstH - desktop_.top;
      const uint8_t* px = static_cast<const uint8_t*>(map.pData) + size_t(sy)*map.RowPitch + size_t(sx)*4;
      size_t o = (size_t(y)*size_t(dstW)+size_t(x))*3;
      out.bgr[o+0]=px[0]; out.bgr[o+1]=px[1]; out.bgr[o+2]=px[2];
    }
    return out;
  }

 private:
  std::optional<Frame> CaptureDxgi(HWND window, DWORD waitMs, std::wstring& error) {
    DXGI_OUTDUPL_FRAME_INFO fi{}; ComPtr<IDXGIResource> resource;
    HRESULT hr=dup_->AcquireNextFrame(waitMs, &fi, &resource);
    if (hr==DXGI_ERROR_WAIT_TIMEOUT) return std::nullopt;
    if (FAILED(hr)) { error=L"AcquireNextFrame failed"; return std::nullopt; }
    struct Release { IDXGIOutputDuplication* d; ~Release(){d->ReleaseFrame();} } release{dup_.Get()};
    ComPtr<ID3D11Texture2D> texture; if (FAILED(resource.As(&texture))) { error=L"Frame is not a texture"; return std::nullopt; }
    D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
    if (!staging_ || d.Width!=width_ || d.Height!=height_) {
      D3D11_TEXTURE2D_DESC s=d; s.BindFlags=0; s.MiscFlags=0; s.CPUAccessFlags=D3D11_CPU_ACCESS_READ; s.Usage=D3D11_USAGE_STAGING;
      if (FAILED(dev_->CreateTexture2D(&s,nullptr,&staging_))) { error=L"Create staging texture failed"; return std::nullopt; }
      width_=d.Width; height_=d.Height;
    }
    ctx_->CopyResource(staging_.Get(), texture.Get()); D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&map))) { error=L"Map desktop frame failed"; return std::nullopt; }
    struct Unmap { ID3D11DeviceContext* c; ID3D11Texture2D* t; ~Unmap(){c->Unmap(t,0);} } unmap{ctx_.Get(),staging_.Get()};
    RECT cr{}; GetClientRect(window,&cr); POINT p{cr.left,cr.top}; ClientToScreen(window,&p);
    RECT r{p.x,p.y,p.x+(cr.right-cr.left),p.y+(cr.bottom-cr.top)};
    r.left=std::max(r.left,desktop_.left); r.top=std::max(r.top,desktop_.top); r.right=std::min(r.right,desktop_.right); r.bottom=std::min(r.bottom,desktop_.bottom);
    if (r.right-r.left<64 || r.bottom-r.top<64) { error=L"Viewer client area is too small or off-screen"; return std::nullopt; }
    int left=r.left+(r.right-r.left)*15/100, right=r.right-(r.right-r.left)*15/100;
    int top=r.top+(r.bottom-r.top)*15/100, bottom=r.bottom-(r.bottom-r.top)*15/100;
    Frame out; out.source=r; constexpr int cols=160, rows=100; out.luma.reserve(cols*rows);
    for(int y=0;y<rows;++y) for(int x=0;x<cols;++x) {
      int sx=left+(right-left)*(2*x+1)/(2*cols)-desktop_.left;
      int sy=top+(bottom-top)*(2*y+1)/(2*rows)-desktop_.top;
      const uint8_t* px=static_cast<const uint8_t*>(map.pData)+size_t(sy)*map.RowPitch+size_t(sx)*4;
      unsigned lum=(unsigned(px[2])*77u+unsigned(px[1])*150u+unsigned(px[0])*29u)>>8;
      out.luma.push_back(uint8_t(lum & 0xF8));
    }
    return out;
  }

  std::optional<Frame> CapturePrintWindow(HWND window, std::wstring& error) {
    RECT cr{}; GetClientRect(window, &cr);
    int width = cr.right - cr.left;
    int height = cr.bottom - cr.top;
    if (width < 64 || height < 64) { error = L"Viewer client area is too small"; return std::nullopt; }
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    HGDIOBJ oldBm = SelectObject(hdcMem, hbm);
    BOOL pwOk = PrintWindow(window, hdcMem, 2 /* PW_RENDERFULLCONTENT */);
    if (!pwOk) pwOk = PrintWindow(window, hdcMem, 0);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
    GetDIBits(hdcMem, hbm, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);
    SelectObject(hdcMem, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    if (!pwOk) { error = L"PrintWindow failed"; return std::nullopt; }
    int left = width * 15 / 100, right = width - width * 15 / 100;
    int top = height * 15 / 100, bottom = height - height * 15 / 100;
    Frame out; out.source = cr; constexpr int cols = 160, rows = 100; out.luma.reserve(cols * rows);
    for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) {
      int sx = left + (right - left) * (2 * x + 1) / (2 * cols);
      int sy = top + (bottom - top) * (2 * y + 1) / (2 * rows);
      const uint8_t* px = pixels.data() + size_t(sy) * width * 4 + size_t(sx) * 4;
      unsigned lum = (unsigned(px[2]) * 77u + unsigned(px[1]) * 150u + unsigned(px[0]) * 29u) >> 8;
      out.luma.push_back(uint8_t(lum & 0xF8));
    }
    return out;
  }

  ComPtr<ID3D11Device> dev_; ComPtr<ID3D11DeviceContext> ctx_; ComPtr<IDXGIOutputDuplication> dup_; ComPtr<ID3D11Texture2D> staging_;
  RECT desktop_{}; UINT width_=0,height_=0;
  bool isDxgi_ = false;
};

static bool SendNavigation(HWND w, WORD vk = VK_RIGHT) {
  // Arrow keys are "extended" keys on a real keyboard (E0-prefixed scan code).
  // Frameworks that consult the raw scan code / extended-key bit (rather than
  // trusting wParam alone) can otherwise interpret an unflagged synthetic
  // VK_RIGHT/VK_LEFT as the corresponding non-extended numpad key instead.
  WORD scan = LOWORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  INPUT in[2]{};
  in[0].type=INPUT_KEYBOARD; in[0].ki.wVk=vk; in[0].ki.wScan=scan; in[0].ki.dwFlags=KEYEVENTF_EXTENDEDKEY;
  in[1]=in[0]; in[1].ki.dwFlags=KEYEVENTF_EXTENDEDKEY|KEYEVENTF_KEYUP;
  bool ok = SendInput(2,in,sizeof(INPUT))==2;
  if (w) {
    LPARAM lp = (LPARAM(scan) << 16) | (LPARAM(1) << 24) | 1;
    PostMessageW(w, WM_KEYDOWN, vk, lp);
    PostMessageW(w, WM_KEYUP, vk, lp | (LPARAM(3) << 30));
  }
  return ok;
}

// Real hardware mouse input is not subject to Windows' foreground-lock
// timeout the way SetForegroundWindow is. A click on the client area (the
// image surface, not a toolbar edge) is the most reliable way to hand a
// freshly-launched window genuine OS-level keyboard focus, which is what
// caused intermittent ImageGlass TIMEOUTs (SendInput of arrow keys landing
// on a window that never actually became foreground).
static void ClickWindowCenter(HWND w) {
  RECT cr{};
  if (!GetClientRect(w, &cr)) return;
  POINT center{ (cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2 };
  ClientToScreen(w, &center);
  POINT before{};
  GetCursorPos(&before);
  int screenW = GetSystemMetrics(SM_CXSCREEN), screenH = GetSystemMetrics(SM_CYSCREEN);
  if (screenW <= 0 || screenH <= 0) return;
  INPUT in[3]{};
  in[0].type = INPUT_MOUSE;
  in[0].mi.dx = LONG((center.x * 65535L) / screenW);
  in[0].mi.dy = LONG((center.y * 65535L) / screenH);
  in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
  in[1] = in[0]; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
  in[2] = in[0]; in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
  SendInput(3, in, sizeof(INPUT));
  Sleep(30);
  SetCursorPos(before.x, before.y);
}

static bool FocusForInput(HWND w) {
  HWND foreground = GetForegroundWindow();
  if (foreground == w) return true;
  if (GetAncestor(foreground, GA_ROOT) == w || GetAncestor(foreground, GA_ROOTOWNER) == w) return true;
  DWORD fgPid = 0; DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, &fgPid) : 0;
  DWORD targetPid = 0; DWORD targetThread = GetWindowThreadProcessId(w, &targetPid);
  if (fgPid != 0 && fgPid == targetPid) return true;

  DWORD thisThread = GetCurrentThreadId();
  if (foregroundThread && foregroundThread != thisThread) AttachThreadInput(thisThread, foregroundThread, TRUE);
  if (targetThread && targetThread != thisThread) AttachThreadInput(thisThread, targetThread, TRUE);

  ShowWindow(w, SW_RESTORE);
  SetForegroundWindow(w);
  BringWindowToTop(w);
  SetActiveWindow(w);
  SetFocus(w);

  if (targetThread && targetThread != thisThread) AttachThreadInput(thisThread, targetThread, FALSE);
  if (foregroundThread && foregroundThread != thisThread) AttachThreadInput(thisThread, foregroundThread, FALSE);

  for (int i = 0; i < 15; ++i) {
    foreground = GetForegroundWindow();
    if (foreground == w) return true;
    if (GetAncestor(foreground, GA_ROOT) == w || GetAncestor(foreground, GA_ROOTOWNER) == w) return true;
    fgPid = 0; if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
    if (fgPid != 0 && fgPid == targetPid) return true;
    Sleep(10);
  }
  return false;
}
static bool SendText(const std::wstring& text) {
  std::vector<INPUT> in; in.reserve(text.size()*2);
  for(wchar_t c:text) { INPUT down{}; down.type=INPUT_KEYBOARD; down.ki.wScan=c; down.ki.dwFlags=KEYEVENTF_UNICODE; INPUT up=down; up.ki.dwFlags|=KEYEVENTF_KEYUP; in.push_back(down); in.push_back(up); }
  return SendInput(UINT(in.size()),in.data(),sizeof(INPUT))==in.size();
}
// peakWorking is read straight from Windows' own PeakWorkingSetSize tracking
// (maintained by the OS for the life of the process, not something we sample
// ourselves) - it's the correct "true peak" for working set regardless of how
// often we call this. There is no equivalent native field for private bytes,
// so peak private bytes is a running max the caller must maintain across
// repeated calls during a measurement window.
static void ProcessStats(HANDLE h, uint64_t& cpu100ns, SIZE_T& working, SIZE_T& peakWorking, SIZE_T& privateBytes) {
  FILETIME c{},e{},k{},u{}; GetProcessTimes(h,&c,&e,&k,&u); cpu100ns=Ft64(k)+Ft64(u);
  PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb=sizeof(pm); GetProcessMemoryInfo(h,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),sizeof(pm));
  working=pm.WorkingSetSize; peakWorking=pm.PeakWorkingSetSize; privateBytes=pm.PrivateUsage;
}

static bool Parse(int argc,wchar_t** argv,Options& o) {
  auto need=[&](int& i,std::wstring& v){if(++i>=argc)return false;v=argv[i];return true;};
  for(int i=1;i<argc;++i) { std::wstring a=argv[i];
    if(a==L"--exe") { if(!need(i,o.exe)) return false; }
    else if(a==L"--app") { if(!need(i,o.app)) return false; }
    else if(a==L"--version") { if(!need(i,o.version)) return false; }
    else if(a==L"--edition") { if(!need(i,o.edition)) return false; }
    else if(a==L"--benchmark") { if(!need(i,o.benchmark)) return false; }
    else if(a==L"--notes") { if(!need(i,o.notes)) return false; }
    else if(a==L"--args") { if(!need(i,o.args)) return false; }
    else if(a==L"--file") { if(!need(i,o.file)) return false; }
    else if(a==L"--csv") { if(!need(i,o.csv)) return false; }
    else if(a==L"--mode") { if(!need(i,o.mode)) return false; }
    else if(a==L"--launch") { if(!need(i,o.launch)) return false; }
    else if(a==L"--progid") { if(!need(i,o.progid)) return false; }
    else if(a==L"--sequence") { if(!need(i,o.sequenceFile)) return false; }
    else if(a==L"--text") { if(!need(i,o.text)) return false; }
    else if(a==L"--samples" && ++i<argc) o.samples=_wtoi(argv[i]);
    else if(a==L"--start-index" && ++i<argc) o.startIndex=_wtoi(argv[i]);
    else if(a==L"--attach-process-name") { if(!need(i,o.attachProcessName)) return false; }
    else if(a==L"--dump-dir") { if(!need(i,o.dumpDir)) return false; }
    else if(a==L"--arm-timeout-ms" && ++i<argc) o.armTimeoutMs=_wtoi(argv[i]);
    else if(a==L"--timeout-ms" && ++i<argc) o.timeoutMs=_wtoi(argv[i]);
    else if(a==L"--settle-ms" && ++i<argc) o.settleMs=_wtoi(argv[i]);
    else if(a==L"--startup-delay-ms" && ++i<argc) o.startupDelayMs=_wtoi(argv[i]);
    else if(a==L"--change-threshold" && ++i<argc) o.changeThreshold=wcstod(argv[i], nullptr);
    else if(a==L"--max-consecutive-timeouts" && ++i<argc) o.maxConsecutiveTimeouts=_wtoi(argv[i]);
    else if(a==L"--min-changed-fraction" && ++i<argc) o.minChangedFraction=wcstod(argv[i], nullptr);
    else if(a==L"--per-cell-change-threshold" && ++i<argc) o.perCellChangeThreshold=wcstod(argv[i], nullptr);
    else if(a==L"--keep-open") o.keepOpen=true;
    else if(a==L"--verbose" || a==L"--diagnostic") o.verbose=true; else return false;
  }
  if (!o.sequenceFile.empty() && fs::exists(o.sequenceFile)) {
    std::ifstream seqIn(fs::path(o.sequenceFile));
    std::string line;
    while (std::getline(seqIn, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::string upper = line;
      for (char& ch : upper) ch = char(toupper(ch));
      if (upper.find("LEFT") != std::string::npos) o.sequenceKeys.push_back(VK_LEFT);
      else if (upper.find("RIGHT") != std::string::npos) o.sequenceKeys.push_back(VK_RIGHT);
    }
  }
  if (o.mode == L"manual-trigger") return !o.attachProcessName.empty() && !o.csv.empty();
  return (!o.exe.empty() || o.launch == L"appx") && !o.csv.empty() && o.samples > 0;
}

int wmain(int argc,wchar_t** argv) {
  // Without this, std::wcout to a redirected (non-console) stdout handle can silently fail on
  // Windows - the first unrepresentable character sets badbit and every later << becomes a no-op,
  // which is exactly what happened debugging manual-trigger mode's diagnostic prints.
  _setmode(_fileno(stdout), _O_U8TEXT);
  _setmode(_fileno(stderr), _O_U8TEXT);
  Options o; if(!Parse(argc,argv,o)) { std::wcerr<<L"Usage: VetroGuiBench --exe PATH --csv PATH [--file PATH] [--args TEXT] [--mode navigate|type|cold-open|manual-trigger] [--text TEST] [--samples N] [--timeout-ms N] [--max-consecutive-timeouts N] [--verbose]\n"; return 2; }

  if (o.mode == L"manual-trigger") {
    // Attaches to an already-running window (the user has driven the app to the state we need to
    // time from) instead of launching. Timed from a real global mouse-down, not app automation, so
    // this never sends SendInput/clicks itself - see the ImageGlass diagnostic that motivated it.
    HWND w = FindWindowByProcessName(o.attachProcessName);
    if (!w) { std::wcerr<<L"No visible top-level window found for process name containing '"<<o.attachProcessName<<L"'\n"; return 4; }
    DWORD pid=0; GetWindowThreadProcessId(w, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    DesktopCapture cap; std::wstring error;
    if (!cap.Init(w, error)) { std::wcerr<<L"Desktop Duplication init: "<<error<<L"\n"; return 5; }
    if (!cap.IsDxgi()) { std::wcerr<<L"manual-trigger requires the DXGI backend (PrintWindow fallback can't produce color dumps)\n"; return 5; }
    if (!o.dumpDir.empty()) { std::error_code ec; fs::create_directories(fs::path(o.dumpDir), ec); }
    wchar_t cls[128]{}; GetClassNameW(w, cls, 128);
    wchar_t title[256]{}; GetWindowTextW(w, title, 256);
    std::wcout<<L"attached pid="<<pid<<L" hwnd="<<uintptr_t(w)<<L" class="<<cls<<L" title='"<<title<<L"'\n"<<std::flush;

    std::optional<Frame> armedBaseline; for(int i=0;i<20&&!armedBaseline;++i) armedBaseline=cap.Capture(w,150,error);
    if (!o.dumpDir.empty()) {
      auto cf = cap.CaptureColor(w, 150, error);
      if (cf) SaveBmp(o.dumpDir + L"\\00_armed_before_click.bmp", *cf);
    }
    std::wcout<<L"ARMED. Waiting up to "<<o.armTimeoutMs<<L"ms for a real mouse click (global low-level hook)...\n"<<std::flush;

    uint64_t clickQpc = 0;
    if (!WaitForGlobalMouseClick(o.armTimeoutMs, clickQpc)) {
      std::wcerr<<L"No click detected within the arm timeout.\n";
      if (process) CloseHandle(process);
      return 8;
    }
    LARGE_INTEGER fqm{}; QueryPerformanceFrequency(&fqm);
    std::wcout<<L"CLICK_DETECTED t0_qpc="<<clickQpc<<L"\n"<<std::flush;

    std::optional<Frame> current = armedBaseline;
    { auto f=cap.Capture(w,100,error); if (f) current=f; }
    if (!o.dumpDir.empty()) {
      auto cf = cap.CaptureColor(w, 100, error);
      if (cf) SaveBmp(o.dumpDir + L"\\01_immediately_after_click.bmp", *cf);
    }

    struct Event { double atMs; double delta; double frac; std::wstring file; };
    std::vector<Event> events;
    const uint64_t deadline = clickQpc + uint64_t((double(o.timeoutMs)*fqm.QuadPart)/1000.0);
    uint64_t lastEventQpc = clickQpc;
    int frames = 0;
    while (QpcNow() < deadline && events.size() < 30) {
      auto f = cap.Capture(w, 80, error); uint64_t now = QpcNow();
      if (f) {
        ++frames;
        double d = Difference(*current, *f);
        double frac = ChangedFraction(*current, *f, uint8_t(o.perCellChangeThreshold));
        if (d >= o.changeThreshold && frac >= o.minChangedFraction) {
          double atMs = QpcMs(clickQpc, now, uint64_t(fqm.QuadPart));
          std::wstring file;
          if (!o.dumpDir.empty()) {
            wchar_t buf[64]; swprintf_s(buf, L"\\%02d_event_%04dms.bmp", int(events.size())+2, int(atMs));
            file = o.dumpDir + buf;
            std::wstring cerr2; auto cf = cap.CaptureColor(w, 20, cerr2);
            if (cf) SaveBmp(file, *cf);
          }
          events.push_back({atMs, d, frac, file});
          current = f;
          lastEventQpc = now;
          std::wcout<<L"  event "<<events.size()<<L": +"<<atMs<<L"ms delta="<<d<<L" frac="<<frac<<(file.empty()?L"":L" saved="+file)<<L"\n"<<std::flush;
        }
      }
      if (!events.empty() && QpcMs(lastEventQpc, now, uint64_t(fqm.QuadPart)) >= o.settleMs) break;
    }
    uint64_t end = QpcNow();
    double stableMs = QpcMs(clickQpc, end, uint64_t(fqm.QuadPart));
    if (!o.dumpDir.empty()) {
      auto cf = cap.CaptureColor(w, 150, error);
      if (cf) SaveBmp(o.dumpDir + L"\\99_final_stable.bmp", *cf);
    }
    std::wstring status = events.empty() ? L"NO_CHANGE_DETECTED" : L"EVENTS_CAPTURED";
    std::wcout<<L"manual-trigger status="<<status<<L" events="<<events.size()<<L" stable_ms="<<stableMs<<L" frames="<<frames<<L"\n";
    std::wcout<<L"NOTE: the LAST event above is the best-guess 'first visible photo' candidate, but this"
              <<L" is NOT confirmed automatically - open the saved BMP frames and verify visually before"
              <<L" trusting any single number here.\n";

    if (fs::path(o.csv).has_parent_path()) { std::error_code ec; fs::create_directories(fs::path(o.csv).parent_path(), ec); }
    bool header = !fs::exists(o.csv);
    std::ofstream out(fs::path(o.csv), std::ios::app);
    if (out) {
      if (header) out<<"timestamp,app,version,edition,benchmark,measurement_mode,fixture,sample,start_index,input_key,capture_backend,latency_first_change_ms,latency_stable_ms,working_set_mb,peak_working_set_mb,private_bytes_mb,peak_private_bytes_mb,cpu_percent,gpu_memory_mb,result,notes\n";
      std::wstring app = o.app.empty()?L"unknown":o.app;
      SIZE_T ws=0,peakWs=0,priv=0; uint64_t cpu=0;
      if (process) { ProcessStats(process,cpu,ws,peakWs,priv); }
      std::wstring notesW = L"manual click-to-photo timeline (best-guess = last event, NOT auto-confirmed): ";
      for (size_t i=0;i<events.size();++i) { notesW += L"event"+std::to_wstring(i+1)+L"=+"+std::to_wstring(events[i].atMs)+L"ms(delta="+std::to_wstring(events[i].delta)+L",frac="+std::to_wstring(events[i].frac)+L") "; }
      double lastMs = events.empty() ? -1.0 : events.back().atMs;
      out<<Narrow(NowIso())<<','<<Narrow(CsvEscape(app))<<','<<Narrow(CsvEscape(o.version))<<','<<Narrow(CsvEscape(o.edition))<<','<<Narrow(CsvEscape(o.benchmark))<<','<<Narrow(CsvEscape(o.mode))<<','<<Narrow(CsvEscape(fs::path(o.file).filename().wstring()))<<",1,N/A,\"click\",\"DXGI\","<<std::fixed<<std::setprecision(3)<<lastMs<<','<<stableMs<<','<<(double(ws)/1048576.0)<<','<<(double(peakWs)/1048576.0)<<','<<(double(priv)/1048576.0)<<','<<(double(priv)/1048576.0)<<",0,,"<<Narrow(status)<<','<<Narrow(CsvEscape(notesW))<<'\n';
    }
    if (process) CloseHandle(process);
    return 0; // window intentionally left open - never closed in manual-trigger mode.
  }

  bool isColdOpen = (o.mode == L"cold-open");
  LARGE_INTEGER fq{}; QueryPerformanceFrequency(&fq);
  const uint64_t t0 = QpcNow(); // Captured before ANY launch work - this is what cold-open times from.

  std::wstring args=o.args; size_t mark=args.find(L"{file}"); if(mark!=std::wstring::npos) args.replace(mark,6,Quote(o.file)); else if(!o.file.empty()) args+=(args.empty()?L"":L" ")+Quote(o.file);
  HANDLE process = nullptr; DWORD pid = 0; HWND w = nullptr;

  if (o.launch == L"appx") {
    std::wstring progId = o.progid;
    if (progId.empty() || progId == L"auto") {
      progId = ResolvePhotosProgId();
    }
    auto before = EnumVisibleWindows();
    SHELLEXECUTEINFOW sei{}; sei.cbSize=sizeof(sei); sei.fMask=SEE_MASK_CLASSNAME | SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open"; sei.lpClass = progId.c_str(); sei.lpFile = o.file.c_str(); sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
      std::wcerr << L"ShellExecuteExW (appx) failed: " << GetLastError() << L"\n";
      return 3;
    }
    w = WaitForAppxWindow(before, o.timeoutMs, pid);
    if (!w) {
      std::wcerr << L"No visible top-level WinUI HWND for AppX " << progId << L"\n";
      return 4;
    }
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
  } else if (o.launch == L"shell") {
    SHELLEXECUTEINFOW sei{}; sei.cbSize=sizeof(sei); sei.fMask=SEE_MASK_NOCLOSEPROCESS; sei.lpFile=o.exe.c_str();
    sei.lpParameters=args.empty()?nullptr:args.c_str(); sei.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&sei) || !sei.hProcess) { std::wcerr<<L"ShellExecuteEx failed "<<GetLastError()<<L"\n"; return 3; }
    process=sei.hProcess; pid=GetProcessId(process);
    w=WaitForWindow(pid,o.timeoutMs);
  } else {
    HDESK inputDesk = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
    if (inputDesk) SetThreadDesktop(inputDesk);
    std::wstring cmd=Quote(o.exe)+(args.empty()?L"":L" "+args); std::vector<wchar_t> mutableCmd(cmd.begin(),cmd.end()); mutableCmd.push_back(L'\0');
    STARTUPINFOW si{}; si.cb=sizeof(si); si.lpDesktop = (LPWSTR)L"winsta0\\Default"; PROCESS_INFORMATION pi{};
    if(!CreateProcessW(o.exe.c_str(),mutableCmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)) { std::wcerr<<L"CreateProcess failed "<<GetLastError()<<L"\n"; return 3; }
    CloseHandle(pi.hThread); process=pi.hProcess; pid=pi.dwProcessId;
    w=WaitForWindow(pid,o.timeoutMs);
  }

  if(!w) { std::wcerr<<L"No visible top-level HWND for launched PID "<<pid<<L"\n"; if(process) CloseHandle(process); return 4; }
  HDESK inputDesk = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
  if (inputDesk) {
    SetThreadDesktop(inputDesk);
  }
  FocusForInput(w);
  // Cold-open must not touch the window beyond bringing it to front - a click
  // or a long "settle" sleep here would eat directly into the open-latency
  // window we're trying to measure, or (worse) trigger an unrelated UI action
  // mid-decode. Navigate/type mode keeps the existing click + settle delay.
  if (!isColdOpen) {
    ClickWindowCenter(w);
    if (o.startupDelayMs > 0) Sleep(o.startupDelayMs); else Sleep(350);
  } else {
    Sleep(30); // just long enough for the window's client rect to be valid for ClientToScreen.
  }

  DesktopCapture cap; std::wstring error; if(!cap.Init(w,error)) { std::wcerr<<L"Desktop Duplication init: "<<error<<L"\n"; if(process) CloseHandle(process); return 5; }
  std::optional<Frame> baseline; for(int i=0;i<30&&!baseline;++i) baseline=cap.Capture(w,150,error);
  if(!baseline) { std::wcerr<<L"Could not capture baseline: "<<error<<L"\n"; if(process) CloseHandle(process); return 6; }
  std::wcout<<L"capture_client_screen_rect="<<baseline->source.left<<L","<<baseline->source.top<<L","<<baseline->source.right<<L","<<baseline->source.bottom<<L" backend="<<(cap.IsDxgi()?L"DXGI":L"PrintWindow")<<L"\n";

  if (fs::path(o.csv).has_parent_path()) {
    std::error_code ec;
    fs::create_directories(fs::path(o.csv).parent_path(), ec);
  }
  bool header= !fs::exists(o.csv); std::ofstream out(fs::path(o.csv),std::ios::app); if(!out) { std::wcerr<<L"Cannot open CSV\n"; if(process) CloseHandle(process); return 7; }
  if(header) out<<"timestamp,app,version,edition,benchmark,measurement_mode,fixture,sample,start_index,input_key,capture_backend,latency_first_change_ms,latency_stable_ms,working_set_mb,peak_working_set_mb,private_bytes_mb,peak_private_bytes_mb,cpu_percent,gpu_memory_mb,result,notes\n";

  std::wstring app=o.app.empty()?fs::path(o.exe).filename().wstring():o.app;
  std::wstring fixture=o.file.empty()?L"":fs::path(o.file).filename().wstring();
  std::wstring backendStr = cap.IsDxgi() ? L"DXGI" : L"PrintWindow";
  std::wstring startIndexStr = o.startIndex >= 0 ? std::to_wstring(o.startIndex) : L"N/A";
  SIZE_T runningPeakPrivate = 0;

  auto writeRow = [&](int sample, const std::wstring& inputKeyStr, double changeMs, double stableMs,
                       SIZE_T ws, SIZE_T peakWs, SIZE_T priv, SIZE_T peakPriv, double cpuPercent,
                       const std::wstring& status, double delta, int frames) {
    std::wstring note = (o.notes.empty()?L"":o.notes+L"; ") + L"pid=" + std::to_wstring(pid) + L" hwnd=" + std::to_wstring(uintptr_t(w)) + L" delta=" + std::to_wstring(delta) + L" frames=" + std::to_wstring(frames);
    out<<Narrow(NowIso())<<','<<Narrow(CsvEscape(app))<<','<<Narrow(CsvEscape(o.version))<<','<<Narrow(CsvEscape(o.edition))<<','<<Narrow(CsvEscape(o.benchmark))<<','<<Narrow(CsvEscape(o.mode))<<','<<Narrow(CsvEscape(fixture))<<','<<sample<<','<<Narrow(startIndexStr)<<','<<Narrow(CsvEscape(inputKeyStr))<<','<<Narrow(CsvEscape(backendStr))<<','<<std::fixed<<std::setprecision(3)<<changeMs<<','<<stableMs<<','<<(double(ws)/1048576.0)<<','<<(double(peakWs)/1048576.0)<<','<<(double(priv)/1048576.0)<<','<<(double(peakPriv)/1048576.0)<<','<<cpuPercent<<",,"<<Narrow(status)<<','<<Narrow(CsvEscape(note))<<'\n'; out.flush();
  };

  if (isColdOpen) {
    // No SendInput at all: baseline was captured as early as possible after
    // the window appeared (the "not yet painted" state), and we simply watch
    // for the fixture to materialize. Timed from t0 (before launch), not
    // from baseline capture, so process startup + window discovery time is
    // correctly included in first_visible_ms.
    uint64_t cpu0=0; SIZE_T ws0=0,peakWs0=0,priv0=0;
    if (process) { ProcessStats(process,cpu0,ws0,peakWs0,priv0); runningPeakPrivate = priv0; }
    bool changed=false; double delta=0; uint64_t changedAt=0,lastVisualChange=0; int frames=0; std::wstring status=L"TIMEOUT";
    const uint64_t deadline=t0+uint64_t((double(o.timeoutMs)*fq.QuadPart)/1000.0);
    while(QpcNow()<deadline) {
      auto f=cap.Capture(w,80,error); uint64_t now=QpcNow();
      if(f) {
        ++frames;
        double d=Difference(*baseline,*f);
        double frac=ChangedFraction(*baseline,*f,uint8_t(o.perCellChangeThreshold));
        bool passes = d>=o.changeThreshold && frac>=o.minChangedFraction;
        if(!changed && passes) { changed=true; changedAt=now; delta=d; lastVisualChange=now; }
        if(changed && passes) lastVisualChange=now;
        if(changed) baseline=*f;
      }
      if (process) { uint64_t cpuNow=0; SIZE_T wsNow=0,peakWsNow=0,privNow=0; ProcessStats(process,cpuNow,wsNow,peakWsNow,privNow); if (privNow>runningPeakPrivate) runningPeakPrivate=privNow; }
      if(changed && QpcMs(lastVisualChange,now,uint64_t(fq.QuadPart))>=o.settleMs) { status=L"CHANGED_STABLE"; break; }
    }
    uint64_t end=QpcNow();
    uint64_t cpu1=0; SIZE_T ws1=0,peakWs1=0,priv1=0;
    if (process) { ProcessStats(process,cpu1,ws1,peakWs1,priv1); if (priv1>runningPeakPrivate) runningPeakPrivate=priv1; }
    double changeMs=changed?QpcMs(t0,changedAt,uint64_t(fq.QuadPart)):-1.0, stableMs=changed?QpcMs(t0,end,uint64_t(fq.QuadPart)):-1.0;
    double cpuMs=double(cpu1-cpu0)/10000.0;
    double elapsedMs=QpcMs(t0,end,uint64_t(fq.QuadPart));
    double cpuPercent=elapsedMs>0 ? (cpuMs/elapsedMs)*100.0 : 0.0;
    writeRow(1, L"N/A", changeMs, stableMs, ws1, peakWs1, priv1, runningPeakPrivate, cpuPercent, status, delta, frames);
    std::wcout<<L"cold-open status="<<status<<L" first_visible_ms="<<changeMs<<L" stable_ms="<<stableMs<<L" frames="<<frames<<L"\n";
    if (o.verbose) {
      std::wcout<<L"  [diag] backend="<<backendStr<<L" pid="<<pid<<L" hwnd="<<uintptr_t(w)
                <<L" delta="<<delta<<L" frames="<<frames<<L" elapsed_ms="<<elapsedMs
                <<L" peak_ws_mb="<<(double(peakWs1)/1048576.0)<<L" peak_priv_mb="<<(double(runningPeakPrivate)/1048576.0)<<L"\n";
    }
  } else {
  int consecutiveTimeouts = 0;
  for(int sample=1;sample<=o.samples;++sample) {
    FocusForInput(w);
    SendMessageW(w, WM_CANCELMODE, 0, 0);
    if (o.mode != L"type") ClickWindowCenter(w);
    Sleep(50);
    std::wstring bErr;
    auto freshBase = cap.Capture(w, 80, bErr);
    if (freshBase) baseline = freshBase;
    uint64_t cpu0=0,cpu1=0; SIZE_T ws0=0,ws1=0,peakWs0=0,peakWs1=0,priv0=0,priv1=0;
    if (process) { ProcessStats(process,cpu0,ws0,peakWs0,priv0); if (priv0>runningPeakPrivate) runningPeakPrivate=priv0; }
    WORD navKey = VK_RIGHT;
    if (!o.sequenceKeys.empty()) {
      size_t idx = size_t(sample - 1) % o.sequenceKeys.size();
      navKey = o.sequenceKeys[idx];
    }
    uint64_t start=QpcNow(); bool sent=(o.mode==L"type")?SendText(o.text):SendNavigation(w, navKey);
    if(!sent) { std::wcerr<<L"SendInput failed\n"; break; }
    bool changed=false; double delta=0; uint64_t changedAt=0,lastVisualChange=0; int frames=0; std::wstring status=L"TIMEOUT";
    const uint64_t deadline=start+uint64_t((double(o.timeoutMs)*fq.QuadPart)/1000.0);
    while(QpcNow()<deadline) {
      auto f=cap.Capture(w,80,error); uint64_t now=QpcNow();
      if(f) { ++frames; double d=Difference(*baseline,*f); if(!changed && d>=o.changeThreshold) { changed=true; changedAt=now; delta=d; lastVisualChange=now; }
        if(changed && d>=o.changeThreshold) lastVisualChange=now; if(changed) baseline=*f; }
      if(changed && QpcMs(lastVisualChange,now,uint64_t(fq.QuadPart))>=o.settleMs) { status=L"CHANGED_STABLE"; break; }
    }
    uint64_t end=QpcNow();
    if (process) { ProcessStats(process,cpu1,ws1,peakWs1,priv1); if (priv1>runningPeakPrivate) runningPeakPrivate=priv1; }
    double changeMs=changed?QpcMs(start,changedAt,uint64_t(fq.QuadPart)):-1.0, stableMs=changed?QpcMs(start,end,uint64_t(fq.QuadPart)):-1.0;
    double cpuMs=double(cpu1-cpu0)/10000.0;
    double elapsedMs=QpcMs(start,end,uint64_t(fq.QuadPart));
    double cpuPercent=elapsedMs>0 ? (cpuMs/elapsedMs)*100.0 : 0.0;
    std::wstring keyStr = (o.mode==L"type") ? L"type" : ((navKey==VK_LEFT) ? L"Left" : L"Right");
    writeRow(sample, keyStr, changeMs, stableMs, ws1, peakWs1, priv1, runningPeakPrivate, cpuPercent, status, delta, frames);
    std::wcout<<L"sample="<<sample<<L" status="<<status<<L" change_ms="<<changeMs<<L" stable_ms="<<stableMs<<L" frames="<<frames<<L"\n";
    if (o.verbose) {
      std::wcout<<L"  [diag] backend="<<backendStr<<L" key="<<keyStr<<L" pid="<<pid<<L" hwnd="<<uintptr_t(w)
                <<L" foreground_match="<<(GetForegroundWindow()==w?L"yes":L"no")
                <<L" delta="<<delta<<L" frames="<<frames<<L" elapsed_ms="<<elapsedMs<<L"\n";
    }
    if (status == L"TIMEOUT") {
      ++consecutiveTimeouts;
      if (consecutiveTimeouts >= o.maxConsecutiveTimeouts) {
        std::wcerr<<L"Aborting after "<<consecutiveTimeouts<<L" consecutive TIMEOUT samples ("
                  <<sample<<L"/"<<o.samples<<L" attempted). This app/fixture/mode combination is not"
                  <<L" producing detectable navigation - not retrying the remaining "
                  <<(o.samples-sample)<<L" samples. Increase --max-consecutive-timeouts to override.\n";
        break;
      }
    } else {
      consecutiveTimeouts = 0;
    }
  }
  } // isColdOpen else-branch
  if(!o.keepOpen && w) {
    PostMessageW(w,WM_CLOSE,0,0);
    if(process) {
      DWORD wr = WaitForSingleObject(process, 2000);
      if (wr == WAIT_TIMEOUT) TerminateProcess(process, 0); // Never leave an orphaned viewer running.
    }
  }
  if (process) CloseHandle(process);
  return 0;
}
