#pragma once

#include "ShellContextMenuCacheService.h"
#include "TrackedContextMenuProviders.h"

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct ContextMenuProviderIconInfo {
    std::wstring providerId;
    bool installed = false;
    bool installedViaProbe = false;
    bool installedInNativeShell = false;
    bool attempted = false;
    ShellContextMenuCachedIcon icon;
};

class ContextMenuProviderIconService {
public:
    using ResolveFunction = std::function<ContextMenuProviderIconInfo(
        const TrackedContextMenuProviderBinding&,
        const std::optional<ShellContextMenuCachedIcon>&,
        std::stop_token)>;

    ContextMenuProviderIconService();
    explicit ContextMenuProviderIconService(std::filesystem::path storageDirectory);
    explicit ContextMenuProviderIconService(ResolveFunction resolver);
    ContextMenuProviderIconService(
        std::filesystem::path storageDirectory,
        ResolveFunction resolver);

    std::vector<ContextMenuProviderIconInfo> Load(std::stop_token stopToken = {}) const;
    std::optional<std::vector<ContextMenuProviderIconInfo>> LoadCached() const;
    bool ResetCache() const;
    // Provider 展示图标必须优先使用稳定的注册表/品牌 EXE 来源；原生菜单
    // 缓存保存的是具体命令图标，只能在静态来源不可用时兜底。
    static ContextMenuProviderIconInfo ResolveProvider(
        const TrackedContextMenuProviderBinding& binding,
        const std::optional<ShellContextMenuCachedIcon>& cachedIcon,
        std::stop_token stopToken);

private:
    bool SaveCached(const std::vector<ContextMenuProviderIconInfo>& icons) const;

    std::filesystem::path storageDirectory_;
    std::filesystem::path providerCachePath_;
    bool persistentCacheEnabled_ = false;
    ResolveFunction resolver_;
};
