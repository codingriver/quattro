#pragma once

#include "Theme.h"

#include <windows.h>

#include <utility>
#include <vector>

struct CatalogDialogMetrics {
    int contentPaddingX = 24;
    int toolbarY = 12;
    int toolbarHeight = 24;
    int verticalGap = 2;
    int statusBarHeight = 13;
    int pagerHeight = 20;
    int bottomPadding = 16;
    int horizontalGap = 8;
    int minListHeight = 80;
    int searchWidth = 180;
    int searchLeftReserve = 410;
};

struct CatalogDialogGeometry {
    RECT toolbar{};
    RECT list{};
    RECT listFrame{};
    RECT pager{};
    RECT status{};
    RECT searchFrame{};
};

struct CatalogDialogControls {
    std::vector<HWND> toolbarLeading;
    const Theme* theme = nullptr;
    HWND searchEdit = nullptr;
    RECT* searchFrame = nullptr;
    HWND toolbarTrailing = nullptr;
    int toolbarTrailingWidth = 0;

    HWND list = nullptr;
    RECT* listFrame = nullptr;
    int listItemHeight = 0;
    int listFrameInset = 1;

    std::vector<std::pair<HWND, int>> pagerLeading;
    HWND pagePrev = nullptr;
    int pagePrevWidth = 0;
    HWND pageText = nullptr;
    int pageTextWidth = 0;
    HWND pageNext = nullptr;
    int pageNextWidth = 0;

    HWND statusSummary = nullptr;
    HWND statusSource = nullptr;
    int statusSourceMinWidth = 160;
    int statusSourceMaxWidth = 260;
};

class CatalogDialogLayout {
public:
    explicit CatalogDialogLayout(CatalogDialogMetrics metrics = {});

    const CatalogDialogMetrics& metrics() const { return metrics_; }

    int listTop() const;
    CatalogDialogGeometry Calculate(const RECT& client, int listItemHeight) const;
    void Layout(HWND parent, const RECT& client, const CatalogDialogControls& controls) const;
    void DrawStatusBar(const Theme& theme, HDC dc, const RECT& client) const;

private:
    CatalogDialogMetrics metrics_;
};
