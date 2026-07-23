#include "IconResolverService.h"

#include "MenuCatalog.h"
#include "ShellItemService.h"
#include "SystemFunctions.h"
#include "TerminalContextMenuService.h"
#include "ThemedUi.h"
#include "Utilities.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <system_error>

namespace {

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool LooksLikeUrl(const Link& link) {
    const std::wstring lower = ToLower(Trim(link.path));
    return link.type == 2 ||
           lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

std::wstring RegistryString(HKEY root, const std::wstring& subkey, const wchar_t* valueName) {
    DWORD size = 0;
    if (RegGetValueW(
            root,
            subkey.c_str(),
            valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            nullptr,
            &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(
            root,
            subkey.c_str(),
            valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            value.data(),
            &size) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    return ExpandEnvironmentStringsSafe(Trim(value));
}

std::wstring ExecutableFromCommand(const std::wstring& command) {
    if (Trim(command).empty()) {
        return {};
    }
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &count);
    if (!arguments || count <= 0) {
        if (arguments) {
            LocalFree(arguments);
        }
        return {};
    }
    std::wstring executable = arguments[0];
    LocalFree(arguments);
    return ExpandEnvironmentStringsSafe(Trim(executable));
}

bool ParseIconLocation(std::wstring value, std::wstring& path, int& index) {
    value = Trim(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() == L'@') {
        value.erase(value.begin());
    }
    index = 0;
    const auto comma = value.rfind(L',');
    if (comma != std::wstring::npos) {
        if (const auto parsed = ParseInt(Trim(value.substr(comma + 1)))) {
            index = *parsed;
            value.resize(comma);
        }
    }
    value = Trim(value);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    path = ExpandEnvironmentStringsSafe(Trim(value));
    return !path.empty();
}

std::wstring ResolveExecutablePath(std::wstring path) {
    path = ExpandEnvironmentStringsSafe(Trim(path));
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        return path;
    }
    const std::wstring fileName = std::filesystem::path(path).filename().wstring();
    if (fileName.empty()) {
        return path;
    }
    const std::array<std::pair<HKEY, const wchar_t*>, 2> appPathRoots{{
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"},
        {HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"},
    }};
    for (const auto& [root, prefix] : appPathRoots) {
        const std::wstring registered = RegistryString(root, std::wstring(prefix) + fileName, nullptr);
        if (!registered.empty() && std::filesystem::is_regular_file(registered, ec)) {
            return registered;
        }
    }
    std::array<wchar_t, 32768> found{};
    const DWORD length = SearchPathW(
        nullptr,
        fileName.c_str(),
        nullptr,
        static_cast<DWORD>(found.size()),
        found.data(),
        nullptr);
    return length > 0 && length < found.size() ? std::wstring(found.data(), length) : path;
}

std::optional<TrackedContextMenuProviderBinding> FindProvider(const std::wstring& providerId) {
    for (const auto& provider : TrackedContextMenuProviders()) {
        if (providerId == (provider.providerId ? provider.providerId : L"")) {
            return provider;
        }
    }
    return std::nullopt;
}

HICON DuplicateIconHandle(HICON icon) {
    return icon ? CopyIcon(icon) : nullptr;
}

}

IconResolverService::IconResolverService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {}

IconRequest IconResolverService::ForLink(const Link& link, int size) {
    IconRequest request;
    request.kind = IconSourceKind::Link;
    request.size = size;
    request.link = link;
    return request;
}

IconRequest IconResolverService::ForPidl(std::vector<std::uint8_t> pidl, int size) {
    IconRequest request;
    request.kind = IconSourceKind::PidlBlob;
    request.size = size;
    request.pidl = std::move(pidl);
    return request;
}

IconRequest IconResolverService::ForContextMenuProvider(std::wstring providerId, int size) {
    IconRequest request;
    request.kind = IconSourceKind::ContextMenuProvider;
    request.size = size;
    request.providerId = std::move(providerId);
    return request;
}

bool IconResolverService::HasPixels(const ResolvedIcon& icon) {
    return icon.ok && icon.width > 0 && icon.height > 0 &&
        icon.pixels.size() == static_cast<std::size_t>(icon.width * icon.height);
}

ResolvedIcon IconResolverService::Resolve(const IconRequest& request, std::stop_token stopToken) const {
    if (stopToken.stop_requested()) {
        return {};
    }
    std::wstring source;
    HICON icon = ResolveIconHandle(request, source);
    if (!icon && request.allowFallback) {
        icon = ResolveStockIcon(request.stockIcon, source);
    }
    ResolvedIcon result = CaptureIcon(icon, std::clamp(request.size, 1, 256), 1, source);
    if (icon) {
        DestroyIcon(icon);
    }
    return result;
}

std::vector<ResolvedIcon> IconResolverService::ResolveBatch(
    const std::vector<IconRequest>& requests,
    std::stop_token stopToken) const {
    std::vector<ResolvedIcon> result;
    result.reserve(requests.size());
    for (const auto& request : requests) {
        if (stopToken.stop_requested()) {
            break;
        }
        result.push_back(Resolve(request, stopToken));
    }
    return result;
}

ResolvedIcon IconResolverService::ResolveContextMenuProvider(
    const TrackedContextMenuProviderBinding& binding,
    int size,
    std::stop_token stopToken) const {
    if (stopToken.stop_requested()) {
        return {};
    }
    if ((binding.providerId ? binding.providerId : L"") != ShellContextMenuProviderId::Terminal) {
        ShellContextMenuItem item;
        TrackedProviderIconSource providerSource = TrackedProviderIconSource::None;
        if (ShellItemService::LoadTrackedProviderIcon(binding, item, &providerSource) &&
            item.iconWidth > 0 && item.iconHeight > 0 &&
            item.iconPixels.size() == static_cast<std::size_t>(item.iconWidth * item.iconHeight)) {
            ResolvedIcon result;
            result.ok = true;
            result.width = item.iconWidth;
            result.height = item.iconHeight;
            result.quality = std::max(1, item.iconQuality);
            result.pixels = item.iconPixels;
            result.source = L"context-menu-provider";
            return result;
        }
        return {};
    }
    std::wstring source;
    HICON icon = ResolveProviderIcon(binding, source);
    ResolvedIcon result = CaptureIcon(icon, std::clamp(size, 1, 256), 1, source);
    if (icon) {
        DestroyIcon(icon);
    }
    return result;
}

HICON IconResolverService::ResolveIconHandle(const IconRequest& request, std::wstring& source) const {
    switch (request.kind) {
    case IconSourceKind::Link:
        return ResolveLinkIcon(request.link, source);
    case IconSourceKind::FilePath:
        return ResolveFileIcon(request.value, false, source);
    case IconSourceKind::DirectoryPath:
        return ResolveFileIcon(request.value, true, source);
    case IconSourceKind::Url:
        return ResolveStockIcon(SIID_WORLD, source);
    case IconSourceKind::ShellParseName:
        return ResolveShellParseNameIcon(request.value, source);
    case IconSourceKind::PidlBlob:
        return ResolvePidlIcon(request.pidl, source);
    case IconSourceKind::IconLocation:
        return ResolveIconLocation(request.value, source);
    case IconSourceKind::CommandLine:
        return ResolveCommandIcon(request.value, source);
    case IconSourceKind::ContextMenuProvider:
        return ResolveProviderIcon(request.providerId, source);
    case IconSourceKind::Stock:
        return ResolveStockIcon(request.stockIcon, source);
    default:
        return nullptr;
    }
}

HICON IconResolverService::ResolveLinkIcon(const Link& link, std::wstring& source) const {
    const MenuIcon menuIcon = SystemFunctionMenuIconForLink(link);
    if (MenuIconIsRenderable(menuIcon)) {
        if (HICON icon = CreateTablerIconHandle(
                appDirectory_,
                MenuIconTablerId(menuIcon),
                54,
                RGB(0, 153, 215))) {
            source = L"tabler-system";
            return icon;
        }
    }

    const std::wstring iconPath = Trim(link.icon);
    if (!iconPath.empty() && iconPath != L"#url" && iconPath != L"默认系统缓存图标") {
        if (HICON icon = ResolveIconLocation(iconPath, source)) {
            return icon;
        }
        if (HICON icon = ResolveFileIcon(iconPath, false, source)) {
            return icon;
        }
    }
    if (LooksLikeUrl(link)) {
        return ResolveStockIcon(SIID_WORLD, source);
    }
    if (HICON icon = ResolvePidlIcon(link.pidl, source)) {
        return icon;
    }
    if (HICON icon = ResolveShellParseNameIcon(link.path, source)) {
        return icon;
    }
    return ResolveFileIcon(link.path, link.type == 1, source);
}

HICON IconResolverService::ResolveProviderIcon(const std::wstring& providerId, std::wstring& source) const {
    if (providerId == ShellContextMenuProviderId::Terminal) {
        const TerminalContextMenuRefreshContext context = TerminalContextMenuService::DetectAvailablePrograms();
        for (const auto& program : context.programs) {
            if (HICON icon = ResolveFileIcon(program.executable, false, source)) {
                source = L"terminal-executable";
                return icon;
            }
        }
        return nullptr;
    }

    const auto provider = FindProvider(providerId);
    if (!provider) {
        return nullptr;
    }
    return ResolveProviderIcon(*provider, source);
}

HICON IconResolverService::ResolveProviderIcon(
    const TrackedContextMenuProviderBinding& provider,
    std::wstring& source) const {
    ShellContextMenuItem item;
    TrackedProviderIconSource providerSource = TrackedProviderIconSource::None;
    if (!ShellItemService::LoadTrackedProviderIcon(provider, item, &providerSource)) {
        return nullptr;
    }
    if (item.iconPixels.empty() || item.iconWidth <= 0 || item.iconHeight <= 0) {
        return nullptr;
    }
    ResolvedIcon resolved;
    resolved.ok = true;
    resolved.width = item.iconWidth;
    resolved.height = item.iconHeight;
    resolved.quality = std::max(1, item.iconQuality);
    resolved.pixels = item.iconPixels;
    resolved.source = L"context-menu-provider";
    source = resolved.source;
    HBITMAP bitmap = CreateBitmapFromPixels(resolved, std::max(item.iconWidth, item.iconHeight), RGB(255, 255, 255));
    if (!bitmap) {
        return nullptr;
    }
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = bitmap;
    iconInfo.hbmMask = bitmap;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(bitmap);
    return icon;
}

HICON IconResolverService::ResolveIconLocation(const std::wstring& value, std::wstring& source) const {
    std::wstring iconPath;
    int iconIndex = 0;
    if (!ParseIconLocation(value, iconPath, iconIndex)) {
        return nullptr;
    }
    iconPath = ResolveExecutablePath(iconPath);
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    if (ExtractIconExW(iconPath.c_str(), iconIndex, &largeIcon, &smallIcon, 1) != 0) {
        source = L"icon-location";
        if (largeIcon && smallIcon) {
            DestroyIcon(smallIcon);
            return largeIcon;
        }
        if (largeIcon) {
            return largeIcon;
        }
        if (smallIcon) {
            return smallIcon;
        }
    }
    return ResolveFileIcon(iconPath, false, source);
}

HICON IconResolverService::ResolveCommandIcon(const std::wstring& command, std::wstring& source) const {
    return ResolveFileIcon(ExecutableFromCommand(command), false, source);
}

HICON IconResolverService::ResolveFileIcon(const std::wstring& value, bool directory, std::wstring& source) const {
    const std::wstring path = ResolveExecutablePath(value);
    if (Trim(path).empty()) {
        return nullptr;
    }
    SHFILEINFOW info{};
    const DWORD attrs = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        source = directory ? L"directory" : L"file";
        return info.hIcon;
    }
    flags |= SHGFI_USEFILEATTRIBUTES;
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        source = directory ? L"directory-attributes" : L"file-attributes";
        return info.hIcon;
    }
    return nullptr;
}

