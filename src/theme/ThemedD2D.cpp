#include "ThemedD2D.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dxgiformat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace ThemedD2D {
namespace {

bool FallbackForced() {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"QUATTRO_FORCE_GDI_FALLBACK", value, static_cast<DWORD>(std::size(value)));
    return length > 0 && value[0] != L'0';
}

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::uint32_t ColorKey(COLORREF color) {
    return (static_cast<std::uint32_t>(GetRValue(color)) << 16) |
           (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
           static_cast<std::uint32_t>(GetBValue(color));
}

D2D1_COLOR_F D2DColor(COLORREF color) {
    return D2D1::ColorF(
        static_cast<float>(GetRValue(color)) / 255.0f,
        static_cast<float>(GetGValue(color)) / 255.0f,
        static_cast<float>(GetBValue(color)) / 255.0f,
        1.0f);
}

struct TextFormatEntry {
    IDWriteTextFormat* format = nullptr;
};

class Runtime final {
public:
    ~Runtime() {
        DiscardDeviceResources();
        for (auto& [_, entry] : textFormats_) {
            SafeRelease(entry.format);
        }
        textFormats_.clear();
        for (auto& [_, face] : fontFaces_) {
            SafeRelease(face);
        }
        fontFaces_.clear();
        SafeRelease(dwriteFactory_);
        SafeRelease(d2dFactory_);
    }

    bool EnsureFactories() {
        if (factoryAttempted_) {
            return d2dFactory_ && dwriteFactory_;
        }
        factoryAttempted_ = true;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) {
            return false;
        }
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&dwriteFactory_)))) {
            SafeRelease(d2dFactory_);
            return false;
        }
        return true;
    }

    bool Bind(HWND hwnd, HDC dc, SurfaceKind surface, RECT& boundRect) {
        if (!dc || FallbackForced() || !EnsureFactories()) {
            return false;
        }
        // Real window DCs must bind the full client surface. BeginPaint can
        // expose only the current update-region bounds through GetClipBox;
        // rebinding a DCRenderTarget to that smaller box may clear the rest of
        // the window on some drivers. Memory-DC screenshot probes have no HWND
        // and remain authoritative through their explicit clip box.
        RECT rect{};
        HWND dcWindow = WindowFromDC(dc);
        if (dcWindow) {
            if (!GetClientRect(dcWindow, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
                return false;
            }
        } else if (GetClipBox(dc, &rect) == ERROR || rect.right <= rect.left || rect.bottom <= rect.top) {
            return false;
        }
        HWND surfaceWindow = WindowFromDC(dc);
        if (!surfaceWindow) surfaceWindow = hwnd;
        const bool changedSurface = target_ && targetSurface_ != surface;
        const bool changedWindow = target_ && surfaceWindow != boundWindow_ && (surfaceWindow || boundWindow_);
        // A new BeginPaint/PrintWindow surface may reuse the same numeric HWND
        // after the previous window was destroyed. Treat a changed HDC as a
        // new target even when WindowFromDC reports the same HWND value; stale
        // DCRenderTarget state otherwise leaves part of the new surface black.
        const bool changedDc = target_ && boundDc_ && boundDc_ != dc;
        const bool changedRect = target_ &&
            (rect.left != boundRect_.left || rect.top != boundRect_.top ||
             rect.right != boundRect_.right || rect.bottom != boundRect_.bottom);
        if (changedSurface || changedWindow || changedDc || changedRect) {
            DiscardDeviceResources();
        }
        if (!target_) {
            const D2D1_ALPHA_MODE alphaMode = surface == SurfaceKind::Transparent
                ? D2D1_ALPHA_MODE_PREMULTIPLIED
                : D2D1_ALPHA_MODE_IGNORE;
            const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode),
                96.0f,
                96.0f,
                D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE,
                D2D1_FEATURE_LEVEL_DEFAULT);
            if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties, &target_))) {
                return false;
            }
            targetSurface_ = surface;
        }

        if (FAILED(target_->BindDC(dc, &rect))) {
            DiscardDeviceResources();
            return false;
        }
        boundWindow_ = surfaceWindow;
        boundDc_ = dc;
        boundRect_ = rect;
        boundRect = rect;
        return true;
    }

    ID2D1DCRenderTarget* target() const { return target_; }
    ID2D1Factory* d2dFactory() const { return d2dFactory_; }
    IDWriteFactory* dwriteFactory() const { return dwriteFactory_; }

    ID2D1SolidColorBrush* Brush(COLORREF color) {
        if (!target_) return nullptr;
        const std::uint32_t key = ColorKey(color);
        auto found = brushes_.find(key);
        if (found != brushes_.end()) {
            return found->second;
        }
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(target_->CreateSolidColorBrush(D2DColor(color), &brush)) || !brush) {
            return nullptr;
        }
        brushes_[key] = brush;
        return brush;
    }

    ID2D1StrokeStyle* RoundedStrokeStyle() {
        if (roundedStrokeStyle_ || !d2dFactory_) return roundedStrokeStyle_;
        const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND,
            10.0f,
            D2D1_DASH_STYLE_SOLID,
            0.0f);
        d2dFactory_->CreateStrokeStyle(properties, nullptr, 0, &roundedStrokeStyle_);
        return roundedStrokeStyle_;
    }

    IDWriteTextFormat* TextFormat(HFONT font) {
        LOGFONTW logicalFont{};
        if (!font || GetObjectW(font, sizeof(logicalFont), &logicalFont) != sizeof(logicalFont)) {
            logicalFont.lfHeight = -14;
            logicalFont.lfWeight = FW_NORMAL;
            wcscpy_s(logicalFont.lfFaceName, L"Microsoft YaHei UI");
        }
        const float size = static_cast<float>(std::max(1L, std::abs(logicalFont.lfHeight)));
        const std::wstring face = logicalFont.lfFaceName[0] ? logicalFont.lfFaceName : L"Microsoft YaHei UI";
        const std::wstring key = face + L"|" + std::to_wstring(static_cast<int>(size * 10.0f)) + L"|" +
            std::to_wstring(logicalFont.lfWeight) + L"|" + std::to_wstring(logicalFont.lfItalic != FALSE);
        auto found = textFormats_.find(key);
        if (found != textFormats_.end()) {
            return found->second.format;
        }
        IDWriteTextFormat* format = nullptr;
        const DWRITE_FONT_WEIGHT weight = static_cast<DWRITE_FONT_WEIGHT>(
            std::clamp<LONG>(logicalFont.lfWeight, DWRITE_FONT_WEIGHT_THIN, DWRITE_FONT_WEIGHT_ULTRA_BLACK));
        const DWRITE_FONT_STYLE style = logicalFont.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
        if (FAILED(dwriteFactory_->CreateTextFormat(
                face.c_str(),
                nullptr,
                weight,
                style,
                DWRITE_FONT_STRETCH_NORMAL,
                size,
                L"zh-cn",
                &format)) || !format) {
            return nullptr;
        }
        textFormats_[key] = TextFormatEntry{format};
        return format;
    }

    IDWriteFontFace* FontFaceFromFile(const wchar_t* path) {
        if (!path || !*path || !EnsureFactories()) return nullptr;
        const std::wstring key(path);
        auto found = fontFaces_.find(key);
        if (found != fontFaces_.end()) return found->second;

        IDWriteFontFile* file = nullptr;
        if (FAILED(dwriteFactory_->CreateFontFileReference(path, nullptr, &file)) || !file) {
            return nullptr;
        }
        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
        DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
        UINT32 faceCount = 0;
        const HRESULT analyze = file->Analyze(&supported, &fileType, &faceType, &faceCount);
        if (FAILED(analyze) || !supported || faceCount == 0) {
            SafeRelease(file);
            return nullptr;
        }

        IDWriteFontFace* face = nullptr;
        IDWriteFontFile* files[]{file};
        const HRESULT created = dwriteFactory_->CreateFontFace(
            faceType, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE, &face);
        SafeRelease(file);
        if (FAILED(created) || !face) return nullptr;
        fontFaces_[key] = face;
        return face;
    }

    void DiscardDeviceResources() {
        for (auto& [_, brush] : brushes_) {
            SafeRelease(brush);
        }
        brushes_.clear();
        SafeRelease(roundedStrokeStyle_);
        SafeRelease(target_);
        boundWindow_ = nullptr;
        boundDc_ = nullptr;
        boundRect_ = {};
    }

