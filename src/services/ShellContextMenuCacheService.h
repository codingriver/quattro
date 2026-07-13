#pragma once

#include "ShellItemService.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

struct ShellContextMenuCachedIcon {
    int width = 0;
    int height = 0;
    int quality = 0;
    std::vector<std::uint32_t> pixels;
};

class ShellContextMenuCacheService {
public:
    explicit ShellContextMenuCacheService(std::filesystem::path appDirectory);

    std::vector<ShellContextMenuItem> ItemsFor(
        const Link& link,
        const ShellContextMenuTrackingOptions& tracking) const;
    void Update(
        const Link& link,
        const ShellContextMenuSnapshot& snapshot,
        const ShellContextMenuTrackingOptions& tracking);
    bool ClearIconPool();
    void Remove(int linkId);
    void RemoveProvider(const std::wstring& providerId);

private:
    struct Entry {
        std::wstring target;
        std::vector<ShellContextMenuItem> items;
    };

    static std::wstring TargetKey(const Link& link);
    static std::wstring IconKey(
        const ShellContextMenuItem& item,
        const std::vector<std::wstring>& path);
    void IngestIcons(
        std::vector<ShellContextMenuItem>& items,
        std::vector<std::wstring>& path);
    void ApplyIcons(
        std::vector<ShellContextMenuItem>& items,
        std::vector<std::wstring>& path) const;
    static void StripIcons(std::vector<ShellContextMenuItem>& items);
    void Load();
    bool Save() const;

    std::filesystem::path cachePath_;
    std::unordered_map<int, Entry> entries_;
    std::unordered_map<std::wstring, ShellContextMenuCachedIcon> iconPool_;
};
