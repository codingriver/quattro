#pragma once

#include <filesystem>
#include <string>

bool SyncStartupShortcut(const std::filesystem::path& appDirectory, bool enabled, std::wstring& error);
bool StartupShortcutExists(const std::filesystem::path& appDirectory);
