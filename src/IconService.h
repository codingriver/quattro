#pragma once

#include "Models.h"

#include <d2d1.h>
#include <wincodec.h>

#include <filesystem>
#include <string>
#include <unordered_map>

class IconService {
public:
    explicit IconService(std::filesystem::path appDirectory);
    ~IconService();

    bool Initialize();
    ID2D1Bitmap* GetBitmap(ID2D1RenderTarget* renderTarget, const Link& link);
    void Clear();
    bool ClearDiskCache();
    bool RefreshDiskCache(const Link& link);

private:
    ID2D1Bitmap* LoadBitmapFile(ID2D1RenderTarget* renderTarget, const std::filesystem::path& path) const;
    bool SaveIconPng(HICON icon, const std::filesystem::path& path) const;
    HICON ExtractIconForLink(const Link& link) const;
    std::filesystem::path FindUrlIconFile(const Link& link) const;
    bool CreateBitmapFromIcon(ID2D1RenderTarget* renderTarget, HICON icon, ID2D1Bitmap** bitmap) const;
    std::wstring CacheKey(const Link& link) const;
    std::filesystem::path CachePath(const Link& link) const;

    std::filesystem::path appDirectory_;
    IWICImagingFactory* wicFactory_ = nullptr;
    std::unordered_map<std::wstring, ID2D1Bitmap*> bitmapCache_;
};
