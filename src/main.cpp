#include "Config.h"
#include "AppLog.h"
#include "EmbeddedAssetInstaller.h"
#include "Elevation.h"
#include "MainWindow.h"
#include "SimpleDialogs.h"
#include "Storage.h"
#include "Theme.h"
#include "Utilities.h"
#include "WebDavRecoveryService.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr int kCurrentConfigVersion = 1;

struct Runtime {
    HANDLE mutex = nullptr;
    HANDLE sharedMemory = nullptr;
    void* sharedView = nullptr;
    bool ownsMutex = false;

    ~Runtime() {
        if (sharedView) {
            UnmapViewOfFile(sharedView);
        }
        if (sharedMemory) {
            CloseHandle(sharedMemory);
        }
        if (mutex && ownsMutex) {
            ReleaseMutex(mutex);
        }
        if (mutex) {
            CloseHandle(mutex);
        }
    }
};

struct SiblingQuattroProcess {
    DWORD processId = 0;
    HWND mainWindow = nullptr;
    HANDLE process = nullptr;
};

std::wstring BuildName(const wchar_t* prefix, const std::filesystem::path& appDirectory) {
    return std::wstring(prefix) + Hex8(StablePathHash(appDirectory.wstring()));
}

std::wstring CurrentPidText() {
    return std::to_wstring(GetCurrentProcessId());
}

std::wstring CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring NormalizeProcessPath(std::wstring path) {
    if (path.empty()) {
        return {};
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0) {
        return ToLower(path);
    }
    if (length >= buffer.size()) {
        buffer.assign(length + 1, L'\0');
        length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length == 0 || length >= buffer.size()) {
            return ToLower(path);
        }
    }
    buffer.resize(length);
    return ToLower(buffer);
}

std::wstring ProcessImagePath(HANDLE process) {
    std::wstring buffer(32768, L'\0');
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length) || length == 0) {
        return {};
    }
    buffer.resize(length);
    return buffer;
}

struct MainWindowEnumerationContext {
    std::vector<std::pair<DWORD, HWND>> windows;
};

BOOL CALLBACK EnumQuattroMainWindows(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<MainWindowEnumerationContext*>(param);
    if (!context) {
        return TRUE;
    }

    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, 128) == 0 || wcscmp(className, L"QuattroMainWindow") != 0) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != 0) {
        context->windows.emplace_back(processId, hwnd);
    }
    return TRUE;
}

HWND FindMainWindowForProcess(DWORD processId, const std::vector<std::pair<DWORD, HWND>>& windows) {
    for (const auto& [windowProcessId, hwnd] : windows) {
        if (windowProcessId == processId && IsWindow(hwnd)) {
            return hwnd;
        }
    }
    return nullptr;
}

void CloseSiblingHandles(std::vector<SiblingQuattroProcess>& siblings) {
    for (auto& sibling : siblings) {
        if (sibling.process) {
            CloseHandle(sibling.process);
            sibling.process = nullptr;
        }
    }
}

std::filesystem::path AbsolutePathForLog(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute;
}

std::filesystem::path ResolvedThemeFileForLog(const std::filesystem::path& themeDirectory, const std::wstring& themeName) {
    if (!themeName.empty()) {
        const std::filesystem::path requested = themeDirectory / (themeName + L".xml");
        if (FileExists(requested)) {
            return requested;
        }
    }
    return themeDirectory / L"default.xml";
}

std::wstring MigrationTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(
        buffer,
        L"%04u%02u%02u-%02u%02u%02u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
    return buffer;
}

bool BackupConfigForMigration(const std::filesystem::path& appDirectory, const std::filesystem::path& configPath) {
    if (!FileExists(configPath)) {
        return true;
    }
    const std::filesystem::path backupPath = appDirectory / L"backups" / L"config" / MigrationTimestamp() / L"conf.ini";
    std::error_code ec;
    std::filesystem::create_directories(backupPath.parent_path(), ec);
    if (ec) {
        WriteAppLog(L"配置迁移备份目录创建失败: " + configPath.wstring());
        return false;
    }
    std::filesystem::copy_file(configPath, backupPath, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        WriteAppLog(L"配置迁移备份失败: " + configPath.wstring());
        return false;
    }
    WriteAppLog(L"配置迁移前已备份: " + backupPath.wstring());
    return true;
}

