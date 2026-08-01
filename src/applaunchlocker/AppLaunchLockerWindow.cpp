#include "AppLaunchLockerWindow.h"

#include "IconResolverService.h"
#include "JsonValue.h"
#include "TaskExecutionService.h"
#include "ThemedTaskProgressDialog.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <filesystem>
#include <sstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {
constexpr int ID_REFRESH = 1011;
constexpr int ID_CURRENT_TABLE = 1012;
constexpr int ID_CURRENT_DETAILS = 1013;
constexpr int ID_DISABLE = 1014;
constexpr int ID_DISABLED_DETAILS = 1021;
constexpr int ID_RESTORE = 1022;
constexpr int ID_TAB_CONTROL = 1030;
constexpr int ID_APP_ENTRY_TABLE = 1040;
constexpr int ID_COPY_DETAILS = 1041;
constexpr int ID_DETAIL_TEXT = 1042;
constexpr int ID_CONTEXT_DETAILS = 1050;
constexpr int ID_CONTEXT_DISABLE = 1051;
constexpr int ID_CONTEXT_RESTORE = 1052;
constexpr int ID_CONTEXT_COPY_PATH = 1053;
constexpr int ID_CONTEXT_COPY_INFO = 1054;
constexpr int ID_CONTEXT_OPEN_LOCATION = 1055;
constexpr int ID_CONTEXT_PROPERTIES = 1056;
constexpr int ID_CONTEXT_COPY_SERVICE_NAME = 1057;
constexpr int ID_CONTEXT_COPY_TASK_PATH = 1058;
constexpr int ID_CONTEXT_MANAGE_ENTRIES = 1059;
constexpr int ID_ADVANCED_SOURCE_FILTER = 1060;
constexpr int ID_CONTEXT_COPY_NAME = 1061;
constexpr int ID_CONTEXT_COPY_COMMAND = 1062;
constexpr UINT WM_APP_SCAN_COMPLETE = WM_APP + 0x150;
constexpr UINT WM_APP_OPERATION_COMPLETE = WM_APP + 0x151;
constexpr UINT WM_APP_ICONS_COMPLETE = WM_APP + 0x0152;
constexpr ULONG_PTR kAppLaunchLockerIpcMessage = 0x414C4C31;

struct AppLaunchLockerIconLoadItem {
    std::intptr_t rowKey = 0;
    IconRequest request;
};

struct AppLaunchLockerIconResult {
    std::intptr_t rowKey = 0;
    ResolvedIcon icon;
};

struct AppLaunchLockerIconLoadResult {
    std::uint64_t generation = 0;
    std::vector<AppLaunchLockerIconResult> icons;
};

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(L'"');
            slashes = 0;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0;
        output.push_back(ch);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

std::wstring CurrentExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!copied) return {};
        if (copied < path.size() - 1) {
            path.resize(copied);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

bool RunningAsAdmin() {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators)) {
        CheckTokenMembership(nullptr, administrators, &isAdmin);
        FreeSid(administrators);
    }
    return isAdmin != FALSE;
}

OperationResult RunElevatedRequest(
    const std::wstring& action,
    const std::vector<std::wstring>& targets,
    const std::wstring& mode = {}) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return {false, L"无法确定程序路径。"};
    ElevatedOperationRequest request;
    std::wstring error;
    if (!CreateElevatedOperationRequest(action, targets, mode, request, error)) {
        return {false, error};
    }
    const std::wstring parameters = L"runas-operation --request " + QuoteArgument(request.requestPath.wstring()) +
        L" --token " + QuoteArgument(request.token);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        DeleteFileW(request.requestPath.c_str());
        DeleteFileW(request.resultPath.c_str());
        return {false, code == ERROR_CANCELLED ? L"已取消管理员授权。" : L"无法启动管理员操作：" + FormatLastError(code)};
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    CloseHandle(info.hProcess);
    OperationResult result;
    if (!ReadElevatedOperationResult(request, result, error)) {
        DeleteFileW(request.requestPath.c_str());
        return {false, error.empty() ? L"管理员操作失败，请刷新后重试。" : error};
    }
    return result;
}

OperationResult RunElevatedBatch(const std::vector<std::wstring>& ids, bool restore) {
    if (ids.empty()) return {false, restore ? L"没有可恢复的入口。" : L"没有可禁用的入口。"};
    return RunElevatedRequest(restore ? L"restore-many" : L"disable-many", ids);
}

std::filesystem::path WindowStatePath() {
    return AppLaunchLockerDataDirectory() / L"startup-window.ini";
}

void RestoreWindowPosition(HWND hwnd) {
    RECT rect{};
    if (!hwnd || !GetWindowRect(hwnd, &rect)) {
        return;
    }
    const std::filesystem::path path = WindowStatePath();
    wchar_t xBuffer[32]{};
    wchar_t yBuffer[32]{};
    GetPrivateProfileStringW(L"window", L"x", L"", xBuffer, _countof(xBuffer), path.c_str());
    GetPrivateProfileStringW(L"window", L"y", L"", yBuffer, _countof(yBuffer), path.c_str());
    const std::optional<int> x = ParseInt(xBuffer);
    const std::optional<int> y = ParseInt(yBuffer);
    if (!x || !y) {
        return;
    }
    const std::optional<POINT> restored = ThemedWindowUi::RestoredWindowPosition(
        *x,
        *y,
        rect.right - rect.left,
        rect.bottom - rect.top);
    if (!restored) {
        return;
    }
    SetWindowPos(hwnd, nullptr, restored->x, restored->y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void SaveWindowPosition(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) {
        return;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return;
    }
    const std::filesystem::path path = WindowStatePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    WritePrivateProfileStringW(L"window", L"version", L"1", path.c_str());
    WritePrivateProfileStringW(L"window", L"x", std::to_wstring(rect.left).c_str(), path.c_str());
    WritePrivateProfileStringW(L"window", L"y", std::to_wstring(rect.top).c_str(), path.c_str());
    WritePrivateProfileStringW(L"window", L"dpi", std::to_wstring(GetDpiForWindow(hwnd)).c_str(), path.c_str());
}

std::wstring DisplayTimestamp(std::wstring value) {
    if (value.size() >= 10) return value.substr(0, 10);
    return value;
}

std::wstring MapField(const std::map<std::wstring, std::wstring>& values, const wchar_t* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::wstring{} : found->second;
}

std::wstring EmptyAsNone(const std::wstring& value) {
    return value.empty() ? std::wstring(L"(无)") : value;
}

std::wstring BoolFieldText(const std::wstring& value) {
    if (value == L"1" || value == L"true" || value == L"TRUE") return L"是";
    if (value == L"0" || value == L"false" || value == L"FALSE") return L"否";
    return value.empty() ? std::wstring(L"(未知)") : value;
}

std::wstring DwordFieldText(const std::wstring& value) {
    return value.empty() ? std::wstring{} : value;
}

std::wstring ServiceStartTypeText(const std::wstring& value) {
    if (value == L"0") return L"Boot（启动加载）";
    if (value == L"1") return L"System（系统加载）";
    if (value == L"2") return L"自动";
    if (value == L"3") return L"手动";
    if (value == L"4") return L"禁用";
    return DwordFieldText(value);
}

std::wstring ServiceCurrentStateText(const std::wstring& value) {
    if (value == L"1") return L"已停止";
    if (value == L"2") return L"正在启动";
    if (value == L"3") return L"正在停止";
    if (value == L"4") return L"正在运行";
    if (value == L"5") return L"继续挂起";
    if (value == L"6") return L"暂停挂起";
    if (value == L"7") return L"已暂停";
    return DwordFieldText(value);
}

std::wstring ServiceTypeText(const std::wstring& value) {
    if (value.empty()) return {};
    const DWORD type = wcstoul(value.c_str(), nullptr, 10);
    std::vector<std::wstring> parts;
    if (type & SERVICE_KERNEL_DRIVER) parts.push_back(L"内核驱动");
    if (type & SERVICE_FILE_SYSTEM_DRIVER) parts.push_back(L"文件系统驱动");
    if (type & SERVICE_RECOGNIZER_DRIVER) parts.push_back(L"识别驱动");
    if (type & SERVICE_WIN32_OWN_PROCESS) parts.push_back(L"独立进程服务");
    if (type & SERVICE_WIN32_SHARE_PROCESS) parts.push_back(L"共享进程服务");
    if (type & SERVICE_INTERACTIVE_PROCESS) parts.push_back(L"可交互服务");
    if (parts.empty()) return value;
    std::wstring output;
    for (const std::wstring& part : parts) {
        if (!output.empty()) output += L"、";
        output += part;
    }
    return output;
}

void AppendDetailLine(std::wostringstream& text, const wchar_t* label, const std::wstring& value) {
    text << label << L"：" << EmptyAsNone(value) << L"\n";
}

IconFallbackKind IconFallbackKindForSource(StartupSourceType source) {
    switch (source) {
    case StartupSourceType::Registry: return IconFallbackKind::Registry;
    case StartupSourceType::StartupFolder: return IconFallbackKind::StartupFolder;
    case StartupSourceType::ScheduledTask: return IconFallbackKind::ScheduledTask;
    case StartupSourceType::Service: return IconFallbackKind::Service;
    case StartupSourceType::ActiveSetup: return IconFallbackKind::ActiveSetup;
    case StartupSourceType::Driver: return IconFallbackKind::Driver;
    case StartupSourceType::WmiSubscription: return IconFallbackKind::Wmi;
    case StartupSourceType::Winlogon:
    case StartupSourceType::WinlogonNotify: return IconFallbackKind::Login;
    case StartupSourceType::AppInitDll:
    case StartupSourceType::AppCertDll:
    case StartupSourceType::KnownDll: return IconFallbackKind::Dll;
    case StartupSourceType::ShellExtension: return IconFallbackKind::ShellExtension;
    case StartupSourceType::BootExecute:
    case StartupSourceType::Ifeo: return IconFallbackKind::System;
    default: return IconFallbackKind::Application;
    }
}

SHSTOCKICONID StockIconForSource(StartupSourceType source) {
    if (source == StartupSourceType::Driver) return SIID_SHIELD;
    if (source == StartupSourceType::StartupFolder) return SIID_FOLDER;
    return SIID_APPLICATION;
}

bool PreferSemanticFallbackForSource(StartupSourceType source) {
    switch (source) {
    case StartupSourceType::ScheduledTask:
    case StartupSourceType::Service:
    case StartupSourceType::ActiveSetup:
    case StartupSourceType::WmiSubscription:
    case StartupSourceType::Winlogon:
    case StartupSourceType::WinlogonNotify:
    case StartupSourceType::AppInitDll:
    case StartupSourceType::AppCertDll:
    case StartupSourceType::BootExecute:
    case StartupSourceType::KnownDll:
    case StartupSourceType::ShellExtension:
    case StartupSourceType::Ifeo:
        return true;
    default:
        return false;
    }
}

IconRequest IconRequestFromFields(
    const StartupSourceType source,
    const std::wstring& command,
    const std::map<std::wstring, std::wstring>& original,
    const int size) {
    IconRequest request;
    request.size = size;
    request.allowFallback = true;
    request.stockIcon = StockIconForSource(source);
    request.fallbackKind = IconFallbackKindForSource(source);
    request.preferFallbackForGenericHost = PreferSemanticFallbackForSource(source);

    const std::wstring originalPath = MapField(original, L"originalPath");
    if (!originalPath.empty()) {
        request.kind = IconSourceKind::FilePath;
        request.value = originalPath;
        return request;
    }

    const std::wstring targetPath = MapField(original, L"targetPath");
    if (!targetPath.empty()) {
        request.kind = IconSourceKind::FilePath;
        request.value = targetPath;
        return request;
    }

    const std::wstring valueData = MapField(original, L"valueData");
    if (!valueData.empty()) {
        request.kind = IconSourceKind::CommandLine;
        request.value = valueData;
        return request;
    }

    const std::wstring binaryPath = MapField(original, L"binaryPath");
    if (!binaryPath.empty()) {
        request.kind = IconSourceKind::CommandLine;
        request.value = binaryPath;
        return request;
    }

    const std::wstring actions = MapField(original, L"actions");
    if (!actions.empty()) {
        request.kind = IconSourceKind::CommandLine;
        request.value = actions;
        return request;
    }

    if (!command.empty()) {
        request.kind = IconSourceKind::CommandLine;
        request.value = command;
        return request;
    }

    request.kind = IconSourceKind::DefaultCategory;
    return request;
}

bool IsAdvancedSource(StartupSourceType source) {
    switch (source) {
    case StartupSourceType::WmiSubscription:
    case StartupSourceType::Winlogon:
    case StartupSourceType::WinlogonNotify:
    case StartupSourceType::AppInitDll:
    case StartupSourceType::AppCertDll:
    case StartupSourceType::BootExecute:
    case StartupSourceType::KnownDll:
    case StartupSourceType::ShellExtension:
    case StartupSourceType::Ifeo:
        return true;
    default:
        return false;
    }
}

std::vector<std::wstring> AdvancedSourceFilterItems() {
    return {L"全部高级项", L"WMI", L"登录项", L"登录通知", L"AppInit DLL", L"AppCert DLL",
        L"启动执行", L"已知 DLL", L"Shell 扩展", L"IFEO"};
}

StartupSourceType AdvancedSourceFilterType(int index) {
    switch (index) {
    case 1: return StartupSourceType::WmiSubscription;
    case 2: return StartupSourceType::Winlogon;
    case 3: return StartupSourceType::WinlogonNotify;
    case 4: return StartupSourceType::AppInitDll;
    case 5: return StartupSourceType::AppCertDll;
    case 6: return StartupSourceType::BootExecute;
    case 7: return StartupSourceType::KnownDll;
    case 8: return StartupSourceType::ShellExtension;
    case 9: return StartupSourceType::Ifeo;
    default: return StartupSourceType::Registry;
    }
}

bool AdvancedSourceFilterMatches(StartupSourceType source, int index) {
    if (index <= 0) return true;
    return source == AdvancedSourceFilterType(index);
}
bool SourceBelongsToTab(StartupSourceType source, AppLaunchLockerWindow::MainTab tab) {
    switch (tab) {
    case AppLaunchLockerWindow::MainTab::StartupItems:
        return source == StartupSourceType::Registry ||
            source == StartupSourceType::StartupFolder ||
            source == StartupSourceType::ActiveSetup;
    case AppLaunchLockerWindow::MainTab::Services:
        return source == StartupSourceType::Service;
    case AppLaunchLockerWindow::MainTab::ScheduledTasks:
        return source == StartupSourceType::ScheduledTask;
    case AppLaunchLockerWindow::MainTab::Drivers:
        return source == StartupSourceType::Driver;
    case AppLaunchLockerWindow::MainTab::Advanced:
        return IsAdvancedSource(source);
    }
    return false;
}

bool SourceBelongsToApplicationPage(StartupSourceType source) {
    return source == StartupSourceType::Registry ||
        source == StartupSourceType::StartupFolder ||
        source == StartupSourceType::ActiveSetup ||
        source == StartupSourceType::ScheduledTask ||
        source == StartupSourceType::Service;
}

bool EntryBelongsToApplicationPage(const StartupApplicationEntry& entry) {
    if (!SourceBelongsToApplicationPage(entry.source)) return false;
    if (entry.source == StartupSourceType::ScheduledTask) {
        return MapField(entry.original, L"autoStartTrigger") != L"0";
    }
    return true;
}

bool ApplicationBelongsToStartupPage(const StartupApplication& application) {
    return std::any_of(application.entries.begin(), application.entries.end(), [](const StartupApplicationEntry& entry) {
        return EntryBelongsToApplicationPage(entry);
    });
}

std::wstring JoinUniqueSources(const StartupApplication& application) {
    std::vector<std::wstring> sources;
    for (const StartupApplicationEntry& entry : application.entries) {
        if (!EntryBelongsToApplicationPage(entry)) continue;
        const std::wstring source = StartupSourceText(entry.source);
        if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
            sources.push_back(source);
        }
    }
    std::wstring text;
    for (const std::wstring& source : sources) {
        if (!text.empty()) text += L" / ";
        text += source;
    }
    return text.empty() ? L"—" : text;
}

