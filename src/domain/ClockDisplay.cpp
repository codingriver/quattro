#include "ClockDisplay.h"

#include <algorithm>
#include <array>
#include <cwchar>

std::wstring FormatClockDate(const SYSTEMTIME& value) {
    static constexpr std::array<const wchar_t*, 7> kWeekdays = {
        L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六",
    };
    const std::size_t weekday = std::min<std::size_t>(value.wDayOfWeek, kWeekdays.size() - 1);
    wchar_t buffer[64]{};
    swprintf_s(
        buffer,
        L"%04u年%02u月%02u日 %ls",
        static_cast<unsigned>(value.wYear),
        static_cast<unsigned>(value.wMonth),
        static_cast<unsigned>(value.wDay),
        kWeekdays[weekday]);
    return buffer;
}

std::wstring FormatClockTime(const SYSTEMTIME& value, bool showMilliseconds) {
    wchar_t buffer[32]{};
    if (showMilliseconds) {
        swprintf_s(
            buffer,
            L"%02u:%02u:%02u.%03u",
            static_cast<unsigned>(value.wHour),
            static_cast<unsigned>(value.wMinute),
            static_cast<unsigned>(value.wSecond),
            static_cast<unsigned>(value.wMilliseconds));
    } else {
        swprintf_s(
            buffer,
            L"%02u:%02u:%02u",
            static_cast<unsigned>(value.wHour),
            static_cast<unsigned>(value.wMinute),
            static_cast<unsigned>(value.wSecond));
    }
    return buffer;
}

UINT ClockRefreshDelayMs(const SYSTEMTIME& value, bool showMilliseconds) {
    if (showMilliseconds) {
        return kClockMillisecondsRefreshMs;
    }
    const UINT milliseconds = std::min<UINT>(value.wMilliseconds, 999);
    return std::max<UINT>(1, 1000 - milliseconds);
}
