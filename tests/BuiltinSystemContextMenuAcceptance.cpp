#include "../src/domain/MenuCatalog.h"
#include "../src/services/Storage.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {
constexpr UINT kGetMenuHandleMessage = 0x01E1;
constexpr DWORD kPopupTimeoutMs = 2000;
constexpr DWORD kMaxPopupLatencyMs = 300;
constexpr double kMaxAveragePopupLatencyMs = 120.0;

struct FindWindowRequest {
    std::wstring windowClass;
    DWORD processId = 0;
};

struct EnumWindowData {
    FindWindowRequest request;
    HWND match = nullptr;
};

std::wstring ClassName(HWND hwnd) {
    wchar_t buffer[128]{};
    GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<EnumWindowData*>(lParam);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (data->request.processId != 0 && processId != data->request.processId) {
        return TRUE;
    }
    if (!data->request.windowClass.empty() && ClassName(hwnd) != data->request.windowClass) {
        return TRUE;
    }
    data->match = hwnd;
    return FALSE;
}

HWND FindTopWindow(const FindWindowRequest& request) {
    EnumWindowData data{request, nullptr};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return data.match;
}

HWND WaitForTopWindow(const FindWindowRequest& request, DWORD timeoutMs) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        if (HWND hwnd = FindTopWindow(request)) {
            return hwnd;
        }
        Sleep(10);
    }
    return nullptr;
}

bool WaitForNoPopupMenu(DWORD processId, DWORD timeoutMs) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        if (!FindTopWindow(FindWindowRequest{L"#32768", processId})) {
            return true;
        }
        Sleep(5);
    }
    return false;
}

bool ClosePopupMenu(HWND mainWindow, HWND popup, DWORD processId) {
    // Message-only dismissal: never inject real keyboard/mouse input, so the
    // test cannot disturb whichever window the user currently has focused.
    PostMessageW(mainWindow, WM_CANCELMODE, 0, 0);
    PostMessageW(popup, WM_CANCELMODE, 0, 0);
    PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
    PostMessageW(popup, WM_KEYUP, VK_ESCAPE, 0);
    return WaitForNoPopupMenu(processId, 1000);
}

std::filesystem::path ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

void WriteConfigValue(const std::filesystem::path& path, const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(L"main", key, value, path.c_str());
}

bool SeedAcceptanceData(const std::filesystem::path& appDirectory) {
    StorageService storage(appDirectory);
    AppModel model = storage.Load();
    if (!storage.sqliteAvailable()) {
        std::wcerr << L"sqlite unavailable: " << storage.lastError() << L"\n";
        return false;
    }

    Group* tag = nullptr;
    for (auto& group : model.groups) {
        if (group.parentGroup != 0) {
            tag = &group;
            break;
        }
    }
    if (!tag) {
        std::cerr << "default tag missing\n";
        return false;
    }
    tag->layout = 0;
    tag->iconSize = 32;
    if (!storage.UpdateGroup(*tag)) {
        std::wcerr << L"update tag failed: " << storage.lastError() << L"\n";
        return false;
    }

    const struct SeedLink {
        const wchar_t* name;
        const wchar_t* path;
        const wchar_t* key;
        int type;
    } links[] = {
        {L"我的电脑", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"this-pc", 3},
        {L"网络", L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", L"network", 3},
        {L"回收站", L"::{645FF040-5081-101B-9F08-00AA002F954E}", L"recycle-bin", 3},
        {L"自定义电脑入口", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"", 3},
    };

    for (const auto& seed : links) {
        Link link;
        link.name = seed.name;
        link.parentGroup = tag->id;
        link.path = seed.path;
        link.type = seed.type;
        link.pos = -1;
        link.showCmd = SW_SHOWNORMAL;
        link.systemFunctionKey = seed.key;
        if (!storage.InsertLink(link)) {
            std::wcerr << L"insert link failed: " << storage.lastError() << L"\n";
            return false;
        }
    }

    const std::filesystem::path configPath = appDirectory / L"conf.ini";
    WriteConfigValue(configPath, L"bShowTitle", L"0");
    WriteConfigValue(configPath, L"bShowGroup", L"0");
    WriteConfigValue(configPath, L"bShowTag", L"0");
    WriteConfigValue(configPath, L"bHideOnStart", L"0");
    WriteConfigValue(configPath, L"bTopMost", L"0");
    WriteConfigValue(configPath, L"bLoggingEnabled", L"0");
    WriteConfigValue(configPath, L"nWidth", L"620");
    WriteConfigValue(configPath, L"nHeight", L"420");
    WriteConfigValue(configPath, L"nPosX", L"120");
    WriteConfigValue(configPath, L"nPosY", L"120");
    WriteConfigValue(configPath, L"nCurGroup", std::to_wstring(tag->parentGroup).c_str());
    WriteConfigValue(configPath, L"nCurTag", std::to_wstring(tag->id).c_str());
    return true;
}

std::vector<UINT> MenuCommandIds(HMENU menu) {
    std::vector<UINT> result;
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_ID | MIIM_FTYPE;
        if (GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item) &&
            (item.fType & MFT_SEPARATOR) == 0 && item.wID != 0 && item.wID != static_cast<UINT>(-1)) {
            result.push_back(item.wID);
        }
    }
    return result;
}

