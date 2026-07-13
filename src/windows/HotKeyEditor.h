#pragma once

#include "Theme.h"

#include <windows.h>

#include <string>

struct HotKeyCaptureDialogOptions {
    bool allowDoubleAlt = false;
    bool useMainHotKeyText = false;
};

std::wstring FormatHotKeyText(int key);
int ShowHotKeyCaptureDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    int currentKey,
    HotKeyCaptureDialogOptions options = {});
