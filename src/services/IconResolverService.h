#pragma once

#include "Models.h"
#include "TrackedContextMenuProviders.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

enum class IconSourceKind {
    Link,
    FilePath,
    DirectoryPath,
    Url,
    ShellParseName,
    PidlBlob,
    IconLocation,
    CommandLine,
    ContextMenuProvider,
    Stock,
    DefaultCategory,
};

enum class IconFallbackKind {
    Application,
    File,
    Directory,
    Url,
    Registry,
    StartupFolder,
    ScheduledTask,
    Service,
    Driver,
    ActiveSetup,
    Wmi,
    Login,
    Dll,
    ShellExtension,
    System,
};

enum class IconCacheMode {
    PreferCache,
    Refresh,
    Bypass,
    Disabled,
};

struct IconRequest {
    IconSourceKind kind = IconSourceKind::Stock;
    int size = 32;
    Link link;
    std::wstring value;
    std::vector<std::uint8_t> pidl;
    std::wstring providerId;
    SHSTOCKICONID stockIcon = SIID_APPLICATION;
    IconFallbackKind fallbackKind = IconFallbackKind::Application;
    bool allowFallback = true;
    bool preferFallbackForGenericHost = false;
    IconCacheMode cacheMode = IconCacheMode::PreferCache;
};

struct ResolvedIcon {
    bool ok = false;
    int width = 0;
    int height = 0;
    int quality = 0;
    std::vector<std::uint32_t> pixels;
    std::wstring source;
};

class IconResolverService {
public:
    explicit IconResolverService(
        std::filesystem::path appDirectory = {},
        std::filesystem::path cacheDirectory = {});

    ResolvedIcon Resolve(const IconRequest& request, std::stop_token stopToken = {}) const;
    ResolvedIcon ResolveContextMenuProvider(
        const TrackedContextMenuProviderBinding& binding,
        int size = 32,
        std::stop_token stopToken = {}) const;
    std::vector<ResolvedIcon> ResolveBatch(
        const std::vector<IconRequest>& requests,
        std::stop_token stopToken = {}) const;

    static IconRequest ForLink(const Link& link, int size = 32);
    static IconRequest ForPidl(std::vector<std::uint8_t> pidl, int size = 32);
    static IconRequest ForContextMenuProvider(std::wstring providerId, int size = 32);
    static bool HasPixels(const ResolvedIcon& icon);
    static std::filesystem::path DefaultCacheDirectory(const std::filesystem::path& appDirectory = {});
    static bool SavePngIcon(const ResolvedIcon& icon, const std::filesystem::path& path);
    static ResolvedIcon LoadPngIconFile(
        const std::filesystem::path& path,
        const std::wstring& source = L"disk-cache");
    static bool ClearDiskCache(
        const std::filesystem::path& appDirectory = {},
        const std::filesystem::path& cacheDirectory = {});
    static HBITMAP CreateBitmapFromPixels(
        const ResolvedIcon& icon,
        int targetSize,
        COLORREF background,
        bool preserveTransparency = false);

private:
    ResolvedIcon ResolveAppxUnplatedIcon(const std::wstring& parseName, int size) const;
    ResolvedIcon ResolveShellItemImage(const IconRequest& request, int size) const;
    ResolvedIcon ResolveLinkShellItemImage(const Link& link, int size) const;
    ResolvedIcon ResolvePidlImage(
        const std::vector<std::uint8_t>& pidl,
        int size,
        const std::wstring& source) const;
    ResolvedIcon ResolveShellParseNameImage(
        const std::wstring& value,
        int size,
        const std::wstring& source) const;
    ResolvedIcon CaptureBitmap(HBITMAP bitmap, int quality, const std::wstring& source) const;
    HICON ResolveIconHandle(const IconRequest& request, std::wstring& source) const;
    HICON ResolveLinkIcon(const Link& link, std::wstring& source) const;
    HICON ResolveProviderIcon(const std::wstring& providerId, std::wstring& source) const;
    HICON ResolveProviderIcon(const TrackedContextMenuProviderBinding& binding, std::wstring& source) const;
    HICON ResolveIconLocation(const std::wstring& value, std::wstring& source) const;
    HICON ResolveCommandIcon(const IconRequest& request, std::wstring& source) const;
    HICON ResolveFileIcon(const std::wstring& value, bool directory, std::wstring& source) const;
    HICON ResolvePidlIcon(const std::vector<std::uint8_t>& pidl, std::wstring& source) const;
    HICON ResolveShellParseNameIcon(const std::wstring& value, std::wstring& source) const;
    HICON ResolveStockIcon(SHSTOCKICONID iconId, std::wstring& source) const;
    HICON ResolveFallbackIcon(IconFallbackKind kind, SHSTOCKICONID stockIcon, std::wstring& source) const;
    ResolvedIcon CaptureIcon(HICON icon, int size, int quality, const std::wstring& source) const;
    std::filesystem::path CacheRoot() const;
    std::filesystem::path CachePathForRequest(const IconRequest& request, int size) const;

    std::filesystem::path appDirectory_;
    std::filesystem::path cacheDirectory_;
};
