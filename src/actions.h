#pragma once
#include "image.h"
Image RotatedPixels(const Image& image, int degrees, bool whiteBackground=false);
Image CropPixels(const Image& image, int x, int y, unsigned w, unsigned h);
bool WriteImage(const Image& image, int degrees, const std::wstring& path, std::wstring& error);
bool RecycleImage(HWND owner, const std::wstring& path, std::wstring& error);
void PrintImage(HWND owner, const Image& image, int degrees);
void ShareImage(HWND owner, const std::wstring& path);
void ShutdownSharing();
bool CopyToClipboard(HWND owner, const Image& image, const std::wstring& path);
bool ViewerRegistered();
bool RegisterAsViewer(std::wstring& error);
// Rewrites an existing registration that still points at a path this
// executable no longer lives at (renamed or moved build). Does nothing when
// nothing was ever registered.
void RepairRegistrationIfStale();
// Removes every registry trace of the app's shell integration (both the
// current and legacy identity) so a following RegisterAsViewer starts from
// nothing, instead of layering on top of however many prior registration
// attempts happen to still be sitting in the registry.
void UnregisterViewer();
void OpenDefaultAppsPage();
bool AutostartEnabled();
bool SetAutostart(bool on);