HICON IconResolverService::ResolvePidlIcon(const std::vector<std::uint8_t>& pidl, std::wstring& source) const {
    if (!ShellItemService::IsPidlBlobPlausible(pidl)) {
        return nullptr;
    }
    SHFILEINFOW info{};
    if (SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(pidl.data()),
            0,
            &info,
            sizeof(info),
            SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON)) {
        source = L"pidl";
        return info.hIcon;
    }
    return nullptr;
}

HICON IconResolverService::ResolveShellParseNameIcon(const std::wstring& value, std::wstring& source) const {
    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(value));
    if (target.empty() || !ShellItemService::IsShellParseName(target)) {
        return nullptr;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
        return nullptr;
    }
    SHFILEINFOW info{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(pidl),
            0,
            &info,
            sizeof(info),
            SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON)) {
        source = L"shell-parse-name";
        icon = info.hIcon;
    }
    CoTaskMemFree(pidl);
    return icon;
}

HICON IconResolverService::ResolveStockIcon(SHSTOCKICONID iconId, std::wstring& source) const {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(SHGetStockIconInfo(iconId, SHGSI_ICON | SHGSI_LARGEICON, &info))) {
        source = L"stock";
        return info.hIcon;
    }
    source = L"default-application";
    return DuplicateIconHandle(LoadIconW(nullptr, IDI_APPLICATION));
}

