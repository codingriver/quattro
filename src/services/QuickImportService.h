#pragma once

#include "Models.h"

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

    static constexpr int kMaxDepth = 5;

    std::vector<Item> Scan(const std::filesystem::path& directory, std::wstring& error) const;
    std::vector<Item> ScanStoreApps(std::wstring& error, std::stop_token stopToken = {}) const;

private:
    void ScanRoot(const std::filesystem::path& root, std::vector<Item>& items) const;
    bool TryCreateItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateShortcutItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateUrlItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateExecutableItem(const std::filesystem::path& path, Item& item) const;
};
