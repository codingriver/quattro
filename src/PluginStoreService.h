#pragma once

#include "PluginRegistry.h"
#include "Storage.h"

#include <filesystem>
#include <string>
#include <vector>

struct StoreLinkDefinition {
    std::wstring name;
    std::wstring path;
    std::wstring parameter;
    std::wstring workDir;
    int type = 0;
};

struct StoreTagDefinition {
    std::wstring name;
    int type = 0;
    std::wstring content;
    std::vector<StoreLinkDefinition> links;
};

struct StoreGroupDefinition {
    std::wstring name;
    std::vector<StoreTagDefinition> tags;
};

struct StoreFileDefinition {
    std::wstring from;
    std::wstring to;
};

struct StorePluginDefinition {
    PluginRecord record;
    std::vector<StoreGroupDefinition> groups;
    std::vector<StoreFileDefinition> files;
    std::filesystem::path packageDirectory;
};

class PluginStoreService {
public:
    PluginStoreService(std::filesystem::path appDirectory, std::wstring storeUrl);

    bool Refresh(PluginRegistry& registry);
    std::vector<StorePluginDefinition> plugins() const { return plugins_; }
    bool Install(const std::wstring& pluginId, PluginRegistry& registry, StorageService& storage);
    bool Remove(const std::wstring& pluginId, PluginRegistry& registry, StorageService& storage);
    const std::wstring& lastError() const { return lastError_; }

private:
    bool LoadIndexText(std::wstring& text, std::filesystem::path& baseDirectory);
    bool LoadPackageText(const StorePluginDefinition& plugin, std::wstring& text, std::filesystem::path& packageDirectory);
    bool ParseIndex(const std::wstring& text, const std::filesystem::path& baseDirectory);
    bool ParsePluginObject(const struct JsonValue& object, const std::filesystem::path& baseDirectory, StorePluginDefinition& plugin);
    bool InstallDefinition(const StorePluginDefinition& plugin, PluginRegistry& registry, StorageService& storage);
    bool CopyDeclaredFile(const StorePluginDefinition& plugin, const StoreFileDefinition& file, PluginRegistry& registry);
    bool ResolveSafeRelativePath(const std::wstring& relative, std::filesystem::path& path) const;
    bool ReadTextFromUrl(const std::wstring& url, std::wstring& text);
    bool ReadTextFromFile(const std::filesystem::path& path, std::wstring& text);
    bool ReadBytesFromUrl(const std::wstring& url, std::vector<std::uint8_t>& bytes);
    bool ReadBytesFromFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes);
    std::wstring ResolvePackageUrl(const std::wstring& packageUrl) const;
    bool ValidatePackageSafety(const StorePluginDefinition& plugin);

    std::filesystem::path appDirectory_;
    std::wstring storeUrl_;
    std::filesystem::path storeBaseDirectory_;
    std::wstring lastError_;
    std::vector<StorePluginDefinition> plugins_;
};
