#pragma once

#include "Models.h"
#include "Theme.h"

#include <windows.h>

class TodoEditDialog {
public:
    static bool Show(HWND owner, HINSTANCE instance, const Theme& theme, TodoItem& item, bool isNew);
};
