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
    ShellContextMenuCacheService();
    explicit ShellContextMenuCacheService(std::filesystem::path storageDirectory);

    std::vector<ShellContextMenuItem> ItemsFor(
        const Link& link,
        const ShellContextMenuTrackingOptions& tracking) const;
    void Update(
        const Link& link,
        const ShellContextMenuSnapshot& snapshot,
        const ShellContextMenuTrackingOptions& tracking);
    bool Reset();
    void Remove(int linkId);
    void RemoveProvider(const std::wstring& providerId);

    // 新增: 增量更新缓存（Phase 2新功能）
    // 仅更新tracking中启用的providers，保留其他providers的菜单
    void UpdateIncremental(
        const Link& link,
        const ShellContextMenuSnapshot& snapshot,
        const ShellContextMenuTrackingOptions& tracking);

    // 新增: 获取缓存统计信息
    struct CacheStatistics {
        std::size_t linkCount = 0;
        std::size_t menuItemCount = 0;
        std::size_t iconCount = 0;
    };
    CacheStatistics GetStatistics() const;

    // 新增: 选择性移除某个provider的菜单（保留其他providers）
    void RemoveProviderSelective(const std::wstring& providerId);

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

    // 新增: 增量更新的辅助方法
    void MergeMenuItems(
        std::vector<ShellContextMenuItem>& existing,
        const std::vector<ShellContextMenuItem>& incoming);

    static void CountMenuItemsRecursive(
        const ShellContextMenuItem& item,
        std::size_t& count);

    std::filesystem::path cachePath_;
    std::unordered_map<int, Entry> entries_;
    std::unordered_map<std::wstring, ShellContextMenuCachedIcon> iconPool_;
};