private:
    bool factoryAttempted_ = false;
    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1DCRenderTarget* target_ = nullptr;
    HWND boundWindow_ = nullptr;
    HDC boundDc_ = nullptr;
    RECT boundRect_{};
    SurfaceKind targetSurface_ = SurfaceKind::Opaque;
    ID2D1StrokeStyle* roundedStrokeStyle_ = nullptr;
    std::unordered_map<std::uint32_t, ID2D1SolidColorBrush*> brushes_;
    std::unordered_map<std::wstring, TextFormatEntry> textFormats_;
    std::unordered_map<std::wstring, IDWriteFontFace*> fontFaces_;
};

thread_local Runtime g_runtime;
thread_local ScopedHdcPaint::State* g_activePaint = nullptr;

D2D1_RECT_F RectF(const RECT& rect) {
    return D2D1::RectF(
        static_cast<float>(rect.left),
        static_cast<float>(rect.top),
        static_cast<float>(rect.right),
        static_cast<float>(rect.bottom));
}

float ClampedRadius(const RECT& rect, float radius) {
    const float width = static_cast<float>(std::max<LONG>(0, rect.right - rect.left));
    const float height = static_cast<float>(std::max<LONG>(0, rect.bottom - rect.top));
    return std::max(0.0f, std::min(radius, std::min(width, height) * 0.5f));
}

} // namespace

