#include "ThemedUi.h"

#include "TablerIconManifest.h"
#include "Utilities.h"

#include <algorithm>
#include <array>
#include <filesystem>

namespace {
std::filesystem::path ResolveFontPath(const std::filesystem::path& appDirectory) {
    const std::filesystem::path relative = L"icons/menu/tabler/tabler-icons.ttf";
    if (!appDirectory.empty()) {
        return appDirectory / relative;
    }
    const std::filesystem::path modulePath = GetModuleDirectory() / relative;
    if (FileExists(modulePath)) {
        return modulePath;
    }
    return std::filesystem::current_path() / relative;
}
}

bool EnsureTablerIconFontLoaded(const std::filesystem::path& appDirectory) {
    static std::filesystem::path loadedPath;
    static bool loaded = false;

    const std::filesystem::path fontPath = ResolveFontPath(appDirectory);
    if (loaded && loadedPath == fontPath) {
        return true;
    }
    loadedPath = fontPath;
    loaded = FileExists(fontPath) && AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) > 0;
    return loaded;
}

wchar_t TablerIconGlyph(TablerIconId id) {
    return TablerIconManifest::Glyph(id);
}

bool DrawTablerIcon(
    HDC dc,
    const RECT& rect,
    const std::filesystem::path& appDirectory,
    TablerIconId id,
    COLORREF color,
    int fontHeight) {
    if (!dc || !EnsureTablerIconFontLoaded(appDirectory)) {
        return false;
    }
    const wchar_t glyph = TablerIconGlyph(id);
    if (glyph == L'\0') {
        return false;
    }

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));
    const int resolvedHeight = fontHeight > 0 ? fontHeight : std::max(12, std::min(width, height) + 1);
    HFONT font = CreateFontW(
        -resolvedHeight,
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
        TablerIconManifest::kFontFamily);
    if (!font) {
        return false;
    }

    const int oldBkMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, color);
    HGDIOBJ oldFont = SelectObject(dc, font);
    wchar_t text[2] = {glyph, L'\0'};
    RECT drawRect = rect;
    DrawTextW(dc, text, 1, &drawRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOCLIP);
    SelectObject(dc, oldFont);
    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBkMode);
    DeleteObject(font);
    return true;
}

HICON CreateTablerIconHandle(
    const std::filesystem::path& appDirectory,
    TablerIconId id,
    int fontHeight,
    COLORREF color) {
    constexpr int size = 64;
    if (!EnsureTablerIconFontLoaded(appDirectory) || TablerIconGlyph(id) == L'\0') {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!bitmap || !mask || !dc || !pixels) {
        if (dc) DeleteDC(dc);
        if (mask) DeleteObject(mask);
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    std::fill_n(static_cast<std::uint32_t*>(pixels), size * size, 0);
    RECT rect{0, 0, size, size};
    if (!DrawTablerIcon(dc, rect, appDirectory, id, color, fontHeight)) {
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);
        DeleteObject(mask);
        DeleteObject(bitmap);
        return nullptr;
    }
    auto* argb = static_cast<std::uint32_t*>(pixels);
    for (int i = 0; i < size * size; ++i) {
        if ((argb[i] & 0x00FFFFFFu) != 0) {
            argb[i] |= 0xFF000000u;
        }
    }

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = bitmap;
    iconInfo.hbmMask = mask;
    HICON icon = CreateIconIndirect(&iconInfo);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(mask);
    DeleteObject(bitmap);
    return icon;
}
