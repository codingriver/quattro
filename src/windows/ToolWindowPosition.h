#pragma once

#include <optional>
#include <string>
#include <windows.h>

std::optional<POINT> LoadToolWindowPosition(const std::wstring& toolId, int width, int height);
void SaveToolWindowPosition(const std::wstring& toolId, HWND hwnd);