struct ScopedHdcPaint::State {
    HDC dc = nullptr;
    ID2D1DCRenderTarget* target = nullptr;
    RECT boundRect{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
};

ScopedHdcPaint::ScopedHdcPaint(HWND hwnd, HDC dc, SurfaceKind surface) {
    previous_ = g_activePaint;
    if (previous_ && previous_->dc == dc) {
        state_ = previous_;
        ready_ = true;
        return;
    }

    state_ = new State{};
    RECT bound{};
    if (!g_runtime.Bind(hwnd, dc, surface, bound)) {
        delete state_;
        state_ = nullptr;
        return;
    }
    state_->dc = dc;
    state_->target = g_runtime.target();
    state_->boundRect = bound;
    state_->dpi = DpiFor(hwnd, dc);
    state_->target->SetTransform(D2D1::Matrix3x2F::Translation(
        -static_cast<float>(bound.left),
        -static_cast<float>(bound.top)));
    state_->target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    state_->target->SetTextAntialiasMode(
        surface == SurfaceKind::Transparent ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    state_->target->BeginDraw();
    g_activePaint = state_;
    ready_ = true;
    ownsDraw_ = true;
}

ScopedHdcPaint::~ScopedHdcPaint() {
    if (!ownsDraw_) {
        return;
    }
    g_activePaint = previous_;
    if (state_ && state_->target) {
        state_->target->SetTransform(D2D1::Matrix3x2F::Identity());
        const HRESULT hr = state_->target->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            g_runtime.DiscardDeviceResources();
        }
    }
    delete state_;
}

bool IsActive(HDC dc) {
    return g_activePaint && g_activePaint->dc == dc && g_activePaint->target;
}

UINT ActiveDpi() {
    return g_activePaint && g_activePaint->dpi ? g_activePaint->dpi : USER_DEFAULT_SCREEN_DPI;
}

bool IsAvailable() {
    return !FallbackForced() && g_runtime.EnsureFactories();
}

void DiscardThreadDeviceResources() {
    g_runtime.DiscardDeviceResources();
}

UINT DpiFor(HWND hwnd, HDC dc) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (!dpi && dc) {
        if (HWND dcWindow = WindowFromDC(dc)) {
            dpi = GetDpiForWindow(dcWindow);
        }
    }
    if (!dpi && dc) {
        dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    }
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

int ScaleDip(int logicalPixels, UINT dpi) {
    return MulDiv(logicalPixels, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

float ScaleDip(float logicalPixels, UINT dpi) {
    return logicalPixels * static_cast<float>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI) /
        static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

bool FillRoundedRect(HDC dc, RECT rect, float radius, COLORREF fill, COLORREF border, float borderWidth) {
    if (!IsActive(dc)) return false;
    ID2D1SolidColorBrush* fillBrush = g_runtime.Brush(fill);
    if (!fillBrush) return false;
    const float stroke = std::max(0.0f, borderWidth);
    D2D1_RECT_F bounds = RectF(rect);
    if (stroke > 0.0f) {
        const float inset = stroke * 0.5f;
        bounds.left += inset;
        bounds.top += inset;
        bounds.right -= inset;
        bounds.bottom -= inset;
    }
    const float r = ClampedRadius(rect, radius);
    g_activePaint->target->FillRoundedRectangle(D2D1::RoundedRect(bounds, r, r), fillBrush);
    if (stroke > 0.0f) {
        if (ID2D1SolidColorBrush* borderBrush = g_runtime.Brush(border)) {
            g_activePaint->target->DrawRoundedRectangle(
                D2D1::RoundedRect(bounds, std::max(0.0f, r - stroke * 0.5f), std::max(0.0f, r - stroke * 0.5f)),
                borderBrush,
                stroke);
        }
    }
    return true;
}

bool FillRect(HDC dc, RECT rect, COLORREF fill) {
    if (!IsActive(dc)) return false;
    ID2D1SolidColorBrush* brush = g_runtime.Brush(fill);
    if (!brush) return false;
    g_activePaint->target->FillRectangle(RectF(rect), brush);
    return true;
}

bool DrawRoundedRect(HDC dc, RECT rect, float radius, COLORREF border, float borderWidth) {
    if (!IsActive(dc)) return false;
    ID2D1SolidColorBrush* brush = g_runtime.Brush(border);
    if (!brush) return false;
    const float stroke = std::max(1.0f, borderWidth);
    D2D1_RECT_F bounds = RectF(rect);
    const float inset = stroke * 0.5f;
    bounds.left += inset;
    bounds.top += inset;
    bounds.right -= inset;
    bounds.bottom -= inset;
    const float r = std::max(0.0f, ClampedRadius(rect, radius) - inset);
    g_activePaint->target->DrawRoundedRectangle(D2D1::RoundedRect(bounds, r, r), brush, stroke);
    return true;
}

bool FillEllipse(HDC dc, RECT rect, COLORREF fill, COLORREF border, float borderWidth) {
    if (!IsActive(dc)) return false;
    ID2D1SolidColorBrush* fillBrush = g_runtime.Brush(fill);
    if (!fillBrush) return false;
    const float stroke = std::max(0.0f, borderWidth);
    const float cx = (static_cast<float>(rect.left) + static_cast<float>(rect.right)) * 0.5f;
    const float cy = (static_cast<float>(rect.top) + static_cast<float>(rect.bottom)) * 0.5f;
    const float rx = std::max(0.0f, (static_cast<float>(rect.right - rect.left) - stroke) * 0.5f);
    const float ry = std::max(0.0f, (static_cast<float>(rect.bottom - rect.top) - stroke) * 0.5f);
    const D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry);
    g_activePaint->target->FillEllipse(ellipse, fillBrush);
    if (stroke > 0.0f) {
        if (ID2D1SolidColorBrush* borderBrush = g_runtime.Brush(border)) {
            g_activePaint->target->DrawEllipse(ellipse, borderBrush, stroke);
        }
    }
    return true;
}

bool DrawLine(
    HDC dc,
    float x1,
    float y1,
    float x2,
    float y2,
    COLORREF color,
    float strokeWidth,
    bool pixelSnap) {
    if (!IsActive(dc)) return false;
    ID2D1SolidColorBrush* brush = g_runtime.Brush(color);
    if (!brush) return false;
    const float stroke = std::max(1.0f, strokeWidth);
    if (pixelSnap) {
        const bool odd = (static_cast<int>(std::round(stroke)) & 1) != 0;
        const float offset = odd ? 0.5f : 0.0f;
        if (std::abs(x1 - x2) < 0.01f) x1 = x2 = std::floor(x1) + offset;
        if (std::abs(y1 - y2) < 0.01f) y1 = y2 = std::floor(y1) + offset;
    }
    g_activePaint->target->DrawLine(
        D2D1::Point2F(x1, y1),
        D2D1::Point2F(x2, y2),
        brush,
        stroke,
        g_runtime.RoundedStrokeStyle());
    return true;
}

bool DrawPolyline(HDC dc, const POINT* points, int pointCount, COLORREF color, float strokeWidth) {
    if (!IsActive(dc) || !points || pointCount < 2) return false;
    ID2D1SolidColorBrush* brush = g_runtime.Brush(color);
    if (!brush) return false;
    ID2D1PathGeometry* geometry = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(g_runtime.d2dFactory()->CreatePathGeometry(&geometry)) || !geometry ||
        FAILED(geometry->Open(&sink)) || !sink) {
        SafeRelease(sink);
        SafeRelease(geometry);
        return false;
    }
    sink->BeginFigure(
        D2D1::Point2F(static_cast<float>(points[0].x), static_cast<float>(points[0].y)),
        D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i < pointCount; ++i) {
        sink->AddLine(D2D1::Point2F(static_cast<float>(points[i].x), static_cast<float>(points[i].y)));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    const HRESULT closeResult = sink->Close();
    SafeRelease(sink);
    if (SUCCEEDED(closeResult)) {
        g_activePaint->target->DrawGeometry(
            geometry,
            brush,
            std::max(1.0f, strokeWidth),
            g_runtime.RoundedStrokeStyle());
    }
    SafeRelease(geometry);
    return SUCCEEDED(closeResult);
}

bool DrawIcon(HDC dc, HICON icon, RECT destination, bool disabled) {
    if (!IsActive(dc) || !icon) return false;
    const int width = destination.right - destination.left;
    const int height = destination.bottom - destination.top;
    if (width <= 0 || height <= 0) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = memory
        ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
        : nullptr;
    if (!memory || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        return false;
    }
    std::memset(bits, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    const BOOL drawn = DrawIconEx(memory, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL);

    auto* pixels = static_cast<std::uint32_t*>(bits);
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    bool hasAlpha = false;
    if (drawn) {
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const std::uint32_t alpha = pixels[index] >> 24;
            hasAlpha = hasAlpha || alpha != 0;
            if (alpha > 0 && alpha < 255) {
                const std::uint32_t blue = pixels[index] & 0xffu;
                const std::uint32_t green = (pixels[index] >> 8) & 0xffu;
                const std::uint32_t red = (pixels[index] >> 16) & 0xffu;
                pixels[index] = (alpha << 24) |
                    (((red * alpha + 127) / 255) << 16) |
                    (((green * alpha + 127) / 255) << 8) |
                    ((blue * alpha + 127) / 255);
            }
        }
    }

    ID2D1Bitmap* d2dBitmap = nullptr;
    bool rendered = false;
    if (drawn && hasAlpha && SUCCEEDED(g_activePaint->target->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
            pixels,
            static_cast<UINT32>(width * 4),
            D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f,
                96.0f),
            &d2dBitmap)) && d2dBitmap) {
        g_activePaint->target->DrawBitmap(
            d2dBitmap,
            RectF(destination),
            disabled ? 0.42f : 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rendered = true;
    }
    SafeRelease(d2dBitmap);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return rendered;
}

bool DrawBitmap(HDC dc, HBITMAP bitmap, RECT destination, float opacity) {
    if (!IsActive(dc) || !bitmap) return false;
    BITMAP source{};
    if (GetObjectW(bitmap, sizeof(source), &source) != sizeof(source) ||
        source.bmWidth <= 0 || source.bmHeight == 0) {
        return false;
    }
    const int width = source.bmWidth;
    const int height = std::abs(source.bmHeight);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    HDC memory = CreateCompatibleDC(dc);
    if (!memory || GetDIBits(
            memory, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS) == 0) {
        if (memory) DeleteDC(memory);
        return false;
    }
    DeleteDC(memory);

    ID2D1Bitmap* d2dBitmap = nullptr;
    const HRESULT hr = g_activePaint->target->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
        pixels.data(),
        static_cast<UINT32>(width * 4),
        D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f),
        &d2dBitmap);
    if (FAILED(hr) || !d2dBitmap) return false;
    g_activePaint->target->DrawBitmap(
        d2dBitmap,
        RectF(destination),
        std::clamp(opacity, 0.0f, 1.0f),
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    SafeRelease(d2dBitmap);
    return true;
}

