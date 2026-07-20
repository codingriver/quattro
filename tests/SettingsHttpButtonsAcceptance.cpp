#ifdef QUATTRO_WITH_HTTPLIB
#include <winsock2.h>
#include <ws2tcpip.h>
#include <httplib.h>
#endif

#include "../src/common/AppLog.h"
#include "../src/domain/TrackedContextMenuProviders.h"
#include "../src/services/LocalHttpServerService.h"
#include "../src/windows/SimpleDialogs.h"
#include "../src/theme/Theme.h"
#include "../src/common/Utilities.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
bool Require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        return false;
    }
    return true;
}

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

struct FindTopWindowData {
    std::wstring windowClass;
    std::wstring title;
    HWND match = nullptr;
};

BOOL CALLBACK FindTopWindowProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<FindTopWindowData*>(lParam);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return TRUE;
    }
    if (!data->windowClass.empty() && ClassName(hwnd) != data->windowClass) {
        return TRUE;
    }
    if (!data->title.empty() && WindowText(hwnd) != data->title) {
        return TRUE;
    }
    data->match = hwnd;
    return FALSE;
}

HWND FindTopWindow(const std::wstring& windowClass, const std::wstring& title) {
    FindTopWindowData data{windowClass, title, nullptr};
    EnumWindows(FindTopWindowProc, reinterpret_cast<LPARAM>(&data));
    return data.match;
}

HWND WaitForTopWindow(const std::wstring& windowClass, const std::wstring& title, DWORD timeoutMs = 5000) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        if (HWND hwnd = FindTopWindow(windowClass, title)) {
            return hwnd;
        }
        Sleep(50);
    }
    return nullptr;
}

struct FindChildData {
    std::wstring text;
    HWND match = nullptr;
};

