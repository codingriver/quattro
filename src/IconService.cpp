#include "IconService.h"

#include "MenuCatalog.h"
#include "ShellItemService.h"
#include "SystemFunctionDialog.h"
#include "Utilities.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wininet.h>

#include <algorithm>
#include <array>
#include <cstdint>

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
           lower.rfind(L"www.", 0) == 0;
}

std::wstring UrlHost(const std::wstring& value) {
    std::wstring text = NormalizeUrl(value);
    URL_COMPONENTSW parts{};
    wchar_t host[260]{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    if (InternetCrackUrlW(text.c_str(), 0, 0, &parts) && parts.dwHostNameLength > 0) {
        return std::wstring(host, parts.dwHostNameLength);
    }
    return {};
}

HICON DuplicateIconHandle(HICON icon) {
    return icon ? CopyIcon(icon) : nullptr;
}

HICON StockIcon(SHSTOCKICONID id) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_LARGEICON, &info))) {
        return info.hIcon;
    }
    return nullptr;
}

bool EnsureMenuIconFontLoaded(const std::filesystem::path& appDirectory) {
    static std::filesystem::path loadedPath;
    static bool loaded = false;

    const std::filesystem::path fontPath = appDirectory / L"icons" / L"menu" / L"tabler" / L"tabler-icons.ttf";
    if (loaded && loadedPath == fontPath) {
        return true;
    }
    loadedPath = fontPath;
    loaded = FileExists(fontPath) && AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) > 0;
    return loaded;
}

bool UsesDangerMenuIconColor(MenuIcon icon) {
    return icon == MenuIconDelete ||
           icon == MenuIconClear ||
           icon == MenuIconExit ||
           icon == MenuIconPower;
}

HICON CreateMenuIconHandle(MenuIcon icon, const std::filesystem::path& appDirectory) {
    const wchar_t glyph = MenuIconGlyph(icon);
    if (glyph == L'\0' || !EnsureMenuIconFontLoaded(appDirectory)) {
        return nullptr;
    }

    constexpr int size = 64;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!color || !dc || !pixels) {
        if (dc) {
            DeleteDC(dc);
        }
        if (color) {
            DeleteObject(color);
        }
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, color);
    std::fill_n(static_cast<std::uint32_t*>(pixels), size * size, 0);
    HFONT font = CreateFontW(
        -54,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"tabler-icons");
    if (!font) {
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);
        DeleteObject(color);
        return nullptr;
    }

    const COLORREF iconColor = UsesDangerMenuIconColor(icon) ? RGB(228, 48, 58) : RGB(0, 153, 215);
    const int oldBkMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, iconColor);
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT rect{0, 0, size, size};
    DrawTextW(dc, &glyph, 1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOCLIP);
    SelectObject(dc, oldFont);
    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBkMode);
    DeleteObject(font);

    auto* argb = static_cast<std::uint32_t*>(pixels);
    for (int i = 0; i < size * size; ++i) {
        if ((argb[i] & 0x00ffffff) != 0) {
            argb[i] |= 0xff000000;
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;
    HICON result = CreateIconIndirect(&iconInfo);

    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(mask);
    DeleteObject(color);
    return result;
}

std::wstring LinkIconCacheToken(const Link& link) {
    const MenuIcon icon = SystemFunctionMenuIconForLink(link);
    if (MenuIconIsRenderable(icon)) {
        return MenuIconLinkIconValue(icon);
    }
    return link.icon;
}

std::wstring ResolveExecutableLikePath(const std::wstring& value) {
    const std::wstring expanded = ExpandEnvironmentStringsSafe(value);
    if (expanded.empty()) {
        return {};
    }
    if (PathIsRelativeW(expanded.c_str()) && expanded.find(L'\\') == std::wstring::npos && expanded.find(L'/') == std::wstring::npos) {
        wchar_t resolved[MAX_PATH]{};
        if (SearchPathW(nullptr, expanded.c_str(), nullptr, static_cast<DWORD>(sizeof(resolved) / sizeof(resolved[0])), resolved, nullptr) > 0) {
            return resolved;
        }
    }
    return expanded;
}

HICON ExtractIconFromPidlBlob(const std::vector<std::uint8_t>& pidl) {
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
        return info.hIcon;
    }
    return nullptr;
}

