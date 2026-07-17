#include "../src/domain/MenuCatalog.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr UINT WM_QUATTRO_TRAY = WM_APP + 0x66;
constexpr UINT kGetMenuHandleMessage = 0x01E1;

std::wstring WindowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::wstring ClassName(HWND hwnd) {
    wchar_t buffer[128]{};
    GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

struct FindWindowRequest {
    std::wstring windowClass;
    std::wstring title;
    DWORD processId = 0;
};

struct EnumWindowData {
    FindWindowRequest request;
    HWND match = nullptr;
};

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
    if (!data->request.title.empty() && WindowText(hwnd) != data->request.title) {
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
        Sleep(50);
    }
    return nullptr;
}

bool WaitForNoTopWindow(const FindWindowRequest& request, DWORD timeoutMs) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        if (!FindTopWindow(request)) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

HWND WaitForPopupMenu(DWORD timeoutMs) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        HWND menu = FindTopWindow(FindWindowRequest{L"#32768", L"", 0});
        if (menu) {
            return menu;
        }
        Sleep(25);
    }
    return nullptr;
}

std::filesystem::path ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool ExerciseAboutTwice(HWND mainWindow, DWORD processId) {
    const FindWindowRequest aboutRequest{L"QuattroAboutDialog", L"关于", processId};
    for (int i = 0; i < 2; ++i) {
        PostMessageW(mainWindow, WM_COMMAND, MAKEWPARAM(ID_MENU_ABOUT, 0), 0);
        HWND about = WaitForTopWindow(aboutRequest, 5000);
        if (!about) {
            std::cerr << "about dialog did not appear\n";
            return false;
        }
        PostMessageW(about, WM_CLOSE, 0, 0);
        if (!WaitForNoTopWindow(aboutRequest, 5000)) {
            std::cerr << "about dialog did not close\n";
            return false;
        }
    }
    return true;
}

bool ExitThroughTrayMenu(HWND mainWindow, HANDLE processHandle, DWORD processId) {
    // Open the tray menu via the tray callback message; do not move the real
    // cursor or inject real input (acceptance rule: no focus, no mouse hijack).
    PostMessageW(mainWindow, WM_QUATTRO_TRAY, 0, WM_RBUTTONUP);
    HWND menu = WaitForPopupMenu(2000);
    if (!menu) {
        std::cerr << "tray popup menu did not appear\n";
        return false;
    }
    HMENU menuHandle = reinterpret_cast<HMENU>(SendMessageW(menu, kGetMenuHandleMessage, 0, 0));
    const int itemCount = menuHandle ? GetMenuItemCount(menuHandle) : 0;
    const UINT exitCommand = itemCount > 0
        ? GetMenuItemID(menuHandle, itemCount - 1)
        : 0;
    if (exitCommand != ID_MENU_EXIT) {
        std::cerr << "tray menu last item is not the exit command\n";
        return false;
    }
    // Dismiss the modal menu with messages, then invoke the exit command the
    // same way TrackPopupMenu's TPM_RETURNCMD path does (WM_COMMAND).
    PostMessageW(mainWindow, WM_CANCELMODE, 0, 0);
    PostMessageW(menu, WM_CANCELMODE, 0, 0);
    if (!WaitForNoTopWindow(FindWindowRequest{L"#32768", L"", processId}, 2000)) {
        std::cerr << "tray popup menu did not close\n";
        return false;
    }
    PostMessageW(mainWindow, WM_COMMAND, MAKEWPARAM(exitCommand, 0), 0);
    return WaitForSingleObject(processHandle, 5000) != WAIT_TIMEOUT;
}
}

int wmain() {
    const std::filesystem::path exe = ModuleDirectory() / L"Quattro.exe";
    if (!std::filesystem::exists(exe)) {
        std::cerr << "Quattro.exe not found beside acceptance executable\n";
        return 2;
    }

    // Acceptance rule: tests must not steal focus or bring windows to front.
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    const HWND initialForeground = GetForegroundWindow();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, ModuleDirectory().c_str(), &startup, &process)) {
        std::cerr << "CreateProcess failed\n";
        return 2;
    }

    int result = 0;
    HWND mainWindow = WaitForTopWindow(FindWindowRequest{L"QuattroMainWindow", L"", process.dwProcessId}, 10000);
    if (!mainWindow) {
        std::cerr << "main window did not appear\n";
        result = 1;
    } else if (!ExerciseAboutTwice(mainWindow, process.dwProcessId)) {
        result = 1;
    } else if (!ExitThroughTrayMenu(mainWindow, process.hProcess, process.dwProcessId)) {
        std::cerr << "process did not exit through tray menu after about dialogs\n";
        result = 1;
    }

    if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 3);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    // Acceptance rule: the test must not have changed foreground ownership.
    const HWND finalForeground = GetForegroundWindow();
    if (initialForeground != finalForeground) {
        std::cerr << "foreground window changed during acceptance run\n";
        result = 1;
    }

    if (result == 0) {
        std::cout << "about_tray_exit_acceptance=passed\n";
    }
    return result;
}