bool ContainsCommand(const std::vector<UINT>& commands, UINT command) {
    return std::find(commands.begin(), commands.end(), command) != commands.end();
}

bool StartsWith(const std::vector<UINT>& commands, const std::vector<UINT>& prefix) {
    return commands.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), commands.begin());
}

struct PopupResult {
    bool opened = false;
    DWORD latencyMs = 0;
    std::vector<UINT> commands;
};

PopupResult OpenLinkMenu(HWND mainWindow, DWORD processId, int rowIndex) {
    if (HWND stalePopup = FindTopWindow(FindWindowRequest{L"#32768", processId})) {
        if (!ClosePopupMenu(mainWindow, stalePopup, processId)) {
            return {};
        }
    }
    const int x = 180;
    const int y = 40 + rowIndex * 46;
    // Menu position is taken from the WM_RBUTTONUP lparam by the app; do not
    // move the real cursor (tests must not touch the user's mouse).

    const ULONGLONG begin = GetTickCount64();
    PostMessageW(mainWindow, WM_RBUTTONUP, 0, MAKELPARAM(x, y));
    HWND popup = WaitForTopWindow(FindWindowRequest{L"#32768", processId}, kPopupTimeoutMs);
    if (!popup) {
        return {};
    }

    PopupResult result;
    result.opened = true;
    result.latencyMs = static_cast<DWORD>(GetTickCount64() - begin);
    HMENU menu = reinterpret_cast<HMENU>(SendMessageW(popup, kGetMenuHandleMessage, 0, 0));
    if (menu) {
        result.commands = MenuCommandIds(menu);
    }
    if (!ClosePopupMenu(mainWindow, popup, processId)) {
        result.opened = false;
    }
    return result;
}

bool ValidateMenu(const PopupResult& popup, const std::vector<UINT>& expectedPrefix) {
    if (!popup.opened || popup.commands.empty()) {
        return false;
    }
    if (!ContainsCommand(popup.commands, ID_MENU_WINDOWS_CONTEXT)) {
        return false;
    }
    for (UINT command = ID_MENU_BUILTIN_SYSTEM_ACTION_BASE;
         command < ID_MENU_BUILTIN_SYSTEM_ACTION_BASE + ID_MENU_BUILTIN_SYSTEM_ACTION_LIMIT;
         ++command) {
        const bool expected = std::find(expectedPrefix.begin(), expectedPrefix.end(), command) != expectedPrefix.end();
        if (ContainsCommand(popup.commands, command) != expected) {
            return false;
        }
    }
    return StartsWith(popup.commands, expectedPrefix);
}

void PrintCommands(const char* label, const PopupResult& popup) {
    std::cout << label << "_opened=" << (popup.opened ? 1 : 0) << "\n";
    std::cout << label << "_latency_ms=" << popup.latencyMs << "\n";
    std::cout << label << "_commands=";
    for (std::size_t index = 0; index < popup.commands.size(); ++index) {
        if (index > 0) {
            std::cout << ',';
        }
        std::cout << popup.commands[index];
    }
    std::cout << "\n";
}
}

