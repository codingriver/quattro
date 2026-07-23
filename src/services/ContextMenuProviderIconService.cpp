#include "ContextMenuProviderIconService.h"

#include "AppLog.h"
#include "TerminalContextMenuService.h"
#include "Utilities.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <unordered_map>

namespace {
constexpr std::array<char, 8> kProviderCacheMagic{{'Q', 'C', 'M', 'P', 'I', 'C', 'O', '1'}};
constexpr std::uint32_t kMaxCachedProviders = 64;
constexpr std::uint32_t kMaxProviderIdLength = 128;

template <typename T>
bool ReadValue(std::istream& stream, T& value) {
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
void WriteValue(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool ReadProviderId(std::istream& stream, std::wstring& value) {
    std::uint32_t length = 0;
    if (!ReadValue(stream, length) || length == 0 || length > kMaxProviderIdLength) {
        return false;
    }
    value.resize(length);
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(value.data()),
        static_cast<std::streamsize>(length * sizeof(wchar_t))));
}

void WriteProviderId(std::ostream& stream, const std::wstring& value) {
    const auto length = static_cast<std::uint32_t>(value.size());
    WriteValue(stream, length);
    stream.write(
        reinterpret_cast<const char*>(value.data()),
        static_cast<std::streamsize>(length * sizeof(wchar_t)));
}

bool ReadCachedIcon(std::istream& stream, ShellContextMenuCachedIcon& icon) {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t quality = 0;
    std::uint32_t pixelCount = 0;
    if (!ReadValue(stream, width) || !ReadValue(stream, height) ||
        !ReadValue(stream, quality) || !ReadValue(stream, pixelCount)) {
        return false;
    }
    if (width == 0 && height == 0 && pixelCount == 0) {
        icon = {};
        return true;
    }
    if (width <= 0 || height <= 0 || width > 64 || height > 64 ||
        pixelCount != static_cast<std::uint32_t>(width * height)) {
        return false;
    }
    icon.width = width;
    icon.height = height;
    icon.quality = quality;
    icon.pixels.resize(pixelCount);
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(icon.pixels.data()),
        static_cast<std::streamsize>(pixelCount * sizeof(std::uint32_t))));
}

void WriteCachedIcon(std::ostream& stream, const ShellContextMenuCachedIcon& icon) {
    const std::int32_t width = icon.width;
    const std::int32_t height = icon.height;
    const std::int32_t quality = icon.quality;
    const std::uint32_t pixelCount = static_cast<std::uint32_t>(icon.pixels.size());
    WriteValue(stream, width);
    WriteValue(stream, height);
    WriteValue(stream, quality);
    WriteValue(stream, pixelCount);
    if (pixelCount != 0) {
        stream.write(
            reinterpret_cast<const char*>(icon.pixels.data()),
            static_cast<std::streamsize>(pixelCount * sizeof(std::uint32_t)));
    }
}

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
    : ContextMenuProviderIconService(QuattroUserConfigDirectory()) {}

ContextMenuProviderIconService::ContextMenuProviderIconService(std::filesystem::path storageDirectory)
    : storageDirectory_(std::move(storageDirectory)),
      providerCachePath_(storageDirectory_ / L"cache" / L"context-menu-provider-icons.bin"),
      persistentCacheEnabled_(true) {}

ContextMenuProviderIconService::ContextMenuProviderIconService(ResolveFunction resolver)
    : resolver_(std::move(resolver)) {}

ContextMenuProviderIconService::ContextMenuProviderIconService(
    std::filesystem::path storageDirectory,
    ResolveFunction resolver)
    : storageDirectory_(std::move(storageDirectory)),
      providerCachePath_(storageDirectory_ / L"cache" / L"context-menu-provider-icons.bin"),
      persistentCacheEnabled_(true),
      resolver_(std::move(resolver)) {}

