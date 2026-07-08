#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct PluginRecord {
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::wstring category;
    std::wstring kind;
    std::wstring engine;
    std::wstring description;
    std::wstring permissions;
    std::wstring author;
    std::wstring license;
    std::wstring packageUrl;
    std::wstring homepageUrl;
    std::wstring sourceUrl;
    std::wstring addedAt;
    std::wstring createdAt;
    std::wstring updatedAt;
    std::wstring sha256;
    bool builtin = false;
    bool deletable = true;
    bool enabled = false;
    bool installed = false;
};

class PluginRegistry {
public:
    explicit PluginRegistry(std::filesystem::path appDirectory);

    std::vector<PluginRecord> LoadPlugins();
    bool SetEnabled(const std::wstring& pluginId, bool enabled);
    bool IsEnabled(const std::wstring& pluginId);
    std::wstring GetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& fallback = L"");
    bool SetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& value);
    const std::wstring& lastError() const { return lastError_; }

    static std::vector<PluginRecord> BuiltinPlugins();

private:
    std::filesystem::path appDirectory_;
    std::wstring stateKey_;
    std::wstring lastError_;
};
