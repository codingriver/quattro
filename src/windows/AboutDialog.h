#pragma once

#include "Theme.h"

#include <windows.h>

void ShowAboutDialog(HWND owner, HINSTANCE instance, const Theme& theme, bool runningAsAdmin);
