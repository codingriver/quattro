#pragma once

#include "Models.h"
#include "Theme.h"

#include <string>
#include <windows.h>

class SearchDialog {
public:
    static int Show(HWND owner, HINSTANCE instance, const Theme& theme, const AppModel& model, AppConfig& config, const std::wstring& initialQuery = std::wstring());
};
