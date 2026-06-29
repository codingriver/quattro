#pragma once

#include <windows.h>

#include <string>

bool IsRunningAsAdmin();
bool RestartCurrentProcessElevated(HWND owner, std::wstring& errorMessage);
bool RestartCurrentProcessUnelevated(HWND owner, std::wstring& errorMessage);
