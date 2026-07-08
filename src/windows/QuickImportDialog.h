#pragma once

#include "Models.h"
#include "QuickImportService.h"
#include "Theme.h"

#include <windows.h>

#include <vector>

class QuickImportDialog {
public:
    static bool Show(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        const std::vector<Link>& existingLinks,
        std::vector<Link>& selectedLinks);
};
