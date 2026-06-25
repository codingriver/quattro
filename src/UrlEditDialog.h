#pragma once

#include "Models.h"

#include <windows.h>

class UrlEditDialog {
public:
    static bool Show(HWND owner, HINSTANCE instance, Link& link, bool isNew);
};
