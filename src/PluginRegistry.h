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
    std::wstring sha256;
    bool builtin = false;
    bool deletable = true;
    bool enabled = false;
    bool installed = false;
};

struct PluginContribution {
    std::wstring pluginId;
    std::wstring objectType;
    int objectId = 0;
    std::wstring objectPath;
};

class PluginRegistry {
public:
    explicit PluginRegistry(std::filesystem::path appDirectory);

    bool Initialize();
    std::vector<PluginRecord> LoadPlugins();
    bool UpsertStorePlugin(const PluginRecord& plugin);
    bool MarkInstalled(const PluginRecord& plugin, const std::wstring& installPath);
    bool RemovePlugin(const std::wstring& pluginId);
    bool SetEnabled(const std::wstring& pluginId, bool enabled);
    bool IsEnabled(const std::wstring& pluginId);
    bool RecordContribution(const std::wstring& pluginId, const std::wstring& objectType, int objectId, const std::wstring& objectPath = L"");
    std::vector<PluginContribution> LoadContributions(const std::wstring& pluginId);
    bool ClearContributions(const std::wstring& pluginId);
    std::wstring GetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& fallback = L"");
    bool SetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& value);
    const std::wstring& lastError() const { return lastError_; }

    static std::vector<PluginRecord> BuiltinPlugins();

private:
    bool EnsureSchema(void* db);
    bool UpsertBuiltinPlugins(void* db);

    std::filesystem::path appDirectory_;
    std::wstring lastError_;
};
