#pragma once

#include "Models.h"
#include "Theme.h"

#include <filesystem>
#include <optional>
#include <windows.h>

class LaunchItemSearchDialog {
public:
    static std::optional<int> Show(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        const std::filesystem::path& appDirectory,
        const AppModel& model);
};
