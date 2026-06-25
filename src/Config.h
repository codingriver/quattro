#pragma once

#include "Models.h"

#include <filesystem>

class ConfigService {
public:
    explicit ConfigService(std::filesystem::path configPath);

    AppConfig Load() const;
    void Save(const AppConfig& config) const;
    void SaveWindowState(const AppConfig& config) const;
    const std::filesystem::path& path() const { return configPath_; }

private:
    int ReadInt(const wchar_t* key, int fallback) const;
    bool ReadBool(const wchar_t* key, bool fallback) const;
    std::wstring ReadString(const wchar_t* key, const wchar_t* fallback) const;
    void WriteInt(const wchar_t* key, int value) const;
    void WriteString(const wchar_t* key, const std::wstring& value) const;

    std::filesystem::path configPath_;
};
