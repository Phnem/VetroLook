// Deliberately minimal target for validating VetroGuiBench without a viewer.
// A real keyboard Right event changes a large part of the client surface.
#include <windows.h>

static bool alternate = false;

static LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_KEYDOWN:
      if (wp == VK_RIGHT) { alternate = !alternate; InvalidateRect(w, nullptr, FALSE); return 0; }
      break;
    case WM_CHAR:
      alternate = !alternate; InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps{}; HDC dc = BeginPaint(w, &ps); RECT r{}; GetClientRect(w, &r);
      FillRect(dc, &r, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
      RECT block{r.right / 6, r.bottom / 6, r.right * 5 / 6, r.bottom * 5 / 6};
      HBRUSH brush = CreateSolidBrush(alternate ? RGB(15, 105, 210) : RGB(232, 105, 20));
      FillRect(dc, &block, brush); DeleteObject(brush);
      SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(20, 20, 20));
      DrawTextW(dc, alternate ? L"changed" : L"baseline", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      EndPaint(w, &ps); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int) {
  const wchar_t* klass = L"VetroGuiBenchTestTarget";
  WNDCLASSW wc{}; wc.hInstance = h; wc.lpszClassName = klass; wc.lpfnWndProc = WindowProc;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, klass, L"VetroGuiBench Test Target", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
    140, 140, 960, 720, nullptr, nullptr, h, nullptr);
  ShowWindow(w, SW_SHOW); UpdateWindow(w);
  MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  return 0;
}