std::wstring ApplicationEntryCountText(const StartupApplication& application) {
    const auto count = std::count_if(application.entries.begin(), application.entries.end(), [](const StartupApplicationEntry& entry) {
        return EntryBelongsToApplicationPage(entry);
    });
    return std::to_wstring(count) + L" 个";
}

std::wstring ApplicationOperationText(const StartupApplication& application) {
    int enabledManageable = 0;
    int disabled = 0;
    for (const StartupApplicationEntry& entry : application.entries) {
        if (!EntryBelongsToApplicationPage(entry)) continue;
        if (entry.state == StartupEntryState::Enabled && entry.canDisable) ++enabledManageable;
        if (entry.state == StartupEntryState::DisabledByTool && entry.canRestore) ++disabled;
    }
    if (enabledManageable > 0 && disabled > 0) return L"管理";
    if (enabledManageable > 0) return L"禁用";
    if (disabled > 0) return L"恢复";
    return L"—";
}

std::vector<std::wstring> ApplicationDisableIds(const StartupApplication& application) {
    std::vector<std::wstring> ids;
    for (const StartupApplicationEntry& entry : application.entries) {
        if (!EntryBelongsToApplicationPage(entry)) continue;
        if (entry.state == StartupEntryState::Enabled && entry.canDisable) ids.push_back(entry.entryId);
    }
    return ids;
}

std::vector<std::wstring> ApplicationRestoreIds(const StartupApplication& application) {
    std::vector<std::wstring> ids;
    for (const StartupApplicationEntry& entry : application.entries) {
        if (!EntryBelongsToApplicationPage(entry)) continue;
        if (entry.state == StartupEntryState::DisabledByTool && entry.canRestore) ids.push_back(entry.entryId);
    }
    return ids;
}

bool ApplicationNeedsAdminForDisable(const StartupApplication& application) {
    return std::any_of(application.entries.begin(), application.entries.end(), [](const StartupApplicationEntry& entry) {
        return EntryBelongsToApplicationPage(entry) &&
            entry.state == StartupEntryState::Enabled && entry.canDisable && entry.requiresAdmin;
    });
}

bool ApplicationNeedsAdminForRestore(const StartupApplication& application) {
    return std::any_of(application.entries.begin(), application.entries.end(), [](const StartupApplicationEntry& entry) {
        return EntryBelongsToApplicationPage(entry) &&
            entry.state == StartupEntryState::DisabledByTool && entry.canRestore && entry.requiresAdmin;
    });
}

IconRequest IconRequestFromApplication(const StartupApplication& application, int size) {
    StartupSourceType primarySource = StartupSourceType::Registry;
    for (const StartupApplicationEntry& entry : application.entries) {
        if (!EntryBelongsToApplicationPage(entry)) continue;
        primarySource = entry.source;
        break;
    }
    if (!application.targetPath.empty()) {
        IconRequest request;
        request.kind = IconSourceKind::FilePath;
        request.value = application.targetPath;
        request.size = size;
        request.allowFallback = true;
        request.stockIcon = StockIconForSource(primarySource);
        request.fallbackKind = IconFallbackKindForSource(primarySource);
        request.preferFallbackForGenericHost = PreferSemanticFallbackForSource(primarySource);
        return request;
    }
    for (const StartupApplicationEntry& entry : application.entries) {
        if (!EntryBelongsToApplicationPage(entry)) continue;
        return IconRequestFromFields(entry.source, entry.command, entry.original, size);
    }
    IconRequest request;
    request.size = size;
    request.allowFallback = true;
    request.stockIcon = StockIconForSource(primarySource);
    request.fallbackKind = IconFallbackKindForSource(primarySource);
    request.kind = IconSourceKind::DefaultCategory;
    return request;
}

std::wstring EntryScopeText(const StartupApplicationEntry& entry) {
    const std::wstring hive = MapField(entry.original, L"hive");
    if (hive == L"HKCU") return L"当前用户";
    if (hive == L"HKLM") return L"系统";
    if (entry.source == StartupSourceType::StartupFolder) return entry.requiresAdmin ? L"所有用户" : L"当前用户";
    if (entry.requiresAdmin) return L"系统";
    return L"当前用户";
}

std::wstring EntryOperationText(const StartupApplicationEntry& entry) {
    if (entry.state == StartupEntryState::Enabled && entry.canDisable) {
        return entry.source == StartupSourceType::Service ? L"改手动" : L"禁用";
    }
    if (entry.state == StartupEntryState::DisabledByTool && entry.canRestore) return L"恢复";
    return L"—";
}

std::wstring EntryDetailsText(const StartupApplicationEntry& entry) {
    std::wostringstream text;
    AppendDetailLine(text, L"来源", StartupSourceText(entry.source));
    AppendDetailLine(text, L"范围", EntryScopeText(entry));
    AppendDetailLine(text, L"入口名称", entry.name);
    AppendDetailLine(text, L"状态", StartupEntryStateText(entry.state));
    if (entry.source == StartupSourceType::Registry) {
        const std::wstring hive = MapField(entry.original, L"hive");
        const std::wstring key = MapField(entry.original, L"key");
        AppendDetailLine(text, L"注册表位置", hive.empty() && key.empty() ? std::wstring{} : hive + L"\\" + key);
        AppendDetailLine(text, L"值名称", MapField(entry.original, L"valueName"));
        AppendDetailLine(text, L"值类型", MapField(entry.original, L"valueType"));
        AppendDetailLine(text, L"值数据", MapField(entry.original, L"valueData"));
    } else if (entry.source == StartupSourceType::StartupFolder) {
        AppendDetailLine(text, L"启动目录文件", MapField(entry.original, L"originalPath"));
        AppendDetailLine(text, L"目标路径", MapField(entry.original, L"targetPath"));
    } else if (entry.source == StartupSourceType::Service || entry.source == StartupSourceType::Driver) {
        AppendDetailLine(text, L"显示名称", MapField(entry.original, L"displayName"));
        AppendDetailLine(text, L"服务名", MapField(entry.original, L"serviceName"));
        AppendDetailLine(text, L"服务类型", ServiceTypeText(MapField(entry.original, L"serviceType")));
        AppendDetailLine(text, L"启动类型", ServiceStartTypeText(MapField(entry.original, L"startType")));
        AppendDetailLine(text, L"运行状态", ServiceCurrentStateText(MapField(entry.original, L"currentState")));
        AppendDetailLine(text, L"登录账户", MapField(entry.original, L"startName"));
        AppendDetailLine(text, L"延迟自动启动", BoolFieldText(MapField(entry.original, L"delayed")));
        AppendDetailLine(text, L"受保护服务", BoolFieldText(MapField(entry.original, L"protected")));
        AppendDetailLine(text, L"二进制路径", MapField(entry.original, L"binaryPath"));
    } else if (entry.source == StartupSourceType::ScheduledTask) {
        AppendDetailLine(text, L"任务路径", MapField(entry.original, L"taskPath"));
        AppendDetailLine(text, L"任务名称", MapField(entry.original, L"taskName"));
        AppendDetailLine(text, L"触发器", MapField(entry.original, L"triggerSummary"));
        AppendDetailLine(text, L"动作", MapField(entry.original, L"actions"));
    } else {
        AppendDetailLine(text, L"位置", entry.location);
        AppendDetailLine(text, L"原始目标", MapField(entry.original, L"targetPath"));
    }
    AppendDetailLine(text, L"命令", entry.command);
    AppendDetailLine(text, L"需要管理员", entry.requiresAdmin ? std::wstring(L"是") : std::wstring(L"否"));
    AppendDetailLine(text, L"可执行操作", EntryOperationText(entry));
    return text.str();
}

