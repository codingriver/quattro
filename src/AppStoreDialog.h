#pragma once

#include "Models.h"
#include "Theme.h"

#include <windows.h>

#include <filesystem>

bool ShowAppStoreDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    const AppConfig& config);