int wmain() {
    const std::filesystem::path sourceExe = ModuleDirectory() / L"Quattro.exe";
    if (!std::filesystem::exists(sourceExe)) {
        std::cerr << "Quattro.exe not found beside acceptance executable\n";
        return 2;
    }

    const std::filesystem::path appDirectory =
        std::filesystem::temp_directory_path() /
        (L"quattro_builtin_context_acceptance_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(appDirectory, ec);
    std::filesystem::create_directories(appDirectory, ec);
    if (ec) {
        std::cerr << "temporary app directory creation failed\n";
        return 2;
    }

    const std::filesystem::path testExe = appDirectory / L"Quattro.exe";
    const std::filesystem::path dataDirectory = appDirectory / L"user-config";
    std::filesystem::copy_file(sourceExe, testExe, std::filesystem::copy_options::overwrite_existing, ec);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", dataDirectory.c_str());
    if (ec || !SeedAcceptanceData(dataDirectory)) {
        std::filesystem::remove_all(appDirectory, ec);
        return 2;
    }

    // Acceptance rule: tests must not steal focus or bring windows to front.
    // The app honors this flag and keeps its windows non-activated / bottom-most.
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    const HWND initialForeground = GetForegroundWindow();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + testExe.wstring() + L"\"";
    if (!CreateProcessW(testExe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, appDirectory.c_str(), &startup, &process)) {
        std::cerr << "CreateProcess failed\n";
        std::filesystem::remove_all(appDirectory, ec);
        return 2;
    }

    int exitCode = 0;
    HWND mainWindow = WaitForTopWindow(FindWindowRequest{L"QuattroMainWindow", process.dwProcessId}, 10000);
    if (!mainWindow) {
        std::cerr << "main window did not appear\n";
        exitCode = 1;
    } else {
        // Do not activate or raise the app window; menus are opened and
        // validated purely through posted messages.
        Sleep(750);

        const PopupResult warmup = OpenLinkMenu(mainWindow, process.dwProcessId, 3);
        if (!warmup.opened) {
            std::cerr << "context menu warmup failed\n";
            exitCode = 1;
        }

        const UINT base = ID_MENU_BUILTIN_SYSTEM_ACTION_BASE;
        const PopupResult thisPc = OpenLinkMenu(mainWindow, process.dwProcessId, 0);
        const PopupResult network = OpenLinkMenu(mainWindow, process.dwProcessId, 1);
        const PopupResult recycleBin = OpenLinkMenu(mainWindow, process.dwProcessId, 2);
        const PopupResult custom = OpenLinkMenu(mainWindow, process.dwProcessId, 3);
        PrintCommands("this_pc", thisPc);
        PrintCommands("network", network);
        PrintCommands("recycle_bin", recycleBin);
        PrintCommands("custom", custom);

        const std::vector<UINT> commonFileActions{
            ID_MENU_OPEN_LOCATION,
            ID_MENU_RUN_ADMIN,
            ID_MENU_COPY_PATH,
            ID_MENU_WINDOWS_CONTEXT,
            ID_MENU_CREATE_DESKTOP_SHORTCUT,
            ID_MENU_REFRESH_LINK_ICON,
        };
        if (!ValidateMenu(thisPc, {
                base,
                base + 1,
                base + 2,
                commonFileActions[0],
                commonFileActions[1],
                commonFileActions[2],
                commonFileActions[3],
                commonFileActions[4],
                commonFileActions[5],
            })) {
            std::cerr << "This PC context menu validation failed\n";
            exitCode = 1;
        }
        if (!ValidateMenu(network, {
                base + 1,
                base + 2,
                commonFileActions[0],
                commonFileActions[1],
                commonFileActions[2],
                commonFileActions[3],
                commonFileActions[4],
                commonFileActions[5],
            })) {
            std::cerr << "Network context menu validation failed\n";
            exitCode = 1;
        }
        if (!ValidateMenu(recycleBin, {
                base + 3,
                commonFileActions[0],
                commonFileActions[1],
                commonFileActions[2],
                commonFileActions[3],
                commonFileActions[4],
                commonFileActions[5],
            })) {
            std::cerr << "Recycle Bin context menu validation failed\n";
            exitCode = 1;
        }
        if (!ValidateMenu(custom, commonFileActions)) {
            std::cerr << "Custom link context menu isolation failed\n";
            exitCode = 1;
        }

        std::vector<DWORD> latencies;
        latencies.reserve(20);
        for (int iteration = 0; iteration < 20 && exitCode == 0; ++iteration) {
            const PopupResult popup = OpenLinkMenu(mainWindow, process.dwProcessId, 2);
            if (!popup.opened || popup.latencyMs > kMaxPopupLatencyMs) {
                std::cerr << "context menu latency threshold failed\n";
                exitCode = 1;
                break;
            }
            latencies.push_back(popup.latencyMs);
        }
        if (!latencies.empty()) {
            const DWORD maxLatency = *std::max_element(latencies.begin(), latencies.end());
            const double averageLatency = static_cast<double>(
                std::accumulate(latencies.begin(), latencies.end(), ULONGLONG{0})) / latencies.size();
            std::cout << "popup_latency_average_ms=" << averageLatency << "\n";
            std::cout << "popup_latency_max_ms=" << maxLatency << "\n";
            if (averageLatency > kMaxAveragePopupLatencyMs) {
                std::cerr << "average context menu latency threshold failed\n";
                exitCode = 1;
            }
        }

        PostMessageW(mainWindow, WM_COMMAND, MAKEWPARAM(ID_MENU_EXIT, 0), 0);
    }

    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 3);
        exitCode = 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);
    std::filesystem::remove_all(appDirectory, ec);

    // Acceptance rule: the test must not have changed foreground ownership.
    if (GetForegroundWindow() != initialForeground) {
        std::cerr << "foreground window changed during acceptance run\n";
        exitCode = 1;
    }

    if (exitCode == 0) {
        std::cout << "builtin_system_context_menu_acceptance=passed\n";
    }
    return exitCode;
}