HICON ExtractIconFromShellParseName(const std::wstring& value) {
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
        icon = info.hIcon;
    }
    CoTaskMemFree(pidl);
    return icon;
}
}

IconService::IconService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

IconService::~IconService() {
    Clear();
    SafeRelease(wicFactory_);
}

bool IconService::Initialize() {
    if (wicFactory_) {
        return true;
    }
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_)));
}

ID2D1Bitmap* IconService::GetBitmap(ID2D1RenderTarget* renderTarget, const Link& link) {
    if (!renderTarget || !Initialize()) {
        return nullptr;
    }

    const std::wstring key = CacheKey(link);
    auto found = bitmapCache_.find(key);
    if (found != bitmapCache_.end()) {
        return found->second;
    }

    ID2D1Bitmap* bitmap = nullptr;

    if (LooksLikeUrl(link)) {
        const std::filesystem::path urlIcon = FindUrlIconFile(link);
        if (!urlIcon.empty()) {
            bitmap = LoadBitmapFile(renderTarget, urlIcon);
        }
    }

    const std::filesystem::path cachePath = CachePath(link);
    if (!bitmap && FileExists(cachePath)) {
        bitmap = LoadBitmapFile(renderTarget, cachePath);
    }

    if (!bitmap) {
        HICON icon = ExtractIconForLink(link);
        if (!icon) {
            icon = DuplicateIconHandle(LoadIconW(nullptr, IDI_APPLICATION));
        }
        if (icon) {
            std::error_code ec;
            std::filesystem::create_directories(cachePath.parent_path(), ec);
            SaveIconPng(icon, cachePath);
            CreateBitmapFromIcon(renderTarget, icon, &bitmap);
            DestroyIcon(icon);
        }
    }

    if (bitmap) {
        bitmapCache_[key] = bitmap;
    }
    return bitmap;
}

void IconService::Clear() {
    for (auto& [_, bitmap] : bitmapCache_) {
        SafeRelease(bitmap);
    }
    bitmapCache_.clear();
}

bool IconService::ClearDiskCache() {
    Clear();
    const std::filesystem::path cacheDirectory = appDirectory_ / L"icons" / L"cache";
    std::error_code ec;
    if (!std::filesystem::exists(cacheDirectory, ec)) {
        return true;
    }
    std::filesystem::remove_all(cacheDirectory, ec);
    if (ec) {
        return false;
    }
    std::filesystem::create_directories(cacheDirectory, ec);
    return !ec;
}

bool IconService::RefreshDiskCache(const Link& link) {
    const std::wstring key = CacheKey(link);
    auto found = bitmapCache_.find(key);
    if (found != bitmapCache_.end()) {
        SafeRelease(found->second);
        bitmapCache_.erase(found);
    }

    const std::filesystem::path cachePath = CachePath(link);
    std::error_code ec;
    if (std::filesystem::exists(cachePath, ec)) {
        std::filesystem::remove(cachePath, ec);
        return !ec;
    }
    return true;
}

ID2D1Bitmap* IconService::LoadBitmapFile(ID2D1RenderTarget* renderTarget, const std::filesystem::path& path) const {
    if (!wicFactory_ || !renderTarget || path.empty()) {
        return nullptr;
    }

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID2D1Bitmap* bitmap = nullptr;

    if (SUCCEEDED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(wicFactory_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut))) {
        renderTarget->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap);
    }

    SafeRelease(converter);
    SafeRelease(frame);
    SafeRelease(decoder);
    return bitmap;
}

bool IconService::SaveIconPng(HICON icon, const std::filesystem::path& path) const {
    if (!wicFactory_ || !icon || path.empty()) {
        return false;
    }

    IWICBitmap* bitmap = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;

    if (SUCCEEDED(wicFactory_->CreateBitmapFromHICON(icon, &bitmap)) &&
        SUCCEEDED(wicFactory_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            bitmap,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut)) &&
        SUCCEEDED(wicFactory_->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr))) {
        UINT width = 0;
        UINT height = 0;
        converter->GetSize(&width, &height);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(width, height)) &&
            SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WriteSource(converter, nullptr)) &&
            SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit())) {
            ok = true;
        }
    }

    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(converter);
    SafeRelease(bitmap);
    return ok;
}

