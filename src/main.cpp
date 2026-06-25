#include "Config.h"
#include "AppLog.h"
#include "EmbeddedAssetInstaller.h"
#include "MainWindow.h"
#include "Storage.h"
#include "Theme.h"
#include "Utilities.h"

#include <windows.h>
#include <objbase.h>

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

bool ActivateExistingInstance(const std::wstring& sharedMemoryName) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemoryName.c_str());
    if (!mapping) {
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(HWND));
    HWND hwnd = nullptr;
    if (view) {
        hwnd = *static_cast<HWND*>(view);
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);

    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_QUATTRO_WAKEUP, 0, 0);
        return true;
    }
    return false;
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
    file << "auto_dock=" << (config.autoDock ? 1 : 0) << "\n";
    file << "top_most=" << (config.topMost ? 1 : 0) << "\n";
    file << "external_file_index=deferred\n";
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::filesystem::path moduleDirectory = GetModuleDirectory();
    EmbeddedAssetInstallResult assetInstall = PrepareEmbeddedAssets(moduleDirectory);
    const std::filesystem::path appDirectory = assetInstall.appDirectory;
    SetCurrentDirectoryW(appDirectory.c_str());
    std::filesystem::create_directories(appDirectory / L"db");
    InitializeAppLog(appDirectory);
    WriteAppLog(L"应用启动。");
    if (assetInstall.filesWritten > 0) {
        WriteAppLog(L"已释放内置资源: " + std::to_wstring(assetInstall.filesWritten));
    }
    if (!assetInstall.message.empty()) {
        WriteAppLog(assetInstall.message);
    }

    Runtime runtime;
    const std::wstring mutexName = BuildName(L"QuattroMutex_", appDirectory);
    const std::wstring sharedMemoryName = BuildName(L"QuattroShareMemory_", appDirectory);
    runtime.mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (runtime.mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance(sharedMemoryName);
        return 0;
    }

    runtime.sharedMemory = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HWND), sharedMemoryName.c_str());

    HRESULT ole = OleInitialize(nullptr);
    ConfigService configService(appDirectory / L"conf.ini");
    AppConfig config = configService.Load();
    StorageService storageService(appDirectory);
    AppModel model = storageService.Load();
    Theme theme = Theme::Load(appDirectory / L"theme", config.theme);
    WriteAppLog(storageService.sqliteAvailable() ? L"数据存储可用: db/link.db" : (L"数据存储降级: " + storageService.lastError()));
    WriteStartupReport(appDirectory, storageService, config, model);

    MainWindow window(instance, appDirectory, configService, storageService, config, model, theme);
    if (!window.Create()) {
        WriteAppLog(L"主窗口初始化失败。");
        MessageBoxW(nullptr, L"主窗口初始化失败。", L"Quattro", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(ole)) {
            OleUninitialize();
        }
        return 1;
    }

    PublishMainWindow(runtime, window.hwnd());
    WriteAppLog(L"主窗口已创建。");
    if (!storageService.sqliteAvailable() && !storageService.lastError().empty()) {
        MessageBoxW(window.hwnd(), storageService.lastError().c_str(), L"数据存储", MB_ICONINFORMATION | MB_OK);
    }

    int result = window.RunMessageLoop();
    WriteAppLog(L"应用退出。");
    if (SUCCEEDED(ole)) {
        OleUninitialize();
    }
    return result;
}
