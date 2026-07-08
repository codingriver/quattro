#pragma once

#include <filesystem>
#include <string>

void InitializeAppLog(const std::filesystem::path& appDirectory);
void WriteAppLog(const std::wstring& message);
void ResetStartupTiming();
void FinishStartupTiming();
bool IsStartupTimingActive();
void WriteStartupTiming(const std::wstring& stage);
void WriteStartupTiming(const std::wstring& stage, const std::wstring& detail);
