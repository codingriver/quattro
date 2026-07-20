#pragma once

#include "Models.h"
#include "PluginRegistry.h"
#include "Theme.h"

#include <windows.h>

#include <string>

bool PreTranslateBuiltinToolMessage(const MSG& message);

bool ShowBuiltinTool(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    PluginRegistry& registry,
    const AppConfig& config,
    const std::wstring& engine,
    bool locateProcessOnOpen = false);
