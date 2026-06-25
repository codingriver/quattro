#pragma once

#include <windows.h>

#include <string>

std::wstring FormatHotKeyText(int key);
int ShowHotKeyCaptureDialog(HWND owner, HINSTANCE instance, int currentKey);
