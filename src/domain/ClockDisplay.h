#pragma once

#include <string>
#include <windows.h>

constexpr UINT kClockMillisecondsRefreshMs = 33;

std::wstring FormatClockDate(const SYSTEMTIME& value);
std::wstring FormatClockTime(const SYSTEMTIME& value, bool showMilliseconds);
UINT ClockRefreshDelayMs(const SYSTEMTIME& value, bool showMilliseconds);
