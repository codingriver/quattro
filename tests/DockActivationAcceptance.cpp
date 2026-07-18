#include "../src/domain/MenuCatalog.h"
#include "../src/windows/MainWindow.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
constexpr DWORD kStepTimeoutMs = 5000;
constexpr DWORD kTotalTimeoutMs = 30000;

std::wstring ClassName(HWND hwnd) {
    wchar_t buffer[128]{};
    GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

struct FindWindowData {
    DWORD processId = 0;
    std::wstring className;
    bool visibleOnly = true;
    HWND match = nullptr;
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<FindWindowData*>(lParam);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != data->processId ||
        (data->visibleOnly && !IsWindowVisible(hwnd)) ||
        ClassName(hwnd) != data->className) {
        return TRUE;
    }
    data->match = hwnd;
    return FALSE;
}

HWND FindProcessWindow(DWORD processId, const wchar_t* className, bool visibleOnly = true) {
    FindWindowData data{processId, className, visibleOnly, nullptr};
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&data));
    return data.match;
}

HWND WaitForProcessWindow(DWORD processId, const wchar_t* className, DWORD timeoutMs) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        if (HWND hwnd = FindProcessWindow(processId, className)) {
            return hwnd;
        }
        Sleep(25);
    }
    return nullptr;
}

bool WaitForFile(const std::filesystem::path& path, DWORD timeoutMs) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        Sleep(25);
    }
    return false;
}

bool IsDockHidden(HWND mainWindow) {
    return SendMessageW(mainWindow, WM_QUATTRO_TEST_DOCK_HIDDEN, 0, 0) == TRUE;
}

bool TryDockAtRect(HWND mainWindow, const RECT& work, int edge) {
    RECT window{};
    if (!GetWindowRect(mainWindow, &window)) {
        return false;
    }
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.top + ((work.bottom - work.top) - height) / 2;
    if (edge == 0) x = work.left;
    if (edge == 1) x = work.right - width;
    if (edge == 2) y = work.top;
    if (edge == 3) y = work.bottom - height;
    if (!SetWindowPos(
            mainWindow,
            HWND_BOTTOM,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        return false;
    }
    return SendMessageW(mainWindow, WM_QUATTRO_TEST_DOCK_HIDE, 0, 0) == TRUE &&
           IsDockHidden(mainWindow);
}

struct MonitorRects {
    RECT values[16]{};
    int count = 0;
};

BOOL CALLBACK CollectMonitorRects(HMONITOR monitor, HDC, LPRECT, LPARAM lParam) {
    auto* rects = reinterpret_cast<MonitorRects*>(lParam);
    if (rects->count >= static_cast<int>(std::size(rects->values))) {
        return FALSE;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        rects->values[rects->count++] = info.rcWork;
    }
    return TRUE;
}

bool DockAtAvailableEdge(HWND mainWindow) {
    MonitorRects rects;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorRects, reinterpret_cast<LPARAM>(&rects));
    for (int index = 0; index < rects.count; ++index) {
        for (int edge = 0; edge < 4; ++edge) {
            if (TryDockAtRect(mainWindow, rects.values[index], edge)) {
                return true;
            }
        }
    }
    return false;
}

std::filesystem::path ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool WriteTestRootMarker(const std::filesystem::path& root, const std::string& runId) {
    std::ofstream marker(root / L".quattro-test-root", std::ios::binary | std::ios::trunc);
    marker << "run_id=" << runId << "\n";
    return marker.good();
}

bool WriteTestConfig(const std::filesystem::path& root) {
    std::ofstream config(root / L"conf.ini", std::ios::binary | std::ios::trunc);
    config << "[main]\n"
           << "bAutoDock=1\n"
           << "nDockDelay=0\n"
           << "bTopMost=0\n"
           << "bGlobalHotKeysEnabled=0\n";
    return config.good();
}

bool ForegroundBelongsToProcess(HWND foreground, DWORD processId) {
    DWORD foregroundProcessId = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &foregroundProcessId);
    }
    return foregroundProcessId == processId;
}
}