std::wstring BoolText(bool value) {
    return value ? L"1" : L"0";
}

struct ActivateExistingInstanceResult {
    bool activated = false;
    std::wstring detail;
};

std::wstring HwndText(HWND hwnd) {
    return std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd));
}

ActivateExistingInstanceResult TryActivateExistingInstance(const std::wstring& sharedMemoryName) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemoryName.c_str());
    if (!mapping) {
        return {false, L"OpenFileMapping failed: " + FormatLastError(GetLastError())};
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(HWND));
    HWND hwnd = nullptr;
    if (view) {
        hwnd = *static_cast<HWND*>(view);
        UnmapViewOfFile(view);
    } else {
        const DWORD error = GetLastError();
        CloseHandle(mapping);
        return {false, L"MapViewOfFile failed: " + FormatLastError(error)};
    }
    CloseHandle(mapping);

    if (!hwnd) {
        return {false, L"shared HWND is empty"};
    }
    if (!IsWindow(hwnd)) {
        return {false, L"shared HWND is stale: " + HwndText(hwnd)};
    }
    if (!PostMessageW(hwnd, WM_QUATTRO_WAKEUP, 0, 0)) {
        return {false, L"PostMessage failed for HWND " + HwndText(hwnd) + L": " + FormatLastError(GetLastError())};
    }
    return {true, L"posted wakeup to HWND " + HwndText(hwnd)};
}

ActivateExistingInstanceResult WaitForExistingInstance(const std::wstring& sharedMemoryName, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    ActivateExistingInstanceResult last;
    for (;;) {
        last = TryActivateExistingInstance(sharedMemoryName);
        if (last.activated) {
            const DWORD elapsed = GetTickCount() - start;
            last.detail += L" after " + std::to_wstring(elapsed) + L"ms";
            return last;
        }

        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) {
            last.detail += L" after waiting " + std::to_wstring(elapsed) + L"ms";
            return last;
        }
        Sleep(100);
    }
}

std::vector<SiblingQuattroProcess> CollectSiblingQuattroProcesses() {
    std::vector<SiblingQuattroProcess> siblings;
    const std::wstring currentPath = NormalizeProcessPath(CurrentExecutablePath());
    if (currentPath.empty()) {
        WriteAppLog(L"单实例恢复：无法获取当前可执行文件路径。");
        return siblings;
    }

    MainWindowEnumerationContext windowContext;
    EnumWindows(EnumQuattroMainWindows, reinterpret_cast<LPARAM>(&windowContext));

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        WriteAppLog(L"单实例恢复：进程快照创建失败: " + FormatLastError(GetLastError()));
        return siblings;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD currentProcessId = GetCurrentProcessId();
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == currentProcessId) {
                continue;
            }

            HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                FALSE,
                entry.th32ProcessID);
            if (!process) {
                continue;
            }

            const std::wstring imagePath = NormalizeProcessPath(ProcessImagePath(process));
            if (imagePath == currentPath) {
                SiblingQuattroProcess sibling;
                sibling.processId = entry.th32ProcessID;
                sibling.mainWindow = FindMainWindowForProcess(entry.th32ProcessID, windowContext.windows);
                sibling.process = process;
                siblings.push_back(sibling);
            } else {
                CloseHandle(process);
            }
        } while (Process32NextW(snapshot, &entry));
    } else {
        WriteAppLog(L"单实例恢复：进程快照读取失败: " + FormatLastError(GetLastError()));
    }
    CloseHandle(snapshot);
    return siblings;
}

bool WaitForSiblingExit(const std::vector<SiblingQuattroProcess>& siblings, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        bool allExited = true;
        for (const auto& sibling : siblings) {
            if (sibling.process && WaitForSingleObject(sibling.process, 0) == WAIT_TIMEOUT) {
                allExited = false;
                break;
            }
        }
        if (allExited) {
            return true;
        }

        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) {
            return false;
        }
        Sleep(50);
    }
}