std::wstring ApplicationDetailsText(const StartupApplication& application) {
    std::wostringstream text;
    text << L"应用：" << application.displayName << L"\n";
    text << L"状态：" << StartupApplicationStateText(application) << L"\n";
    text << L"路径：" << (application.targetPath.empty() ? std::wstring(L"(无法确定)") : application.targetPath) << L"\n";
    text << L"入口来源：" << JoinUniqueSources(application) << L"\n";
    text << L"入口数量：" << ApplicationEntryCountText(application) << L"\n\n";
    for (const StartupApplicationEntry& entry : application.entries) {
        if (!EntryBelongsToApplicationPage(entry)) continue;
        text << L"--- " << StartupSourceText(entry.source) << L" / " << entry.name << L" ---\n";
        text << EntryDetailsText(entry) << L"\n";
    }
    return text.str();
}
std::wstring FirstExistingPathCandidate(const std::vector<std::wstring>& candidates) {
    for (const std::wstring& candidate : candidates) {
        if (candidate.empty()) continue;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    }
    for (const std::wstring& candidate : candidates) {
        if (!candidate.empty()) return candidate;
    }
    return {};
}

bool OpenFileLocation(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring parameters = L"/select," + QuoteArgument(path);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShowFilePropertiesDialog(const std::wstring& path) {
    if (path.empty()) return false;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST;
    info.lpVerb = L"properties";
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

std::wstring EntrySummary(const StartupItem& item) {
    if (item.source == StartupSourceType::Registry) {
        const std::wstring hive = MapField(item.original, L"hive");
        const std::wstring key = MapField(item.original, L"key");
        if (!hive.empty() && !key.empty()) {
            const std::wstring lowerKey = ToLower(key);
            if (lowerKey.find(L"runonce") != std::wstring::npos) return hive + L" RunOnce";
            if (lowerKey.find(L"run") != std::wstring::npos) return hive + L" Run";
            return hive + L" 注册表";
        }
    }
    if (item.source == StartupSourceType::StartupFolder) {
        return item.requiresAdmin ? L"公共启动目录" : L"当前用户启动目录";
    }
    if (item.source == StartupSourceType::ScheduledTask) {
        const std::wstring trigger = MapField(item.original, L"triggerSummary");
        return trigger.empty() ? std::wstring(L"计划任务") : L"触发：" + trigger;
    }
    if (item.source == StartupSourceType::Service) {
        const std::wstring startType = ServiceStartTypeText(MapField(item.original, L"startType"));
        const std::wstring currentState = ServiceCurrentStateText(MapField(item.original, L"currentState"));
        std::wstring summary;
        if (!startType.empty()) summary += startType;
        if (!currentState.empty()) {
            if (!summary.empty()) summary += L" · ";
            summary += currentState;
        }
        if (MapField(item.original, L"delayed") == L"1") {
            if (!summary.empty()) summary += L" · ";
            summary += L"延迟";
        }
        return summary.empty() ? std::wstring(L"服务") : summary;
    }
    if (item.source == StartupSourceType::Driver) {
        const std::wstring startType = ServiceStartTypeText(MapField(item.original, L"startType"));
        const std::wstring currentState = ServiceCurrentStateText(MapField(item.original, L"currentState"));
        std::wstring summary = startType.empty() ? std::wstring(L"驱动服务") : startType;
        if (!currentState.empty()) summary += L" · " + currentState;
        if (MapField(item.original, L"protected") == L"1") summary += L" · 受保护";
        return summary;
    }
    if (IsAdvancedSource(item.source)) {
        return StartupSourceText(item.source);
    }
    return StartupSourceText(item.source);
}

std::wstring EntryStateText(const StartupItem& item) {
    if (item.source == StartupSourceType::ScheduledTask && MapField(item.original, L"currentlyEnabled") == L"0") {
        return L"已禁用（外部）";
    }
    if (item.readOnly || !item.canDisable) return L"仅查看";
    if (item.source == StartupSourceType::Service) return L"可管理";
    return L"已启用";
}

int ActionColumnWidth(const ThemedUi& ui, std::initializer_list<std::wstring_view> candidateTexts) {
    int width = ui.tableColumnWidth(candidateTexts);
    for (const std::wstring_view text : candidateTexts) {
        width = std::max(width, ui.buttonWidth(std::wstring(text),
            ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text));
    }
    return width + ui.scale(4);
}

std::vector<ThemedTableColumn> MainTableColumns(const ThemedUi& ui, AppLaunchLockerWindow::MainTab tab) {
    switch (tab) {
    case AppLaunchLockerWindow::MainTab::Services:
        return {{L"service", L"服务", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
            {L"start", L"启动类型", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"延迟自动")},
            {L"running", L"运行状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"正在运行")},
            {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"已禁用")},
            {L"details", L"详情", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"详情"})},
            {L"operation", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"改手动", L"禁用", L"恢复"})}};
    case AppLaunchLockerWindow::MainTab::ScheduledTasks:
        return {{L"task", L"任务", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
            {L"trigger", L"触发器", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"会话变化（共 2 个触发器）")},
            {L"source", L"来源", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"计划任务")},
            {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"已禁用")},
            {L"details", L"详情", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"详情"})},
            {L"operation", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"禁用", L"恢复"})}};
    case AppLaunchLockerWindow::MainTab::Drivers:
        return {{L"driver", L"驱动", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
            {L"load", L"加载类型", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"System（系统加载）")},
            {L"running", L"运行状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"正在运行")},
            {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"仅查看")},
            {L"details", L"详情", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"详情"})},
            {L"operation", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"—")}};
    case AppLaunchLockerWindow::MainTab::Advanced:
        return {{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
            {L"source", L"来源", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"Shell 扩展")},
            {L"risk", L"风险摘要", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"系统高级启动来源")},
            {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"仅查看")},
            {L"details", L"详情", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"详情"})},
            {L"operation", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"—")}};
    case AppLaunchLockerWindow::MainTab::StartupItems:
    default:
        return {{L"name", L"应用", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
            {L"source", L"入口来源", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"注册表 / 计划任务")},
            {L"entry", L"入口", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"12 个", L"166 个"})},
            {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"部分禁用")},
            {L"details", L"详情", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"详情"})},
            {L"operation", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ActionColumnWidth(ui, {L"改手动", L"禁用", L"恢复", L"管理"})}};
    }
}

std::vector<ThemedTableCell> ApplicationRowCells(const StartupApplication& application, int image) {
    const std::wstring operation = ApplicationOperationText(application);
    const int operationAction = operation == L"禁用" ? ID_CONTEXT_DISABLE
        : operation == L"恢复" ? ID_CONTEXT_RESTORE
        : operation == L"管理" ? ID_CONTEXT_DETAILS : 0;
    return {{application.displayName, image}, {JoinUniqueSources(application)},
        {ApplicationEntryCountText(application), -1, ThemedTableCellRole::Action, ID_CONTEXT_DETAILS},
        {StartupApplicationStateText(application)},
        {L"详情", -1, ThemedTableCellRole::Action, ID_CONTEXT_DETAILS},
        {operation, -1, operationAction ? ThemedTableCellRole::Action : ThemedTableCellRole::Text, operationAction}};
}

std::wstring StartupItemOperationText(const StartupItem& item) {
    if (!item.canDisable) return L"—";
    return item.source == StartupSourceType::Service ? std::wstring(L"改手动") : std::wstring(L"禁用");
}

std::vector<ThemedTableCell> StartupItemRowCells(const StartupItem& item, int image, AppLaunchLockerWindow::MainTab tab) {
    const std::wstring operation = StartupItemOperationText(item);
    const ThemedTableCell details{L"详情", -1, ThemedTableCellRole::Action, ID_CONTEXT_DETAILS};
    const ThemedTableCell operationCell{operation, -1,
        operation == L"—" ? ThemedTableCellRole::Text : ThemedTableCellRole::Action,
        operation == L"—" ? 0 : ID_CONTEXT_DISABLE};
    switch (tab) {
    case AppLaunchLockerWindow::MainTab::Services:
        return {{item.name, image},
            {ServiceStartTypeText(MapField(item.original, L"startType"))},
            {ServiceCurrentStateText(MapField(item.original, L"currentState"))},
            {EntryStateText(item)}, details, operationCell};
    case AppLaunchLockerWindow::MainTab::ScheduledTasks:
        return {{item.name, image},
            {EmptyAsNone(MapField(item.original, L"triggerSummary"))},
            {StartupSourceText(item.source)},
            {EntryStateText(item)}, details, operationCell};
    case AppLaunchLockerWindow::MainTab::Drivers:
        return {{item.name, image},
            {ServiceStartTypeText(MapField(item.original, L"startType"))},
            {ServiceCurrentStateText(MapField(item.original, L"currentState"))},
            {EntryStateText(item)}, details, {L"—"}};
    case AppLaunchLockerWindow::MainTab::Advanced:
        return {{item.name, image}, {StartupSourceText(item.source)}, {EntrySummary(item)}, {EntryStateText(item)}, details, {L"—"}};
    default:
        return {{item.name, image}, {StartupSourceText(item.source)}, {EntrySummary(item)}, {EntryStateText(item)}, details, operationCell};
    }
}

std::vector<ThemedTableCell> DisabledRecordRowCells(const DisabledRecord& record, int image, AppLaunchLockerWindow::MainTab tab) {
    StartupItem item{record.itemId, record.name, record.source, L"", L"", record.requiresAdmin, false, true, record.original};
    const ThemedTableCell details{L"详情", -1, ThemedTableCellRole::Action, ID_CONTEXT_DETAILS};
    const ThemedTableCell restore{L"恢复", -1, ThemedTableCellRole::Action, ID_CONTEXT_RESTORE};
    switch (tab) {
    case AppLaunchLockerWindow::MainTab::Services:
        return {{record.name, image}, {ServiceStartTypeText(MapField(record.original, L"startType"))},
            {L"—"}, {L"已禁用"}, details, restore};
    case AppLaunchLockerWindow::MainTab::ScheduledTasks:
        return {{record.name, image}, {EmptyAsNone(MapField(record.original, L"triggerSummary"))},
            {StartupSourceText(record.source)}, {L"已禁用"}, details, restore};
    case AppLaunchLockerWindow::MainTab::Drivers:
        return {{record.name, image}, {ServiceStartTypeText(MapField(record.original, L"startType"))},
            {L"—"}, {L"已禁用"}, details, {L"—"}};
    case AppLaunchLockerWindow::MainTab::Advanced:
        return {{record.name, image}, {StartupSourceText(record.source)}, {EntrySummary(item)}, {L"已禁用"}, details, restore};
    default:
        return {{record.name, image}, {StartupSourceText(record.source)}, {EntrySummary(item)}, {L"已禁用"}, details, restore};
    }
}
std::wstring SnapshotDiffSummary(const StartupSnapshotDiff& diff) {
    std::wostringstream summary;
    bool hasPart = false;
    const auto append = [&](const wchar_t* label, std::size_t count) {
        if (count == 0) return;
        if (hasPart) summary << L" · ";
        summary << label << L" " << count;
        hasPart = true;
    };
    append(L"新增", diff.added.size());
    append(L"移除", diff.removed.size());
    append(L"变化", diff.changed.size());
    append(L"状态变化", diff.stateChanged.size());
    return hasPart ? summary.str() : std::wstring(L"无变化");
}

StartupApplicationEntry EntryFromStartupItem(const StartupItem& item) {
    StartupApplicationEntry entry;
    entry.entryId = item.id;
    entry.source = item.source;
    entry.name = item.name;
    entry.location = item.location;
    entry.command = item.command;
    entry.state = item.readOnly || !item.canDisable ? StartupEntryState::ReadOnly : StartupEntryState::Enabled;
    entry.requiresAdmin = item.requiresAdmin;
    entry.canDisable = item.canDisable;
    entry.canRestore = false;
    entry.readOnly = item.readOnly;
    entry.original = item.original;
    return entry;
}

