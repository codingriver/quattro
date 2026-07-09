#pragma once

#include <filesystem>
#include <string>

void InitializeAppLog(const std::filesystem::path& appDirectory, bool enabled = true);
void SetAppLogEnabled(bool enabled);
bool IsAppLogEnabled();
void WriteAppLog(const std::wstring& message);
void ResetStartupTiming();
void FinishStartupTiming();
bool IsStartupTimingActive();
void WriteStartupTiming(const std::wstring& stage);
void WriteStartupTiming(const std::wstring& stage, const std::wstring& detail);
