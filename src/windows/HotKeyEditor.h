#pragma once

#include "Theme.h"
#include "../domain/DoubleModifierGesture.h"

#include <windows.h>

#include <string>

struct HotKeyCaptureDialogOptions {
    DoubleModifierGestureKind allowedDoubleTap = DoubleModifierGestureKind::None;
};

std::wstring FormatHotKeyText(int key);
int ShowHotKeyCaptureDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    int currentKey,
    HotKeyCaptureDialogOptions options = {});
