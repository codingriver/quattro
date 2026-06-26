#pragma once

#include "Models.h"
#include "Theme.h"

#include <windows.h>

#include <string>

bool ShowTextInputDialog(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& title, const std::wstring& label, std::wstring& value);
bool ShowSettingsDialog(HWND owner, HINSTANCE instance, AppConfig& config, const Theme& theme);