bool DrawGlyphFromFontFile(
    HDC dc,
    const wchar_t* fontPath,
    wchar_t glyph,
    const RECT& rect,
    COLORREF color,
    float emSize) {
    if (!IsActive(dc) || !fontPath || !*fontPath || glyph == L'\0' || emSize <= 0.0f) {
        return false;
    }
    IDWriteFontFace* face = g_runtime.FontFaceFromFile(fontPath);
    ID2D1SolidColorBrush* brush = g_runtime.Brush(color);
    if (!face || !brush) return false;

    const UINT32 codePoint = static_cast<UINT32>(glyph);
    UINT16 glyphIndex = 0;
    if (FAILED(face->GetGlyphIndicesW(&codePoint, 1, &glyphIndex)) || glyphIndex == 0) {
        return false;
    }

    DWRITE_FONT_METRICS fontMetrics{};
    DWRITE_GLYPH_METRICS glyphMetrics{};
    face->GetMetrics(&fontMetrics);
    if (fontMetrics.designUnitsPerEm == 0 ||
        FAILED(face->GetDesignGlyphMetrics(&glyphIndex, 1, &glyphMetrics, FALSE))) {
        return false;
    }
    const float scale = emSize / static_cast<float>(fontMetrics.designUnitsPerEm);
    const float inkWidth = std::max(
        0.0f,
        static_cast<float>(glyphMetrics.advanceWidth - glyphMetrics.leftSideBearing - glyphMetrics.rightSideBearing) * scale);
    const float inkHeight = std::max(
        0.0f,
        static_cast<float>(glyphMetrics.advanceHeight - glyphMetrics.topSideBearing - glyphMetrics.bottomSideBearing) * scale);
    const float centerX = (static_cast<float>(rect.left) + static_cast<float>(rect.right)) * 0.5f;
    const float centerY = (static_cast<float>(rect.top) + static_cast<float>(rect.bottom)) * 0.5f;
    const D2D1_POINT_2F baseline{
        centerX - inkWidth * 0.5f - static_cast<float>(glyphMetrics.leftSideBearing) * scale,
        centerY - inkHeight * 0.5f +
            static_cast<float>(glyphMetrics.verticalOriginY - glyphMetrics.topSideBearing) * scale};
    const DWRITE_GLYPH_RUN run{
        face,
        emSize,
        1,
        &glyphIndex,
        nullptr,
        nullptr,
        FALSE,
        0};
    g_activePaint->target->DrawGlyphRun(
        baseline, &run, brush, DWRITE_MEASURING_MODE_NATURAL);
    return true;
}

