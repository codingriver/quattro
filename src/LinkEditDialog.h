#pragma once

#include "Models.h"
#include "Theme.h"

#include <windows.h>

#include <vector>

class LinkEditDialog {
public:
    static bool Show(HWND owner, HINSTANCE instance, const Theme& theme, Link& link, const std::vector<Group>& groups, bool isNew);
};
