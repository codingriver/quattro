#pragma once

#include "Theme.h"

#include <string>
#include <windows.h>

enum class ConfirmDialogResult {
    Primary,
    Secondary,
    Cancel,
};

ConfirmDialogResult ShowConfirmDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::wstring& title,
    const std::wstring& message,
    const std::wstring& primaryText,
    const std::wstring& secondaryText);