HICON IconService::ExtractIconForLink(const Link& link) const {
    const MenuIcon menuIcon = SystemFunctionMenuIconForLink(link);
    if (MenuIconIsRenderable(menuIcon)) {
        if (HICON icon = CreateMenuIconHandle(menuIcon, appDirectory_)) {
            return icon;
        }
    }

    const std::wstring iconPath = Trim(link.icon);
    if (!iconPath.empty() && iconPath != L"#url" && iconPath != L"默认系统缓存图标") {
        const std::wstring resolvedIcon = ResolveExecutableLikePath(iconPath);
        SHFILEINFOW iconInfo{};
        if (SHGetFileInfoW(resolvedIcon.c_str(), 0, &iconInfo, sizeof(iconInfo), SHGFI_ICON | SHGFI_LARGEICON)) {
            return iconInfo.hIcon;
        }
    }

    if (LooksLikeUrl(link)) {
        if (HICON icon = StockIcon(SIID_WORLD)) {
            return icon;
        }
    }

    if (HICON icon = ExtractIconFromPidlBlob(link.pidl)) {
        return icon;
    }
    if (HICON icon = ExtractIconFromShellParseName(link.path)) {
        return icon;
    }

    std::wstring path = ResolveExecutableLikePath(link.path);
    SHFILEINFOW info{};
    DWORD flags = SHGFI_ICON | SHGFI_LARGEICON;
    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) || link.type == 1) {
        attrs = FILE_ATTRIBUTE_DIRECTORY;
    }
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        return info.hIcon;
    }
    flags |= SHGFI_USEFILEATTRIBUTES;
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        return info.hIcon;
    }
    if (link.type == 1) {
        if (HICON icon = StockIcon(SIID_FOLDER)) {
            return icon;
        }
    }
    if (link.type == 3 || link.type == 4) {
        if (HICON icon = StockIcon(SIID_APPLICATION)) {
            return icon;
        }
    }
    return nullptr;
}

std::filesystem::path IconService::FindUrlIconFile(const Link& link) const {
    const std::wstring host = UrlHost(link.path);
    if (host.empty()) {
        return {};
    }

    const std::filesystem::path iconDir = appDirectory_ / L"icons" / L"url";
    const std::array<std::filesystem::path, 4> candidates = {
        iconDir / (host + L".png"),
        iconDir / (host + L".ico"),
        iconDir / (ToLower(host) + L".png"),
        iconDir / (ToLower(host) + L".ico"),
    };

    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool IconService::CreateBitmapFromIcon(ID2D1RenderTarget* renderTarget, HICON icon, ID2D1Bitmap** bitmap) const {
    if (!wicFactory_ || !renderTarget || !icon || !bitmap) {
        return false;
    }

    IWICBitmap* wicBitmap = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(wicFactory_->CreateBitmapFromHICON(icon, &wicBitmap)) &&
        SUCCEEDED(wicFactory_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            wicBitmap,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut)) &&
        SUCCEEDED(renderTarget->CreateBitmapFromWicBitmap(converter, nullptr, bitmap))) {
        ok = true;
    }
    SafeRelease(converter);
    SafeRelease(wicBitmap);
    return ok;
}

std::wstring IconService::CacheKey(const Link& link) const {
    return std::to_wstring(link.id) + L"|" + ToLower(link.path) + L"|" + LinkIconCacheToken(link);
}

std::filesystem::path IconService::CachePath(const Link& link) const {
    const std::wstring hash = Hex8(StablePathHash(ToLower(link.path + L"|" + LinkIconCacheToken(link))));
    const std::wstring prefix = link.id > 0 ? (L"link_" + std::to_wstring(link.id)) : L"link";
    return appDirectory_ / L"icons" / L"cache" / (prefix + L"_" + hash + L"_32.png");
}