StartupApplicationEntry EntryFromDisabledRecord(const DisabledRecord& record) {
    StartupApplicationEntry entry;
    entry.entryId = record.recordId;
    entry.source = record.source;
    entry.name = record.name;
    entry.location = FirstExistingPathCandidate({
        MapField(record.original, L"originalPath"),
        MapField(record.original, L"taskPath"),
        MapField(record.original, L"serviceName"),
        MapField(record.original, L"key"),
    });
    entry.command = MapField(record.original, L"valueData");
    if (entry.command.empty()) entry.command = MapField(record.original, L"binaryPath");
    if (entry.command.empty()) entry.command = MapField(record.original, L"actions");
    entry.state = StartupEntryState::DisabledByTool;
    entry.requiresAdmin = record.requiresAdmin;
    entry.canDisable = false;
    entry.canRestore = true;
    entry.readOnly = false;
    entry.original = record.original;
    return entry;
}

StartupApplication SingleEntryApplication(StartupApplicationEntry entry, std::wstring targetPath = {}) {
    StartupApplication application;
    application.displayName = entry.name.empty() ? StartupSourceText(entry.source) : entry.name;
    application.targetPath = std::move(targetPath);
    if (application.targetPath.empty()) {
        application.targetPath = FirstExistingPathCandidate({
            MapField(entry.original, L"targetPath"),
            MapField(entry.original, L"originalPath"),
            MapField(entry.original, L"valueData"),
            MapField(entry.original, L"binaryPath"),
            entry.command,
            entry.location,
        });
    }
    application.appId = entry.entryId.empty() ? application.displayName : entry.entryId;
    application.entries.push_back(std::move(entry));
    return application;
}

class ApplicationDetailsDialog {
public:
    ApplicationDetailsDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        StartupApplication application,
        std::function<void(const StartupApplicationEntry&)> entryAction,
        std::wstring title = L"应用自启动详情",
        bool applicationPageOnly = true)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          application_(std::move(application)),
          entryAction_(std::move(entryAction)),
          title_(std::move(title)),
          applicationPageOnly_(applicationPageOnly) {}

    void Run() {
        const std::wstring className = L"AppLaunchLockerApplicationDetails_" + std::to_wstring(GetTickCount64());
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        auto options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), title_.c_str(), Proc, this, icon, icon);
        options.clientWidth = 760;
        options.clientHeight = 560;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) return;
        windowUi_->ShowModal();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ApplicationDetailsDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ApplicationDetailsDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ApplicationDetailsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool VisibleEntry(const StartupApplicationEntry& entry) const {
        return !applicationPageOnly_ || EntryBelongsToApplicationPage(entry);
    }

    std::wstring VisibleEntryCountText() const {
        const auto count = std::count_if(application_.entries.begin(), application_.entries.end(),
            [&](const StartupApplicationEntry& entry) { return VisibleEntry(entry); });
        return std::to_wstring(count) + L" 个";
    }

    std::wstring VisibleSourcesText() const {
        std::vector<std::wstring> sources;
        for (const StartupApplicationEntry& entry : application_.entries) {
            if (!VisibleEntry(entry)) continue;
            const std::wstring source = StartupSourceText(entry.source);
            if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
                sources.push_back(source);
            }
        }
        std::wstring text;
        for (const std::wstring& source : sources) {
            if (!text.empty()) text += L" / ";
            text += source;
        }
        return text.empty() ? std::wstring(L"—") : text;
    }

    std::wstring DialogDetailsText() const {
        std::wostringstream text;
        text << L"名称：" << application_.displayName << L"\n";
        text << L"状态：" << StartupApplicationStateText(application_) << L"\n";
        text << L"路径：" << (application_.targetPath.empty() ? std::wstring(L"(无法确定)") : application_.targetPath) << L"\n";
        text << L"入口来源：" << VisibleSourcesText() << L"\n";
        text << L"入口数量：" << VisibleEntryCountText() << L"\n\n";
        for (const StartupApplicationEntry& entry : application_.entries) {
            if (!VisibleEntry(entry)) continue;
            text << L"--- " << StartupSourceText(entry.source) << L" / " << entry.name << L" ---\n";
            text << EntryDetailsText(entry) << L"\n";
        }
        return text.str();
    }

    void PopulateEntries() {
        std::vector<ThemedTableRow> rows;
        for (const StartupApplicationEntry& entry : application_.entries) {
            if (!VisibleEntry(entry)) continue;
            const std::intptr_t rowKey = EntryRowKey(entry.entryId);
            rows.push_back({rowKey,
                {{StartupSourceText(entry.source)},
                 {EntryScopeText(entry)},
                 {entry.name},
                 {StartupEntryStateText(entry.state)},
                 {EntryOperationText(entry), -1,
                     EntryOperationText(entry) == L"—" ? ThemedTableCellRole::Text : ThemedTableCellRole::Action,
                     EntryOperationText(entry) == L"—" ? 0 : 1}},
                false,
                true});
        }
        ThemedUi::SetTableRows(entryTable_, rows);
        if (!rows.empty()) {
            ThemedUi::SetTableSelectedIndex(entryTable_, 0);
            UpdateEntryDetails();
        }
    }

    std::intptr_t EntryRowKey(const std::wstring& entryId) {
        const auto found = entryRowKeys_.find(entryId);
        if (found != entryRowKeys_.end()) return found->second;
        const std::intptr_t key = nextEntryRowKey_++;
        entryRowKeys_.emplace(entryId, key);
        return key;
    }

    const StartupApplicationEntry* SelectedEntry() const {
        int selected = ThemedUi::TableSelectedIndex(entryTable_);
        if (selected < 0) selected = 0;
        int current = 0;
        for (const StartupApplicationEntry& entry : application_.entries) {
            if (!VisibleEntry(entry)) continue;
            if (current == selected) return &entry;
            ++current;
        }
        return nullptr;
    }

    void UpdateEntryDetails() {
        const StartupApplicationEntry* entry = SelectedEntry();
        ThemedUi::SetText(detailText_, entry ? EntryDetailsText(*entry) : std::wstring(L"未选择入口。"));
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT result = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
            if (message == WM_DESTROY) done_ = true;
            return result;
        }
        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact, 760, 560);
            const ThemedUi ui = windowUi_->ui();
            const RECT content = ui.contentRect();
            const int labelHeight = ui.labelHeight();
            const int pathHeight = ui.editHeight();
            const int headerY = content.top;
            const int pathY = ui.nextRowY(headerY, labelHeight);
            const int summaryY = ui.nextRowY(pathY, pathHeight);
            const int tableTop = ui.nextRowY(summaryY, labelHeight) + ui.layout().rowGap;
            const int footerHeight = ui.footerButtonHeight();
            const int detailsBottom = ui.footerButtonY(footerHeight) - ui.layout().footerGap;
            const int detailTop = detailsBottom - ui.scale(138);
            const int tableBottom = detailTop - ui.layout().rowGap;

            ui.SelectableLabel(application_.displayName + L"    状态：" + StartupApplicationStateText(application_),
                content.left, headerY, content.right - content.left);
            ui.SelectableFieldText(0, ui.editFrame(
                    content.left,
                    pathY,
                    content.right - content.left),
                L"路径：" + (application_.targetPath.empty() ? std::wstring(L"(无法确定)") : application_.targetPath));
            ui.SelectableLabel(L"共发现 " + VisibleEntryCountText() + L"：入口来源 " + VisibleSourcesText(),
                content.left, summaryY, content.right - content.left);

            ThemedTableOptions tableOptions{};
            tableOptions.allowColumnResize = true;
            tableOptions.showRowGridLines = true;
            tableOptions.showColumnGridLines = true;
            entryTable_ = ui.Table(ID_APP_ENTRY_TABLE, RECT{content.left, tableTop, content.right, tableBottom},
                {{L"source", L"来源", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"启动文件夹")},
                 {L"scope", L"范围", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"当前用户")},
                 {L"name", L"入口名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                 {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"部分禁用")},
                 {L"operation", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"改手动")}},
                tableOptions);

            detailText_ = ui.DetailText(ID_DETAIL_TEXT, RECT{content.left, detailTop, content.right, detailsBottom}, L"当前入口详细信息");
            ui.FooterButton(ID_COPY_DETAILS, L"复制详情", 0, 2);
            ui.FooterButton(IDOK, L"关闭", 1, 2, true, true);
            PopulateEntries();
            return 0;
        }
        case WM_NOTIFY: {
            ThemedTableEvent event{};
            if (ThemedUi::DecodeTableEvent(entryTable_, lParam, event)) {
                UpdateEntryDetails();
                if (event.kind == ThemedTableEventKind::ActionInvoked && entryAction_) {
                    const StartupApplicationEntry* entry = SelectedEntry();
                    if (entry && EntryOperationText(*entry) != L"—") {
                        entryAction_(*entry);
                        DestroyWindow(hwnd_);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_COPY_DETAILS) {
                if (ThemedUi::CopyTextToClipboard(hwnd_, DialogDetailsText()) && windowUi_) {
                    ThemedToastOptions toast{};
                    toast.role = ThemedToastRole::Success;
                    windowUi_->ui().ShowToast(L"详情已复制。", toast);
                }
                return 0;
            }
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) DestroyWindow(hwnd_);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    StartupApplication application_;
    std::wstring title_;
    HWND hwnd_ = nullptr;
    HWND entryTable_ = nullptr;
    HWND detailText_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    std::function<void(const StartupApplicationEntry&)> entryAction_;
    std::map<std::wstring, std::intptr_t> entryRowKeys_;
    std::intptr_t nextEntryRowKey_ = 1;
    bool applicationPageOnly_ = true;
    bool done_ = false;
};
}

AppLaunchLockerWindow::AppLaunchLockerWindow(HINSTANCE instance, Theme theme)
    : instance_(instance), theme_(std::move(theme)) {}

AppLaunchLockerWindow::~AppLaunchLockerWindow() {
    closing_ = true;
    if (scanTask_) scanTask_->RequestStop();
    StopIconLoadTask();
    if (operationTask_) {
        operationTask_->RequestStop();
        operationTask_.reset();
    }
    if (scanProgressDialog_) scanProgressDialog_->Close();
    DestroyItemImages();
}

