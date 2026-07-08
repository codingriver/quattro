#pragma once

#include "Theme.h"
#include "UpdateCheckService.h"

#include <windows.h>

enum class UpdateCheckDialogChoice {
    Cancel,
    Download,
    OpenRelease,
};

UpdateCheckDialogChoice ShowUpdateCheckDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const UpdateReleaseInfo& info);