BOOL CALLBACK FindChildByTextProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<FindChildData*>(lParam);
    if (WindowText(hwnd) == data->text) {
        data->match = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindChildByText(HWND parent, const std::wstring& text) {
    FindChildData data{text, nullptr};
    EnumChildWindows(parent, FindChildByTextProc, reinterpret_cast<LPARAM>(&data));
    return data.match;
}

bool ClickButton(HWND parent, const std::wstring& text) {
    HWND button = FindChildByText(parent, text);
    if (!button) {
        std::wcerr << L"Missing button: " << text << L"\n";
        return false;
    }
    PostMessageW(button, BM_CLICK, 0, 0);
    Sleep(100);
    return true;
}

bool ClickCommandButton(HWND parent, const std::wstring& text) {
    HWND button = FindChildByText(parent, text);
    if (!button) {
        std::wcerr << L"Missing button: " << text << L"\n";
        return false;
    }
    PostMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
    Sleep(100);
    return true;
}

void CloseHttpMessageBox() {
    if (HWND message = WaitForTopWindow(L"QuattroThemedMessageDialog", L"HTTP 服务", 4000)) {
        PostMessageW(message, WM_COMMAND, IDOK, 0);
    }
}

void ConfirmResetContextMenu() {
    if (HWND message = WaitForTopWindow(L"QuattroThemedMessageDialog", L"重置右键菜单", 4000)) {
        PostMessageW(message, WM_COMMAND, IDYES, 0);
    }
}

void CloseResetContextMenuMessageBox() {
    if (HWND message = WaitForTopWindow(L"QuattroThemedMessageDialog", L"重置右键菜单", 4000)) {
        PostMessageW(message, WM_COMMAND, IDOK, 0);
    }
}

std::wstring ClipboardText(HWND owner) {
    if (!OpenClipboard(owner)) {
        return {};
    }
    std::wstring text;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(handle));
        if (data) {
            text = data;
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return text;
}

bool WaitForFlag(const std::atomic_bool& value, DWORD timeoutMs = 3000) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        if (value.load()) {
            return true;
        }
        Sleep(50);
    }
    return value.load();
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int wmain() {
#ifndef QUATTRO_WITH_HTTPLIB
    std::wcout << L"Settings HTTP button acceptance skipped: QUATTRO_WITH_HTTPLIB is not enabled.\n";
    return 0;
#else
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    // Acceptance rule: tests must not steal focus or bring windows to front.
    // With this flag the settings dialog opens via SW_SHOWNOACTIVATE.
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    const HWND initialForeground = GetForegroundWindow();
    const auto root = std::filesystem::temp_directory_path() / (L"quattro-settings-http-buttons-" + std::to_wstring(GetCurrentProcessId()));
    const auto userConfig = std::filesystem::temp_directory_path() / (L"quattro-settings-http-user-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", userConfig.c_str());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(userConfig, ec);
    std::filesystem::create_directories(root, ec);
    InitializeAppLog(root);
    WriteTextFile(LocalHttpServerService::DetailConfigPath(root),
        "[http]\r\n"
        "GuestRead=1\r\n"
        "GuestUpload=1\r\n"
        "GuestWrite=1\r\n"
        "AuthEnabled=0\r\n"
        "DirectoryListing=1\r\n"
        "AllowPost=1\r\n"
        "AllowPut=1\r\n"
        "MimeMap=.txt=text/plain; charset=utf-8\r\n");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OwnerProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"QuattroSettingsHttpButtonsOwner";
    RegisterClassExW(&wc);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW,
        40, 40, 320, 240, nullptr, nullptr, instance, nullptr);
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    SetWindowPos(owner, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    AppConfig config;
    config.mainHotKey = VK_F12;
    config.processLocatorHotKey = VK_F12;
    config.globalHotKeysEnabled = true;
    config.hideOnStart = false;
    for (const auto& provider : TrackedContextMenuProviders()) {
        config.*(provider.configMember) = true;
    }
    config.httpServerRootPath = root.wstring();
    config.httpServerPort = 45200 + static_cast<int>(GetCurrentProcessId() % 1000);
    config.httpServerLanAccess = false;
    Theme theme = Theme::Load(std::filesystem::current_path() / L"theme", L"default");
    LocalHttpServerService service;
    std::atomic_bool interactionDone{false};
    std::atomic_bool interactionOk{true};
    std::atomic_bool applyCalled{false};
    std::atomic_bool resetContextMenuCalled{false};
    std::atomic_bool appliedShowTooltip{true};
    std::atomic_bool appliedHideOnStart{true};
    bool importedData = false;
    const DWORD dialogThreadId = GetCurrentThreadId();

    std::thread interactor([&]() {
        bool localOk = true;
        HWND settings = WaitForTopWindow(L"QuattroSettingsDialog", L"设置", 7000);
        localOk = Require(settings != nullptr, L"settings dialog should open") && localOk;
        if (!settings) {
            interactionOk = false;
            interactionDone = true;
            PostThreadMessageW(dialogThreadId, WM_QUIT, 0, 0);
            return;
        }
        localOk = Require(ClickButton(settings, L"行为"), L"Behavior tab should be clickable") && localOk;
        localOk = Require(ClickButton(settings, L"启动后隐藏"), L"Hide on start checkbox should be clickable") && localOk;
        localOk = Require(ClickButton(settings, L"显示"), L"Display tab should be clickable") && localOk;
        localOk = Require(ClickButton(settings, L"显示提示"), L"Display tooltip checkbox should be clickable") && localOk;
        localOk = Require(ClickCommandButton(settings, L"应用"), L"Apply button should be clickable") && localOk;
        localOk = Require(WaitForFlag(applyCalled), L"Apply button should invoke settings apply callback") && localOk;
        localOk = Require(!appliedShowTooltip.load(), L"Apply button should publish edited settings") && localOk;
        localOk = Require(!appliedHideOnStart.load(), L"Apply button should not publish edits from another settings tab") && localOk;
        localOk = Require(FindTopWindow(L"QuattroThemedMessageDialog", L"热键冲突") == nullptr,
            L"Applying a non-hotkey tab should not show a hotkey conflict") && localOk;
        localOk = Require(IsWindow(settings), L"Apply button should keep settings dialog open") && localOk;

        localOk = Require(ClickButton(settings, L"HTTP"), L"HTTP tab should be clickable") && localOk;
        localOk = Require(FindChildByText(settings, L"打开目录") != nullptr, L"Open Web Root button should exist") && localOk;
        localOk = Require(FindChildByText(settings, L"未启动") != nullptr, L"Stopped HTTP status tag should be visible") && localOk;
        localOk = Require(ClickButton(settings, L"启动"), L"Start button should be clickable") && localOk;
        CloseHttpMessageBox();
        localOk = Require(service.IsRunning(), L"Start button should start HTTP service") && localOk;
        localOk = Require(FindChildByText(settings, L"运行中") != nullptr, L"Running HTTP status tag should be visible") && localOk;

        localOk = Require(ClickButton(settings, L"复制地址"), L"Copy URL button should be clickable") && localOk;
        const std::wstring copied = ClipboardText(settings);
        localOk = Require(copied.rfind(L"http://", 0) == 0 &&
                copied.find(L":" + std::to_wstring(config.httpServerPort) + L"/") != std::wstring::npos,
            L"Copy URL button should write HTTP URL with configured port to clipboard") && localOk;

        localOk = Require(ClickButton(settings, L"重启"), L"Restart button should be clickable") && localOk;
        CloseHttpMessageBox();
        localOk = Require(service.IsRunning(), L"Restart button should keep HTTP service running") && localOk;

        localOk = Require(ClickButton(settings, L"停止"), L"Stop button should be clickable") && localOk;
        CloseHttpMessageBox();
        localOk = Require(!service.IsRunning(), L"Stop button should stop HTTP service") && localOk;
        localOk = Require(FindChildByText(settings, L"未启动") != nullptr, L"Stopped HTTP status tag should return after stop") && localOk;

        localOk = Require(ClickButton(settings, L"备份"), L"Backup tab should be clickable") && localOk;
        localOk = Require(
            ClickCommandButton(settings, L"重置右键菜单"),
            L"Reset context menu button should be clickable") && localOk;
        ConfirmResetContextMenu();
        localOk = Require(
            WaitForFlag(resetContextMenuCalled),
            L"Reset context menu button should invoke callback") && localOk;
        CloseResetContextMenuMessageBox();

        PostMessageW(settings, WM_CLOSE, 0, 0);
        interactionOk = localOk;
        interactionDone = true;
    });

    auto applyCallback = [&](const AppConfig& applied, bool) -> bool {
        appliedShowTooltip = applied.showTooltip;
        appliedHideOnStart = applied.hideOnStart;
        applyCalled = true;
        return false;
    };
    auto resetContextMenuCallback = [&]() -> bool {
        resetContextMenuCalled = true;
        return true;
    };
    const std::filesystem::path baseDirectory = std::filesystem::current_path();
    ShowSettingsDialog(
        owner,
        instance,
        config,
        theme,
        baseDirectory,
        baseDirectory,
        &importedData,
        &service,
        false,
        false,
        false,
        applyCallback,
        resetContextMenuCallback);
    if (interactor.joinable()) {
        interactor.join();
    }
    DestroyWindow(owner);
    ShutdownAppLog();
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(userConfig, ec);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", nullptr);

    bool trackingReset = true;
    for (const auto& provider : TrackedContextMenuProviders()) {
        trackingReset = trackingReset && !(config.*(provider.configMember));
    }
    // Acceptance rule: the test must not have changed foreground ownership.
    const bool foregroundPreserved = GetForegroundWindow() == initialForeground;
    if (!foregroundPreserved) {
        std::wcerr << L"FAIL: foreground window changed during acceptance run\n";
    }
    const bool ok = interactionDone && interactionOk && trackingReset && foregroundPreserved;
    if (!ok) {
        return 1;
    }
    std::wcout << L"Settings HTTP button acceptance passed.\n";
    return 0;
#endif
}