enum class StaleInstanceRecoveryResult {
    NotRecovered,
    ActivatedWindow,
    TookOwnership,
};

StaleInstanceRecoveryResult RecoverStaleExistingInstance(Runtime& runtime, const ActivateExistingInstanceResult& activation) {
    WriteAppLog(L"单实例恢复：已有实例不可唤醒，尝试处理失效实例。最后状态=" + activation.detail);
    std::vector<SiblingQuattroProcess> siblings = CollectSiblingQuattroProcesses();
    if (siblings.empty()) {
        WriteAppLog(L"单实例恢复：未发现同路径后台实例。");
        return StaleInstanceRecoveryResult::NotRecovered;
    }

    for (const auto& sibling : siblings) {
        if (sibling.mainWindow && IsWindow(sibling.mainWindow)) {
            if (PostMessageW(sibling.mainWindow, WM_QUATTRO_WAKEUP, 0, 0)) {
                WriteAppLog(
                    L"单实例恢复：已通过枚举窗口唤醒已有实例。pid=" + std::to_wstring(sibling.processId) +
                    L"，hwnd=" + HwndText(sibling.mainWindow));
                CloseSiblingHandles(siblings);
                return StaleInstanceRecoveryResult::ActivatedWindow;
            }
            WriteAppLog(
                L"单实例恢复：枚举窗口唤醒失败。pid=" + std::to_wstring(sibling.processId) +
                L"，hwnd=" + HwndText(sibling.mainWindow) +
                L"，错误=" + FormatLastError(GetLastError()));
        }
    }

    bool requestedTermination = false;
    for (const auto& sibling : siblings) {
        if (sibling.mainWindow && IsWindow(sibling.mainWindow)) {
            continue;
        }
        if (sibling.process && TerminateProcess(sibling.process, 0)) {
            requestedTermination = true;
            WriteAppLog(L"单实例恢复：已结束无主窗口残留实例。pid=" + std::to_wstring(sibling.processId));
        } else {
            WriteAppLog(
                L"单实例恢复：结束无主窗口残留实例失败。pid=" + std::to_wstring(sibling.processId) +
                L"，错误=" + FormatLastError(GetLastError()));
        }
    }

    if (!requestedTermination) {
        CloseSiblingHandles(siblings);
        return StaleInstanceRecoveryResult::NotRecovered;
    }

    WaitForSiblingExit(siblings, 3000);
    CloseSiblingHandles(siblings);

    const DWORD wait = WaitForSingleObject(runtime.mutex, 5000);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        runtime.ownsMutex = true;
        WriteAppLog(L"单实例恢复：已接管 mutex。wait=" + std::to_wstring(wait));
        return StaleInstanceRecoveryResult::TookOwnership;
    }

    WriteAppLog(L"单实例恢复：等待 mutex 接管失败。wait=" + std::to_wstring(wait));
    return StaleInstanceRecoveryResult::NotRecovered;
}

bool PublishMainWindow(Runtime& runtime, HWND hwnd) {
    if (!runtime.sharedMemory) {
        return false;
    }

    runtime.sharedView = MapViewOfFile(runtime.sharedMemory, FILE_MAP_WRITE, 0, 0, sizeof(HWND));
    if (!runtime.sharedView) {
        return false;
    }
    *static_cast<HWND*>(runtime.sharedView) = hwnd;
    return true;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), output.data(), length, nullptr, nullptr);
    return output;
}

