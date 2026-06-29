#include "CatalogDialogLayout.h"

#include "ThemedControls.h"

#include <algorithm>

namespace {
COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

COLORREF Blend(COLORREF a, COLORREF b, float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    const auto channel = [clamped](int ca, int cb) -> BYTE {
        return static_cast<BYTE>(ca + (cb - ca) * clamped + 0.5f);
    };
    return RGB(
        channel(GetRValue(a), GetRValue(b)),
        channel(GetGValue(a), GetGValue(b)),
        channel(GetBValue(a), GetBValue(b)));
}

void FillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}
}

CatalogDialogLayout::CatalogDialogLayout(CatalogDialogMetrics metrics)
    : metrics_(metrics) {}

int CatalogDialogLayout::listTop() const {
    return metrics_.toolbarY + metrics_.toolbarHeight + metrics_.verticalGap;
}

CatalogDialogGeometry CatalogDialogLayout::Calculate(const RECT& client, int listItemHeight) const {
    CatalogDialogGeometry geometry{};
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return geometry;
    }

    const int contentLeft = metrics_.contentPaddingX;
    const int contentRight = std::max(contentLeft + 1, width - metrics_.contentPaddingX);
    const int listY = listTop();
    const int statusY = std::max(metrics_.toolbarY + 60, height - metrics_.bottomPadding - metrics_.statusBarHeight);
    const int pagerY = std::max(listY + metrics_.minListHeight, statusY - metrics_.verticalGap - metrics_.pagerHeight);
    const int minListHeight = std::max(1, listItemHeight);
    const int listBottom = std::max(listY + minListHeight, pagerY - metrics_.verticalGap);

    geometry.toolbar = RECT{contentLeft, metrics_.toolbarY, contentRight, metrics_.toolbarY + metrics_.toolbarHeight};
    geometry.list = RECT{contentLeft, listY, contentRight, listBottom};
    geometry.listFrame = RECT{contentLeft - 1, listY - 1, contentRight + 1, listBottom + 1};
    geometry.pager = RECT{contentLeft, pagerY, contentRight, pagerY + metrics_.pagerHeight};
    geometry.status = RECT{0, statusY, width, statusY + metrics_.statusBarHeight};
    return geometry;
}

void CatalogDialogLayout::Layout(HWND parent, const RECT& client, const CatalogDialogControls& controls) const {
    if (!parent) {
        return;
    }
    CatalogDialogGeometry geometry = Calculate(client, controls.listItemHeight);
    const int contentWidth = geometry.toolbar.right - geometry.toolbar.left;
    if (contentWidth <= 0) {
        return;
    }

    for (HWND control : controls.toolbarLeading) {
        if (!control) {
            continue;
        }
        RECT rect{};
        GetWindowRect(control, &rect);
        POINT point{rect.left, rect.top};
        ScreenToClient(parent, &point);
        MoveWindow(control, point.x, geometry.toolbar.top + 1, rect.right - rect.left, std::max(1, metrics_.toolbarHeight - 2), TRUE);
    }

    const int toolbarLeft = static_cast<int>(geometry.toolbar.left);
    const int toolbarRight = static_cast<int>(geometry.toolbar.right);
    const int trailingX = std::max(toolbarLeft, toolbarRight - controls.toolbarTrailingWidth);
    if (controls.toolbarTrailing) {
        MoveWindow(controls.toolbarTrailing, trailingX, geometry.toolbar.top + 1, controls.toolbarTrailingWidth, std::max(1, metrics_.toolbarHeight - 2), TRUE);
    }
    if (controls.searchEdit && controls.searchFrame && controls.theme) {
        const int searchRight = trailingX - metrics_.horizontalGap;
        const int searchLeft = std::max(toolbarLeft + metrics_.searchLeftReserve, searchRight - metrics_.searchWidth);
        *controls.searchFrame = RECT{searchLeft, geometry.toolbar.top + 1, searchRight, geometry.toolbar.bottom - 1};
        const RECT editRect = ThemedControls::SingleLineEditRect(*controls.theme, *controls.searchFrame);
        MoveWindow(controls.searchEdit, editRect.left, editRect.top, editRect.right - editRect.left, editRect.bottom - editRect.top, TRUE);
    }

    if (controls.listFrame) {
        *controls.listFrame = geometry.listFrame;
    }
    if (controls.list) {
        MoveWindow(controls.list, geometry.list.left, geometry.list.top, geometry.list.right - geometry.list.left, geometry.list.bottom - geometry.list.top, TRUE);
        if (controls.listItemHeight > 0) {
            SendMessageW(controls.list, LB_SETITEMHEIGHT, 0, controls.listItemHeight);
        }
    }

    int pagerLeft = geometry.pager.left;
    for (const auto& [control, width] : controls.pagerLeading) {
        if (!control) {
            continue;
        }
        MoveWindow(control, pagerLeft, geometry.pager.top + 1, width, std::max(1, metrics_.pagerHeight - 2), TRUE);
        pagerLeft += width + metrics_.horizontalGap;
    }

    const int nextX = geometry.pager.right - controls.pageNextWidth;
    const int pageX = nextX - metrics_.horizontalGap - controls.pageTextWidth;
    const int prevX = pageX - metrics_.horizontalGap - controls.pagePrevWidth;
    if (controls.pagePrev) {
        MoveWindow(controls.pagePrev, prevX, geometry.pager.top + 1, controls.pagePrevWidth, std::max(1, metrics_.pagerHeight - 2), TRUE);
    }
    if (controls.pageText) {
        MoveWindow(controls.pageText, pageX, geometry.pager.top + 1, controls.pageTextWidth, std::max(1, metrics_.pagerHeight - 2), TRUE);
    }
    if (controls.pageNext) {
        MoveWindow(controls.pageNext, nextX, geometry.pager.top + 1, controls.pageNextWidth, std::max(1, metrics_.pagerHeight - 2), TRUE);
    }

    if (controls.statusSummary) {
        MoveWindow(
            controls.statusSummary,
            geometry.toolbar.left + 8,
            geometry.status.top + 1,
            std::max(120, contentWidth - controls.statusSourceMaxWidth - 20),
            std::max(1, metrics_.statusBarHeight - 2),
            TRUE);
    }
    if (controls.statusSource) {
        const int sourceWidth = std::min(controls.statusSourceMaxWidth, std::max(controls.statusSourceMinWidth, contentWidth / 3));
        MoveWindow(controls.statusSource, geometry.toolbar.right - sourceWidth - 8, geometry.status.top + 1, sourceWidth, std::max(1, metrics_.statusBarHeight - 2), TRUE);
    }
}

void CatalogDialogLayout::DrawStatusBar(const Theme& theme, HDC dc, const RECT& client) const {
    const CatalogDialogGeometry geometry = Calculate(client, 1);
    const COLORREF line = ToColorRef(theme.color(L"separator", L"normal", L"line"));
    const COLORREF panel = Blend(
        ToColorRef(theme.color(L"panel", L"raised", L"bg")),
        ToColorRef(theme.color(L"dialog", L"normal", L"bg")),
        0.45f);
    FillRectColor(dc, geometry.status, panel);
    DrawLine(dc, 0, geometry.status.top, client.right, geometry.status.top, line);
}
