#pragma once

#include "Models.h"
#include "Theme.h"

#include <windows.h>

class SearchDialog {
public:
    static int Show(HWND owner, HINSTANCE instance, const Theme& theme, const AppModel& model, AppConfig& config);
};
