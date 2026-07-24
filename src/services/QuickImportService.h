#pragma once

#include "Models.h"
#include "ScanExecutionService.h"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

class QuickImportService {
public:
    enum class Source {
        Directory,
        StoreApps,
    };

    struct Item {
        Link link;
        std::filesystem::path sourcePath;
        std::wstring sourceName;
        std::wstring status;
        std::wstring stableKey;
        bool selected = true;
    };

    struct ScanRequest {
        Source source = Source::Directory;
        std::filesystem::path directory;
    };

    struct ScanResult {
        std::vector<Item> items;
        std::wstring error;
        bool cancelled = false;
    };

    std::vector<Item> Scan(const std::filesystem::path& directory, std::wstring& error) const;
    std::vector<Item> ScanStoreApps(std::wstring& error, std::stop_token stopToken = {}) const;
    std::shared_ptr<ScanTaskHandle> StartScan(const ScanRequest& request) const;

private:
    ScanResult RunScan(const ScanRequest& request, ScanTaskContext& context,
        std::stop_token externalStopToken = {}) const;
    void EnumerateRoot(const std::filesystem::path& root, std::vector<std::filesystem::path>& candidates,
        ScanTaskContext& context) const;
    ScanResult ScanStoreAppsCore(ScanTaskContext& context, std::stop_token externalStopToken) const;
    bool TryCreateItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateShortcutItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateUrlItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateExecutableItem(const std::filesystem::path& path, Item& item) const;
};
