#include "TodoSchedule.h"

#include "Utilities.h"

#include <ccronexpr.h>

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <string>

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
    tm.tm_sec = static_cast<int>(value.wSecond);
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
    result.wSecond = static_cast<WORD>(tm.tm_sec);
    return result;
}

SYSTEMTIME AddSeconds(const SYSTEMTIME& value, int seconds) {
    return FromTimeT(ToTimeT(value) + static_cast<std::time_t>(seconds));
}

SYSTEMTIME AddDays(const SYSTEMTIME& value, int days) {
    return AddSeconds(value, days * 24 * 60 * 60);
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
    case TodoScheduleKind::Secondly:
        return AddSeconds(anchor, periods);
    case TodoScheduleKind::Minutely:
        return AddSeconds(anchor, periods * 60);
    case TodoScheduleKind::Hourly:
        return AddSeconds(anchor, periods * 60 * 60);
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
        value.wHour > 23 || value.wMinute > 59 || value.wSecond > 59) {
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
           normalized.wMinute == value.wMinute &&
           normalized.wSecond == value.wSecond;
}

int FixedIntervalSeconds(TodoScheduleKind kind, int repeatInterval) {
    const int interval = std::max(1, repeatInterval);
    switch (kind) {
    case TodoScheduleKind::Secondly:
        return interval;
    case TodoScheduleKind::Minutely:
        return interval * 60;
    case TodoScheduleKind::Hourly:
        return interval * 60 * 60;
    case TodoScheduleKind::Daily:
        return interval * 24 * 60 * 60;
    case TodoScheduleKind::Weekly:
        return interval * 7 * 24 * 60 * 60;
    default:
        return 0;
    }
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}

bool TryParseCronExpression(const std::wstring& value, cron_expr& parsed) {
    const std::string expression = WideToUtf8(NormalizeTodoCronExpression(value));
    if (expression.empty()) {
        return false;
    }
    const char* error = nullptr;
    cron_parse_expr(expression.c_str(), &parsed, &error);
    return error == nullptr;
}
}

bool IsRecurringTodoSchedule(TodoScheduleKind kind) {
    return kind == TodoScheduleKind::Secondly ||
           kind == TodoScheduleKind::Minutely ||
           kind == TodoScheduleKind::Hourly ||
           kind == TodoScheduleKind::Daily ||
           kind == TodoScheduleKind::Weekly ||
           kind == TodoScheduleKind::Monthly ||
           kind == TodoScheduleKind::Yearly ||
           kind == TodoScheduleKind::Cron;
}

bool IsTodoCronSchedule(TodoScheduleKind kind) {
    return kind == TodoScheduleKind::Cron;
}

bool IsFixedPointSchedule(TodoScheduleKind kind) {
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
        L"%04u-%02u-%02u %02u:%02u:%02u",
        value.wYear,
        value.wMonth,
        value.wDay,
        value.wHour,
        value.wMinute,
        value.wSecond);
    return buffer;
}

bool TryParseTodoTimestamp(const std::wstring& value, SYSTEMTIME& result) {
    const std::wstring text = ReplaceAll(Trim(value), L"/", L"-");
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int matched = swscanf_s(text.c_str(), L"%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if (matched < 3) {
        return false;
    }
    if (matched == 3) {
        hour = 0;
        minute = 0;
        second = 0;
    } else if (matched == 5) {
        second = 0;
    } else if (matched != 6) {
        return false;
    }

    SYSTEMTIME parsed{};
    parsed.wYear = static_cast<WORD>(year);
    parsed.wMonth = static_cast<WORD>(month);
    parsed.wDay = static_cast<WORD>(day);
    parsed.wHour = static_cast<WORD>(hour);
    parsed.wMinute = static_cast<WORD>(minute);
    parsed.wSecond = static_cast<WORD>(second);
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

std::wstring NormalizeTodoCronExpression(const std::wstring& value) {
    const std::wstring text = Trim(value);
    std::wstring result;
    bool previousSpace = false;
    for (wchar_t ch : text) {
        if (std::iswspace(ch) != 0) {
            if (!result.empty() && !previousSpace) {
                result += L' ';
            }
            previousSpace = true;
        } else {
            result += ch;
            previousSpace = false;
        }
    }
    return Trim(result);
}

bool IsValidTodoCronExpression(const std::wstring& value) {
    cron_expr parsed{};
    return TryParseCronExpression(value, parsed);
}

std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, const std::wstring& anchorAt, const std::wstring& afterAt) {
    return ComputeNextTodoDueAt(kind, 1, anchorAt, afterAt);
}