int AppLaunchLockerWindow::Run() {
    HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    auto options = ThemedWindowUi::DialogOptions(instance_, nullptr, L"AppLaunchLockerMainWindow", L"自启动管理", Proc, this, icon, icon);
    options.clientWidth = 860;
    options.clientHeight = 580;
    std::wstring error;
    hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
    if (!hwnd_) {
        ThemedWindowUi::ShowMessageBox(nullptr, instance_, theme_, error, L"自启动管理", MB_OK | MB_ICONERROR);
        return 1;
    }
    wchar_t acceptanceDpiText[16]{};
    if (GetEnvironmentVariableW(L"QUATTRO_APP_LAUNCH_LOCKER_ACCEPTANCE_DPI", acceptanceDpiText,
            static_cast<DWORD>(std::size(acceptanceDpiText))) > 0) {
        const UINT targetDpi = static_cast<UINT>(wcstoul(acceptanceDpiText, nullptr, 10));
        const UINT currentDpi = windowUi_ ? windowUi_->dpi() : USER_DEFAULT_SCREEN_DPI;
        if (targetDpi >= 96 && targetDpi <= 480 && targetDpi != currentDpi) {
            RECT windowRect{};
            GetWindowRect(hwnd_, &windowRect);
            const int targetWidth = MulDiv(
                windowRect.right - windowRect.left, static_cast<int>(targetDpi), static_cast<int>(currentDpi));
            const int targetHeight = MulDiv(
                windowRect.bottom - windowRect.top, static_cast<int>(targetDpi), static_cast<int>(currentDpi));
            const POINT targetPosition = CenterWindowOnOwnerMonitor(nullptr, targetWidth, targetHeight);
            RECT suggested{
                targetPosition.x,
                targetPosition.y,
                targetPosition.x + targetWidth,
                targetPosition.y + targetHeight};
            SendMessageW(hwnd_, WM_DPICHANGED, MAKELONG(targetDpi, targetDpi), reinterpret_cast<LPARAM>(&suggested));
        }
    }
    RestoreWindowPosition(hwnd_);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    StartScan();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK AppLaunchLockerWindow::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    AppLaunchLockerWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<AppLaunchLockerWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<AppLaunchLockerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return window ? window->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AppLaunchLockerWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT result = 0;
    if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
        if (message == WM_DESTROY) {
            SaveWindowPosition(hwnd_);
            closing_ = true;
            if (scanTask_) scanTask_->RequestStop();
            StopIconLoadTask();
            if (scanProgressDialog_) scanProgressDialog_->Close();
            if (operationTask_) {
                operationTask_->RequestStop();
                operationTask_.reset();
            }
            DestroyItemImages();
            PostQuitMessage(0);
        }
        return result;
    }
    switch (message) {
    case WM_COPYDATA: {
        const COPYDATASTRUCT* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!data || data->dwData != kAppLaunchLockerIpcMessage || !data->lpData ||
            data->cbData < sizeof(wchar_t) || data->cbData > 4096 || data->cbData % sizeof(wchar_t) != 0) {
            return 0;
        }
        std::wstring payload(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
        if (!payload.empty() && payload.back() == L'\0') payload.pop_back();
        JsonValue root;
        std::wstring error;
        if (!ParseJson(payload, root, error) || !root.isObject() ||
            !root.get(L"protocolVersion") || root.get(L"protocolVersion")->intOr(0) != 1 ||
            !root.get(L"requestId") || root.get(L"requestId")->stringOr().empty() ||
            !root.get(L"intent") || !root.get(L"intent")->isString()) {
            return 0;
        }
        const std::wstring intent = root.get(L"intent")->stringValue;
        if (intent != L"show-main" && intent != L"select-tab") return 0;
        if (intent == L"select-tab") {
            const std::wstring tab = root.get(L"tab") ? root.get(L"tab")->stringOr() : std::wstring{};
            if (tab == L"startup") SelectTab(0);
            else if (tab == L"services") SelectTab(1);
            else if (tab == L"tasks") SelectTab(2);
            else if (tab == L"drivers") SelectTab(3);
            else if (tab == L"advanced") SelectTab(4);
            else return 0;
        }
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        else ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return 1;
    }
    case WM_CREATE:
        windowUi_ = std::make_unique<ThemedWindowUi>(instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact,
            860, 580);
        CreateControls();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        windowUi_->FillBackground(dc);
        windowUi_->DrawRegisteredTableFrames(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == ID_TAB_CONTROL && HIWORD(wParam) == CBN_SELCHANGE) SelectTab(ThemedUi::ActiveTab(tabControl_));
        else if (id == ID_REFRESH) {
            StartScan();
        } else if (id == ID_ADVANCED_SOURCE_FILTER && HIWORD(wParam) == CBN_SELCHANGE) {
            advancedSourceFilterIndex_ =
                ThemedUi::ComboBoxSelectedIndex(advancedSourceFilter_);
            RebuildRows();
        }
        else if (id == ID_DISABLE) StartDisable();
        else if (id == ID_RESTORE) StartRestore();
        else if (id == ID_CURRENT_DETAILS || id == ID_DISABLED_DETAILS || id == ID_CONTEXT_DETAILS) ShowSelectedDetails();
        else if (id == ID_CONTEXT_DISABLE) StartDisable();
        else if (id == ID_CONTEXT_RESTORE) StartRestore();
        else if (id == ID_CONTEXT_COPY_INFO) CopySelectedStartupInfo();
        else if (id == ID_CONTEXT_COPY_PATH) CopySelectedPath();
        else if (id == ID_CONTEXT_COPY_NAME) CopySelectedName();
        else if (id == ID_CONTEXT_COPY_COMMAND) CopySelectedCommand();
        else if (id == ID_CONTEXT_MANAGE_ENTRIES) ShowSelectedDetails();
        else if (id == ID_CONTEXT_COPY_SERVICE_NAME) {
            CopySelectedSourceField(L"serviceName", L"服务名已复制。");
        }
        else if (id == ID_CONTEXT_COPY_TASK_PATH) CopySelectedSourceField(L"taskPath", L"任务路径已复制。");
        else if (id == ID_CONTEXT_OPEN_LOCATION) OpenSelectedLocation();
        else if (id == ID_CONTEXT_PROPERTIES) ShowSelectedFileProperties();
        else if (id == IDCANCEL) DestroyWindow(hwnd_);
        return 0;
    }
    case WM_CONTEXTMENU: {
        if (reinterpret_cast<HWND>(wParam) != itemTable_) break;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            const int selected = ThemedUi::TableSelectedIndex(itemTable_);
            RECT cell{};
            if (selected >= 0 && ThemedUi::TableCellScreenRect(itemTable_, selected, 0, cell)) {
                point = POINT{cell.left + 8, cell.bottom};
            } else {
                GetCursorPos(&point);
            }
        }
        ShowContextMenu(point);
        return 0;
    }
    case WM_NOTIFY: {
        ThemedTableEvent event{};
        if (ThemedUi::DecodeTableEvent(itemTable_, lParam, event)) {
            if (event.kind == ThemedTableEventKind::Activated ||
                (event.kind == ThemedTableEventKind::ActionInvoked && event.actionId == ID_CONTEXT_DETAILS)) {
                ShowSelectedDetails();
            } else if (event.kind == ThemedTableEventKind::ActionInvoked && event.actionId == ID_CONTEXT_DISABLE) {
                StartDisable();
            } else if (event.kind == ThemedTableEventKind::ActionInvoked && event.actionId == ID_CONTEXT_RESTORE) {
                StartRestore();
            }
            UpdateButtons();
            return 0;
        }
        break;
    }
    case WM_APP_SCAN_COMPLETE: {
        if (!scanTask_ || !scanTask_->IsFinished()) return 0;
        scanTask_->Wait();
        ScanResult scan;
        if (scanTask_->Status() == ScanTaskStatus::Failed) {
            scan.warnings.push_back(scanTask_->Snapshot().error);
        } else {
            scan = scanTask_->ResultCopy<ScanResult>();
        }
        scanTask_.reset();
        if (scanProgressDialog_ && !QuattroTestMode()) {
            scanProgressDialog_->Close();
        }
        std::vector<DisabledRecord> disabled;
        std::wstring storeError;
        StartupManager().LoadDisabled(disabled, storeError);
        CompleteScan(std::move(scan), std::move(disabled), std::move(storeError));
        return 0;
    }
    case WM_APP_ICONS_COMPLETE:
        ApplyIconLoadResult(static_cast<std::uint64_t>(wParam));
        return 0;
    case WM_APP_OPERATION_COMPLETE: {
        if (!operationTask_ || !operationTask_->IsFinished()) return 0;
        OperationResult operation;
        if (operationTask_->Status() == TaskStatus::Completed) {
            operation = operationTask_->ResultCopy<OperationResult>();
        } else {
            operation = {false, operationTask_->Snapshot().error.empty()
                ? std::wstring(L"操作已停止。") : operationTask_->Snapshot().error};
        }
        operationTask_.reset();
        CompleteOperation(std::move(operation));
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void AppLaunchLockerWindow::CreateControls() {
    const ThemedUi ui = windowUi_->ui();
    const RECT content = ui.contentRect();
    const int scanWidth = ui.buttonWidth(
        L"扫描", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    const int filterWidth = ui.tableColumnWidth(L"全部高级项");
    const int headerY = content.top;
    const int tabTop = ui.nextRowY(headerY, ui.buttonHeight(ThemedButtonRole::Normal, ThemedButtonSize::Normal));
    const int tabHeight = ui.tabButtonHeight() + ui.layout().rowGap;
    const int listTop = tabTop + tabHeight + ui.layout().rowGap;
    const int footerY = ui.footerButtonY(ui.footerButtonHeight());
    const int statusY = footerY - ui.layout().sectionGap - ui.labelHeight();
    const int tableBottom = statusY - ui.layout().rowGap;

    ui.SelectableLabel(L"AppLaunchLocker 自启动管理", content.left, headerY,
        content.right - content.left - scanWidth - filterWidth - ui.layout().controlGapX * 2);
    advancedSourceFilter_ = ui.ComboBox(ID_ADVANCED_SOURCE_FILTER,
        content.right - scanWidth - filterWidth - ui.layout().controlGapX,
        headerY,
        filterWidth);
    ThemedUi::SetComboBoxItems(advancedSourceFilter_, AdvancedSourceFilterItems(), advancedSourceFilterIndex_);
    ThemedUi::SetVisible(advancedSourceFilter_, false);
    ui.Button(ID_REFRESH, L"扫描", content.right - scanWidth, headerY,
        ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    ThemedTabControlOptions tabOptions{};
    tabOptions.activeIndex = 0;
    tabOptions.appearance = ThemedTabControlAppearance::ConnectedTabs;
    tabOptions.orientation = ThemedTabControlOrientation::Horizontal;
    tabOptions.containerStyle = ThemedTabControlContainerStyle::Borderless;
    tabControl_ = ui.TabControl(ID_TAB_CONTROL, RECT{content.left, tabTop, content.right, tabTop + tabHeight},
        {{100, L"自启动项", true},
         {101, L"服务", true},
         {102, L"计划任务", true},
         {103, L"驱动", true},
         {104, L"系统高级项", true}},
        tabOptions);
    ThemedTableOptions gridTableOptions{};
    gridTableOptions.allowColumnResize = true;
    gridTableOptions.showRowGridLines = true;
    gridTableOptions.showColumnGridLines = true;
    itemTable_ = ui.Table(ID_CURRENT_TABLE, RECT{content.left, listTop, content.right, tableBottom},
        MainTableColumns(ui, MainTab::StartupItems),
        gridTableOptions);
    statusText_ = ui.SelectableStatusText(L"正在扫描…", content.left, statusY,
        content.right - content.left,
        {ThemedStatusRole::Info, ThemedTextAlign::Start});
    detailsButton_ = ui.FooterButton(ID_CURRENT_DETAILS, L"详情", 0, 2);
    disableButton_ = ui.FooterButton(ID_DISABLE, L"禁用", 1, 2, true, true);
    restoreButton_ = ui.FooterButton(ID_RESTORE, L"恢复", 1, 2, true, true);
    ThemedUi::SetVisible(restoreButton_, false);
    RebuildTabs();
    RebuildRows();
    UpdateButtons();
}

void AppLaunchLockerWindow::StartOperationTask(std::function<OperationResult()> operation) {
    const HWND target = hwnd_;
    TaskOptions options{};
    options.mode = TaskExecutionMode::BackgroundSingle;
    options.maxWorkers = 1;
    options.completionCallback = [target]() {
        PostMessageW(target, WM_APP_OPERATION_COMPLETE, 0, 0);
    };
    operationTask_ = TaskExecutionService::StartTyped<OperationResult>(
        std::move(options),
        [operation = std::move(operation)](TaskContext&) mutable {
            SetOperationAuditContext(
                L"gui-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()),
                L"gui",
                RunningAsAdmin());
            return operation();
        });
}

void AppLaunchLockerWindow::StartScan() {
    if (busy_) return;
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在扫描…");
    UpdateButtons();
    const HWND target = hwnd_;
    scanTask_ = StartupManager().StartScanAll(
        [target]() { PostMessageW(target, WM_APP_SCAN_COMPLETE, 0, 0); });
    ThemedTaskProgressDialogOptions progressOptions{};
    progressOptions.owner = hwnd_;
    progressOptions.instance = instance_;
    progressOptions.theme = theme_;
    progressOptions.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    progressOptions.className = L"AppLaunchLockerScanProgress_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    progressOptions.title = L"启动项扫描进度";
    const std::size_t startupSourceWorkers = std::max<std::size_t>(
        1,
        std::min<std::size_t>({
            std::size_t{9},
            std::max<std::size_t>(1, std::thread::hardware_concurrency()),
            std::size_t{8}}));
    progressOptions.initialStatus = L"正在扫描 Windows 启动来源";
    progressOptions.initialDetail = L"准备使用 " + std::to_wstring(startupSourceWorkers) +
        L" 个工作线程读取注册表、启动目录、服务和计划任务。";
    progressOptions.closeOnCompleted = !QuattroTestMode();
    progressOptions.readSnapshot = [task = scanTask_]() {
        return ToThemedTaskProgressSnapshot(task->Snapshot());
    };
    progressOptions.requestStop = [task = scanTask_]() { task->RequestStop(); };
    scanProgressDialog_ = std::make_unique<ThemedTaskProgressDialog>(std::move(progressOptions));
    scanProgressDialog_->Show();
}

void AppLaunchLockerWindow::StartDisable() {
    if (busy_ || !storeAvailable_) return;
    if (const StartupApplication* application = SelectedApplication()) {
        std::vector<std::wstring> ids = ApplicationDisableIds(*application);
        bool restore = false;
        if (ids.empty()) {
            ids = ApplicationRestoreIds(*application);
            restore = true;
        }
        if (ids.empty()) return;
        const std::wstring prompt = restore
            ? L"确定恢复“" + application->displayName + L"”的 " + std::to_wstring(ids.size()) + L" 个已禁用入口？"
            : L"确定禁用“" + application->displayName + L"”的 " + std::to_wstring(ids.size()) + L" 个可管理入口？\n\n禁用后它们将不再随系统启动，仍可手动运行。";
        if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, prompt, restore ? L"恢复自启动" : L"禁用自启动",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        const bool elevate = (restore ? ApplicationNeedsAdminForRestore(*application) : ApplicationNeedsAdminForDisable(*application)) && !RunningAsAdmin();
        busy_ = true;
        ThemedUi::SetText(statusText_, restore ? L"正在恢复…" : L"正在禁用…");
        UpdateButtons();
        StartOperationTask([ids = std::move(ids), restore, elevate]() {
            if (elevate) return RunElevatedBatch(ids, restore);
            return restore ? StartupManager().RestoreMany(ids) : StartupManager().DisableMany(ids);
        });
        return;
    }

    const StartupItem* selected = SelectedStartupItem();
    if (!selected || !selected->canDisable) return;
    std::wstring prompt = L"确定禁用“" + selected->name + L"”？\n\n禁用后它将不再随系统启动，仍可手动运行。";
    if (selected->source == StartupSourceType::Service) {
        prompt = L"确定将“" + selected->name + L"”改为手动启动？\n\n不会停止当前正在运行的服务。";
    }
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, prompt, L"禁用自启动",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const std::wstring itemId = selected->id;
    const bool elevate = selected->requiresAdmin && !RunningAsAdmin();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在禁用…");
    UpdateButtons();
    StartOperationTask([itemId, elevate]() {
        return elevate ? RunElevatedRequest(L"disable", {itemId}) : StartupManager().Disable(itemId);
    });
}
void AppLaunchLockerWindow::StartEntryOperation(StartupApplicationEntry entry) {
    if (busy_ || !storeAvailable_) return;
    const bool restore = entry.state == StartupEntryState::DisabledByTool && entry.canRestore;
    const bool disable = entry.state == StartupEntryState::Enabled && entry.canDisable;
    if (!restore && !disable) return;
    const std::wstring prompt = restore
        ? L"确定恢复“" + entry.name + L"”？"
        : (entry.source == StartupSourceType::Service
            ? L"确定将“" + entry.name + L"”改为手动启动？\n\n不会停止当前正在运行的服务。"
            : L"确定禁用“" + entry.name + L"”？\n\n禁用后它将不再随系统启动，仍可手动运行。");
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, prompt, restore ? L"恢复自启动" : L"禁用自启动",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const bool elevate = entry.requiresAdmin && !RunningAsAdmin();
    busy_ = true;
    ThemedUi::SetText(statusText_, restore ? L"正在恢复…" : L"正在禁用…");
    UpdateButtons();
    StartOperationTask([entryId = entry.entryId, restore, elevate]() {
        if (elevate) {
            return restore
                ? RunElevatedRequest(L"restore", {entryId})
                : RunElevatedRequest(L"disable", {entryId});
        }
        return restore ? StartupManager().Restore(entryId) : StartupManager().Disable(entryId);
    });
}
void AppLaunchLockerWindow::StartRestore() {
    if (busy_ || !storeAvailable_) return;
    if (const StartupApplication* application = SelectedApplication()) {
        std::vector<std::wstring> ids = ApplicationRestoreIds(*application);
        if (ids.empty()) return;
        const std::wstring prompt = L"确定恢复“" + application->displayName + L"”的 " + std::to_wstring(ids.size()) + L" 个已禁用入口？";
        if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, prompt, L"恢复自启动",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        const bool elevate = ApplicationNeedsAdminForRestore(*application) && !RunningAsAdmin();
        busy_ = true;
        ThemedUi::SetText(statusText_, L"正在恢复…");
        UpdateButtons();
        StartOperationTask([ids = std::move(ids), elevate]() {
            return elevate ? RunElevatedBatch(ids, true) : StartupManager().RestoreMany(ids);
        });
        return;
    }

    const DisabledRecord* selected = SelectedDisabledRecord();
    if (!selected) return;
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, L"确定恢复“" + selected->name + L"”？", L"恢复自启动",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const std::wstring recordId = selected->recordId;
    const bool elevate = selected->requiresAdmin && !RunningAsAdmin();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在恢复…");
    UpdateButtons();
    StartOperationTask([recordId, elevate]() {
        return elevate ? RunElevatedRequest(L"restore", {recordId}) : StartupManager().Restore(recordId);
    });
}
std::wstring SelectedTargetPath(const StartupApplication* application, const StartupItem* item, const DisabledRecord* record) {
    if (application) return application->targetPath;
    if (item) {
        return FirstExistingPathCandidate({
            MapField(item->original, L"targetPath"),
            MapField(item->original, L"originalPath"),
            item->command,
            item->location,
        });
    }
    if (record) {
        return FirstExistingPathCandidate({
            MapField(record->original, L"targetPath"),
            MapField(record->original, L"originalPath"),
            MapField(record->original, L"valueData"),
        });
    }
    return {};
}

void AppLaunchLockerWindow::ShowContextMenu(POINT screenPoint) {
    if (!itemTable_) return;
    const int row = ThemedUi::TableScreenHitTest(itemTable_, screenPoint, true);
    if (row >= 0) ThemedUi::SetTableSelectedIndex(itemTable_, row);
    const StartupApplication* application = SelectedApplication();
    const StartupItem* item = SelectedStartupItem();
    const DisabledRecord* record = SelectedDisabledRecord();
    if (!application && !item && !record) return;

    const bool canDisable = item ? item->canDisable : (application && !ApplicationDisableIds(*application).empty());
    const bool canRestore = record != nullptr || (application && !ApplicationRestoreIds(*application).empty());
    const std::wstring targetPath =
        SelectedTargetPath(application, item, record);
    const bool serviceSource =
        (item && item->source == StartupSourceType::Service) ||
        (record && record->source == StartupSourceType::Service);
    const bool taskSource =
        (item && item->source == StartupSourceType::ScheduledTask) ||
        (record && record->source == StartupSourceType::ScheduledTask);
    const std::wstring serviceName = item ? MapField(item->original, L"serviceName")
        : (record ? MapField(record->original, L"serviceName") : std::wstring{});
    const std::wstring taskPath = item ? MapField(item->original, L"taskPath") : (record ? MapField(record->original, L"taskPath") : std::wstring{});

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_CONTEXT_DETAILS, L"详情");
    if (application) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (canDisable ? 0 : MF_GRAYED), ID_CONTEXT_DISABLE, L"禁用全部可管理入口");
        AppendMenuW(menu, MF_STRING | (canRestore ? 0 : MF_GRAYED), ID_CONTEXT_RESTORE, L"恢复全部已禁用入口");
        AppendMenuW(menu, MF_STRING, ID_CONTEXT_MANAGE_ENTRIES, L"管理各启动入口…");
    } else if (serviceSource || taskSource) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const wchar_t* disableText = serviceSource ? L"改为手动启动" : L"禁用任务";
        const wchar_t* restoreText = serviceSource ? L"恢复原启动类型" : L"恢复任务";
        AppendMenuW(menu, MF_STRING | (canDisable ? 0 : MF_GRAYED), ID_CONTEXT_DISABLE, disableText);
        AppendMenuW(menu, MF_STRING | (canRestore ? 0 : MF_GRAYED), ID_CONTEXT_RESTORE, restoreText);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!application) AppendMenuW(menu, MF_STRING, ID_CONTEXT_COPY_NAME, L"复制名称");
    if (application || serviceSource) {
        AppendMenuW(menu, MF_STRING | (targetPath.empty() ? MF_GRAYED : 0), ID_CONTEXT_COPY_PATH,
            application ? L"复制应用路径" : L"复制程序路径");
    }
    if (serviceSource) AppendMenuW(menu, MF_STRING | (serviceName.empty() ? MF_GRAYED : 0), ID_CONTEXT_COPY_SERVICE_NAME, L"复制服务名");
    if (taskSource) {
        AppendMenuW(menu, MF_STRING | (taskPath.empty() ? MF_GRAYED : 0), ID_CONTEXT_COPY_TASK_PATH, L"复制任务路径");
        AppendMenuW(menu, MF_STRING, ID_CONTEXT_COPY_COMMAND, L"复制执行命令");
    } else if (!application && !serviceSource) {
        AppendMenuW(menu, MF_STRING, ID_CONTEXT_COPY_COMMAND, L"复制路径/命令");
    }
    if (application) AppendMenuW(menu, MF_STRING, ID_CONTEXT_COPY_INFO, L"复制启动信息");
    AppendMenuW(menu, MF_STRING | (targetPath.empty() ? MF_GRAYED : 0), ID_CONTEXT_OPEN_LOCATION, L"打开文件所在位置");
    AppendMenuW(menu, MF_STRING | (targetPath.empty() ? MF_GRAYED : 0), ID_CONTEXT_PROPERTIES, L"文件属性");
    ThemedPopupMenuOptions options{};
    options.source = ThemedPopupMenuSource::ClientArea;
    options.horizontalAlign = ThemedPopupMenuHorizontalAlign::Right;
    options.returnCommand = true;
    const UINT command = ThemedUi::ShowPopupMenu(hwnd_, menu, screenPoint, options).command;
    DestroyMenu(menu);
    if (command) SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void AppLaunchLockerWindow::CopySelectedStartupInfo() {
    const StartupApplication* application = SelectedApplication();
    const StartupItem* item = SelectedStartupItem();
    const DisabledRecord* record = SelectedDisabledRecord();
    std::wstring text;
    if (application) text = ApplicationDetailsText(*application);
    else if (item) text = EntryDetailsText(EntryFromStartupItem(*item));
    else if (record) {
        text = EntryDetailsText(EntryFromDisabledRecord(*record));
        text += L"禁用时间：" + EmptyAsNone(record->disabledAt) + L"\n";
    }
    if (text.empty()) return;
    if (ThemedUi::CopyTextToClipboard(hwnd_, text) && windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Success;
        windowUi_->ui().ShowToast(L"启动信息已复制。", toast);
    }
}

