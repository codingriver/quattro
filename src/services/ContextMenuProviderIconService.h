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

    std::vector<ContextMenuProviderIconInfo> Load(std::stop_token stopToken = {}) const;

private:
    static ContextMenuProviderIconInfo ResolveDefault(
        const TrackedContextMenuProviderBinding& binding,
        const std::optional<ShellContextMenuCachedIcon>& cachedIcon,
        std::stop_token stopToken);

    std::filesystem::path storageDirectory_;
    ResolveFunction resolver_;
};
