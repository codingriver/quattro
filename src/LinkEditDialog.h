#pragma once

#include "Models.h"

#include <windows.h>

#include <vector>

class LinkEditDialog {
public:
    static bool Show(HWND owner, HINSTANCE instance, Link& link, const std::vector<Group>& groups, bool isNew);
};