void AppLaunchLockerWindow::CopySelectedPath() {
    const std::wstring path = SelectedTargetPath(SelectedApplication(), SelectedStartupItem(), SelectedDisabledRecord());
    if (path.empty()) return;
    if (ThemedUi::CopyTextToClipboard(hwnd_, path) && windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Success;
        windowUi_->ui().ShowToast(L"路径已复制。", toast);
    }
}

void AppLaunchLockerWindow::CopySelectedName() {
    const StartupItem* item = SelectedStartupItem();
    const DisabledRecord* record = SelectedDisabledRecord();
    const std::wstring name = item ? item->name : (record ? record->name : std::wstring{});
    if (name.empty()) return;
    if (ThemedUi::CopyTextToClipboard(hwnd_, name) && windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Success;
        windowUi_->ui().ShowToast(L"名称已复制。", toast);
    }
}

void AppLaunchLockerWindow::CopySelectedCommand() {
    const StartupItem* item = SelectedStartupItem();
    const DisabledRecord* record = SelectedDisabledRecord();
    std::wstring command = item ? item->command : std::wstring{};
    if (command.empty() && record) command = MapField(record->original, L"actions");
    if (command.empty() && record) command = MapField(record->original, L"valueData");
    if (command.empty() && record) command = MapField(record->original, L"binaryPath");
    if (command.empty() && record) command = MapField(record->original, L"targetPath");
    if (command.empty()) return;
    if (ThemedUi::CopyTextToClipboard(hwnd_, command) && windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Success;
        windowUi_->ui().ShowToast(L"命令已复制。", toast);
    }
}

