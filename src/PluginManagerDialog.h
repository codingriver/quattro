#pragma once

#include "PluginRegistry.h"
#include "Storage.h"
#include "Theme.h"

#include <windows.h>

#include <filesystem>

bool ShowPluginManagerDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    PluginRegistry& registry,
    StorageService& storage,
    const std::filesystem::path& appDirectory,
    const std::wstring& storeUrl);