void WriteStartupReport(
    const std::filesystem::path& appDirectory,
    const StorageService& storageService,
    const AppConfig& config,
    const AppModel& model) {
    std::error_code ec;
    const std::filesystem::path logDirectory = appDirectory / L"logs";
    std::filesystem::create_directories(logDirectory, ec);
    std::ofstream file(logDirectory / L"startup-report.txt", std::ios::binary | std::ios::trunc);
    if (!file) {
        return;
    }

    int majorGroups = 0;
    int tags = 0;
    int effectiveGroup = config.currentGroupId;
    int effectiveTag = config.currentTagId;
    bool effectiveGroupValid = false;
    for (const auto& group : model.groups) {
        if (group.parentGroup == 0) {
            ++majorGroups;
            if (group.id == effectiveGroup) {
                effectiveGroupValid = true;
            }
            if (!effectiveGroupValid && effectiveGroup == 0) {
                effectiveGroup = group.id;
                effectiveGroupValid = true;
            }
        } else {
            ++tags;
        }
    }
    bool effectiveTagValid = false;
    for (const auto& group : model.groups) {
        if (group.parentGroup == effectiveGroup) {
            if (group.id == effectiveTag) {
                effectiveTagValid = true;
                break;
            }
            if (!effectiveTagValid && effectiveTag == 0) {
                effectiveTag = group.id;
                effectiveTagValid = true;
            }
        }
    }

    file << "Quattro startup report\n";
    file << "storage=" << (storageService.sqliteAvailable() ? "sqlite" : "fallback") << "\n";
    if (!storageService.lastError().empty()) {
        file << "storage_message=" << WideToUtf8(storageService.lastError()) << "\n";
    }
    file << "major_groups=" << majorGroups << "\n";
    file << "tags=" << tags << "\n";
    file << "links=" << model.links.size() << "\n";
    file << "theme=" << WideToUtf8(config.theme) << "\n";
    file << "current_group=" << effectiveGroup << "\n";
    file << "current_tag=" << effectiveTag << "\n";
    file << "show_title=" << (config.showTitle ? 1 : 0) << "\n";
    file << "show_group=" << (config.showGroup ? 1 : 0) << "\n";
    file << "show_tag=" << (config.showTag ? 1 : 0) << "\n";
    file << "show_run_count=" << (config.showRunCount ? 1 : 0) << "\n";
    file << "show_toolbox_button=" << (config.showToolboxButton ? 1 : 0) << "\n";
    file << "show_skin_button=" << (config.showSkinButton ? 1 : 0) << "\n";
    file << "link_name_single_line=" << (config.linkNameSingleLine ? 1 : 0) << "\n";
    file << "show_tooltip=" << (config.showTooltip ? 1 : 0) << "\n";
    file << "group_right=" << (config.groupRight ? 1 : 0) << "\n";
    file << "tag_right=" << (config.tagRight ? 1 : 0) << "\n";
    file << "tag_align=" << WideToUtf8(config.tagAlign) << "\n";
    file << "group_width=" << config.groupWidth << "\n";
    file << "tag_width=" << config.tagWidth << "\n";
    file << "auto_run=" << (config.autoRun ? 1 : 0) << "\n";
    file << "auto_dock=" << (config.autoDock ? 1 : 0) << "\n";
    file << "dock_delay=" << config.dockDelay << "\n";
    file << "hide_on_start=" << (config.hideOnStart ? 1 : 0) << "\n";
    file << "top_most=" << (config.topMost ? 1 : 0) << "\n";
    file << "hide_after_link=" << (config.hideAfterLink ? 1 : 0) << "\n";
    file << "hide_when_inactive=" << (config.hideWhenInactive ? 1 : 0) << "\n";
    file << "double_click_to_run=" << (config.doubleClickToRun ? 1 : 0) << "\n";
    file << "delete_confirm=" << (config.deleteConfirm ? 1 : 0) << "\n";
    file << "save_run_count=" << (config.saveRunCount ? 1 : 0) << "\n";
    file << "hide_notify_icon=" << (config.hideNotifyIcon ? 1 : 0) << "\n";
    file << "is_admin=" << (IsRunningAsAdmin() ? 1 : 0) << "\n";
    file << "prefer_admin_run=" << (config.preferAdminRun ? 1 : 0) << "\n";
    file << "mouse_enter_active_group=" << (config.mouseEnterActiveGroup ? 1 : 0) << "\n";
    file << "mouse_enter_active_tag=" << (config.mouseEnterActiveTag ? 1 : 0) << "\n";
    file << "active_group_delay=" << config.activeGroupDelay << "\n";
    file << "active_tag_delay=" << config.activeTagDelay << "\n";
    file << "main_hot_key=" << config.mainHotKey << "\n";
    file << "open_dir_command=" << WideToUtf8(config.openDirCommand) << "\n";
    file << "help_url=" << WideToUtf8(config.helpUrl) << "\n";
    file << "update_url=" << WideToUtf8(config.updateUrl) << "\n";
    file << "faq_url=" << WideToUtf8(config.faqUrl) << "\n";
    file << "reward_url=" << WideToUtf8(config.rewardUrl) << "\n";
    file << "external_file_index=deferred\n";
}

