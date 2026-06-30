#pragma once

#include "Models.h"

#include <string>
#include <windows.h>

bool IsRecurringTodoSchedule(TodoScheduleKind kind);
bool IsTodoCronSchedule(TodoScheduleKind kind);
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
