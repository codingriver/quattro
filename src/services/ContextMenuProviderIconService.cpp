#include "ContextMenuProviderIconService.h"

#include "AppLog.h"
#include "TerminalContextMenuService.h"
#include "Utilities.h"

#include <algorithm>

namespace {
ShellContextMenuCachedIcon CachedIconFrom(const ShellContextMenuItem& item) {
    ShellContextMenuCachedIcon icon;
    if (item.iconWidth <= 0 || item.iconHeight <= 0 || item.iconWidth > 64 || item.iconHeight > 64 ||
        item.iconPixels.size() != static_cast<std::size_t>(item.iconWidth * item.iconHeight)) {
        return icon;
    }
    icon.width = item.iconWidth;
    icon.height = item.iconHeight;
    icon.quality = std::max(1, item.iconQuality);
    icon.pixels = item.iconPixels;
    return icon;
}

bool HasIcon(const ShellContextMenuCachedIcon& icon) {
    return icon.width > 0 && icon.height > 0 && icon.width <= 64 && icon.height <= 64 &&
        icon.pixels.size() == static_cast<std::size_t>(icon.width * icon.height);
}

const wchar_t* IconSourceText(TrackedProviderIconSource source) {
    switch (source) {
    case TrackedProviderIconSource::ExplicitRegistry: return L"registry-icon";
    case TrackedProviderIconSource::CommandExecutable: return L"command-executable";
    case TrackedProviderIconSource::BrandExecutable: return L"brand-executable";
    case TrackedProviderIconSource::ApplicationExecutable: return L"application-executable";
    default: return L"fallback";
    }
}
}

ContextMenuProviderIconService::ContextMenuProviderIconService()
    : storageDirectory_(QuattroUserConfigDirectory()) {}

ContextMenuProviderIconService::ContextMenuProviderIconService(std::filesystem::path storageDirectory)
    : storageDirectory_(std::move(storageDirectory)) {}

ContextMenuProviderIconService::ContextMenuProviderIconService(ResolveFunction resolver)
    : resolver_(std::move(resolver)) {}

std::vector<ContextMenuProviderIconInfo> ContextMenuProviderIconService::Load(
    std::stop_token stopToken) const {
    const auto providers = TrackedContextMenuProviders();
    std::vector<ContextMenuProviderIconInfo> result;
    result.reserve(providers.size());

    std::optional<ShellContextMenuCacheService> cache;
    if (!resolver_) {
        cache.emplace(storageDirectory_);
    }
    for (const auto& provider : providers) {
        if (stopToken.stop_requested()) {
            break;
        }
        const std::optional<ShellContextMenuCachedIcon> cachedIcon = cache
            ? cache->BestIconForProvider(provider.providerId)
            : std::nullopt;
        result.push_back(resolver_
            ? resolver_(provider, cachedIcon, stopToken)
            : ResolveDefault(provider, cachedIcon, stopToken));
    }
    return result;
}

ContextMenuProviderIconInfo ContextMenuProviderIconService::ResolveDefault(
    const TrackedContextMenuProviderBinding& binding,
    const std::optional<ShellContextMenuCachedIcon>& cachedIcon,
    std::stop_token stopToken) {
    ContextMenuProviderIconInfo info;
    info.providerId = binding.providerId ? binding.providerId : L"";
    info.installedViaProbe = ShellItemService::IsTrackedProviderInstalled(binding);
    // Automatic page entry stays bounded: do not enumerate the complete HKCR
    // tree or construct third-party shell menus here. A persisted native-menu
    // icon proves that this provider was observed by an explicit refresh.
    info.installedInNativeShell = !info.installedViaProbe && cachedIcon && HasIcon(*cachedIcon);
    info.installed = info.installedViaProbe || info.installedInNativeShell;
    if (!info.installed || stopToken.stop_requested()) {
        return info;
    }

    info.attempted = true;
    if (cachedIcon && HasIcon(*cachedIcon)) {
        info.icon = *cachedIcon;
        WriteAppLog(L"右键菜单 provider 图标：provider=" + info.providerId + L"，source=native-cache");
        return info;
    }

    ShellContextMenuItem item;
    TrackedProviderIconSource source = TrackedProviderIconSource::None;
    if (info.providerId == ShellContextMenuProviderId::Terminal) {
        const TerminalContextMenuRefreshContext context = TerminalContextMenuService::DetectAvailablePrograms();
        for (const auto& program : context.programs) {
            if (!program.iconTemplate.iconPixels.empty()) {
                item = program.iconTemplate;
                break;
            }
            if (ShellItemService::LoadExecutableMenuIcon(program.executable, item)) {
                break;
            }
        }
    } else {
        ShellItemService::LoadTrackedProviderIcon(binding, item, &source);
    }
    info.icon = CachedIconFrom(item);
    WriteAppLog(
        L"右键菜单 provider 图标：provider=" + info.providerId + L"，source=" +
        (info.providerId == ShellContextMenuProviderId::Terminal
            ? (HasIcon(info.icon) ? L"terminal-executable" : L"fallback")
            : IconSourceText(source)));
    return info;
}
