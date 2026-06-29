#include "WebDavRecoveryService.h"

#include "Utilities.h"
#include "WebDavCredentialService.h"

#include <windows.h>

#include <algorithm>

namespace {
constexpr const wchar_t* kSection = L"WebDavRecovery";

std::filesystem::path LocalAppDataPath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0 || copied >= buffer.size()) {
        return std::filesystem::temp_directory_path();
    }
    buffer.resize(copied);
    return buffer;
}
}

WebDavRecoveryService::WebDavRecoveryService(std::filesystem::path recoveryPath)
    : recoveryPath_(std::move(recoveryPath)) {
}

std::filesystem::path WebDavRecoveryService::DefaultRecoveryPath() {
    return LocalAppDataPath() / L"Quattro" / L"recovery" / L"webdav.ini";
}

bool WebDavRecoveryService::HasWebDavSettings(const AppConfig& config) {
    return !Trim(config.webDavUrl).empty() &&
        !Trim(config.webDavRemotePath).empty() &&
        !Trim(config.webDavUserName).empty();
}

bool WebDavRecoveryService::Load(AppConfig& config) const {
    if (!HasRecoverableSettings()) {
        return false;
    }

    AppConfig recovered = config;
    recovered.webDavEnabled = ReadBool(L"enabled", recovered.webDavEnabled);
    recovered.webDavUrl = ReadString(L"url", L"");
    recovered.webDavRemotePath = ReadString(L"remote_path", L"/Quattro/backups/");
    recovered.webDavUserName = ReadString(L"username", L"");
    recovered.webDavKeepCount = std::max(1, std::min(100, ReadInt(L"keep_count", recovered.webDavKeepCount)));

    if (!HasWebDavSettings(recovered)) {
        return false;
    }

    config.webDavEnabled = recovered.webDavEnabled;
    config.webDavUrl = recovered.webDavUrl;
    config.webDavRemotePath = recovered.webDavRemotePath;
    config.webDavUserName = recovered.webDavUserName;
    config.webDavKeepCount = recovered.webDavKeepCount;
    return true;
}

bool WebDavRecoveryService::Save(const AppConfig& config, std::wstring& error) const {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(recoveryPath_.parent_path(), ec);
    if (ec) {
        const std::string message = ec.message();
        error = L"创建 WebDAV 本机恢复目录失败: " + std::wstring(message.begin(), message.end());
        return false;
    }

    WriteInt(L"version", 1);
    WriteInt(L"enabled", config.webDavEnabled ? 1 : 0);
    WriteString(L"url", config.webDavUrl);
    WriteString(L"remote_path", Trim(config.webDavRemotePath).empty() ? L"/Quattro/backups/" : config.webDavRemotePath);
    WriteString(L"username", config.webDavUserName);
    WriteInt(L"keep_count", std::max(1, std::min(100, config.webDavKeepCount)));
    WriteString(L"credential_target", WebDavCredentialService::TargetName(config));
    return true;
}

bool WebDavRecoveryService::HasRecoverableSettings() const {
    return FileExists(recoveryPath_) &&
        !Trim(ReadString(L"url", L"")).empty() &&
        !Trim(ReadString(L"remote_path", L"")).empty() &&
        !Trim(ReadString(L"username", L"")).empty();
}

int WebDavRecoveryService::ReadInt(const wchar_t* key, int fallback) const {
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, recoveryPath_.c_str()));
}

bool WebDavRecoveryService::ReadBool(const wchar_t* key, bool fallback) const {
    return ReadInt(key, fallback ? 1 : 0) != 0;
}

std::wstring WebDavRecoveryService::ReadString(const wchar_t* key, const wchar_t* fallback) const {
    std::wstring buffer(256, L'\0');
    DWORD copied = GetPrivateProfileStringW(kSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), recoveryPath_.c_str());
    while (copied == buffer.size() - 1) {
        buffer.resize(buffer.size() * 2);
        copied = GetPrivateProfileStringW(kSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), recoveryPath_.c_str());
    }
    buffer.resize(copied);
    return buffer;
}

void WebDavRecoveryService::WriteInt(const wchar_t* key, int value) const {
    WriteString(key, std::to_wstring(value));
}

void WebDavRecoveryService::WriteString(const wchar_t* key, const std::wstring& value) const {
    WritePrivateProfileStringW(kSection, key, value.empty() ? nullptr : value.c_str(), recoveryPath_.c_str());
}
