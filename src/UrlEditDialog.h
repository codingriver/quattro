#pragma once

#include "Models.h"
#include "Theme.h"

#include <windows.h>

class UrlEditDialog {
public:
    static bool Show(HWND owner, HINSTANCE instance, const Theme& theme, Link& link, bool isNew);
};
