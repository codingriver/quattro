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

void ClickAt(int x, int y) {
    SetCursorPos(x, y);
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void PressKey(WORD key) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = key;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
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

bool ExitThroughTrayMenu(HWND mainWindow, HANDLE processHandle) {
    RECT rect{};
    GetWindowRect(mainWindow, &rect);
    const int menuX = rect.left + 24;
    const int menuY = rect.top + 24;
    SetCursorPos(menuX, menuY);
    PostMessageW(mainWindow, WM_QUATTRO_TRAY, 0, WM_RBUTTONUP);
    HWND menu = WaitForPopupMenu(2000);
    if (!menu) {
        std::cerr << "tray popup menu did not appear\n";
        return false;
    }
    HMENU menuHandle = reinterpret_cast<HMENU>(SendMessageW(menu, kGetMenuHandleMessage, 0, 0));
    const int itemCount = menuHandle ? GetMenuItemCount(menuHandle) : 0;
    RECT exitRect{};
    if (itemCount > 0 && GetMenuItemRect(mainWindow, menuHandle, static_cast<UINT>(itemCount - 1), &exitRect)) {
        ClickAt((exitRect.left + exitRect.right) / 2, (exitRect.top + exitRect.bottom) / 2);
    } else {
        PressKey(VK_END);
        Sleep(100);
        PressKey(VK_RETURN);
    }
    return WaitForSingleObject(processHandle, 5000) != WAIT_TIMEOUT;
}
}

int wmain() {
    const std::filesystem::path exe = ModuleDirectory() / L"Quattro.exe";
    if (!std::filesystem::exists(exe)) {
        std::cerr << "Quattro.exe not found beside acceptance executable\n";
        return 2;
    }

    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"0");

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
    } else if (!ExitThroughTrayMenu(mainWindow, process.hProcess)) {
        std::cerr << "process did not exit through tray menu after about dialogs\n";
        result = 1;
    }

    if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 3);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (result == 0) {
        std::cout << "about_tray_exit_acceptance=passed\n";
    }
    return result;
}
