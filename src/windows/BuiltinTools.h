#pragma once

#include "Models.h"
#include "PluginRegistry.h"
#include "Theme.h"
#include "FileLockQueryService.h"
#include "PortScanService.h"

#include <windows.h>

#include <string>
#include <set>
#include <vector>

// Process-local acceptance entry; ignored outside isolated background test mode.
constexpr UINT WM_QUATTRO_TEST_PROCESS_TOOLS = WM_APP + 0x88;
struct ProcessToolsTestRow {
    DWORD pid = 0;
    std::wstring name;
    std::wstring path;
    bool protectedProcess = false;
    bool terminated = false;
};
enum class ProcessToolsTestCommand {
    SetRows, RefreshRows, SortDescending, KillSelected, KillAll,
    QueryFileLock, StopFileLock, FileLockPending, QueryPort, PortPending,
};
struct ProcessToolsTestRequest {
    std::vector<ProcessToolsTestRow> rows;
    std::set<DWORD> failedPids;
    bool confirm = true;
    std::wstring path;
    std::function<FileLockQueryResult(TaskContext&)> fileLockQuery;
    std::wstring portText;
    PortScanOperations portScanOperations;
};

bool PreTranslateBuiltinToolMessage(const MSG& message);

bool ShowBuiltinTool(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    PluginRegistry& registry,
    const AppConfig& config,
    const std::wstring& engine,
    bool locateProcessOnOpen = false);
