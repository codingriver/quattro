#pragma once

#include "Models.h"

#include <string>
#include <windows.h>

bool IsRecurringTodoSchedule(TodoScheduleKind kind);
bool IsTodoCronSchedule(TodoScheduleKind kind);
enum class TodoReminderStatus {
    Pending,
    Sent,
    Viewed,
    Ignored,
    Snoozed,
    Completed,
    Disabled,
};

std::wstring CurrentTodoTimestamp();
std::wstring FormatTodoTimestamp(const SYSTEMTIME& value);
bool TryParseTodoTimestamp(const std::wstring& value, SYSTEMTIME& result);
std::wstring NormalizeTodoTimestamp(const std::wstring& value);
std::wstring NormalizeTodoCronExpression(const std::wstring& value);
bool IsValidTodoCronExpression(const std::wstring& value);
std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, const std::wstring& anchorAt, const std::wstring& afterAt);
std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, int repeatInterval, const std::wstring& anchorAt, const std::wstring& afterAt);
std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, TodoRepeatMode repeatMode, int repeatInterval, const std::wstring& anchorAt, const std::wstring& afterAt);
std::wstring ComputeNextTodoCronDueAt(const std::wstring& cronExpression, const std::wstring& afterAt);
std::wstring ComputeNextTodoDueAt(const TodoItem& item, const std::wstring& afterAt);
std::wstring OffsetTodoTimestamp(const std::wstring& value, int seconds);
std::wstring EffectiveTodoReminderDueAt(const TodoItem& item);
bool IsTodoReminderDue(const TodoItem& item, const std::wstring& now);
bool IsTodoReminderDelivered(const TodoItem& item);
TodoReminderStatus GetTodoReminderStatus(const TodoItem& item, const std::wstring& now);
void ResetTodoReminderState(TodoItem& item);
void MarkTodoReminderSent(TodoItem& item, const std::wstring& now);
void MarkTodoReminderViewed(TodoItem& item, const std::wstring& now);
void IgnoreTodoReminderOccurrence(TodoItem& item);
bool SnoozeTodoReminder(TodoItem& item, const std::wstring& now, int minutes);