std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, int repeatInterval, const std::wstring& anchorAt, const std::wstring& afterAt) {
    return ComputeNextTodoDueAt(kind, IsFixedPointSchedule(kind) ? TodoRepeatMode::FixedPoint : TodoRepeatMode::Interval, repeatInterval, anchorAt, afterAt);
}

std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, TodoRepeatMode repeatMode, int repeatInterval, const std::wstring& anchorAt, const std::wstring& afterAt) {
    if (kind == TodoScheduleKind::None || kind == TodoScheduleKind::Cron) {
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
    const int fixedSeconds = repeatMode == TodoRepeatMode::Interval ? FixedIntervalSeconds(kind, repeatInterval) : 0;
    if (fixedSeconds > 0) {
        const std::time_t anchorTime = ToTimeT(anchor);
        if (anchorTime == static_cast<std::time_t>(-1)) {
            return {};
        }
        if (anchorTime > afterTime) {
            return FormatTodoTimestamp(anchor);
        }
        const std::time_t elapsed = afterTime - anchorTime;
        const std::time_t periods = elapsed / fixedSeconds + 1;
        return FormatTodoTimestamp(FromTimeT(anchorTime + periods * fixedSeconds));
    }

    const int interval = std::max(1, repeatInterval);
    SYSTEMTIME due = anchor;
    std::time_t dueTime = ToTimeT(due);
    int periods = 0;
    int guard = 0;
    while (dueTime <= afterTime && guard < 10000) {
        periods += interval;
        due = AddPeriodsFromAnchor(kind, anchor, periods);
        dueTime = ToTimeT(due);
        ++guard;
    }
    return FormatTodoTimestamp(due);
}

std::wstring ComputeNextTodoCronDueAt(const std::wstring& cronExpression, const std::wstring& afterAt) {
    cron_expr parsed{};
    if (!TryParseCronExpression(cronExpression, parsed)) {
        return {};
    }

    SYSTEMTIME after{};
    if (!TryParseTodoTimestamp(afterAt, after)) {
        GetLocalTime(&after);
    }
    const std::time_t next = cron_next(&parsed, ToTimeT(after));
    if (next == static_cast<std::time_t>(-1)) {
        return {};
    }
    return FormatTodoTimestamp(FromTimeT(next));
}

std::wstring ComputeNextTodoDueAt(const TodoItem& item, const std::wstring& afterAt) {
    if (item.scheduleKind == TodoScheduleKind::Cron) {
        return ComputeNextTodoCronDueAt(item.cronExpression, afterAt);
    }
    return ComputeNextTodoDueAt(item.scheduleKind, item.repeatMode, item.repeatInterval, item.anchorAt, afterAt);
}

std::wstring OffsetTodoTimestamp(const std::wstring& value, int seconds) {
    SYSTEMTIME parsed{};
    if (!TryParseTodoTimestamp(value, parsed)) {
        return {};
    }
    return FormatTodoTimestamp(AddSeconds(parsed, seconds));
}

std::wstring EffectiveTodoReminderDueAt(const TodoItem& item) {
    return item.snoozedUntil.empty() ? item.nextDueAt : item.snoozedUntil;
}

bool IsTodoReminderDue(const TodoItem& item, const std::wstring& now) {
    if (!item.enabled || !item.completedAt.empty()) {
        return false;
    }
    const std::wstring dueAt = EffectiveTodoReminderDueAt(item);
    SYSTEMTIME due{};
    SYSTEMTIME current{};
    if (!TryParseTodoTimestamp(dueAt, due) || !TryParseTodoTimestamp(now, current)) {
        return false;
    }
    const std::time_t dueTime = ToTimeT(due);
    const std::time_t currentTime = ToTimeT(current);
    return dueTime != static_cast<std::time_t>(-1) &&
           currentTime != static_cast<std::time_t>(-1) &&
           dueTime <= currentTime;
}

bool IsTodoOverdueAt(const TodoItem& item, const std::wstring& now) {
    if (!item.enabled || !item.completedAt.empty() || item.nextDueAt.empty()) {
        return false;
    }
    SYSTEMTIME due{};
    SYSTEMTIME current{};
    if (!TryParseTodoTimestamp(item.nextDueAt, due) || !TryParseTodoTimestamp(now, current)) {
        return false;
    }
    const std::time_t dueTime = ToTimeT(due);
    const std::time_t currentTime = ToTimeT(current);
    return dueTime != static_cast<std::time_t>(-1) &&
           currentTime != static_cast<std::time_t>(-1) &&
           dueTime < currentTime;
}

bool IsTodoReminderDelivered(const TodoItem& item) {
    const std::wstring dueAt = EffectiveTodoReminderDueAt(item);
    return !dueAt.empty() &&
           (item.lastNotifiedDueAt == dueAt || item.ignoredDueAt == dueAt);
}

TodoReminderStatus GetTodoReminderStatus(const TodoItem& item, const std::wstring& now) {
    if (!item.completedAt.empty()) {
        return TodoReminderStatus::Completed;
    }
    if (!item.enabled || item.nextDueAt.empty()) {
        return TodoReminderStatus::Disabled;
    }
    const std::wstring dueAt = EffectiveTodoReminderDueAt(item);
    if (!dueAt.empty() && item.ignoredDueAt == dueAt) {
        return TodoReminderStatus::Ignored;
    }
    if (!dueAt.empty() && item.lastViewedDueAt == dueAt) {
        return TodoReminderStatus::Viewed;
    }
    if (!dueAt.empty() && item.lastNotifiedDueAt == dueAt) {
        return TodoReminderStatus::Sent;
    }
    if (!item.snoozedUntil.empty() && !IsTodoReminderDue(item, now)) {
        return TodoReminderStatus::Snoozed;
    }
    return TodoReminderStatus::Pending;
}

void ResetTodoReminderState(TodoItem& item) {
    item.lastNotifiedDueAt.clear();
    item.lastNotifiedAt.clear();
    item.lastViewedDueAt.clear();
    item.lastViewedAt.clear();
    item.ignoredDueAt.clear();
    item.snoozedUntil.clear();
}

void MarkTodoReminderSent(TodoItem& item, const std::wstring& now) {
    item.lastNotifiedDueAt = EffectiveTodoReminderDueAt(item);
    item.lastNotifiedAt = NormalizeTodoTimestamp(now);
}

void MarkTodoReminderViewed(TodoItem& item, const std::wstring& now) {
    const std::wstring dueAt = EffectiveTodoReminderDueAt(item);
    item.lastViewedDueAt = dueAt;
    item.lastViewedAt = NormalizeTodoTimestamp(now);
}

void IgnoreTodoReminderOccurrence(TodoItem& item) {
    item.ignoredDueAt = EffectiveTodoReminderDueAt(item);
}

bool SnoozeTodoReminder(TodoItem& item, const std::wstring& now, int minutes) {
    if (minutes <= 0 || !item.completedAt.empty() || !item.enabled || item.nextDueAt.empty()) {
        return false;
    }
    const std::wstring snoozedUntil = OffsetTodoTimestamp(now, minutes * 60);
    if (snoozedUntil.empty()) {
        return false;
    }
    item.snoozedUntil = snoozedUntil;
    item.lastNotifiedDueAt.clear();
    item.lastNotifiedAt.clear();
    item.lastViewedDueAt.clear();
    item.lastViewedAt.clear();
    item.ignoredDueAt.clear();
    return true;
}

TodoCompletionOutcome CompleteTodoOccurrence(TodoItem& item, const std::wstring& now) {
    if (!item.completedAt.empty()) {
        return TodoCompletionOutcome::NoChange;
    }
    if (IsRecurringTodoSchedule(item.scheduleKind)) {
        ++item.repeatFinished;
        if (item.repeatLimit > 0 && item.repeatFinished >= item.repeatLimit) {
            item.completedAt = now;
            item.nextDueAt.clear();
            ResetTodoReminderState(item);
            item.updatedAt = now;
            return TodoCompletionOutcome::Completed;
        }
        item.completedAt.clear();
        item.nextDueAt = ComputeNextTodoDueAt(item, now);
        ResetTodoReminderState(item);
        item.updatedAt = now;
        return TodoCompletionOutcome::AdvancedRecurring;
    }
    item.completedAt = now;
    ResetTodoReminderState(item);
    item.updatedAt = now;
    return TodoCompletionOutcome::Completed;
}
