#pragma once

#include <filesystem>
#include <string>

void InitializeAppLog(const std::filesystem::path& appDirectory);
void WriteAppLog(const std::wstring& message);