std::optional<std::vector<ContextMenuProviderIconInfo>> ContextMenuProviderIconService::LoadCached() const {
    if (!persistentCacheEnabled_) {
        return std::nullopt;
    }
    std::ifstream stream(providerCachePath_, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::array<char, 8> magic{};
    std::uint32_t count = 0;
    if (!stream.read(magic.data(), magic.size()) || magic != kProviderCacheMagic ||
        !ReadValue(stream, count) || count > kMaxCachedProviders) {
        return std::nullopt;
    }
    std::unordered_map<std::wstring, ContextMenuProviderIconInfo> loaded;
    loaded.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        ContextMenuProviderIconInfo info;
        std::uint8_t flags = 0;
        if (!ReadProviderId(stream, info.providerId) || !ReadValue(stream, flags) ||
            !ReadCachedIcon(stream, info.icon) || loaded.contains(info.providerId)) {
            return std::nullopt;
        }
        info.installed = (flags & 0x01) != 0;
        info.installedViaProbe = (flags & 0x02) != 0;
        info.installedInNativeShell = (flags & 0x04) != 0;
        info.attempted = (flags & 0x08) != 0;
        loaded.emplace(info.providerId, std::move(info));
    }
    const auto providers = TrackedContextMenuProviders();
    if (loaded.size() != providers.size()) {
        return std::nullopt;
    }
    std::vector<ContextMenuProviderIconInfo> result;
    result.reserve(providers.size());
    for (const auto& provider : providers) {
        const auto found = loaded.find(provider.providerId ? provider.providerId : L"");
        if (found == loaded.end()) {
            return std::nullopt;
        }
        result.push_back(found->second);
    }
    return result;
}

bool ContextMenuProviderIconService::SaveCached(
    const std::vector<ContextMenuProviderIconInfo>& icons) const {
    if (!persistentCacheEnabled_ || icons.size() != TrackedContextMenuProviders().size()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(providerCachePath_.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::filesystem::path tempPath = providerCachePath_;
    tempPath += L".tmp";
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }
        stream.write(kProviderCacheMagic.data(), kProviderCacheMagic.size());
        WriteValue(stream, static_cast<std::uint32_t>(icons.size()));
        for (const auto& info : icons) {
            WriteProviderId(stream, info.providerId);
            const std::uint8_t flags = static_cast<std::uint8_t>(
                (info.installed ? 0x01 : 0) |
                (info.installedViaProbe ? 0x02 : 0) |
                (info.installedInNativeShell ? 0x04 : 0) |
                (info.attempted ? 0x08 : 0));
            WriteValue(stream, flags);
            WriteCachedIcon(stream, info.icon);
        }
        stream.flush();
        if (!stream.good()) {
            stream.close();
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }
    std::filesystem::rename(tempPath, providerCachePath_, ec);
    if (!ec) {
        return true;
    }
    ec.clear();
    std::filesystem::copy_file(
        tempPath, providerCachePath_, std::filesystem::copy_options::overwrite_existing, ec);
    std::error_code removeEc;
    std::filesystem::remove(tempPath, removeEc);
    return !ec;
}

bool ContextMenuProviderIconService::ResetCache() const {
    if (!persistentCacheEnabled_) {
        return true;
    }
    std::error_code ec;
    std::filesystem::remove(providerCachePath_, ec);
    if (ec) {
        return false;
    }
    std::filesystem::path tempPath = providerCachePath_;
    tempPath += L".tmp";
    ec.clear();
    std::filesystem::remove(tempPath, ec);
    return !ec;
}

std::vector<ContextMenuProviderIconInfo> ContextMenuProviderIconService::Load(
    std::stop_token stopToken) const {
    if (const auto cached = LoadCached()) {
        for (const auto& info : *cached) {
            WriteAppLog(L"右键菜单 provider 图标：provider=" + info.providerId + L"，source=provider-cache");
        }
        return *cached;
    }
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
            : ResolveProvider(provider, cachedIcon, stopToken));
    }
    if (!stopToken.stop_requested() && result.size() == providers.size()) {
        SaveCached(result);
    }
    return result;
}

ContextMenuProviderIconInfo ContextMenuProviderIconService::ResolveProvider(
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
    if (HasIcon(info.icon)) {
        WriteAppLog(
            L"右键菜单 provider 图标：provider=" + info.providerId + L"，source=" +
            (info.providerId == ShellContextMenuProviderId::Terminal
                ? L"terminal-executable"
                : IconSourceText(source)));
        return info;
    }
    if (cachedIcon && HasIcon(*cachedIcon)) {
        info.icon = *cachedIcon;
        WriteAppLog(L"右键菜单 provider 图标：provider=" + info.providerId + L"，source=native-cache-fallback");
        return info;
    }
    WriteAppLog(L"右键菜单 provider 图标：provider=" + info.providerId + L"，source=fallback");
    return info;
}
