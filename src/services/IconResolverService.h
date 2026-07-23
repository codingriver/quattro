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
};

struct IconRequest {
    IconSourceKind kind = IconSourceKind::Stock;
    int size = 32;
    Link link;
    std::wstring value;
    std::vector<std::uint8_t> pidl;
    std::wstring providerId;
    SHSTOCKICONID stockIcon = SIID_APPLICATION;
    bool allowFallback = true;
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
    explicit IconResolverService(std::filesystem::path appDirectory = {});

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
    static HBITMAP CreateBitmapFromPixels(
        const ResolvedIcon& icon,
        int targetSize,
        COLORREF background);

private:
    HICON ResolveIconHandle(const IconRequest& request, std::wstring& source) const;
    HICON ResolveLinkIcon(const Link& link, std::wstring& source) const;
    HICON ResolveProviderIcon(const std::wstring& providerId, std::wstring& source) const;
    HICON ResolveProviderIcon(const TrackedContextMenuProviderBinding& binding, std::wstring& source) const;
    HICON ResolveIconLocation(const std::wstring& value, std::wstring& source) const;
    HICON ResolveCommandIcon(const std::wstring& command, std::wstring& source) const;
    HICON ResolveFileIcon(const std::wstring& value, bool directory, std::wstring& source) const;
    HICON ResolvePidlIcon(const std::vector<std::uint8_t>& pidl, std::wstring& source) const;
    HICON ResolveShellParseNameIcon(const std::wstring& value, std::wstring& source) const;
    HICON ResolveStockIcon(SHSTOCKICONID iconId, std::wstring& source) const;
    ResolvedIcon CaptureIcon(HICON icon, int size, int quality, const std::wstring& source) const;

    std::filesystem::path appDirectory_;
};