int wmain() {
    const ULONGLONG totalBegin = GetTickCount64();
    const std::filesystem::path sourceExe = ModuleDirectory() / L"Quattro.exe";
    if (!std::filesystem::exists(sourceExe)) {
        std::cerr << "Quattro.exe not found beside acceptance executable\n";
        return 2;
    }

    const std::string runIdText = "dock-activation-" + std::to_string(GetCurrentProcessId());
    const std::wstring runId(runIdText.begin(), runIdText.end());
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (L"quattro-" + runId);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec || !WriteTestRootMarker(root, runIdText) || !WriteTestConfig(root)) {
        std::cerr << "unable to create isolated test root\n";
        return 2;
    }

    const std::filesystem::path testExe = root / L"Quattro.exe";
    if (!CopyFileW(sourceExe.c_str(), testExe.c_str(), FALSE)) {
        std::cerr << "unable to copy Quattro.exe into isolated test root\n";
        std::filesystem::remove_all(root, ec);
        return 2;
    }

    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    SetEnvironmentVariableW(L"QUATTRO_TEST_SUPPRESS_TRAY", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_RUN_ID", runId.c_str());
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", root.c_str());

    const HWND initialForeground = GetForegroundWindow();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + testExe.wstring() + L"\"";
    if (!CreateProcessW(
            testExe.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            root.c_str(),
            &startup,
            &process)) {
        std::cerr << "CreateProcess failed\n";
        std::filesystem::remove_all(root, ec);
        return 2;
    }

    int result = 0;
    HWND mainWindow = nullptr;
    if (!WaitForFile(root / L"logs" / L"test-ready.txt", kStepTimeoutMs)) {
        std::cerr << "test process did not become ready\n";
        result = 1;
    } else {
        mainWindow = WaitForProcessWindow(process.dwProcessId, L"QuattroMainWindow", kStepTimeoutMs);
        if (!mainWindow) {
            std::cerr << "main window did not appear\n";
            result = 1;
        }
    }

    if (result == 0) {
        SendMessageW(mainWindow, WM_HOTKEY, 1, 0);
        if (!IsWindowVisible(mainWindow) || IsIconic(mainWindow) || IsDockHidden(mainWindow)) {
            std::cerr << "main hotkey hid a visible background main window instead of waking it\n";
            result = 1;
        }
    }

    if (result == 0 && !DockAtAvailableEdge(mainWindow)) {
        std::cerr << "unable to enter dock-hidden state\n";
        result = 1;
    }
    if (result == 0) {
        HWND peek = WaitForProcessWindow(process.dwProcessId, L"QuattroDockPeekWindow", kStepTimeoutMs);
        if (!peek) {
            std::cerr << "dock peek window did not appear\n";
            result = 1;
        } else {
            const LONG_PTR peekExStyle = GetWindowLongPtrW(peek, GWL_EXSTYLE);
            if ((peekExStyle & WS_EX_NOACTIVATE) == 0) {
                std::cerr << "dock peek window can activate the desktop\n";
                result = 1;
            }
            if (result == 0) {
                SendMessageW(mainWindow, WM_COMMAND, MAKEWPARAM(ID_MENU_TOGGLE_TOPMOST, 0), 0);
                SendMessageW(mainWindow, WM_COMMAND, MAKEWPARAM(ID_MENU_TOGGLE_TOPMOST, 0), 0);
                if (!IsWindowVisible(peek) || !IsDockHidden(mainWindow)) {
                    std::cerr << "dock peek state changed after main topmost changes: visible="
                              << (IsWindowVisible(peek) ? 1 : 0)
                              << " dock_hidden=" << (IsDockHidden(mainWindow) ? 1 : 0) << "\n";
                    result = 1;
                }
            }
        }
        if (result == 0) {
            SendMessageW(peek, WM_SETCURSOR, reinterpret_cast<WPARAM>(peek), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            if (IsDockHidden(mainWindow)) {
                std::cerr << "edge hover did not restore the main window\n";
                result = 1;
            }
        }
    }

    if (result == 0 && !DockAtAvailableEdge(mainWindow)) {
        std::cerr << "unable to re-enter dock-hidden state for taskbar command\n";
        result = 1;
    }
    if (result == 0) {
        SendMessageW(mainWindow, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        if (IsDockHidden(mainWindow) || IsIconic(mainWindow)) {
            std::cerr << "taskbar minimize command did not reveal the docked window\n";
            result = 1;
        }
    }

    if (result == 0 && !DockAtAvailableEdge(mainWindow)) {
        std::cerr << "unable to re-enter dock-hidden state for activation\n";
        result = 1;
    }
    if (result == 0) {
        SendMessageW(mainWindow, WM_ACTIVATE, WA_ACTIVE, 0);
        if (IsDockHidden(mainWindow)) {
            std::cerr << "taskbar activation did not reveal the docked window\n";
            result = 1;
        }
    }

    if (mainWindow && IsWindow(mainWindow)) {
        PostMessageW(mainWindow, WM_COMMAND, MAKEWPARAM(ID_MENU_EXIT, 0), 0);
    }
    if (WaitForSingleObject(process.hProcess, kStepTimeoutMs) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 3);
        result = 1;
    }

    const HWND finalForeground = GetForegroundWindow();
    if (ForegroundBelongsToProcess(finalForeground, process.dwProcessId)) {
        std::cerr << "test process became the foreground window during background acceptance\n";
        result = 1;
    }
    if (GetTickCount64() - totalBegin > kTotalTimeoutMs) {
        std::cerr << "acceptance exceeded total timeout\n";
        result = 1;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::filesystem::remove_all(root, ec);

    if (result == 0) {
        std::cout << "dock_activation_acceptance=passed foreground_unchanged="
                  << (initialForeground == finalForeground ? 1 : 0) << "\n";
    }
    return result;
}