void AppLaunchLockerWindow::CopySelectedSourceField(const wchar_t* key, const std::wstring& successMessage) {
    const StartupItem* item = SelectedStartupItem();
    const DisabledRecord* record = SelectedDisabledRecord();
    const std::wstring value = item ? MapField(item->original, key) : (record ? MapField(record->original, key) : std::wstring{});
    if (value.empty()) return;
    if (ThemedUi::CopyTextToClipboard(hwnd_, value) && windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Success;
        windowUi_->ui().ShowToast(successMessage, toast);
    }
}

void AppLaunchLockerWindow::OpenSelectedLocation() {
    const std::wstring path = SelectedTargetPath(SelectedApplication(), SelectedStartupItem(), SelectedDisabledRecord());
    if (!OpenFileLocation(path) && windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Warning;
        windowUi_->ui().ShowToast(L"无法打开文件所在位置。", toast);
    }
}

void AppLaunchLockerWindow::ShowSelectedFileProperties() {
    const std::wstring path = SelectedTargetPath(SelectedApplication(), SelectedStartupItem(), SelectedDisabledRecord());
    if (!ShowFilePropertiesDialog(path) && windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Warning;
        windowUi_->ui().ShowToast(L"无法打开文件属性。", toast);
    }
}
void AppLaunchLockerWindow::CompleteScan(ScanResult result, std::vector<DisabledRecord> disabled, std::wstring storeError) {
    busy_ = false;
    storeAvailable_ = storeError.empty();
    const StartupSnapshot currentSnapshot = BuildStartupSnapshot(result, disabled);
    const bool completeScan = storeError.empty() && result.warnings.empty();
    items_ = std::move(result.items);
    disabled_ = std::move(disabled);
    ScanResult applicationScan;
    applicationScan.items = items_;
    applications_ = BuildStartupApplications(applicationScan, disabled_);
    RebuildTabs();
    RebuildRows();
    if (!storeError.empty()) {
        AppendAppLaunchLockerLog(storeError);
        ThemedUi::SetText(statusText_, storeError);
        windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Danger);
    }
    if (!result.warnings.empty()) {
        for (const auto& warning : result.warnings) AppendAppLaunchLockerLog(L"扫描警告：" + warning);
        const std::wstring title = activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())
            ? tabs_[static_cast<std::size_t>(activeTab_)].title
            : std::wstring(L"自启动项");
        const std::wstring status = L"当前页：" + title +
            L"，共 " + std::to_wstring(visibleApplicationIndexes_.size() + visibleItemIndexes_.size() + visibleDisabledIndexes_.size()) +
            L" 项；部分系统项目未能读取。";
        ThemedUi::SetText(statusText_, status);
        windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Warning);
    }
    if (completeScan) {
        StartupSnapshot previousSnapshot;
        std::wstring snapshotError;
        StartupSnapshotStore snapshotStore;
        if (!snapshotStore.Load(previousSnapshot, snapshotError)) {
            AppendAppLaunchLockerLog(snapshotError);
            ThemedUi::SetText(statusText_, L"扫描完成，但无法读取上次快照。");
            windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Warning);
        } else {
            const StartupSnapshotDiff diff = DiffStartupSnapshots(previousSnapshot, currentSnapshot);
            if (!snapshotStore.Save(currentSnapshot, snapshotError)) {
                AppendAppLaunchLockerLog(snapshotError);
                ThemedUi::SetText(statusText_, L"扫描完成，但无法保存本次快照。");
                windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Warning);
            } else {
                const std::wstring title = activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())
                    ? tabs_[static_cast<std::size_t>(activeTab_)].title
                    : std::wstring(L"自启动项");
                ThemedUi::SetText(statusText_, L"当前页：" + title +
                    L"，共 " + std::to_wstring(visibleApplicationIndexes_.size() + visibleItemIndexes_.size() + visibleDisabledIndexes_.size()) +
                    L" 项；" + SnapshotDiffSummary(diff));
                windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Normal);
            }
        }
    } else if (!result.warnings.empty()) {
        AppendAppLaunchLockerLog(L"扫描不完整，未覆盖启动项快照。");
    }
    UpdateButtons();
}

void AppLaunchLockerWindow::CompleteOperation(OperationResult result) {
    busy_ = false;
    if (!result.success) {
        AppendAppLaunchLockerLog(L"操作失败：" + result.message);
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, result.message, L"自启动管理", MB_OK | MB_ICONWARNING);
    }
    else if (windowUi_) {
        ThemedToastOptions toast{};
        toast.role = result.partial ? ThemedToastRole::Warning : ThemedToastRole::Success;
        if (result.partial) toast.durationMs = 5000;
        windowUi_->ui().ShowToast(result.message.empty() ? L"操作完成。" : result.message, toast);
    }
    StartScan();
}

void AppLaunchLockerWindow::RebuildTabs() {
    const auto countFor = [&](MainTab tab) {
        if (tab == MainTab::StartupItems) {
            return static_cast<int>(std::count_if(applications_.begin(), applications_.end(), [](const StartupApplication& application) {
                return ApplicationBelongsToStartupPage(application);
            }));
        }
        const int live = static_cast<int>(std::count_if(items_.begin(), items_.end(), [&](const StartupItem& item) {
            return SourceBelongsToTab(item.source, tab);
        }));
        const int disabled = static_cast<int>(std::count_if(disabled_.begin(), disabled_.end(), [&](const DisabledRecord& record) {
            return SourceBelongsToTab(record.source, tab);
        }));
        return live + disabled;
    };

    tabs_ = {
        {MainTab::StartupItems, L"自启动项", countFor(MainTab::StartupItems)},
        {MainTab::Services, L"服务", countFor(MainTab::Services)},
        {MainTab::ScheduledTasks, L"计划任务", countFor(MainTab::ScheduledTasks)},
        {MainTab::Drivers, L"驱动", countFor(MainTab::Drivers)},
        {MainTab::Advanced, L"系统高级项", countFor(MainTab::Advanced)},
    };

    if (activeTab_ < 0 || activeTab_ >= static_cast<int>(tabs_.size())) activeTab_ = 0;
    if (tabControl_) ThemedUi::SetActiveTab(tabControl_, activeTab_, false);
}

void AppLaunchLockerWindow::RebuildRows() {
    StopIconLoadTask();
    const int previousSelectedIndex = itemTable_ ? ThemedUi::TableSelectedIndex(itemTable_) : -1;
    const std::intptr_t previousSelectedKey = previousSelectedIndex >= 0
        ? ThemedUi::TableRowKey(itemTable_, previousSelectedIndex) : 0;
    const std::intptr_t previousTopKey = itemTable_ ? ThemedUi::TableTopVisibleRowKey(itemTable_) : 0;
    visibleApplicationIndexes_.clear();
    visibleApplicationRowKeys_.clear();
    visibleItemIndexes_.clear();
    visibleItemRowKeys_.clear();
    visibleDisabledIndexes_.clear();
    visibleDisabledRowKeys_.clear();
    std::vector<ThemedTableRow> rows;
    const TabEntry tab = activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())
        ? tabs_[static_cast<std::size_t>(activeTab_)]
        : TabEntry{MainTab::StartupItems, L"自启动项", 0};

    if (advancedSourceFilter_) {
        const bool showAdvancedFilter = tab.tab == MainTab::Advanced;
        ThemedUi::SetVisible(advancedSourceFilter_, showAdvancedFilter);
        if (showAdvancedFilter) {
            ThemedUi::SetComboBoxSelectedIndex(advancedSourceFilter_, advancedSourceFilterIndex_, false);
        }
    }
    if (tab.tab == MainTab::StartupItems) {
        for (std::size_t index = 0; index < applications_.size(); ++index) {
            const StartupApplication& application = applications_[index];
            if (!ApplicationBelongsToStartupPage(application)) continue;
            visibleApplicationIndexes_.push_back(index);
            const std::intptr_t rowKey = RowKeyForIdentity(L"app:" + application.appId);
            visibleApplicationRowKeys_.push_back(rowKey);
            rows.push_back({rowKey,
                ApplicationRowCells(application, -1),
                false, true});
        }
    } else {
        for (std::size_t index = 0; index < items_.size(); ++index) {
            const StartupItem& item = items_[index];
            if (!SourceBelongsToTab(item.source, tab.tab)) continue;
            if (tab.tab == MainTab::Advanced && !AdvancedSourceFilterMatches(item.source, advancedSourceFilterIndex_)) continue;
            visibleItemIndexes_.push_back(index);
            const std::intptr_t rowKey = RowKeyForIdentity(L"entry:" + item.id);
            visibleItemRowKeys_.push_back(rowKey);
            rows.push_back({rowKey,
                StartupItemRowCells(item, -1, tab.tab),
                false, true});
        }
        for (std::size_t index = 0; index < disabled_.size(); ++index) {
            const DisabledRecord& record = disabled_[index];
            if (!SourceBelongsToTab(record.source, tab.tab)) continue;
            if (tab.tab == MainTab::Advanced && !AdvancedSourceFilterMatches(record.source, advancedSourceFilterIndex_)) continue;
            visibleDisabledIndexes_.push_back(index);
            const std::intptr_t rowKey = RowKeyForIdentity(L"disabled:" + record.recordId);
            visibleDisabledRowKeys_.push_back(rowKey);
            rows.push_back({rowKey,
                DisabledRecordRowCells(record, -1, tab.tab),
                false, true});
        }
    }

    pendingRows_ = std::move(rows);
    pendingSelectedKey_ = previousSelectedKey;
    pendingTopKey_ = previousTopKey;
    pendingRowsTab_ = tab.tab;
    rowDisplayPending_ = true;
    itemIconSize_ = std::max(16, GetSystemMetrics(SM_CXSMICON));
    const std::wstring status = L"当前页：" + tab.title + L"，共 " + std::to_wstring(pendingRows_.size()) + L" 项";
    ThemedUi::SetText(statusText_, status);
    windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Normal);
    UpdateButtons();
    if (!StartIconLoadTask()) {
        ShowPendingRows(nullptr, {});
    }
}

void AppLaunchLockerWindow::DestroyItemImages() {
    if (itemTable_ && IsWindow(itemTable_)) {
        ThemedUi::SetTableImageLists(itemTable_, nullptr, nullptr);
    }
    if (itemSmallImages_) {
        ImageList_Destroy(itemSmallImages_);
        itemSmallImages_ = nullptr;
    }
}

