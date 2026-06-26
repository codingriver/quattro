#pragma once

#include "PluginRegistry.h"
#include "Theme.h"

#include <windows.h>

#include <string>

bool ShowBuiltinTool(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry, const std::wstring& engine);
