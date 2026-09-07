#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct Image { unsigned w=0,h=0; std::vector<uint8_t> pixels; std::wstring codec; double ms=0; bool hasAlpha=false; };
std::shared_ptr<Image> Decode(const std::wstring& path, std::wstring& error);
std::shared_ptr<Image> DecodeWuffs(const std::vector<uint8_t>& data);
bool Supported(const std::wstring& path);
std::wstring ExplorerSelection(HWND window);
bool ExplorerCanPreview(HWND window);
