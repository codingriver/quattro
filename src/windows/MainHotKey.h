#pragma once

#include <string>

constexpr int kMainHotKeyDoubleAlt = -1;

bool IsDoubleAltMainHotKey(int key);
std::wstring FormatMainHotKeyText(int key);
std::wstring FormatGlobalHotKeyText(int key);
