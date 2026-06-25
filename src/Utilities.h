#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

std::filesystem::path GetModuleDirectory();
std::wstring Trim(const std::wstring& value);
std::wstring ToLower(std::wstring value);
std::wstring FormatLastError(DWORD error);
std::wstring LoadUtf8File(const std::filesystem::path& path);
bool FileExists(const std::filesystem::path& path);
bool DirectoryExists(const std::filesystem::path& path);
std::wstring QuoteForCommandLine(const std::wstring& value);
std::wstring ReplaceAll(std::wstring value, const std::wstring& from, const std::wstring& to);
std::uint32_t StablePathHash(const std::wstring& value);
std::wstring Hex8(std::uint32_t value);
bool HasUrlScheme(const std::wstring& value);
std::wstring NormalizeUrl(std::wstring value);
std::wstring ExpandEnvironmentStringsSafe(const std::wstring& value);
std::optional<int> ParseInt(const std::wstring& value);
