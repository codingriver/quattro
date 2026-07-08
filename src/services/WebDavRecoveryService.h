#pragma once

#include "Models.h"

#include <filesystem>
#include <string>

class WebDavRecoveryService {
public:
    explicit WebDavRecoveryService(std::filesystem::path recoveryPath = DefaultRecoveryPath());

    bool Load(AppConfig& config) const;
    bool Save(const AppConfig& config, std::wstring& error) const;
    bool HasRecoverableSettings() const;

    const std::filesystem::path& path() const { return recoveryPath_; }

    static std::filesystem::path DefaultRecoveryPath();
    static bool HasWebDavSettings(const AppConfig& config);

private:
    int ReadInt(const wchar_t* key, int fallback) const;
    bool ReadBool(const wchar_t* key, bool fallback) const;
    std::wstring ReadString(const wchar_t* key, const wchar_t* fallback) const;
    void WriteInt(const wchar_t* key, int value) const;
    void WriteString(const wchar_t* key, const std::wstring& value) const;

    std::filesystem::path recoveryPath_;
};
