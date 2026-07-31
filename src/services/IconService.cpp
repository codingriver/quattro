#include "IconService.h"

#include "IconResolverService.h"
#include "MenuCatalog.h"
#include "ShellItemService.h"
#include "SystemFunctions.h"
#include "ThemedUi.h"
#include "Utilities.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>

namespace {
constexpr wchar_t kResolverCacheVersion[] = L"resolver-v11";

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::wstring LinkIconCacheToken(const Link& link) {
    const MenuIcon icon = SystemFunctionMenuIconForLink(link);
    if (MenuIconIsRenderable(icon)) {
        return MenuIconLinkIconValue(icon);
    }
    return link.icon;
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

    auto prepared = preparedIconCache_.find(key);
    if (prepared != preparedIconCache_.end()) {
        CreateBitmapFromResolvedIcon(renderTarget, prepared->second, &bitmap);
        preparedIconCache_.erase(prepared);
    }

    if (!bitmap) {
        const ResolvedIcon icon = IconResolverService(appDirectory_).Resolve(
            IconResolverService::ForLink(link, 64));
        CreateBitmapFromResolvedIcon(renderTarget, icon, &bitmap);
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
    preparedIconCache_.clear();
}

bool IconService::ClearDiskCache() {
    Clear();
    std::error_code ec;
    const std::filesystem::path legacyCacheDirectory = appDirectory_ / L"icons" / L"cache";
    const std::filesystem::path sharedCacheDirectory = appDirectory_ / L"cache" / L"icons";
    bool ok = true;
    if (std::filesystem::exists(legacyCacheDirectory, ec)) {
        std::filesystem::remove_all(legacyCacheDirectory, ec);
        ok = ok && !ec;
        ec.clear();
    }
    if (std::filesystem::exists(sharedCacheDirectory, ec)) {
        std::filesystem::remove_all(sharedCacheDirectory, ec);
        ok = ok && !ec;
        ec.clear();
    }
    std::filesystem::create_directories(IconResolverService::DefaultCacheDirectory(appDirectory_), ec);
    return ok && !ec;
}

bool IconService::RefreshDiskCache(const Link& link) {
    InvalidateMemoryCache(link);
    return true;
}

void IconService::InvalidateMemoryCache(const Link& link) {
    const std::wstring key = CacheKey(link);
    auto found = bitmapCache_.find(key);
    if (found != bitmapCache_.end()) {
        SafeRelease(found->second);
        bitmapCache_.erase(found);
    }
    preparedIconCache_.erase(key);
}

bool IconService::ApplyPreparedRefresh(const Link& link, ResolvedIcon icon) {
    if (!IconResolverService::HasPixels(icon)) {
        return false;
    }
    const std::wstring key = CacheKey(link);
    auto found = bitmapCache_.find(key);
    if (found != bitmapCache_.end()) {
        SafeRelease(found->second);
        bitmapCache_.erase(found);
    }
    preparedIconCache_[key] = std::move(icon);
    return true;
}

bool IconService::CreateBitmapFromResolvedIcon(
    ID2D1RenderTarget* renderTarget,
    const ResolvedIcon& icon,
    ID2D1Bitmap** bitmap) const {
    if (!renderTarget || !bitmap || !IconResolverService::HasPixels(icon)) {
        return false;
    }
    D2D1_BITMAP_PROPERTIES properties{};
    properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    const HRESULT hr = renderTarget->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(icon.width), static_cast<UINT32>(icon.height)),
        icon.pixels.data(),
        static_cast<UINT32>(icon.width * sizeof(std::uint32_t)),
        &properties,
        bitmap);
    return SUCCEEDED(hr);
}

std::wstring IconService::CacheKey(const Link& link) const {
    return std::wstring(kResolverCacheVersion) + L"|" + std::to_wstring(link.id) + L"|" +
        ToLower(link.path) + L"|" + LinkIconCacheToken(link);
}