ResolvedIcon IconResolverService::CaptureIcon(HICON icon, int size, int quality, const std::wstring& source) const {
    if (!icon || size <= 0) {
        return {};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !dc || !bits) {
        if (dc) {
            DeleteDC(dc);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return {};
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    std::fill_n(static_cast<std::uint32_t*>(bits), static_cast<std::size_t>(size) * size, 0);
    const BOOL drew = DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    GdiFlush();

    ResolvedIcon result;
    if (drew) {
        result.width = size;
        result.height = size;
        result.quality = quality;
        result.pixels.resize(static_cast<std::size_t>(size) * size);
        std::memcpy(result.pixels.data(), bits, result.pixels.size() * sizeof(std::uint32_t));
        result.ok = std::any_of(result.pixels.begin(), result.pixels.end(), [](std::uint32_t pixel) {
            return (pixel >> 24) != 0 || (pixel & 0x00FFFFFFu) != 0;
        });
        for (auto& pixel : result.pixels) {
            const std::uint32_t alpha = pixel >> 24;
            if (alpha == 0 || alpha == 255) {
                continue;
            }
            const std::uint32_t blue = pixel & 0xFFu;
            const std::uint32_t green = (pixel >> 8) & 0xFFu;
            const std::uint32_t red = (pixel >> 16) & 0xFFu;
            pixel = (alpha << 24) |
                    ((red * alpha / 255) << 16) |
                    ((green * alpha / 255) << 8) |
                    (blue * alpha / 255);
        }
        result.source = source;
    }
    DeleteDC(dc);
    DeleteObject(bitmap);
    return result;
}

HBITMAP IconResolverService::CreateBitmapFromPixels(
    const ResolvedIcon& icon,
    int targetSize,
    COLORREF background) {
    if (!HasPixels(icon) || targetSize <= 0) {
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = targetSize;
    info.bmiHeader.biHeight = -targetSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }
    auto* target = static_cast<std::uint32_t*>(bits);
    const std::uint32_t bg =
        0xFF000000u |
        (static_cast<std::uint32_t>(GetRValue(background)) << 16) |
        (static_cast<std::uint32_t>(GetGValue(background)) << 8) |
        static_cast<std::uint32_t>(GetBValue(background));
    std::fill_n(target, static_cast<std::size_t>(targetSize) * targetSize, bg);

    const double scale = std::min(
        static_cast<double>(targetSize) / icon.width,
        static_cast<double>(targetSize) / icon.height);
    const int drawWidth = std::max(1, static_cast<int>(std::round(icon.width * scale)));
    const int drawHeight = std::max(1, static_cast<int>(std::round(icon.height * scale)));
    const int offsetX = (targetSize - drawWidth) / 2;
    const int offsetY = (targetSize - drawHeight) / 2;

    for (int y = 0; y < drawHeight; ++y) {
        const int srcY = std::clamp(static_cast<int>(y / scale), 0, icon.height - 1);
        for (int x = 0; x < drawWidth; ++x) {
            const int srcX = std::clamp(static_cast<int>(x / scale), 0, icon.width - 1);
            const std::uint32_t src = icon.pixels[static_cast<std::size_t>(srcY) * icon.width + srcX];
            const std::uint32_t alpha = src >> 24;
            if (alpha == 0) {
                continue;
            }
            const std::size_t dstIndex = static_cast<std::size_t>(offsetY + y) * targetSize + offsetX + x;
            if (alpha == 255) {
                target[dstIndex] = src;
                continue;
            }
            const std::uint32_t dst = target[dstIndex];
            const std::uint32_t srcR = (src >> 16) & 0xFFu;
            const std::uint32_t srcG = (src >> 8) & 0xFFu;
            const std::uint32_t srcB = src & 0xFFu;
            const std::uint32_t dstR = (dst >> 16) & 0xFFu;
            const std::uint32_t dstG = (dst >> 8) & 0xFFu;
            const std::uint32_t dstB = dst & 0xFFu;
            target[dstIndex] =
                0xFF000000u |
                (((srcR * alpha + dstR * (255 - alpha)) / 255) << 16) |
                (((srcG * alpha + dstG * (255 - alpha)) / 255) << 8) |
                ((srcB * alpha + dstB * (255 - alpha)) / 255);
        }
    }
    return bitmap;
}
