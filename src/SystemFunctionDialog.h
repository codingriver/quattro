#pragma once

#include "Models.h"

#include <windows.h>

class SystemFunctionDialog {
public:
    static bool Show(HWND owner, HINSTANCE instance, Link& link);
};
