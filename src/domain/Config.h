#pragma once

#include "Models.h"

#include <filesystem>

class ConfigService {
public:
    explicit ConfigService(std::filesystem::path configPath);

    AppConfig Load() const;
    AppConfig LoadForSchemaUpgrade(int targetVersion, bool& compatible) const;
    bool UpgradeToSchemaVersion(int targetVersion) const;
    void Save(const AppConfig& config) const;
    void SaveWindowState(const AppConfig& config) const;
    const std::filesystem::path& path() const { return configPath_; }

private:
    bool HasKey(const wchar_t* key) const;
    bool TryReadInt(const wchar_t* key, int& value) const;
    bool TryReadBool(const wchar_t* key, bool& value) const;
    bool TryReadString(const wchar_t* key, std::wstring& value) const;
    int ReadInt(const wchar_t* key, int fallback) const;
    bool ReadBool(const wchar_t* key, bool fallback) const;
    std::wstring ReadString(const wchar_t* key, const wchar_t* fallback) const;
    void WriteInt(const wchar_t* key, int value) const;
    void WriteString(const wchar_t* key, const std::wstring& value) const;
    std::filesystem::path WebDavConfigPath() const;
    std::filesystem::path HttpConfigPath() const;
    int ReadExternalInt(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, int fallback) const;
    bool ReadExternalBool(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, bool fallback) const;
    std::wstring ReadExternalString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const;
    void WriteExternalInt(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, int value) const;
    void WriteExternalString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const std::wstring& value) const;
    void SaveExternalNetworkSettings(const AppConfig& config) const;
    void DeleteLegacyNetworkSettings() const;

    std::filesystem::path configPath_;
};
