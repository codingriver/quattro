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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
struct Runtime {
    HANDLE mutex = nullptr;
    HANDLE sharedMemory = nullptr;
    void* sharedView = nullptr;

    ~Runtime() {
        if (sharedView) {
            UnmapViewOfFile(sharedView);
        }
        if (sharedMemory) {
            CloseHandle(sharedMemory);
        }
        if (mutex) {
            CloseHandle(mutex);
        }
    }
};

std::wstring BuildName(const wchar_t* prefix, const std::filesystem::path& appDirectory) {
    return std::wstring(prefix) + Hex8(StablePathHash(appDirectory.wstring()));
}

std::wstring CurrentPidText() {
    return std::to_wstring(GetCurrentProcessId());
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

bool HasPrivilegeRestartArgument() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--quattro-privilege-restart") {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
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
            L", embedded_failures=" + std::to_wstring(assetInstall.failures) +
            L", fallback_dir=" + BoolText(assetInstall.usedFallbackDirectory));
    if (assetInstall.filesWritten > 0) {
        WriteAppLog(L"已释放内置资源: " + std::to_wstring(assetInstall.filesWritten));
    }
    if (!assetInstall.message.empty()) {
        WriteAppLog(assetInstall.message);
    }

    const bool privilegeRestart = HasPrivilegeRestartArgument();
    ConfigService configService(appDirectory / L"conf.ini");
    AppConfig config = configService.Load();
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
        L"error=" + std::to_wstring(mutexError) + L", privilege_restart=" + BoolText(privilegeRestart));
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
    if (mutexError == ERROR_ALREADY_EXISTS) {
        if (privilegeRestart) {
            DWORD wait = WaitForSingleObject(runtime.mutex, 5000);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                WriteAppLog(L"权限切换重启已接管单实例。pid=" + CurrentPidText());
            } else {
                WriteAppLog(L"权限切换重启等待旧实例退出超时。pid=" + CurrentPidText() + L"，wait=" + std::to_wstring(wait));
                const ActivateExistingInstanceResult activation = WaitForExistingInstance(sharedMemoryName, 5000);
                if (activation.activated) {
                    WriteAppLog(L"已有实例已唤醒。pid=" + CurrentPidText() + L"，" + activation.detail);
                    return 0;
                }
                WriteAppLog(L"已有实例不可唤醒，取消启动以避免多实例。pid=" + CurrentPidText() + L"，最后状态=" + activation.detail);
                return 1;
            }
        } else {
            const ActivateExistingInstanceResult activation = WaitForExistingInstance(sharedMemoryName, 10000);
            if (activation.activated) {
                WriteAppLog(L"已有实例已唤醒。pid=" + CurrentPidText() + L"，" + activation.detail);
                return 0;
            }
            WriteAppLog(L"已有实例不可唤醒，取消启动以避免多实例。pid=" + CurrentPidText() + L"，最后状态=" + activation.detail);
            return 0;
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
    Theme theme = Theme::Load(appDirectory / L"theme", config.theme);
    WriteStartupTiming(L"theme loaded", L"name=" + config.theme);
    WriteAppLog(storageService.sqliteAvailable() ? L"数据存储可用: db/link.db" : (L"数据存储降级: " + storageService.lastError()));
    WriteStartupReport(appDirectory, storageService, config, model);
    WriteStartupTiming(L"startup report written");

    MainWindow window(instance, appDirectory, configService, storageService, config, model, theme);
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
