#pragma once

#include "Theme.h"

#include <windows.h>

#include <string>

std::wstring FormatHotKeyText(int key);
int ShowHotKeyCaptureDialog(HWND owner, HINSTANCE instance, const Theme& theme, int currentKey);
