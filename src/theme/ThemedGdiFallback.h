#pragma once

#include <windows.h>

// Emergency rendering backend used only when Direct2D/DirectWrite cannot be
// initialized or a device target is temporarily unavailable. Product drawing
// code must call ThemedD2D first and delegate here only on failure.
namespace ThemedGdiFallback {

void FillRoundedRect(
    HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int borderWidth);
void DrawRoundedRect(HDC dc, RECT rect, int radius, COLORREF border, int borderWidth);
void DrawText(
    HDC dc, HFONT font, const wchar_t* text, int textLength,
    RECT rect, UINT format, COLORREF color);
SIZE MeasureText(HDC dc, HFONT font, const wchar_t* text, int textLength);
void FillSolidRect(HDC dc, RECT rect, COLORREF fill);
void FillEllipse(HDC dc, RECT rect, COLORREF fill, COLORREF border, int borderWidth);
void DrawPolyline(HDC dc, const POINT* points, int pointCount, COLORREF color, int strokeWidth);
void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int strokeWidth);
void DrawIcon(HDC dc, HICON icon, RECT destination, bool disabled);
bool ApplyRoundedWindowRegion(HWND hwnd, SIZE size, int radius, bool redraw);

} // namespace ThemedGdiFallback
