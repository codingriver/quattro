#pragma once

#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

class QuickImportService {
public:
    enum class Source {
        Directory,
        Desktop,
        StartMenu,
    };

    struct Item {
        Link link;
        std::filesystem::path sourcePath;
        std::wstring sourceName;
        std::wstring status;
        bool duplicate = false;
        bool selected = true;
    };

    static constexpr int kMaxDepth = 5;

    std::vector<Item> Scan(Source source, const std::filesystem::path& directory, const std::vector<Link>& existingLinks, std::wstring& error) const;

private:
    std::vector<std::filesystem::path> RootsFor(Source source, const std::filesystem::path& directory) const;
    void ScanRoot(const std::filesystem::path& root, std::vector<Item>& items) const;
    bool TryCreateItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateShortcutItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateUrlItem(const std::filesystem::path& path, Item& item) const;
    bool TryCreateExecutableItem(const std::filesystem::path& path, Item& item) const;
    void MarkDuplicates(std::vector<Item>& items, const std::vector<Link>& existingLinks) const;
};