bool HasCommandLineArgument(const wchar_t* expected) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == expected) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

bool HasPrivilegeRestartArgument() {
    return HasCommandLineArgument(L"--quattro-privilege-restart");
}

bool HasUpdateRestartArgument() {
    return HasCommandLineArgument(L"--quattro-update-restart");
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    ResetStartupTiming();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::filesystem::path moduleDirectory = GetModuleDirectory();
    EmbeddedAssetInstallResult assetInstall = PrepareEmbeddedAssets(moduleDirectory);
    const std::filesystem::path appDirectory = assetInstall.appDirectory;
    SetCurrentDirectoryW(appDirectory.c_str());
    std::filesystem::create_directories(appDirectory / L"db");
    InitializeAppLog(appDirectory);
    WriteAppLog(L"应用启动。pid=" + CurrentPidText());
    WriteStartupTiming(
        L"app directory ready",
        L"module=" + moduleDirectory.wstring() +
            L", app=" + appDirectory.wstring() +
            L", embedded_written=" + std::to_wstring(assetInstall.filesWritten) +
            L", embedded_updated=" + std::to_wstring(assetInstall.filesUpdated) +
            L", embedded_backed_up=" + std::to_wstring(assetInstall.filesBackedUp) +
            L", embedded_skipped=" + std::to_wstring(assetInstall.filesSkipped) +
            L", embedded_failures=" + std::to_wstring(assetInstall.failures) +
            L", fallback_dir=" + BoolText(assetInstall.usedFallbackDirectory));
    if (assetInstall.filesWritten > 0) {
        WriteAppLog(L"已释放内置资源: " + std::to_wstring(assetInstall.filesWritten));
    }
    if (assetInstall.filesUpdated > 0) {
        WriteAppLog(
            L"已同步内置托管资源: " + std::to_wstring(assetInstall.filesUpdated) +
            (assetInstall.backupDirectory.empty() ? L"" : (L"，备份目录: " + assetInstall.backupDirectory.wstring())));
    }
    if (!assetInstall.message.empty()) {
        WriteAppLog(assetInstall.message);
    }

    const bool privilegeRestart = HasPrivilegeRestartArgument();
    const bool updateRestart = HasUpdateRestartArgument();
    const bool takeoverRestart = privilegeRestart || updateRestart;
    if (updateRestart) {
        WriteAppLog(L"检测到更新后重启参数。pid=" + CurrentPidText());
    }
    ConfigService configService(appDirectory / L"conf.ini");
    AppConfig config = configService.Load();
    if (config.version < kCurrentConfigVersion) {
        const int previousVersion = config.version;
        if (BackupConfigForMigration(appDirectory, configService.path())) {
            config.version = kCurrentConfigVersion;
            configService.Save(config);
            WriteAppLog(
                L"配置已迁移: " + std::to_wstring(previousVersion) +
                L" -> " + std::to_wstring(kCurrentConfigVersion));
        } else {
            WriteAppLog(L"配置迁移已跳过：备份失败。");
        }
    }
    WriteStartupTiming(
        L"config loaded",
        L"theme=" + config.theme +
            L", prefer_admin=" + BoolText(config.preferAdminRun) +
            L", hide_on_start=" + BoolText(config.hideOnStart) +
            L", http_auto=" + BoolText(config.httpServerAutoStart));
    if (config.preferAdminRun && !IsRunningAsAdmin() && !privilegeRestart) {
        std::wstring error;
        if (!RestartCurrentProcessElevated(nullptr, error)) {
            WriteAppLog(L"按上次权限以管理员身份启动失败: " + error);
        } else {
            WriteAppLog(L"按上次权限请求管理员启动。");
            return 0;
        }
    }

    Runtime runtime;
    const std::wstring mutexName = BuildName(L"QuattroMutex_", appDirectory);
    const std::wstring sharedMemoryName = BuildName(L"QuattroShareMemory_", appDirectory);
    runtime.mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    const DWORD mutexError = GetLastError();
    WriteStartupTiming(
        L"single instance mutex checked",
        L"error=" + std::to_wstring(mutexError) +
            L", privilege_restart=" + BoolText(privilegeRestart) +
            L", update_restart=" + BoolText(updateRestart));
    if (!runtime.mutex) {
        WriteAppLog(L"单实例 mutex 创建失败，取消启动以避免多实例。pid=" + CurrentPidText() + L"，错误=" + FormatLastError(mutexError));
        const ActivateExistingInstanceResult activation = WaitForExistingInstance(sharedMemoryName, 5000);
        if (activation.activated) {
            WriteAppLog(L"已有实例已唤醒。pid=" + CurrentPidText() + L"，" + activation.detail);
            return 0;
        }
        WriteAppLog(L"无法确认已有实例状态。pid=" + CurrentPidText() + L"，最后状态=" + activation.detail);
        return 1;
    }
    runtime.ownsMutex = mutexError != ERROR_ALREADY_EXISTS;
    if (mutexError == ERROR_ALREADY_EXISTS) {
        if (takeoverRestart) {
            DWORD wait = WaitForSingleObject(runtime.mutex, 5000);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                runtime.ownsMutex = true;
                WriteAppLog(
                    std::wstring(updateRestart ? L"更新后重启" : L"权限切换重启") +
                    L"已接管单实例。pid=" + CurrentPidText());
            } else {
                WriteAppLog(
                    std::wstring(updateRestart ? L"更新后重启" : L"权限切换重启") +
                    L"等待旧实例退出超时。pid=" + CurrentPidText() + L"，wait=" + std::to_wstring(wait));
                const ActivateExistingInstanceResult activation = WaitForExistingInstance(sharedMemoryName, 5000);
                if (activation.activated) {
                    WriteAppLog(L"已有实例已唤醒。pid=" + CurrentPidText() + L"，" + activation.detail);
                    return 0;
                }
                const StaleInstanceRecoveryResult recovery = RecoverStaleExistingInstance(runtime, activation);
                if (recovery == StaleInstanceRecoveryResult::ActivatedWindow) {
                    WriteAppLog(L"已有实例已唤醒。pid=" + CurrentPidText() + L"，recovered by enumerated window");
                    return 0;
                }
                if (recovery != StaleInstanceRecoveryResult::TookOwnership) {
                    WriteAppLog(L"已有实例不可唤醒，取消启动以避免多实例。pid=" + CurrentPidText() + L"，最后状态=" + activation.detail);
                    return 1;
                }
            }
        } else {
            const ActivateExistingInstanceResult activation = WaitForExistingInstance(sharedMemoryName, 10000);
            if (activation.activated) {
                WriteAppLog(L"已有实例已唤醒。pid=" + CurrentPidText() + L"，" + activation.detail);
                return 0;
            }
            const StaleInstanceRecoveryResult recovery = RecoverStaleExistingInstance(runtime, activation);
            if (recovery == StaleInstanceRecoveryResult::ActivatedWindow) {
                WriteAppLog(L"已有实例已唤醒。pid=" + CurrentPidText() + L"，recovered by enumerated window");
                return 0;
            }
            if (recovery != StaleInstanceRecoveryResult::TookOwnership) {
                WriteAppLog(L"已有实例不可唤醒，取消启动以避免多实例。pid=" + CurrentPidText() + L"，最后状态=" + activation.detail);
                return 0;
            }
        }
    }

    runtime.sharedMemory = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HWND), sharedMemoryName.c_str());
    const DWORD sharedMemoryError = GetLastError();
    if (!runtime.sharedMemory) {
        WriteAppLog(L"单实例 shared memory 创建失败。pid=" + CurrentPidText() + L"，错误=" + FormatLastError(sharedMemoryError));
    } else if (sharedMemoryError == ERROR_ALREADY_EXISTS) {
        WriteAppLog(L"单实例 shared memory 已存在，将发布当前主窗口。pid=" + CurrentPidText());
    }
    WriteStartupTiming(
        L"single instance shared memory ready",
        L"created=" + BoolText(runtime.sharedMemory != nullptr) + L", error=" + std::to_wstring(sharedMemoryError));

    HRESULT ole = OleInitialize(nullptr);
    WriteStartupTiming(L"ole initialized", L"hr=" + std::to_wstring(static_cast<long>(ole)));
    if (!WebDavRecoveryService::HasWebDavSettings(config)) {
        WebDavRecoveryService recoveryService;
        AppConfig recoveredConfig = config;
        if (recoveryService.Load(recoveredConfig)) {
            config = recoveredConfig;
            configService.Save(config);
            WriteAppLog(L"已从本机恢复 WebDAV 设置: " + recoveryService.path().wstring());
        }
    }
    WriteStartupTiming(L"webdav recovery checked", L"enabled=" + BoolText(config.webDavEnabled));
    StorageService storageService(appDirectory);
    AppModel model = storageService.Load();
    WriteStartupTiming(
        L"storage loaded",
        L"backend=" + std::wstring(storageService.sqliteAvailable() ? L"sqlite" : L"fallback") +
            L", groups=" + std::to_wstring(model.groups.size()) +
            L", links=" + std::to_wstring(model.links.size()) +
            L", notes=" + std::to_wstring(model.notes.size()) +
            L", todos=" + std::to_wstring(model.todos.size()));
    const std::filesystem::path themeDirectory = appDirectory / L"theme";
    const std::filesystem::path themeFile = ResolvedThemeFileForLog(themeDirectory, config.theme);
    Theme theme = Theme::Load(themeDirectory, config.theme);
    WriteStartupTiming(
        L"theme loaded",
        L"name=" + config.theme +
            L", directory=" + AbsolutePathForLog(themeDirectory).wstring() +
            L", file=" + AbsolutePathForLog(themeFile).wstring() +
            L", exists=" + BoolText(FileExists(themeFile)));
    WriteAppLog(storageService.sqliteAvailable() ? L"数据存储可用: db/link.db" : (L"数据存储降级: " + storageService.lastError()));
    WriteStartupReport(appDirectory, storageService, config, model);
    WriteStartupTiming(L"startup report written");

    MainWindow window(instance, appDirectory, moduleDirectory, configService, storageService, config, model, theme);
    WriteStartupTiming(L"main window constructed");
    if (!window.Create()) {
        WriteAppLog(L"主窗口初始化失败。");
        MessageBoxW(nullptr, L"主窗口初始化失败。", L"Quattro快速启动器", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(ole)) {
            OleUninitialize();
        }
        return 1;
    }
    WriteStartupTiming(L"main window create completed");

    if (PublishMainWindow(runtime, window.hwnd())) {
        WriteAppLog(L"主窗口已创建并发布单实例句柄。pid=" + CurrentPidText());
        WriteStartupTiming(L"main window handle published", L"ok=1");
    } else {
        WriteAppLog(L"主窗口已创建，但单实例句柄发布失败。pid=" + CurrentPidText());
        WriteStartupTiming(L"main window handle published", L"ok=0");
    }
    FinishStartupTiming();
    if (!storageService.sqliteAvailable() && !storageService.lastError().empty()) {
        ShowThemedMessageBox(window.hwnd(), instance, theme, storageService.lastError(), L"数据存储", MB_ICONINFORMATION | MB_OK);
    }

    int result = window.RunMessageLoop();
    WriteAppLog(L"应用退出。pid=" + CurrentPidText());
    if (SUCCEEDED(ole)) {
        OleUninitialize();
    }
    return result;
}
