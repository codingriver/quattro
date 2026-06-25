#pragma once

#include "Models.h"

#include <windows.h>

class SearchDialog {
public:
    static int Show(HWND owner, HINSTANCE instance, const AppModel& model, AppConfig& config);
};
