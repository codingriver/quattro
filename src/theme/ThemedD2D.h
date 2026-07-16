#pragma once

#include <windows.h>

#include <string>

namespace ThemedD2D {

enum class SurfaceKind {
    Opaque,
    Transparent,
};

// Binds the current paint HDC to the shared Direct2D DC render target. Public
// controls keep their native HWND and message behavior; only their visible
// surface is rendered through this scoped context.
class ScopedHdcPaint final {
public:
    struct State;

    ScopedHdcPaint(HWND hwnd, HDC dc, SurfaceKind surface = SurfaceKind::Opaque);
    ~ScopedHdcPaint();

    ScopedHdcPaint(const ScopedHdcPaint&) = delete;
    ScopedHdcPaint& operator=(const ScopedHdcPaint&) = delete;

    bool ready() const { return ready_; }

private:
    State* state_ = nullptr;
    State* previous_ = nullptr;
    bool ready_ = false;
    bool ownsDraw_ = false;
};

bool IsActive(HDC dc);
UINT ActiveDpi();
bool IsAvailable();
void DiscardThreadDeviceResources();

UINT DpiFor(HWND hwnd, HDC dc = nullptr);
int ScaleDip(int logicalPixels, UINT dpi);
float ScaleDip(float logicalPixels, UINT dpi);

bool FillRoundedRect(
    HDC dc,
    RECT rect,
    float radius,
    COLORREF fill,
    COLORREF border,
    float borderWidth);

bool FillRect(HDC dc, RECT rect, COLORREF fill);

bool DrawRoundedRect(
    HDC dc,
    RECT rect,
    float radius,
    COLORREF border,
    float borderWidth);

bool FillEllipse(
    HDC dc,
    RECT rect,
    COLORREF fill,
    COLORREF border,
    float borderWidth);

bool DrawLine(
    HDC dc,
    float x1,
    float y1,
    float x2,
    float y2,
    COLORREF color,
    float strokeWidth,
    bool pixelSnap = false);

bool DrawPolyline(
    HDC dc,
    const POINT* points,
    int pointCount,
    COLORREF color,
    float strokeWidth);

bool DrawIcon(HDC dc, HICON icon, RECT destination, bool disabled = false);

bool DrawTextLayout(
    HDC dc,
    HFONT font,
    const wchar_t* text,
    int textLength,
    const RECT& rect,
    UINT format,
    COLORREF color);

int MeasureTextWidth(HFONT font, const std::wstring& text);
SIZE MeasureText(HFONT font, const std::wstring& text, int maxWidth, bool wrap);

} // namespace ThemedD2D