void AppLaunchLockerWindow::StopIconLoadTask() {
    ++iconGeneration_;
    if (iconTask_) {
        iconTask_->RequestStop();
        if (iconTask_->IsFinished()) {
            iconTask_.reset();
        }
    }
}

bool AppLaunchLockerWindow::StartIconLoadTask() {
    if (!hwnd_ || !itemTable_) return false;

    std::vector<AppLaunchLockerIconLoadItem> iconItems;
    iconItems.reserve(visibleApplicationIndexes_.size() + visibleItemIndexes_.size() + visibleDisabledIndexes_.size());
    for (std::size_t row = 0; row < visibleApplicationIndexes_.size(); ++row) {
        const std::size_t applicationIndex = visibleApplicationIndexes_[row];
        if (applicationIndex >= applications_.size()) continue;
        const StartupApplication& application = applications_[applicationIndex];
        iconItems.push_back({visibleApplicationRowKeys_[row],
            IconRequestFromApplication(application, itemIconSize_)});
    }
    for (std::size_t row = 0; row < visibleItemIndexes_.size(); ++row) {
        const std::size_t itemIndex = visibleItemIndexes_[row];
        if (itemIndex >= items_.size()) continue;
        const StartupItem& item = items_[itemIndex];
        iconItems.push_back({visibleItemRowKeys_[row],
            IconRequestFromFields(item.source, item.command, item.original, itemIconSize_)});
    }
    for (std::size_t row = 0; row < visibleDisabledIndexes_.size(); ++row) {
        const std::size_t recordIndex = visibleDisabledIndexes_[row];
        if (recordIndex >= disabled_.size()) continue;
        const DisabledRecord& record = disabled_[recordIndex];
        iconItems.push_back({visibleDisabledRowKeys_[row],
            IconRequestFromFields(record.source, std::wstring{}, record.original, itemIconSize_)});
    }
    if (iconItems.empty()) return false;

    const std::uint64_t generation = iconGeneration_;
    const HWND target = hwnd_;
    TaskOptions options{};
    options.mode = TaskExecutionMode::BackgroundSingle;
    options.completionCallback = [target, generation] {
        if (IsWindow(target)) {
            PostMessageW(target, WM_APP_ICONS_COMPLETE, static_cast<WPARAM>(generation), 0);
        }
    };
    iconTask_ = TaskExecutionService::StartTyped<AppLaunchLockerIconLoadResult>(
        std::move(options),
        [generation, iconItems = std::move(iconItems)](TaskContext& context) {
            AppLaunchLockerIconLoadResult result;
            result.generation = generation;
            result.icons.reserve(iconItems.size());

            TaskProgressUpdate progress{};
            progress.phase = L"app-launch-locker-icons";
            progress.title = L"自启动图标刷新";
            progress.status = L"正在刷新图标";
            progress.total = iconItems.size();
            progress.current = 0;
            progress.workerCount = 1;
            progress.indeterminate = false;
            context.Report(std::move(progress));

            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            IconResolverService resolver({}, AppLaunchLockerDataDirectory() / L"icon-cache" / L"resolver-v1");
            std::uint64_t completed = 0;
            for (const AppLaunchLockerIconLoadItem& item : iconItems) {
                if (context.StopRequested()) break;
                AppLaunchLockerIconResult iconResult;
                iconResult.rowKey = item.rowKey;
                iconResult.icon = resolver.Resolve(item.request, context.StopToken());
                result.icons.push_back(std::move(iconResult));
                ++completed;
                context.UpdateProgress([completed, total = static_cast<std::uint64_t>(iconItems.size())](TaskProgressUpdate& value) {
                    value.current = completed;
                    value.completed = completed;
                    value.total = total;
                    value.status = L"正在刷新图标";
                    value.detail = L"已刷新 " + std::to_wstring(completed) + L" / " + std::to_wstring(total) + L" 个图标";
                    value.indeterminate = false;
                });
            }
            if (SUCCEEDED(comResult)) {
                CoUninitialize();
            }
            return result;
        });
    return iconTask_ != nullptr;
}

void AppLaunchLockerWindow::ApplyIconLoadResult(std::uint64_t generation) {
    if (!iconTask_ || generation != iconGeneration_ || !iconTask_->IsFinished()) return;
    if (iconTask_->Status() != TaskStatus::Completed) {
        iconTask_.reset();
        if (rowDisplayPending_) {
            ShowPendingRows(nullptr, {});
        }
        return;
    }

    AppLaunchLockerIconLoadResult result = iconTask_->ResultCopy<AppLaunchLockerIconLoadResult>();
    iconTask_.reset();
    if (result.generation != iconGeneration_ || !itemTable_) return;

    HIMAGELIST newImages = ImageList_Create(itemIconSize_, itemIconSize_, ILC_COLOR32 | ILC_MASK,
        std::max(1, static_cast<int>(result.icons.size())), 8);
    std::map<std::intptr_t, int> imageIndexes;

    for (const AppLaunchLockerIconResult& icon : result.icons) {
        if (!newImages) break;
        HBITMAP bitmap = IconResolverService::CreateBitmapFromPixels(
            icon.icon,
            itemIconSize_,
            ThemedUi::ListSurfaceColor(theme_),
            true);
        if (!bitmap) continue;
        const int imageIndex = ImageList_Add(newImages, bitmap, nullptr);
        DeleteObject(bitmap);
        if (imageIndex < 0) continue;
        imageIndexes[icon.rowKey] = imageIndex;
    }
    ShowPendingRows(newImages, imageIndexes);
}

void AppLaunchLockerWindow::ShowPendingRows(HIMAGELIST newImages, const std::map<std::intptr_t, int>& imageIndexes) {
    if (!itemTable_) {
        if (newImages) ImageList_Destroy(newImages);
        return;
    }

    std::vector<ThemedTableRow> rows = pendingRows_;
    for (ThemedTableRow& row : rows) {
        if (row.cells.empty()) continue;
        const auto found = imageIndexes.find(row.key);
        row.cells.front().image = found == imageIndexes.end() ? -1 : found->second;
    }

    const ThemedUi ui = windowUi_->ui();
    ThemedUi::SetTableColumns(itemTable_, MainTableColumns(ui, pendingRowsTab_));

    HIMAGELIST oldImages = itemSmallImages_;
    itemSmallImages_ = newImages;
    ThemedUi::SetTableImageLists(itemTable_, itemSmallImages_, nullptr);
    ThemedUi::SetTableRows(itemTable_, rows);
    if (oldImages && oldImages != itemSmallImages_) {
        ImageList_Destroy(oldImages);
    }

    if (pendingSelectedKey_ != 0) {
        const int restoredIndex = ThemedUi::FindTableRowByKey(itemTable_, pendingSelectedKey_);
        if (restoredIndex >= 0) ThemedUi::SetTableSelectedIndex(itemTable_, restoredIndex);
    }
    ThemedUi::RestoreTableTopVisibleRowByKey(itemTable_, pendingTopKey_);
    pendingRows_.clear();
    pendingSelectedKey_ = 0;
    pendingTopKey_ = 0;
    rowDisplayPending_ = false;
    UpdateButtons();
}

std::intptr_t AppLaunchLockerWindow::RowKeyForIdentity(const std::wstring& identity) {
    const auto found = stableRowKeys_.find(identity);
    if (found != stableRowKeys_.end()) return found->second;
    const std::intptr_t key = nextStableRowKey_++;
    stableRowKeys_.emplace(identity, key);
    return key;
}

void AppLaunchLockerWindow::SelectTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size()) || index == activeTab_) return;
    activeTab_ = index;
    ThemedUi::SetActiveTab(tabControl_, activeTab_, false);
    RebuildRows();
}

const StartupApplication* AppLaunchLockerWindow::SelectedApplication() const {
    if (rowDisplayPending_) return nullptr;
    if (activeTab_ < 0 || activeTab_ >= static_cast<int>(tabs_.size()) ||
        tabs_[static_cast<std::size_t>(activeTab_)].tab != MainTab::StartupItems) {
        return nullptr;
    }
    const int row = ThemedUi::TableSelectedIndex(itemTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= visibleApplicationIndexes_.size()) return nullptr;
    const std::size_t index = visibleApplicationIndexes_[static_cast<std::size_t>(row)];
    return index < applications_.size() ? &applications_[index] : nullptr;
}

const StartupItem* AppLaunchLockerWindow::SelectedStartupItem() const {
    if (rowDisplayPending_) return nullptr;
    if (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size()) &&
        tabs_[static_cast<std::size_t>(activeTab_)].tab == MainTab::StartupItems) {
        return nullptr;
    }
    const int row = ThemedUi::TableSelectedIndex(itemTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= visibleItemIndexes_.size()) return nullptr;
    const std::size_t index = visibleItemIndexes_[static_cast<std::size_t>(row)];
    return index < items_.size() ? &items_[index] : nullptr;
}

const DisabledRecord* AppLaunchLockerWindow::SelectedDisabledRecord() const {
    if (rowDisplayPending_) return nullptr;
    if (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size()) &&
        tabs_[static_cast<std::size_t>(activeTab_)].tab == MainTab::StartupItems) {
        return nullptr;
    }
    const int row = ThemedUi::TableSelectedIndex(itemTable_);
    const std::size_t disabledOffset = visibleItemIndexes_.size();
    if (row < 0 || static_cast<std::size_t>(row) < disabledOffset) return nullptr;
    const std::size_t disabledRow = static_cast<std::size_t>(row) - disabledOffset;
    if (disabledRow >= visibleDisabledIndexes_.size()) return nullptr;
    const std::size_t index = visibleDisabledIndexes_[disabledRow];
    return index < disabled_.size() ? &disabled_[index] : nullptr;
}

void AppLaunchLockerWindow::UpdateButtons() {
    if (!windowUi_) return;
    const ThemedUi ui = windowUi_->ui();
    const StartupApplication* application = SelectedApplication();
    const StartupItem* item = SelectedStartupItem();
    const DisabledRecord* record = SelectedDisabledRecord();
    const bool appCanDisable = application && !ApplicationDisableIds(*application).empty();
    const bool appCanRestore = application && !ApplicationRestoreIds(*application).empty();
    ui.SetEnabled(detailsButton_, !busy_ && (application != nullptr || item != nullptr || record != nullptr));
    ThemedUi::SetText(disableButton_, appCanRestore && !appCanDisable ? L"恢复" : L"禁用");
    ui.SetEnabled(disableButton_, !busy_ && storeAvailable_ && ((item && item->canDisable) || appCanDisable || appCanRestore));
    ui.SetEnabled(restoreButton_, !busy_ && storeAvailable_ && record != nullptr);
    ThemedUi::SetVisible(disableButton_, record == nullptr);
    ThemedUi::SetVisible(restoreButton_, record != nullptr);
}

void AppLaunchLockerWindow::ShowSelectedDetails() {
    if (busy_) return;
    const auto runDetails = [&](StartupApplication application, const wchar_t* title) {
        ApplicationDetailsDialog(hwnd_, instance_, theme_, std::move(application),
            [this](const StartupApplicationEntry& entry) { StartEntryOperation(entry); },
            title,
            false).Run();
    };
    if (const StartupApplication* application = SelectedApplication()) {
        ApplicationDetailsDialog(hwnd_, instance_, theme_, *application,
            [this](const StartupApplicationEntry& entry) { StartEntryOperation(entry); }).Run();
    } else if (const StartupItem* item = SelectedStartupItem()) {
        runDetails(SingleEntryApplication(EntryFromStartupItem(*item)), L"入口详情");
    } else if (const DisabledRecord* record = SelectedDisabledRecord()) {
        runDetails(SingleEntryApplication(EntryFromDisabledRecord(*record)), L"入口详情");
    }
}
