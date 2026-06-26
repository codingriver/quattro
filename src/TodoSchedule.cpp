#include "TodoSchedule.h"

#include "Utilities.h"

#include <algorithm>
#include <ctime>
#include <cwchar>

namespace {
bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return kDays[month - 1];
}

std::time_t ToTimeT(const SYSTEMTIME& value) {
    std::tm tm{};
    tm.tm_year = static_cast<int>(value.wYear) - 1900;
    tm.tm_mon = static_cast<int>(value.wMonth) - 1;
    tm.tm_mday = static_cast<int>(value.wDay);
    tm.tm_hour = static_cast<int>(value.wHour);
    tm.tm_min = static_cast<int>(value.wMinute);
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

SYSTEMTIME FromTimeT(std::time_t value) {
    std::tm tm{};
    localtime_s(&tm, &value);
    SYSTEMTIME result{};
    result.wYear = static_cast<WORD>(tm.tm_year + 1900);
    result.wMonth = static_cast<WORD>(tm.tm_mon + 1);
    result.wDay = static_cast<WORD>(tm.tm_mday);
    result.wHour = static_cast<WORD>(tm.tm_hour);
    result.wMinute = static_cast<WORD>(tm.tm_min);
    return result;
}

SYSTEMTIME AddDays(const SYSTEMTIME& value, int days) {
    return FromTimeT(ToTimeT(value) + static_cast<std::time_t>(days) * 24 * 60 * 60);
}

SYSTEMTIME AddMonths(const SYSTEMTIME& value, int months) {
    int year = value.wYear;
    int month = static_cast<int>(value.wMonth) + months;
    while (month > 12) {
        month -= 12;
        ++year;
    }
    while (month < 1) {
        month += 12;
        --year;
    }

    SYSTEMTIME result = value;
    result.wYear = static_cast<WORD>(year);
    result.wMonth = static_cast<WORD>(month);
    result.wDay = static_cast<WORD>(std::min<int>(value.wDay, DaysInMonth(year, month)));
    return result;
}

SYSTEMTIME AddYears(const SYSTEMTIME& value, int years) {
    SYSTEMTIME result = value;
    result.wYear = static_cast<WORD>(static_cast<int>(value.wYear) + years);
    result.wDay = static_cast<WORD>(std::min<int>(value.wDay, DaysInMonth(result.wYear, result.wMonth)));
    return result;
}

SYSTEMTIME AddPeriodsFromAnchor(TodoScheduleKind kind, const SYSTEMTIME& anchor, int periods) {
    switch (kind) {
    case TodoScheduleKind::Daily:
        return AddDays(anchor, periods);
    case TodoScheduleKind::Weekly:
        return AddDays(anchor, periods * 7);
    case TodoScheduleKind::Monthly:
        return AddMonths(anchor, periods);
    case TodoScheduleKind::Yearly:
        return AddYears(anchor, periods);
    default:
        return anchor;
    }
}

bool IsValidParsedTime(const SYSTEMTIME& value) {
    if (value.wYear < 1900 || value.wMonth < 1 || value.wMonth > 12 ||
        value.wDay < 1 || value.wDay > DaysInMonth(value.wYear, value.wMonth) ||
        value.wHour > 23 || value.wMinute > 59) {
        return false;
    }
    const std::time_t roundTrip = ToTimeT(value);
    if (roundTrip == static_cast<std::time_t>(-1)) {
        return false;
    }
    const SYSTEMTIME normalized = FromTimeT(roundTrip);
    return normalized.wYear == value.wYear &&
           normalized.wMonth == value.wMonth &&
           normalized.wDay == value.wDay &&
           normalized.wHour == value.wHour &&
           normalized.wMinute == value.wMinute;
}
}

bool IsRecurringTodoSchedule(TodoScheduleKind kind) {
    return kind == TodoScheduleKind::Daily ||
           kind == TodoScheduleKind::Weekly ||
           kind == TodoScheduleKind::Monthly ||
           kind == TodoScheduleKind::Yearly;
}

std::wstring CurrentTodoTimestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return FormatTodoTimestamp(now);
}

std::wstring FormatTodoTimestamp(const SYSTEMTIME& value) {
    wchar_t buffer[32]{};
    swprintf_s(
        buffer,
        L"%04u-%02u-%02u %02u:%02u",
        value.wYear,
        value.wMonth,
        value.wDay,
        value.wHour,
        value.wMinute);
    return buffer;
}

bool TryParseTodoTimestamp(const std::wstring& value, SYSTEMTIME& result) {
    const std::wstring text = ReplaceAll(Trim(value), L"/", L"-");
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int matched = swscanf_s(text.c_str(), L"%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute);
    if (matched < 3) {
        return false;
    }
    if (matched == 3) {
        hour = 0;
        minute = 0;
    }

    SYSTEMTIME parsed{};
    parsed.wYear = static_cast<WORD>(year);
    parsed.wMonth = static_cast<WORD>(month);
    parsed.wDay = static_cast<WORD>(day);
    parsed.wHour = static_cast<WORD>(hour);
    parsed.wMinute = static_cast<WORD>(minute);
    if (!IsValidParsedTime(parsed)) {
        return false;
    }
    result = parsed;
    return true;
}

std::wstring NormalizeTodoTimestamp(const std::wstring& value) {
    SYSTEMTIME parsed{};
    if (!TryParseTodoTimestamp(value, parsed)) {
        return {};
    }
    return FormatTodoTimestamp(parsed);
}

std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, const std::wstring& anchorAt, const std::wstring& afterAt) {
    if (kind == TodoScheduleKind::None) {
        return {};
    }

    SYSTEMTIME anchor{};
    if (!TryParseTodoTimestamp(anchorAt, anchor)) {
        return {};
    }
    if (kind == TodoScheduleKind::Once) {
        return FormatTodoTimestamp(anchor);
    }

    SYSTEMTIME after{};
    if (!TryParseTodoTimestamp(afterAt, after)) {
        return FormatTodoTimestamp(anchor);
    }

    const std::time_t afterTime = ToTimeT(after);
    SYSTEMTIME due = anchor;
    std::time_t dueTime = ToTimeT(due);
    int periods = 0;
    int guard = 0;
    while (dueTime <= afterTime && guard < 10000) {
        ++periods;
        due = AddPeriodsFromAnchor(kind, anchor, periods);
        dueTime = ToTimeT(due);
        ++guard;
    }
    return FormatTodoTimestamp(due);
}
