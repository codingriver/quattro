#pragma once

#include "Models.h"

#include <string>
#include <windows.h>

bool IsRecurringTodoSchedule(TodoScheduleKind kind);
std::wstring CurrentTodoTimestamp();
std::wstring FormatTodoTimestamp(const SYSTEMTIME& value);
bool TryParseTodoTimestamp(const std::wstring& value, SYSTEMTIME& result);
std::wstring NormalizeTodoTimestamp(const std::wstring& value);
std::wstring ComputeNextTodoDueAt(TodoScheduleKind kind, const std::wstring& anchorAt, const std::wstring& afterAt);