bool DrawTextLayout(
    HDC dc,
    HFONT font,
    const wchar_t* text,
    int textLength,
    const RECT& rect,
    UINT formatFlags,
    COLORREF color) {
    if (!IsActive(dc) || !text) return false;
    if (textLength < 0) textLength = static_cast<int>(wcslen(text));
    if (textLength == 0 || rect.right <= rect.left || rect.bottom <= rect.top) return true;
    IDWriteTextFormat* format = g_runtime.TextFormat(font);
    ID2D1SolidColorBrush* brush = g_runtime.Brush(color);
    if (!format || !brush) return false;

    format->SetTextAlignment((formatFlags & DT_CENTER) != 0
        ? DWRITE_TEXT_ALIGNMENT_CENTER
        : ((formatFlags & DT_RIGHT) != 0 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING));
    format->SetParagraphAlignment((formatFlags & DT_VCENTER) != 0
        ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
        : ((formatFlags & DT_BOTTOM) != 0 ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
    format->SetWordWrapping((formatFlags & DT_WORDBREAK) != 0
        ? DWRITE_WORD_WRAPPING_WRAP
        : DWRITE_WORD_WRAPPING_NO_WRAP);

    IDWriteInlineObject* ellipsis = nullptr;
    if ((formatFlags & DT_END_ELLIPSIS) != 0 &&
        SUCCEEDED(g_runtime.dwriteFactory()->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        format->SetTrimming(&trimming, ellipsis);
    } else {
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_NONE;
        format->SetTrimming(&trimming, nullptr);
    }

    IDWriteTextLayout* layout = nullptr;
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    const HRESULT createResult = g_runtime.dwriteFactory()->CreateTextLayout(
        text,
        static_cast<UINT32>(textLength),
        format,
        std::max(1.0f, width),
        std::max(1.0f, height),
        &layout);
    if (SUCCEEDED(createResult) && layout) {
        g_activePaint->target->DrawTextLayout(
            D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)),
            layout,
            brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    SafeRelease(layout);
    SafeRelease(ellipsis);
    return SUCCEEDED(createResult);
}

int MeasureTextWidth(HFONT font, const std::wstring& text) {
    return MeasureText(font, text, 100000, false).cx;
}

SIZE MeasureText(HFONT font, const std::wstring& text, int maxWidth, bool wrap) {
    if (text.empty() || FallbackForced() || !g_runtime.EnsureFactories()) return SIZE{};
    IDWriteTextFormat* format = g_runtime.TextFormat(font);
    if (!format) return SIZE{};
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_NONE;
    format->SetTrimming(&trimming, nullptr);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_runtime.dwriteFactory()->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            static_cast<float>(std::max(1, maxWidth)),
            1000.0f,
            &layout)) || !layout) {
        return SIZE{};
    }
    DWRITE_TEXT_METRICS metrics{};
    const HRESULT result = layout->GetMetrics(&metrics);
    SafeRelease(layout);
    if (FAILED(result)) return SIZE{};
    return SIZE{
        static_cast<LONG>(std::ceil(metrics.widthIncludingTrailingWhitespace)),
        static_cast<LONG>(std::ceil(metrics.height))};
}

} // namespace ThemedD2D
