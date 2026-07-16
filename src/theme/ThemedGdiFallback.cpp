#include "ThemedGdiFallback.h"

#include <algorithm>

namespace ThemedGdiFallback {

void FillRoundedRect(
    HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int borderWidth) {
    if (!dc) return;
    const int stroke = std::max(1, borderWidth);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, stroke, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (stroke > 1) InflateRect(&rect, -(stroke / 2), -(stroke / 2));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
        std::max(0, radius) * 2, std::max(0, radius) * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawRoundedRect(HDC dc, RECT rect, int radius, COLORREF border, int borderWidth) {
    if (!dc) return;
    const int stroke = std::max(1, borderWidth);
    HPEN pen = CreatePen(PS_SOLID, stroke, border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (stroke > 1) InflateRect(&rect, -(stroke / 2), -(stroke / 2));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
        std::max(0, radius) * 2, std::max(0, radius) * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void DrawText(
    HDC dc, HFONT font, const wchar_t* text, int textLength,
    RECT rect, UINT format, COLORREF color) {
    if (!dc || !text) return;
    const int oldBkMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, color);
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    DrawTextW(dc, text, textLength, &rect, format);
    if (oldFont) SelectObject(dc, oldFont);
    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBkMode);
}

SIZE MeasureText(HDC dc, HFONT font, const wchar_t* text, int textLength) {
    SIZE size{};
    if (!dc || !text) return size;
    if (textLength < 0) textLength = static_cast<int>(wcslen(text));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    GetTextExtentPoint32W(dc, text, textLength, &size);
    if (oldFont) SelectObject(dc, oldFont);
    return size;
}

void FillSolidRect(HDC dc, RECT rect, COLORREF fill) {
    if (!dc) return;
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillEllipse(HDC dc, RECT rect, COLORREF fill, COLORREF border, int borderWidth) {
    if (!dc) return;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawPolyline(HDC dc, const POINT* points, int pointCount, COLORREF color, int strokeWidth) {
    if (!dc || !points || pointCount < 2) return;
    HPEN pen = CreatePen(PS_SOLID, std::max(1, strokeWidth), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Polyline(dc, points, pointCount);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int strokeWidth) {
    if (!dc) return;
    HPEN pen = CreatePen(PS_SOLID, std::max(1, strokeWidth), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawIcon(HDC dc, HICON icon, RECT destination, bool disabled) {
    if (!dc || !icon) return;
    const int width = destination.right - destination.left;
    const int height = destination.bottom - destination.top;
    if (width <= 0 || height <= 0) return;
    if (disabled) {
        DrawStateW(dc, nullptr, nullptr, reinterpret_cast<LPARAM>(icon), 0,
            destination.left, destination.top, width, height, DST_ICON | DSS_DISABLED);
    } else {
        DrawIconEx(dc, destination.left, destination.top, icon, width, height, 0, nullptr, DI_NORMAL);
    }
}

bool ApplyRoundedWindowRegion(HWND hwnd, SIZE size, int radius, bool redraw) {
    if (!hwnd || size.cx <= 0 || size.cy <= 0 || radius <= 0) return false;
    HRGN region = CreateRoundRectRgn(
        0, 0, size.cx + 1, size.cy + 1, radius * 2, radius * 2);
    if (!region) return false;
    if (SetWindowRgn(hwnd, region, redraw ? TRUE : FALSE) == 0) {
        DeleteObject(region);
        return false;
    }
    return true;
}

} // namespace ThemedGdiFallback
