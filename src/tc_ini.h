#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Total Commander INI files are user-owned documents, not disposable config
// blobs.  These helpers patch one key while retaining the original encoding,
// BOM, line endings, comments, ordering and unrelated duplicate sections.
std::optional<std::wstring> TcIniValue(const std::vector<uint8_t>& bytes,
                                       const std::wstring& section,
                                       const std::wstring& key);
bool TcIniSet(std::vector<uint8_t>& bytes,const std::wstring& section,
              const std::wstring& key,
              const std::optional<std::wstring>& value,
              std::wstring& error);
bool TcIniReadFile(const std::wstring& path,std::vector<uint8_t>& bytes,
                   std::wstring& error);
bool TcIniWriteFileAtomic(const std::wstring& path,
                          const std::vector<uint8_t>& bytes,
                          std::wstring& error);
