#pragma once

#include "Models.h"
#include "Theme.h"

#include <windows.h>

// Isolated in-process acceptance only: wParam selects day/month/year (0/1/2).
constexpr UINT WM_QUATTRO_TEST_TODO_CALENDAR = WM_APP + 380;

class TodoEditDialog {
public:
    static bool Show(HWND owner, HINSTANCE instance, const Theme& theme, TodoItem& item, bool isNew);
};
