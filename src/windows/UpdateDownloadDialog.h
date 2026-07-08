#pragma once

#include "Theme.h"
#include "UpdateCheckService.h"

#include <filesystem>
#include <string>
#include <windows.h>

bool ShowUpdateDownloadDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    const UpdateReleaseInfo& info,
    UpdateDownloadResult& result,
    std::wstring& error);
