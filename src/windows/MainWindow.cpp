#include "MainWindow.h"

#include "AboutDialog.h"
#include "AppLog.h"
#include "BuiltinTools.h"
#include "EmbeddedExecutableManager.h"
#include "Elevation.h"
#include "FileDialog.h"
#include "HotKeyEditor.h"
#include "LinkEditDialog.h"
#include "LinkSorting.h"
#include "MainHotKey.h"
#include "MainTitleBuildMarkerLayout.h"
#include "MenuAnchorGeometry.h"
#include "MenuCatalog.h"
#include "QuickImportDialog.h"
#include "SelectedPathCopyService.h"
#include "ShellItemService.h"
#include "SimpleDialogs.h"
#include "StartupService.h"
#include "SystemFunctions.h"
#include "ThemedControls.h"
#include "ThemedWindowUi.h"
#include "TodoEditDialog.h"
#include "TodoSchedule.h"
#include "ToastFeedback.h"
#include "UpdateCheckService.h"
#include "UpdateCheckDialog.h"
#include "UpdateDownloadDialog.h"
#include "UpdateInstaller.h"
#include "UrlEditDialog.h"
#include "Utilities.h"
#include "Version.h"
#include "WebDavRecoveryService.h"
#include "../../resources/resource.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mutex>
#include <winnetwk.h>
#include <optional>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <sstream>
#include <system_error>
#include <tlhelp32.h>
#include <unordered_map>
#include <uxtheme.h>
#include <windowsx.h>

namespace {
constexpr int ID_HOTKEY_MAIN = 1;
constexpr int ID_HOTKEY_PROCESS_LOCATOR = 2;
constexpr int ID_HOTKEY_COPY_SELECTED_PATHS = 3;
constexpr int ID_HOTKEY_LINK_BASE = 1000;
constexpr int ID_NOTE_EDIT = 2100;
constexpr UINT_PTR ID_TIMER_DOCK = 10;
constexpr UINT_PTR ID_TIMER_HOVER_ACTIVATE = 11;
constexpr UINT_PTR ID_TIMER_NOTE_AUTOSAVE = 12;
constexpr UINT_PTR ID_TIMER_REMINDER_SCAN = 13;
constexpr UINT kTrayIconId = 1;
constexpr UINT WM_QUATTRO_DOUBLE_ALT_HOTKEY = WM_APP + 0x6C;
constexpr int kDockVisiblePixels = 3;
constexpr int kDockPeekVisiblePixels = 6;
constexpr int kDockRestoreGraceMs = 1500;
constexpr int kDockSnapThreshold = 12;
constexpr const wchar_t* kDockPeekWindowClass = L"QuattroDockPeekWindow";
constexpr const wchar_t* kAppDisplayName = L"Quattro快速启动器";
constexpr DWORD kDoubleAltMaxIntervalMs = 450;

bool SuppressTrayForIsolatedTest() {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"QUATTRO_TEST_SUPPRESS_TRAY", value, static_cast<DWORD>(std::size(value)));
    return length > 0 && length < std::size(value) && value[0] != L'0';
}

bool FailFirstReminderForIsolatedTest() {
    if (!QuattroTestMode()) return false;
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"QUATTRO_TEST_REMINDER_FAIL_ONCE", value, static_cast<DWORD>(std::size(value)));
    return length > 0 && length < std::size(value) && value[0] != L'0';
}

ULONGLONG ReminderRetryDelayMs() {
    if (QuattroTestMode()) {
        wchar_t value[16]{};
        const DWORD length = GetEnvironmentVariableW(
            L"QUATTRO_TEST_REMINDER_RETRY_MS", value, static_cast<DWORD>(std::size(value)));
        if (length > 0 && length < std::size(value)) {
            const int parsed = _wtoi(value);
            if (parsed >= 50) return static_cast<ULONGLONG>(parsed);
        }
    }
    return 15000;
}

std::wstring BuildMarkerDisplayText() {
    const std::wstring marker = QuattroBuildMarkerText();
    return marker.empty() ? std::wstring{} : L"（" + marker + L"）";
}

std::wstring AppWindowTitle() {
    return std::wstring(kAppDisplayName) + BuildMarkerDisplayText();
}

HHOOK gDoubleAltHook = nullptr;
HWND gDoubleAltTarget = nullptr;
DWORD gLastAltUpTick = 0;
bool gOtherKeySinceAlt = false;

bool IsAltVirtualKey(DWORD vkCode) {
    return vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU;
}

LRESULT CALLBACK DoubleAltKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && gDoubleAltTarget) {
        const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        if (keyDown || keyUp) {
            if (IsAltVirtualKey(event->vkCode)) {
                if (keyDown) {
                    gOtherKeySinceAlt = false;
                } else if (!gOtherKeySinceAlt) {
                    const DWORD tick = event->time != 0 ? event->time : GetTickCount();
                    if (gLastAltUpTick != 0 && tick - gLastAltUpTick <= kDoubleAltMaxIntervalMs) {
                        PostMessageW(gDoubleAltTarget, WM_QUATTRO_DOUBLE_ALT_HOTKEY, 0, 0);
                        gLastAltUpTick = 0;
                    } else {
                        gLastAltUpTick = tick;
                    }
                }
            } else if (keyDown) {
                gOtherKeySinceAlt = true;
                gLastAltUpTick = 0;
            }
        }
    }
    return CallNextHookEx(gDoubleAltHook, code, wParam, lParam);
}

bool InstallDoubleAltHotKeyHook(HWND hwnd) {
    if (gDoubleAltHook) {
        gDoubleAltTarget = hwnd;
        return true;
    }
    gDoubleAltTarget = hwnd;
    gLastAltUpTick = 0;
    gOtherKeySinceAlt = false;
    gDoubleAltHook = SetWindowsHookExW(WH_KEYBOARD_LL, DoubleAltKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (!gDoubleAltHook) {
        gDoubleAltTarget = nullptr;
        return false;
    }
    return true;
}

void UninstallDoubleAltHotKeyHook(HWND hwnd) {
    if (gDoubleAltTarget != hwnd) {
        return;
    }
    if (gDoubleAltHook) {
        UnhookWindowsHookEx(gDoubleAltHook);
        gDoubleAltHook = nullptr;
    }
    gDoubleAltTarget = nullptr;
    gLastAltUpTick = 0;
    gOtherKeySinceAlt = false;
}

std::wstring VersionCompareText(int comparison) {
    if (comparison < 0) {
        return L"local_older";
    }
    if (comparison > 0) {
        return L"local_newer";
    }
    return L"equal";
}

enum class DockEdge {
    None,
    Left,
    Right,
    Top,
    Bottom,
};

struct DockTarget {
    DockEdge edge = DockEdge::None;
    HMONITOR monitor = nullptr;
    RECT work{};
};

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

struct SiblingQuattroProcess {
    DWORD processId = 0;
    HWND mainWindow = nullptr;
    HANDLE process = nullptr;
};

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

std::filesystem::path AbsolutePathForLog(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute;
}

std::wstring FileSizeForLog(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? L"unknown" : std::to_wstring(size);
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

std::vector<SiblingQuattroProcess> CollectSiblingQuattroProcesses() {
    std::vector<SiblingQuattroProcess> siblings;
    const std::wstring currentPath = NormalizeProcessPath(CurrentExecutablePath());
    if (currentPath.empty()) {
        WriteAppLog(L"退出清理：无法获取当前可执行文件路径。");
        return siblings;
    }

    MainWindowEnumerationContext windowContext;
    EnumWindows(EnumQuattroMainWindows, reinterpret_cast<LPARAM>(&windowContext));

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        WriteAppLog(L"退出清理：进程快照创建失败: " + FormatLastError(GetLastError()));
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

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
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
        WriteAppLog(L"退出清理：进程快照读取失败: " + FormatLastError(GetLastError()));
    }
    CloseHandle(snapshot);
    return siblings;
}

void CloseSiblingHandles(std::vector<SiblingQuattroProcess>& siblings) {
    for (auto& sibling : siblings) {
        if (sibling.process) {
            CloseHandle(sibling.process);
            sibling.process = nullptr;
        }
    }
}

void DeleteTrayIconForWindow(HWND hwnd, const wchar_t* context) {
    if (!hwnd) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    if (!Shell_NotifyIconW(NIM_DELETE, &data)) {
        WriteAppLog(std::wstring(context) + L"：托盘图标删除请求失败 hwnd=" +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd)) +
                    L"，错误=" + FormatLastError(GetLastError()));
    }
}

void TerminateSiblingQuattroProcesses() {
    std::vector<SiblingQuattroProcess> siblings = CollectSiblingQuattroProcesses();
    if (siblings.empty()) {
        WriteAppLog(L"退出清理：未发现同路径后台实例。");
        return;
    }

    WriteAppLog(L"退出清理：发现同路径后台实例 " + std::to_wstring(siblings.size()) + L" 个。");
    for (const auto& sibling : siblings) {
        if (sibling.mainWindow && IsWindow(sibling.mainWindow)) {
            if (PostMessageW(sibling.mainWindow, WM_QUATTRO_EXIT_INSTANCE, 0, 0)) {
                WriteAppLog(L"退出清理：已通知实例退出 pid=" + std::to_wstring(sibling.processId));
            } else {
                WriteAppLog(L"退出清理：通知实例退出失败 pid=" + std::to_wstring(sibling.processId) +
                            L"，错误=" + FormatLastError(GetLastError()));
            }
        } else {
            WriteAppLog(L"退出清理：实例无可用主窗口，将等待后强制结束 pid=" + std::to_wstring(sibling.processId));
        }
    }

    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (;;) {
        bool allExited = true;
        for (const auto& sibling : siblings) {
            if (sibling.process && WaitForSingleObject(sibling.process, 0) == WAIT_TIMEOUT) {
                allExited = false;
                break;
            }
        }
        if (allExited || GetTickCount64() >= deadline) {
            break;
        }
        Sleep(50);
    }

    for (const auto& sibling : siblings) {
        if (!sibling.process || WaitForSingleObject(sibling.process, 0) != WAIT_TIMEOUT) {
            continue;
        }
        if (sibling.mainWindow && IsWindow(sibling.mainWindow)) {
            DeleteTrayIconForWindow(sibling.mainWindow, L"退出清理");
        }
        if (TerminateProcess(sibling.process, 0)) {
            WriteAppLog(L"退出清理：已强制结束失控实例 pid=" + std::to_wstring(sibling.processId));
        } else {
            WriteAppLog(L"退出清理：强制结束失控实例失败 pid=" + std::to_wstring(sibling.processId) +
                        L"，错误=" + FormatLastError(GetLastError()));
        }
    }

    CloseSiblingHandles(siblings);
}

float Width(const D2D1_RECT_F& rect) {
    return rect.right - rect.left;
}

float Height(const D2D1_RECT_F& rect) {
    return rect.bottom - rect.top;
}

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

int ClampSpanStart(int value, int span, int minValue, int maxValue) {
    if (span >= maxValue - minValue) {
        return minValue;
    }
    return std::max(minValue, std::min(value, maxValue - span));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

DWORD MainWindowExStyle(int alpha) {
    DWORD style = WS_EX_APPWINDOW;
    if (BackgroundAcceptanceMode()) {
        style |= WS_EX_NOACTIVATE;
    }
    if (alpha < 255) {
        style |= WS_EX_LAYERED;
    }
    return style;
}

void ApplyMainWindowAlpha(HWND hwnd, int alpha) {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (alpha < 255) {
        if ((exStyle & WS_EX_LAYERED) == 0) {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        }
        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
    } else if ((exStyle & WS_EX_LAYERED) != 0) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }
}

HWND MainWindowTopMostInsertAfter(bool topMost) {
    if (BackgroundAcceptanceMode()) {
        return HWND_BOTTOM;
    }
    return topMost ? HWND_TOPMOST : HWND_NOTOPMOST;
}

HWND MainWindowMoveInsertAfter(bool topMost) {
    if (BackgroundAcceptanceMode()) {
        return HWND_BOTTOM;
    }
    return topMost ? HWND_TOPMOST : nullptr;
}

HWND MainWindowActivationInsertAfter(bool topMost) {
    if (BackgroundAcceptanceMode()) {
        return HWND_BOTTOM;
    }
    return topMost ? HWND_TOPMOST : HWND_TOP;
}

UINT MainWindowMoveFlags(bool topMost, UINT flags) {
    if (topMost) {
        return flags & ~SWP_NOZORDER;
    }
    return flags | SWP_NOZORDER;
}

Color RgbColor(int r, int g, int b, float a = 1.0f) {
    return Color{
        ClampFloat(static_cast<float>(r) / 255.0f, 0.0f, 1.0f),
        ClampFloat(static_cast<float>(g) / 255.0f, 0.0f, 1.0f),
        ClampFloat(static_cast<float>(b) / 255.0f, 0.0f, 1.0f),
        ClampFloat(a, 0.0f, 1.0f)};
}

// Linear blend between two colors. t=0 returns a, t=1 returns b.
Color LerpColor(const Color& a, const Color& b, float t) {
    t = ClampFloat(t, 0.0f, 1.0f);
    return Color{
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t};
}

bool OpenClipboardWithRetry(HWND owner, DWORD timeoutMs = 250) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(15);
    } while (GetTickCount64() < deadline);
    return OpenClipboard(owner) != FALSE;
}

std::optional<DockTarget> FindDockTarget(const RECT& window, int threshold) {
    HMONITOR monitor = MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return std::nullopt;
    }

    const RECT work = info.rcWork;
    const int leftDistance = std::abs(window.left - work.left);
    const int rightDistance = std::abs(window.right - work.right);
    const int topDistance = std::abs(window.top - work.top);
    const int bottomDistance = std::abs(window.bottom - work.bottom);
    const int nearest = std::min(std::min(leftDistance, rightDistance), std::min(topDistance, bottomDistance));
    if (nearest > threshold) {
        return std::nullopt;
    }

    DockEdge edge = DockEdge::Left;
    if (nearest == topDistance) {
        edge = DockEdge::Top;
    } else if (nearest == bottomDistance) {
        edge = DockEdge::Bottom;
    } else if (nearest == rightDistance) {
        edge = DockEdge::Right;
    }

    return DockTarget{edge, monitor, work};
}

RECT SnapRectToDockTarget(RECT window, const DockTarget& target) {
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    RECT snapped = window;
    switch (target.edge) {
    case DockEdge::Left:
        snapped.left = target.work.left;
        snapped.right = snapped.left + width;
        snapped.top = ClampSpanStart(window.top, height, target.work.top, target.work.bottom);
        snapped.bottom = snapped.top + height;
        break;
    case DockEdge::Right:
        snapped.right = target.work.right;
        snapped.left = snapped.right - width;
        snapped.top = ClampSpanStart(window.top, height, target.work.top, target.work.bottom);
        snapped.bottom = snapped.top + height;
        break;
    case DockEdge::Top:
        snapped.top = target.work.top;
        snapped.bottom = snapped.top + height;
        snapped.left = ClampSpanStart(window.left, width, target.work.left, target.work.right);
        snapped.right = snapped.left + width;
        break;
    case DockEdge::Bottom:
        snapped.bottom = target.work.bottom;
        snapped.top = snapped.bottom - height;
        snapped.left = ClampSpanStart(window.left, width, target.work.left, target.work.right);
        snapped.right = snapped.left + width;
        break;
    case DockEdge::None:
        break;
    }
    return snapped;
}

RECT HiddenRectForDockTarget(const RECT& restore, const DockTarget& target) {
    const int width = restore.right - restore.left;
    const int height = restore.bottom - restore.top;
    RECT hidden = restore;
    switch (target.edge) {
    case DockEdge::Left:
        hidden.left = target.work.left - width + kDockVisiblePixels;
        hidden.right = hidden.left + width;
        break;
    case DockEdge::Right:
        hidden.left = target.work.right - kDockVisiblePixels;
        hidden.right = hidden.left + width;
        break;
    case DockEdge::Top:
        hidden.top = target.work.top - height + kDockVisiblePixels;
        hidden.bottom = hidden.top + height;
        break;
    case DockEdge::Bottom:
        hidden.top = target.work.bottom - kDockVisiblePixels;
        hidden.bottom = hidden.top + height;
        break;
    case DockEdge::None:
        break;
    }
    return hidden;
}

RECT DockPeekRectForDockTarget(const RECT& restore, const DockTarget& target) {
    RECT peek = restore;
    switch (target.edge) {
    case DockEdge::Left:
        peek.left = target.work.left;
        peek.right = target.work.left + kDockPeekVisiblePixels;
        break;
    case DockEdge::Right:
        peek.left = target.work.right - kDockPeekVisiblePixels;
        peek.right = target.work.right;
        break;
    case DockEdge::Top:
        peek.top = target.work.top;
        peek.bottom = target.work.top + kDockPeekVisiblePixels;
        break;
    case DockEdge::Bottom:
        peek.top = target.work.bottom - kDockPeekVisiblePixels;
        peek.bottom = target.work.bottom;
        break;
    case DockEdge::None:
        break;
    }
    return peek;
}

long long IntersectionArea(const RECT& a, const RECT& b) {
    const int left = std::max(a.left, b.left);
    const int top = std::max(a.top, b.top);
    const int right = std::min(a.right, b.right);
    const int bottom = std::min(a.bottom, b.bottom);
    if (left >= right || top >= bottom) {
        return 0;
    }
    return static_cast<long long>(right - left) * static_cast<long long>(bottom - top);
}

struct OtherMonitorIntersectionContext {
    HMONITOR dockMonitor = nullptr;
    RECT hidden{};
    bool intersects = false;
};

BOOL CALLBACK CheckOtherMonitorIntersection(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* context = reinterpret_cast<OtherMonitorIntersectionContext*>(data);
    if (!context || monitor == context->dockMonitor) {
        return TRUE;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && IntersectionArea(context->hidden, info.rcMonitor) > 0) {
        context->intersects = true;
        return FALSE;
    }
    return TRUE;
}

bool HiddenRectIntersectsOtherMonitor(const RECT& hidden, HMONITOR dockMonitor) {
    OtherMonitorIntersectionContext context{dockMonitor, hidden, false};
    EnumDisplayMonitors(nullptr, nullptr, CheckOtherMonitorIntersection, reinterpret_cast<LPARAM>(&context));
    return context.intersects;
}

std::vector<std::wstring> SplitTooltipLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (lines.empty()) {
        lines.push_back(L"");
    }
    return lines;
}

LRESULT CALLBACK DockPeekWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        HWND owner = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (owner) {
            SendMessageW(owner, WM_QUATTRO_DOCK_PEEK_ACTIVATE, 0, 0);
        }
        return message == WM_SETCURSOR ? TRUE : 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool Intersects(const D2D1_RECT_F& left, const D2D1_RECT_F& right) {
    return left.right > right.left &&
           left.left < right.right &&
           left.bottom > right.top &&
           left.top < right.bottom;
}

D2D1_RECT_F IntersectRectF(const D2D1_RECT_F& left, const D2D1_RECT_F& right) {
    return D2D1::RectF(
        std::max(left.left, right.left),
        std::max(left.top, right.top),
        std::min(left.right, right.right),
        std::min(left.bottom, right.bottom));
}

D2D1_RECT_F Inset(D2D1_RECT_F rect, float dx, float dy) {
    rect.left += dx;
    rect.right -= dx;
    rect.top += dy;
    rect.bottom -= dy;
    return rect;
}

std::wstring TodayText() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    static constexpr const wchar_t* kWeekdays[] = {
        L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六",
    };
    return std::to_wstring(now.wMonth) + L"月" + std::to_wstring(now.wDay) + L"日 " + kWeekdays[now.wDayOfWeek % 7];
}

std::wstring ConfigPackageFileName() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"quattro-%04u%02u%02u-%02u%02u.q4cfg",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    return buffer;
}

std::wstring FormatConfigPackageReport(const ConfigPackageReport& report) {
    std::wstring text = report.message.empty() ? (report.ok ? L"操作完成。" : L"操作失败。") : report.message;
    if (report.groupsAdded > 0 || report.groupsMerged > 0 || report.tagsAdded > 0 ||
        report.tagsMerged > 0 || report.linksAdded > 0 || report.linksSkippedDuplicate > 0 ||
        report.notesAdded > 0 || report.notesMerged > 0 || report.todosAdded > 0 ||
        report.urlIconsAdded > 0) {
        text += L"\n\n新增分组: " + std::to_wstring(report.groupsAdded);
        text += L"\n复用分组: " + std::to_wstring(report.groupsMerged);
        text += L"\n新增标签: " + std::to_wstring(report.tagsAdded);
        text += L"\n复用标签: " + std::to_wstring(report.tagsMerged);
        text += L"\n新增启动项: " + std::to_wstring(report.linksAdded);
        text += L"\n跳过重复启动项: " + std::to_wstring(report.linksSkippedDuplicate);
        text += L"\n新增便签: " + std::to_wstring(report.notesAdded);
        text += L"\n合并便签: " + std::to_wstring(report.notesMerged);
        text += L"\n新增待办: " + std::to_wstring(report.todosAdded);
        text += L"\n新增 URL 图标: " + std::to_wstring(report.urlIconsAdded);
    }
    if (!report.warnings.empty()) {
        text += L"\n\n警告:";
        for (const auto& warning : report.warnings) {
            text += L"\n- " + warning;
        }
    }
    return text;
}

POINT ClampWindowPosition(int x, int y, int width, int height) {
    RECT proposed{x, y, x + width, y + height};
    HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return POINT{x, y};
    }

    const RECT work = info.rcWork;
    const int minVisible = 80;
    POINT result{};
    result.x = std::max(static_cast<int>(work.left) - width + minVisible,
                        std::min(x, static_cast<int>(work.right) - minVisible));
    result.y = std::max(static_cast<int>(work.top),
                        std::min(y, static_cast<int>(work.bottom) - minVisible));
    return result;
}

int NextModelLinkPosition(const std::vector<Link>& links, int parentGroup) {
    int maxPosition = -1;
    for (const auto& link : links) {
        if (link.parentGroup == parentGroup) {
            maxPosition = std::max(maxPosition, link.pos);
        }
    }
    return maxPosition + 1;
}

int NextModelGroupPosition(const std::vector<Group>& groups, int parentGroup) {
    int maxPosition = -1;
    for (const auto& group : groups) {
        if (group.parentGroup == parentGroup) {
            maxPosition = std::max(maxPosition, group.pos);
        }
    }
    return maxPosition + 1;
}

std::optional<Color> ParseCustomColor(const std::wstring& value) {
    const std::wstring text = Trim(value);
    if (text.size() != 9 || text[0] != L'#') {
        return std::nullopt;
    }
    auto hexDigit = [](wchar_t ch) -> int {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
        if (ch >= L'A' && ch <= L'F') return 10 + ch - L'A';
        return -1;
    };
    auto byteAt = [&](std::size_t index) -> int {
        const int hi = hexDigit(text[index]);
        const int lo = hexDigit(text[index + 1]);
        return (hi < 0 || lo < 0) ? -1 : hi * 16 + lo;
    };
    const int a = byteAt(1);
    const int r = byteAt(3);
    const int g = byteAt(5);
    const int b = byteAt(7);
    if (a < 0 || r < 0 || g < 0 || b < 0) {
        return std::nullopt;
    }
    return Color{
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        static_cast<float>(a) / 255.0f,
    };
}

bool IsFullScreenForegroundWindow() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == GetDesktopWindow() || foreground == GetShellWindow()) {
        return false;
    }
    RECT rect{};
    if (!GetWindowRect(foreground, &rect)) {
        return false;
    }
    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    return rect.left <= info.rcMonitor.left &&
           rect.top <= info.rcMonitor.top &&
           rect.right >= info.rcMonitor.right &&
           rect.bottom >= info.rcMonitor.bottom;
}

int EffectiveIconSize(const Group* tag) {
    const int value = tag && tag->iconSize > 0 ? tag->iconSize : 32;
    return std::max(24, std::min(48, value));
}

int EffectiveLinkLayout(const Group* tag) {
    return tag && tag->layout == 0 ? 0 : 1;
}

float Metric(const Theme& theme, const wchar_t* component, const wchar_t* name, float fallback) {
    return theme.metric(component, name, fallback);
}

float LinkTileSide(const Theme& theme, int iconSize) {
    return std::max(
        Metric(theme, L"linkItem", L"tileMinSide", 74.0f),
        std::min(
            Metric(theme, L"linkItem", L"tileMaxSide", 96.0f),
            static_cast<float>(iconSize) + Metric(theme, L"linkItem", L"tileIconExtra", 42.0f)));
}

int LinkGridColumns(const Theme& theme, const D2D1_RECT_F& rect, float tileSide, float gap) {
    const float width = std::max(1.0f, Width(rect) - Metric(theme, L"linkItem", L"viewportPaddingX", 16.0f));
    return std::max(1, static_cast<int>((width + gap) / (tileSide + gap)));
}

struct LinkLayoutMetrics {
    int layout = 1;
    int iconSize = 32;
    bool compactTile = false;
    int columns = 1;
    float leftInset = 8.0f;
    float topInset = 8.0f;
    float bottomInset = 8.0f;
    float gapX = 4.0f;
    float gapY = 4.0f;
    float itemWidth = 74.0f;
    float itemHeight = 74.0f;
};

struct TodoVisualStyle {
    Color bg;
    Color border;
    Color title;
    Color dot;
    Color tagBg;
    Color tagText;
};

// Derive a full todo card style from a single semantic accent color, blending
// toward the theme surface (for soft backgrounds) and toward text (for readable
// foregrounds). This keeps todo state colors in sync with the active theme
// instead of hardcoding per-state RGB values.
TodoVisualStyle MakeTodoStyle(const Color& accent, const Color& surface, const Color& text) {
    TodoVisualStyle style{};
    style.bg = LerpColor(surface, accent, 0.12f);
    style.border = LerpColor(surface, accent, 0.45f);
    style.dot = accent;
    style.title = LerpColor(accent, text, 0.55f);
    style.tagBg = LerpColor(surface, accent, 0.18f);
    style.tagText = LerpColor(accent, text, 0.35f);
    return style;
}

LinkLayoutMetrics MakeLinkLayout(const Theme& theme, const D2D1_RECT_F& rect, const Group* tag) {
    LinkLayoutMetrics metrics;
    metrics.layout = EffectiveLinkLayout(tag);
    metrics.iconSize = EffectiveIconSize(tag);

    const float available = std::max(1.0f, Width(rect) - Metric(theme, L"linkItem", L"viewportPaddingX", 16.0f));
    if (metrics.layout == 0) {
        metrics.leftInset = Metric(theme, L"linkItem", L"listLeftInset", 4.0f);
        metrics.topInset = Metric(theme, L"linkItem", L"listTopInset", 6.0f);
        metrics.bottomInset = Metric(theme, L"linkItem", L"listBottomInset", 6.0f);
        metrics.gapX = Metric(theme, L"linkItem", L"listGapX", 0.0f);
        metrics.gapY = Metric(theme, L"linkItem", L"listGapY", 4.0f);
        metrics.columns = 1;
        metrics.itemWidth = std::max(1.0f, Width(rect) - metrics.leftInset * 2.0f);
        metrics.itemHeight = std::max(Metric(theme, L"linkItem", L"listMinHeight", 32.0f), static_cast<float>(metrics.iconSize) + Metric(theme, L"linkItem", L"listHeightExtra", 12.0f));
        return metrics;
    }

    metrics.leftInset = Metric(theme, L"linkItem", L"gridLeftInset", 8.0f);
    metrics.topInset = Metric(theme, L"linkItem", L"gridTopInset", 8.0f);
    metrics.bottomInset = Metric(theme, L"linkItem", L"gridBottomInset", 8.0f);
    if (metrics.iconSize <= 24) {
        metrics.compactTile = true;
        metrics.gapX = Metric(theme, L"linkItem", L"compactGapX", 6.0f);
        metrics.gapY = Metric(theme, L"linkItem", L"compactGapY", 4.0f);
        const float preferredWidth = Metric(theme, L"linkItem", L"compactPreferredWidth", 128.0f);
        metrics.columns = std::max(1, static_cast<int>((available + metrics.gapX) / (preferredWidth + metrics.gapX)));
        metrics.itemWidth = std::max(Metric(theme, L"linkItem", L"compactMinWidth", 72.0f), (available - static_cast<float>(metrics.columns - 1) * metrics.gapX) / static_cast<float>(metrics.columns));
        metrics.itemHeight = std::max(Metric(theme, L"linkItem", L"listMinHeight", 32.0f), static_cast<float>(metrics.iconSize) + Metric(theme, L"linkItem", L"listHeightExtra", 12.0f));
        return metrics;
    }

    metrics.gapX = Metric(theme, L"linkItem", L"gridGapX", 4.0f);
    metrics.gapY = Metric(theme, L"linkItem", L"gridGapY", 4.0f);
    metrics.itemWidth = LinkTileSide(theme, metrics.iconSize);
    metrics.itemHeight = metrics.itemWidth;
    metrics.columns = LinkGridColumns(theme, rect, metrics.itemWidth, metrics.gapX);
    return metrics;
}

D2D1_RECT_F LinkItemRect(const D2D1_RECT_F& rect, const LinkLayoutMetrics& metrics, int index, float scrollOffset) {
    const int column = index % metrics.columns;
    const int row = index / metrics.columns;
    const float x = rect.left + metrics.leftInset + static_cast<float>(column) * (metrics.itemWidth + metrics.gapX);
    const float y = rect.top + metrics.topInset + static_cast<float>(row) * (metrics.itemHeight + metrics.gapY) - scrollOffset;
    return D2D1::RectF(x, y, x + metrics.itemWidth, y + metrics.itemHeight);
}

float LinkContentHeight(std::size_t count, const LinkLayoutMetrics& metrics) {
    if (count == 0) {
        return 0.0f;
    }
    const int rows = static_cast<int>((count + static_cast<std::size_t>(metrics.columns) - 1) / static_cast<std::size_t>(metrics.columns));
    return metrics.topInset +
           static_cast<float>(rows) * metrics.itemHeight +
           static_cast<float>(std::max(0, rows - 1)) * metrics.gapY +
           metrics.bottomInset;
}

float NavigationItemWidth(const Theme& theme, const std::wstring& text) {
    return std::max(
        Metric(theme, L"majorNavItem", L"textMinWidth", 86.0f),
        std::min(
            Metric(theme, L"majorNavItem", L"textMaxWidth", 168.0f),
            Metric(theme, L"majorNavItem", L"textBaseWidth", 28.0f) + static_cast<float>(text.size()) * Metric(theme, L"majorNavItem", L"textCharWidth", 12.0f)));
}

float TabGroupItemWidth(const Theme& theme, const std::wstring& text, IDWriteTextFormat* format, float measuredTextWidth) {
    const float minWidth = Metric(theme, L"majorNavItem", L"minWidth", 72.0f);
    const float maxWidth = Metric(theme, L"majorNavItem", L"textMaxWidth", 168.0f);
    const float paddingX = Metric(theme, L"tabButton", L"paddingX", 12.0f);
    const float fallback = NavigationItemWidth(theme, text);
    const float desired = (measuredTextWidth > 0.0f && format)
        ? measuredTextWidth + paddingX * 2.0f
        : fallback;
    return std::max(minWidth, std::min(maxWidth, desired));
}

bool HasSiblingGroupName(const std::vector<Group>& groups, int parentGroup, const std::wstring& name) {
    const std::wstring normalized = ToLower(Trim(name));
    for (const auto& group : groups) {
        if (group.parentGroup == parentGroup && ToLower(Trim(group.name)) == normalized) {
            return true;
        }
    }
    return false;
}

std::wstring UniqueSiblingGroupName(const std::vector<Group>& groups, int parentGroup, const std::wstring& baseName) {
    if (!HasSiblingGroupName(groups, parentGroup, baseName)) {
        return baseName;
    }

    for (int index = 2; index < 10000; ++index) {
        std::wstring candidate = baseName + L" " + std::to_wstring(index);
        if (!HasSiblingGroupName(groups, parentGroup, candidate)) {
            return candidate;
        }
    }
    return baseName + L" " + std::to_wstring(static_cast<int>(groups.size()) + 1);
}

bool LooksLikeTodoLink(const Link& link) {
    const std::wstring text = ToLower(link.name + L" " + link.path + L" " + link.parameter + L" " + link.remark);
    return text.find(L"todo") != std::wstring::npos ||
           text.find(L"[ ]") != std::wstring::npos ||
           text.find(L"[x]") != std::wstring::npos ||
           text.find(L"待办") != std::wstring::npos ||
           text.find(L"任务") != std::wstring::npos;
}

bool IsAllTag(const Group& tag) {
    return tag.type == 1 || tag.name == L"全部" || ToLower(tag.content) == L"all";
}

bool IsTodoTag(const Group& tag) {
    const std::wstring content = ToLower(tag.content);
    return tag.type == 2 || tag.name == L"待办" || content == L"todo";
}

bool IsNoteTag(const Group& tag) {
    return tag.type == 3 || ToLower(tag.content) == L"note";
}

bool IsTodoItemsTag(const Group& tag) {
    return tag.type == 4 || ToLower(tag.content) == L"todoitems";
}

bool IsOrdinaryTag(const Group& tag) {
    return tag.parentGroup != 0 &&
           !IsAllTag(tag) &&
           !IsTodoTag(tag) &&
           !IsNoteTag(tag) &&
           !IsTodoItemsTag(tag);
}

std::wstring ClipboardPathTextForLink(const Link& link, bool isUrl) {
    std::wstring text = isUrl ? NormalizeUrl(link.path) : link.path;
    if (!isUrl) {
        std::replace(text.begin(), text.end(), L'\\', L'/');
    }
    return text;
}

std::wstring TodoScheduleText(TodoScheduleKind kind) {
    switch (kind) {
    case TodoScheduleKind::Once:
        return L"一次性";
    case TodoScheduleKind::Secondly:
        return L"每秒";
    case TodoScheduleKind::Minutely:
        return L"每分";
    case TodoScheduleKind::Hourly:
        return L"每时";
    case TodoScheduleKind::Daily:
        return L"每日";
    case TodoScheduleKind::Weekly:
        return L"每周";
    case TodoScheduleKind::Monthly:
        return L"每月";
    case TodoScheduleKind::Yearly:
        return L"每年";
    case TodoScheduleKind::Cron:
        return L"Cron";
    default:
        return L"无时间";
    }
}

std::vector<std::wstring> CronFields(const std::wstring& expression) {
    std::vector<std::wstring> fields;
    const std::wstring normalized = NormalizeTodoCronExpression(expression);
    std::size_t start = 0;
    while (start < normalized.size()) {
        while (start < normalized.size() && std::iswspace(normalized[start]) != 0) {
            ++start;
        }
        if (start >= normalized.size()) {
            break;
        }
        const std::size_t end = normalized.find(L' ', start);
        fields.push_back(normalized.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

std::optional<int> ParseCronNumberField(const std::vector<std::wstring>& fields, std::size_t index, int minValue, int maxValue) {
    if (index >= fields.size()) {
        return std::nullopt;
    }
    const auto value = ParseInt(fields[index]);
    if (!value || *value < minValue || *value > maxValue) {
        return std::nullopt;
    }
    return value;
}

bool CronFieldIsAny(const std::vector<std::wstring>& fields, std::size_t index) {
    return index < fields.size() && fields[index] == L"*";
}

bool CronFieldIsWorkday(const std::vector<std::wstring>& fields, std::size_t index) {
    return index < fields.size() && fields[index] == L"1-5";
}

std::wstring TwoDigit(int value) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%02d", value);
    return buffer;
}

std::wstring CronTimeText(const std::vector<std::wstring>& fields) {
    const auto hour = ParseCronNumberField(fields, 2, 0, 23);
    const auto minute = ParseCronNumberField(fields, 1, 0, 59);
    if (!hour || !minute) {
        return {};
    }
    return TwoDigit(*hour) + L":" + TwoDigit(*minute);
}

std::wstring CronWeekdayText(const std::wstring& field) {
    if (field == L"1-5") {
        return L"工作日";
    }
    if (field == L"*") {
        return L"每天";
    }

    static constexpr const wchar_t* kWeekdays[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    std::wstring result;
    std::size_t start = 0;
    while (start < field.size()) {
        const std::size_t end = field.find(L',', start);
        const std::wstring token = field.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        const auto value = ParseInt(token);
        if (value && *value >= 0 && *value <= 6) {
            if (!result.empty()) {
                result += L"、";
            }
            result += kWeekdays[*value];
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::wstring CronScheduleText(const TodoItem& item) {
    const auto fields = CronFields(item.cronExpression);
    if (fields.size() < 6) {
        return item.cronExpression.empty() ? L"Cron" : L"Cron " + item.cronExpression;
    }

    const std::wstring time = CronTimeText(fields);
    auto withTime = [&](std::wstring text) {
        if (!time.empty()) {
            text += L" " + time;
        }
        return text;
    };

    if (CronFieldIsAny(fields, 3) && CronFieldIsAny(fields, 4)) {
        if (CronFieldIsWorkday(fields, 5)) {
            return withTime(L"工作日");
        }
        if (CronFieldIsAny(fields, 5)) {
            return withTime(L"每日");
        }
        const std::wstring weekdays = CronWeekdayText(fields[5]);
        if (!weekdays.empty()) {
            return withTime(L"每周 " + weekdays);
        }
    }

    if (!CronFieldIsAny(fields, 3) && CronFieldIsAny(fields, 4) && CronFieldIsAny(fields, 5)) {
        if (const auto day = ParseCronNumberField(fields, 3, 1, 31)) {
            return withTime(L"每月 " + std::to_wstring(*day) + L" 号");
        }
    }

    return item.cronExpression.empty() ? L"Cron" : L"Cron " + item.cronExpression;
}

std::wstring TodoScheduleText(const TodoItem& item) {
    if (item.scheduleKind == TodoScheduleKind::Cron) {
        return CronScheduleText(item);
    }
    if (!IsRecurringTodoSchedule(item.scheduleKind) || item.repeatInterval <= 1) {
        return TodoScheduleText(item.scheduleKind);
    }
    std::wstring unit;
    switch (item.scheduleKind) {
    case TodoScheduleKind::Secondly:
        unit = L"秒";
        break;
    case TodoScheduleKind::Minutely:
        unit = L"分";
        break;
    case TodoScheduleKind::Hourly:
        unit = L"时";
        break;
    case TodoScheduleKind::Daily:
        unit = L"天";
        break;
    case TodoScheduleKind::Weekly:
        unit = L"周";
        break;
    case TodoScheduleKind::Monthly:
        unit = L"月";
        break;
    case TodoScheduleKind::Yearly:
        unit = L"年";
        break;
    default:
        unit = L"";
        break;
    }
    return L"每 " + std::to_wstring(item.repeatInterval) + L" " + unit;
}

bool IsTodoOverdue(const TodoItem& item) {
    return IsTodoOverdueAt(item, CurrentTodoTimestamp());
}

std::optional<std::int64_t> TodoRemainingSeconds(const TodoItem& item) {
    if (!item.enabled || !item.completedAt.empty() || item.nextDueAt.empty()) {
        return std::nullopt;
    }

    SYSTEMTIME due{};
    SYSTEMTIME now{};
    if (!TryParseTodoTimestamp(item.nextDueAt, due)) {
        return std::nullopt;
    }
    GetLocalTime(&now);

    FILETIME dueFile{};
    FILETIME nowFile{};
    if (!SystemTimeToFileTime(&due, &dueFile) || !SystemTimeToFileTime(&now, &nowFile)) {
        return std::nullopt;
    }

    ULARGE_INTEGER dueValue{};
    dueValue.LowPart = dueFile.dwLowDateTime;
    dueValue.HighPart = dueFile.dwHighDateTime;
    ULARGE_INTEGER nowValue{};
    nowValue.LowPart = nowFile.dwLowDateTime;
    nowValue.HighPart = nowFile.dwHighDateTime;

    if (dueValue.QuadPart <= nowValue.QuadPart) {
        return 0;
    }
    constexpr std::uint64_t kFileTimeTicksPerSecond = 10000000ull;
    const std::uint64_t diff = dueValue.QuadPart - nowValue.QuadPart;
    return static_cast<std::int64_t>((diff + kFileTimeTicksPerSecond - 1) / kFileTimeTicksPerSecond);
}

std::wstring TodoRemainingText(const TodoItem& item) {
    const auto seconds = TodoRemainingSeconds(item);
    if (!seconds) {
        return {};
    }
    if (*seconds <= 0) {
        return L"已逾期";
    }

    constexpr std::int64_t minute = 60;
    constexpr std::int64_t hour = 60 * minute;
    constexpr std::int64_t day = 24 * hour;
    if (*seconds >= day) {
        const std::int64_t days = (*seconds + day - 1) / day;
        return L"还有 " + std::to_wstring(days) + L" 天提醒";
    }
    if (*seconds >= 10 * minute) {
        const std::int64_t totalMinutes = (*seconds + minute - 1) / minute;
        const std::int64_t hours = totalMinutes / 60;
        const std::int64_t minutes = totalMinutes % 60;
        if (hours <= 0) {
            return L"还有 " + std::to_wstring(minutes) + L" 分钟提醒";
        }
        if (minutes <= 0) {
            return L"还有 " + std::to_wstring(hours) + L" 小时提醒";
        }
        return L"还有 " + std::to_wstring(hours) + L" 小时 " + std::to_wstring(minutes) + L" 分钟提醒";
    }
    return L"还有 " + std::to_wstring(*seconds) + L" 秒提醒";
}

std::wstring TodoReminderKey(const TodoItem& item) {
    return std::to_wstring(item.id) + L"|" + EffectiveTodoReminderDueAt(item);
}

std::wstring TodoReminderText(const TodoItem& item) {
    std::wstring text = item.title;
    if (!Trim(item.content).empty()) {
        text += L"\n" + Trim(item.content);
    }
    if (!item.nextDueAt.empty()) {
        text += L"\n" + TodoScheduleText(item) + L" " + item.nextDueAt;
        if (IsRecurringTodoSchedule(item.scheduleKind) && item.repeatLimit > 0) {
            text += L" (" + std::to_wstring(item.repeatFinished) + L"/" + std::to_wstring(item.repeatLimit) + L")";
        }
    }
    return text;
}

std::wstring LimitNotificationText(const std::wstring& value, std::size_t limit) {
    if (value.size() <= limit) {
        return value;
    }
    return value.substr(0, limit > 3 ? limit - 3 : limit) + L"...";
}

std::vector<std::wstring> LimitedTooltipContentLines(const std::wstring& value) {
    constexpr std::size_t kMaxLines = 3;
    constexpr std::size_t kMaxLineLength = 120;
    std::vector<std::wstring> lines;
    bool truncated = false;
    for (auto line : SplitTooltipLines(Trim(value))) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.size() > kMaxLineLength) {
            line = line.substr(0, kMaxLineLength - 3) + L"...";
            truncated = true;
        }
        if (lines.size() >= kMaxLines) {
            truncated = true;
            break;
        }
        lines.push_back(std::move(line));
    }
    if (truncated && !lines.empty() && lines.back().size() >= 3 && lines.back().substr(lines.back().size() - 3) != L"...") {
        lines.back() += L"...";
    }
    return lines;
}

int SortDirectionMenuIcon(int sort, int sortDirection) {
    if (sort == 0) {
        return MenuIconSort;
    }
    return sortDirection == 0 ? MenuIconSortAsc : MenuIconSortDesc;
}

struct ThemeMenuItem {
    std::wstring name;
    std::wstring label;
};

std::vector<PluginRecord> BuiltinToolMenuPlugins(const std::vector<PluginRecord>& loaded) {
    std::vector<PluginRecord> tools;
    const auto builtins = PluginRegistry::BuiltinPlugins();
    for (const auto& builtin : builtins) {
        if (builtin.kind != L"builtin-tool" || builtin.engine.empty()) {
            continue;
        }
        PluginRecord item = builtin;
        for (const auto& plugin : loaded) {
            if (plugin.kind == L"builtin-tool" && (plugin.id == builtin.id || plugin.engine == builtin.engine)) {
                item = plugin;
                if (item.id.empty()) item.id = builtin.id;
                if (item.name.empty()) item.name = builtin.name;
                if (item.engine.empty()) item.engine = builtin.engine;
                if (item.kind.empty()) item.kind = builtin.kind;
                break;
            }
        }
        tools.push_back(std::move(item));
    }

    for (const auto& plugin : loaded) {
        if (plugin.kind != L"builtin-tool" || plugin.engine.empty()) {
            continue;
        }
        const auto exists = std::find_if(tools.begin(), tools.end(), [&](const PluginRecord& item) {
            return item.id == plugin.id || item.engine == plugin.engine;
        });
        if (exists == tools.end()) {
            tools.push_back(plugin);
        }
    }
    return tools;
}

std::wstring XmlAttributeValue(const std::wstring& tag, const std::wstring& name) {
    const std::wstring pattern = name + L"=\"";
    std::size_t begin = tag.find(pattern);
    if (begin == std::wstring::npos) {
        return {};
    }
    begin += pattern.size();
    const std::size_t end = tag.find(L'"', begin);
    if (end == std::wstring::npos) {
        return {};
    }
    return tag.substr(begin, end - begin);
}

std::wstring ReadThemeDisplayName(const std::filesystem::path& path, const std::wstring& fallback) {
    const std::wstring xml = LoadUtf8File(path);
    const std::size_t begin = xml.find(L"<Theme");
    if (begin == std::wstring::npos) {
        return fallback;
    }
    const std::size_t end = xml.find(L">", begin);
    if (end == std::wstring::npos) {
        return fallback;
    }
    const std::wstring tag = xml.substr(begin, end - begin + 1);
    std::wstring label = XmlAttributeValue(tag, L"displayName");
    return label.empty() ? fallback : label;
}

std::vector<ThemeMenuItem> DiscoverThemeMenuItems(const std::filesystem::path& themeDirectory, const std::wstring& currentTheme) {
    std::vector<ThemeMenuItem> items;
    std::error_code ec;
    if (std::filesystem::exists(themeDirectory, ec) && std::filesystem::is_directory(themeDirectory, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(themeDirectory, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec) || entry.path().extension() != L".xml") {
                continue;
            }
            const std::wstring name = entry.path().stem().wstring();
            if (name.empty()) {
                continue;
            }
            items.push_back(ThemeMenuItem{name, ReadThemeDisplayName(entry.path(), name)});
        }
    }

    auto hasTheme = [&](const std::wstring& name) {
        return std::any_of(items.begin(), items.end(), [&](const ThemeMenuItem& item) {
            return item.name == name;
        });
    };
    if (!hasTheme(L"default")) {
        items.push_back(ThemeMenuItem{L"default", L"default"});
    }
    if (!currentTheme.empty() && currentTheme != L"default" && !hasTheme(currentTheme)) {
        const std::filesystem::path path = themeDirectory / (currentTheme + L".xml");
        if (FileExists(path)) {
            items.push_back(ThemeMenuItem{currentTheme, ReadThemeDisplayName(path, currentTheme)});
        }
    }

    std::sort(items.begin(), items.end(), [](const ThemeMenuItem& left, const ThemeMenuItem& right) {
        if (left.name == L"default" || right.name == L"default") {
            return left.name == L"default" && right.name != L"default";
        }
        return left.label < right.label;
    });
    items.erase(std::unique(items.begin(), items.end(), [](const ThemeMenuItem& left, const ThemeMenuItem& right) {
        return left.name == right.name;
    }), items.end());
    return items;
}

std::wstring EffectiveThemeName(const std::filesystem::path& themeDirectory, const std::wstring& themeName) {
    if (!themeName.empty() && FileExists(themeDirectory / (themeName + L".xml"))) {
        return themeName;
    }
    return L"default";
}

std::wstring MenuTextFromRaw(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&' && i + 1 < text.size() && text[i + 1] != L'&') {
            continue;
        }
        result.push_back(text[i]);
    }
    return result;
}

bool DrawStockMenuIcon(HDC dc, const RECT& rect, SHSTOCKICONID id) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_SMALLICON, &info)) || !info.hIcon) {
        return false;
    }
    const int size = std::min(rect.right - rect.left, rect.bottom - rect.top);
    DrawIconEx(dc, rect.left + ((rect.right - rect.left) - size) / 2, rect.top + ((rect.bottom - rect.top) - size) / 2,
               info.hIcon, size, size, 0, nullptr, DI_NORMAL);
    DestroyIcon(info.hIcon);
    return true;
}

bool DrawSystemImageListIcon(HDC dc, const RECT& rect, int imageIndex, bool disabled) {
    if (imageIndex < 0) {
        return false;
    }
    SHFILEINFOW info{};
    HIMAGELIST imageList = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
        L"C:\\",
        FILE_ATTRIBUTE_DIRECTORY,
        &info,
        sizeof(info),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    if (!imageList) {
        return false;
    }

    const int size = std::min(rect.right - rect.left, rect.bottom - rect.top);
    const int x = rect.left + ((rect.right - rect.left) - size) / 2;
    const int y = rect.top + ((rect.bottom - rect.top) - size) / 2;
    return ImageList_Draw(imageList, imageIndex, dc, x, y, disabled ? ILD_BLEND50 : ILD_NORMAL) != FALSE;
}

struct MenuIconPalette {
    COLORREF accent = RGB(0, 153, 215);
    COLORREF danger = RGB(228, 48, 58);
    COLORREF warning = RGB(245, 180, 40);
    COLORREF success = RGB(24, 150, 92);
    COLORREF muted = RGB(100, 116, 139);
    COLORREF disabled = RGB(160, 168, 178);
    COLORREF neutral = RGB(255, 255, 255);
};

bool DrawLocalMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, COLORREF color, const MenuIconPalette& palette, const std::filesystem::path& appDirectory) {
    const COLORREF accent = disabled ? palette.disabled :
        (icon == MenuIconDelete || icon == MenuIconClear || icon == MenuIconExit || icon == MenuIconPower ? palette.danger : palette.accent);
    return DrawTablerIcon(
        dc,
        rc,
        appDirectory,
        MenuIconTablerId(static_cast<MenuIcon>(icon)),
        color == CLR_INVALID ? accent : color);
}

bool DrawMenuChevronRight(HDC dc, const RECT& rc, COLORREF color, const std::filesystem::path& appDirectory) {
    return DrawTablerIcon(dc, rc, appDirectory, TablerIconId::ChevronRight, color);
}

void DrawFallbackMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, COLORREF color, const MenuIconPalette& palette) {
    const COLORREF blue = disabled ? palette.disabled : (color == CLR_INVALID ? palette.accent : color);
    const COLORREF red = disabled ? palette.disabled : (color == CLR_INVALID ? palette.danger : color);
    const COLORREF mutedColor = disabled ? palette.disabled : palette.muted;
    const COLORREF amber = disabled ? palette.disabled : palette.warning;
    const COLORREF green = disabled ? palette.disabled : palette.success;
    const int l = rc.left + 1;
    const int t = rc.top + 1;
    const int r = rc.right - 1;
    const int b = rc.bottom - 1;
    HPEN pen = CreatePen(PS_SOLID, 2, icon == MenuIconDelete || icon == MenuIconClear ? red : blue);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HBRUSH brush = CreateSolidBrush(icon == MenuIconFolder ? amber : palette.neutral);
    HGDIOBJ oldBrush = SelectObject(dc, brush);

    switch (icon) {
    case MenuIconRefresh:
        Arc(dc, l, t, r, b, r - 2, t + 4, l + 3, t + 4);
        MoveToEx(dc, r - 3, t + 3, nullptr);
        LineTo(dc, r - 1, t + 8);
        LineTo(dc, r - 7, t + 7);
        break;
    case MenuIconMove:
        MoveToEx(dc, l + 2, (t + b) / 2, nullptr);
        LineTo(dc, r - 3, (t + b) / 2);
        MoveToEx(dc, r - 7, t + 4, nullptr);
        LineTo(dc, r - 3, (t + b) / 2);
        LineTo(dc, r - 7, b - 4);
        break;
    case MenuIconCopy:
        Rectangle(dc, l + 4, t + 2, r - 1, b - 4);
        Rectangle(dc, l + 1, t + 5, r - 4, b - 1);
        break;
    case MenuIconCut:
        Ellipse(dc, l + 1, t + 2, l + 6, t + 7);
        Ellipse(dc, l + 1, b - 7, l + 6, b - 2);
        MoveToEx(dc, l + 6, t + 7, nullptr);
        LineTo(dc, r - 2, b - 3);
        MoveToEx(dc, l + 6, b - 7, nullptr);
        LineTo(dc, r - 2, t + 3);
        break;
    case MenuIconPaste:
        RoundRect(dc, l + 3, t + 4, r - 2, b - 1, 3, 3);
        Rectangle(dc, l + 6, t + 1, r - 5, t + 6);
        MoveToEx(dc, l + 6, t + 10, nullptr);
        LineTo(dc, r - 5, t + 10);
        MoveToEx(dc, l + 6, t + 13, nullptr);
        LineTo(dc, r - 7, t + 13);
        break;
    case MenuIconClear:
    case MenuIconDelete:
        MoveToEx(dc, l + 2, t + 2, nullptr);
        LineTo(dc, r - 2, b - 2);
        MoveToEx(dc, r - 2, t + 2, nullptr);
        LineTo(dc, l + 2, b - 2);
        break;
    case MenuIconSort:
        MoveToEx(dc, l + 2, t + 3, nullptr);
        LineTo(dc, r - 2, t + 3);
        MoveToEx(dc, l + 2, (t + b) / 2, nullptr);
        LineTo(dc, r - 6, (t + b) / 2);
        MoveToEx(dc, l + 2, b - 3, nullptr);
        LineTo(dc, r - 10, b - 3);
        break;
    case MenuIconSize:
        Rectangle(dc, l + 2, t + 2, l + 7, t + 7);
        Rectangle(dc, r - 8, t + 2, r - 2, t + 8);
        Rectangle(dc, l + 2, b - 8, l + 9, b - 2);
        Rectangle(dc, r - 9, b - 9, r - 2, b - 2);
        break;
    case MenuIconList:
        for (int i = 0; i < 3; ++i) {
            const int y = t + 3 + i * 5;
            Rectangle(dc, l + 2, y, l + 5, y + 3);
            MoveToEx(dc, l + 8, y + 1, nullptr);
            LineTo(dc, r - 2, y + 1);
        }
        break;
    case MenuIconTile:
        Rectangle(dc, l + 2, t + 2, l + 7, t + 7);
        Rectangle(dc, r - 7, t + 2, r - 2, t + 7);
        Rectangle(dc, l + 2, b - 7, l + 7, b - 2);
        Rectangle(dc, r - 7, b - 7, r - 2, b - 2);
        break;
    case MenuIconGroup:
        {
            HBRUSH fillBrush = CreateSolidBrush(amber);
            HGDIOBJ previousBrush = SelectObject(dc, fillBrush);
            RoundRect(dc, l + 1, t + 4, r - 1, b - 1, 4, 4);
            SelectObject(dc, previousBrush);
            DeleteObject(fillBrush);
        }
        MoveToEx(dc, l + 2, t + 4, nullptr);
        LineTo(dc, l + 6, t + 1);
        LineTo(dc, l + 10, t + 4);
        break;
    case MenuIconTag:
        MoveToEx(dc, l + 3, t + 2, nullptr);
        LineTo(dc, r - 2, t + 2);
        LineTo(dc, r - 2, b - 6);
        LineTo(dc, (l + r) / 2, b - 2);
        LineTo(dc, l + 3, b - 6);
        LineTo(dc, l + 3, t + 2);
        break;
    case MenuIconTheme:
        {
            HBRUSH fillBrush = CreateSolidBrush(disabled ? palette.disabled : palette.neutral);
            HGDIOBJ previousBrush = SelectObject(dc, fillBrush);
            Ellipse(dc, l + 1, t + 1, r - 1, b - 1);
            SelectObject(dc, previousBrush);
            DeleteObject(fillBrush);
        }
        {
            HBRUSH greenBrush = CreateSolidBrush(green);
            HGDIOBJ previousBrush = SelectObject(dc, greenBrush);
            Ellipse(dc, l + 4, t + 4, l + 8, t + 8);
            Ellipse(dc, r - 8, t + 5, r - 4, t + 9);
            Ellipse(dc, l + 7, b - 8, l + 11, b - 4);
            SelectObject(dc, previousBrush);
            DeleteObject(greenBrush);
        }
        break;
    case MenuIconEyeOff:
        Arc(dc, l + 1, t + 3, r - 1, b - 3, l + 3, (t + b) / 2, r - 3, (t + b) / 2);
        Arc(dc, l + 1, t + 3, r - 1, b - 3, r - 3, (t + b) / 2, l + 3, (t + b) / 2);
        MoveToEx(dc, l + 2, b - 2, nullptr);
        LineTo(dc, r - 2, t + 2);
        break;
    case MenuIconView:
        Rectangle(dc, l + 1, t + 3, r - 1, b - 3);
        MoveToEx(dc, l + 4, (t + b) / 2, nullptr);
        LineTo(dc, r - 4, (t + b) / 2);
        break;
    case MenuIconReward:
        {
            HBRUSH fillBrush = CreateSolidBrush(disabled ? palette.disabled : palette.warning);
            HGDIOBJ previousBrush = SelectObject(dc, fillBrush);
            Ellipse(dc, l + 2, t + 2, r - 2, b - 2);
            SelectObject(dc, previousBrush);
            DeleteObject(fillBrush);
        }
        MoveToEx(dc, (l + r) / 2, t + 5, nullptr);
        LineTo(dc, (l + r) / 2, b - 5);
        MoveToEx(dc, l + 6, t + 8, nullptr);
        LineTo(dc, r - 6, t + 8);
        MoveToEx(dc, l + 6, b - 8, nullptr);
        LineTo(dc, r - 6, b - 8);
        break;
    default:
        HPEN mutedPen = CreatePen(PS_SOLID, 2, mutedColor);
        SelectObject(dc, mutedPen);
        Rectangle(dc, l + 2, t + 2, r - 2, b - 2);
        SelectObject(dc, pen);
        DeleteObject(mutedPen);
        break;
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, COLORREF color, const MenuIconPalette& palette, const std::filesystem::path& appDirectory) {
    if (icon == MenuIconNone) {
        return;
    }
    bool drawn = false;
    switch (icon) {
    case MenuIconFile: drawn = DrawStockMenuIcon(dc, rc, SIID_DOCNOASSOC); break;
    case MenuIconFolder: drawn = DrawStockMenuIcon(dc, rc, SIID_FOLDER); break;
    case MenuIconUrl: drawn = DrawStockMenuIcon(dc, rc, SIID_WORLD); break;
    case MenuIconRun: drawn = DrawStockMenuIcon(dc, rc, SIID_APPLICATION); break;
    case MenuIconShield: drawn = DrawStockMenuIcon(dc, rc, SIID_SHIELD); break;
    case MenuIconOpenFolder: drawn = DrawStockMenuIcon(dc, rc, SIID_FOLDEROPEN); break;
    case MenuIconShortcut: drawn = DrawStockMenuIcon(dc, rc, SIID_LINK); break;
    case MenuIconEdit: drawn = DrawStockMenuIcon(dc, rc, SIID_RENAME); break;
    case MenuIconGroup: drawn = DrawStockMenuIcon(dc, rc, SIID_FOLDER); break;
    case MenuIconInfo: drawn = DrawStockMenuIcon(dc, rc, SIID_INFO); break;
    case MenuIconDelete: drawn = DrawStockMenuIcon(dc, rc, SIID_DELETE); break;
    default:
        break;
    }
    if (!drawn) {
        drawn = DrawLocalMenuIcon(dc, rc, icon, disabled, color, palette, appDirectory);
    }
    if (!drawn) {
        DrawFallbackMenuIcon(dc, rc, icon, disabled, color, palette);
    }
}

HBITMAP CreateTrackedMenuIconBitmap(const ShellContextMenuItem& source) {
    if (source.iconWidth <= 0 || source.iconHeight <= 0 ||
        source.iconWidth > 64 || source.iconHeight > 64 ||
        source.iconPixels.size() != static_cast<std::size_t>(source.iconWidth * source.iconHeight)) {
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = source.iconWidth;
    info.bmiHeader.biHeight = -source.iconHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }
    std::memcpy(
        pixels,
        source.iconPixels.data(),
        source.iconPixels.size() * sizeof(std::uint32_t));
    return bitmap;
}

bool DrawTrackedMenuIcon(HDC dc, const RECT& target, HBITMAP bitmap, bool disabled) {
    if (!dc || !bitmap) {
        return false;
    }
    BITMAP source{};
    if (GetObjectW(bitmap, sizeof(source), &source) != sizeof(source) ||
        source.bmWidth <= 0 || source.bmHeight == 0) {
        return false;
    }
    HDC memoryDc = CreateCompatibleDC(dc);
    if (!memoryDc) {
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = disabled ? 120 : 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL drawn = AlphaBlend(
        dc,
        target.left,
        target.top,
        target.right - target.left,
        target.bottom - target.top,
        memoryDc,
        0,
        0,
        source.bmWidth,
        std::abs(source.bmHeight),
        blend);
    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);
    return drawn != FALSE;
}

bool LooksLikeUrlText(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

bool IsVolumeMenuIcon(MenuIcon icon) {
    return icon == MenuIconVolumeUp ||
           icon == MenuIconVolumeDown ||
           icon == MenuIconVolumeMute;
}

std::optional<std::wstring> CurrentMasterVolumeText() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* endpoint = nullptr;
    std::optional<std::wstring> result;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&endpoint)))) {
        float level = 0.0f;
        BOOL muted = FALSE;
        if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&level)) &&
            SUCCEEDED(endpoint->GetMute(&muted))) {
            const int percent = static_cast<int>(ClampFloat(level, 0.0f, 1.0f) * 100.0f + 0.5f);
            result = L"当前音量: " + std::to_wstring(percent) + L"%" + (muted ? L"（已静音）" : L"");
        }
    }

    SafeRelease(endpoint);
    SafeRelease(device);
    SafeRelease(enumerator);
    return result;
}

std::wstring SafeFileName(std::wstring name) {
    name = Trim(name);
    if (name.empty()) {
        name = kAppDisplayName;
    }
    for (wchar_t& ch : name) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    return name;
}

std::filesystem::path DesktopDirectory() {
    PWSTR desktop = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) {
        result = desktop;
        CoTaskMemFree(desktop);
    }
    return result;
}

bool CreateUrlShortcutFile(const Link& link, const std::filesystem::path& path) {
    const std::wstring url = NormalizeUrl(link.path);
    return WritePrivateProfileStringW(L"InternetShortcut", L"URL", url.c_str(), path.c_str()) != FALSE;
}

bool CreateShellShortcutFile(const Link& link, const std::filesystem::path& path) {
    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) || !shellLink) {
        return false;
    }

    bool ok = false;
    if (!link.pidl.empty() && ShellItemService::IsPidlBlobPlausible(link.pidl)) {
        shellLink->SetIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(link.pidl.data()));
    } else {
        shellLink->SetPath(ExpandEnvironmentStringsSafe(link.path).c_str());
    }
    if (!Trim(link.parameter).empty()) {
        shellLink->SetArguments(link.parameter.c_str());
    }
    if (!Trim(link.workDir).empty()) {
        shellLink->SetWorkingDirectory(ExpandEnvironmentStringsSafe(link.workDir).c_str());
    }
    if (!Trim(link.icon).empty() && link.icon != L"#url") {
        shellLink->SetIconLocation(ExpandEnvironmentStringsSafe(link.icon).c_str(), 0);
    }
    shellLink->SetShowCmd(link.showCmd > 0 ? link.showCmd : SW_SHOWNORMAL);
    if (!Trim(link.remark).empty()) {
        shellLink->SetDescription(link.remark.c_str());
    }

    IPersistFile* persist = nullptr;
    if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&persist))) && persist) {
        ok = SUCCEEDED(persist->Save(path.c_str(), TRUE));
        persist->Release();
    }
    shellLink->Release();
    return ok;
}
}

class OleDropTarget final : public IDropTarget {
public:
    explicit OleDropTarget(MainWindow* window) : window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        if (window_) {
            window_->SetDragOver(true);
        }
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (window_) {
            window_->SetDragOver(true);
        }
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (window_) {
            window_->SetDragOver(false);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD, POINTL, DWORD* effect) override {
        if (window_) {
            window_->SetDragOver(false);
        }
        const bool imported = window_ && window_->ImportDropData(dataObject);
        if (effect) {
            *effect = imported ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return imported ? S_OK : DV_E_FORMATETC;
    }

private:
    LONG refCount_ = 1;
    MainWindow* window_ = nullptr;
};

#define MessageBoxW(owner, message, title, flags) ShowThemedMessageBox((owner), instance_, theme_, (message), (title), (flags))

MainWindow::MainWindow(
    HINSTANCE instance,
    std::filesystem::path appDirectory,
    std::filesystem::path httpRootBaseDirectory,
    ConfigService& configService,
    StorageService& storageService,
    AppConfig config,
    AppModel model,
    Theme theme)
    : instance_(instance),
      appDirectory_(std::move(appDirectory)),
      httpRootBaseDirectory_(std::move(httpRootBaseDirectory)),
      configService_(configService),
      storageService_(storageService),
      pluginRegistry_(appDirectory_),
      shellContextMenuCache_(),
      config_(std::move(config)),
      model_(std::move(model)),
      theme_(std::move(theme)),
      launcher_(appDirectory_, &config_),
      iconService_(appDirectory_),
      urlIconDownloadService_(appDirectory_),
      runningAsAdmin_(IsRunningAsAdmin()) {
    RestoreLegacyBuiltinSystemFunctionKeys();
    if (config_.preferAdminRun != runningAsAdmin_) {
        config_.preferAdminRun = runningAsAdmin_;
        configService_.Save(config_);
    }
    SelectInitialItems();
}

MainWindow::~MainWindow() {
    httpServerService_.Stop();
    SaveCurrentNotePage();
    if (dockPeek_) {
        DestroyWindow(dockPeek_);
        dockPeek_ = nullptr;
    }
    if (noteEdit_) {
        DestroyWindow(noteEdit_);
        noteEdit_ = nullptr;
    }
    embeddedUi_.reset();
    DiscardDeviceResources();
    SafeRelease(titleFormat_);
    SafeRelease(textFormat_);
    SafeRelease(navSelectedFormat_);
    SafeRelease(smallFormat_);
    SafeRelease(uiWicFactory_);
    SafeRelease(dwriteFactory_);
    SafeRelease(d2dFactory_);
}

bool MainWindow::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = MainWindow::WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wc.lpszClassName = L"QuattroMainWindow";
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    WriteStartupTiming(L"main window class registered");
    WriteStartupTiming(L"builtin tool catalog ready", L"mode=memory_only");

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) {
        return false;
    }
    WriteStartupTiming(L"d2d factory created");
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwriteFactory_)))) {
        return false;
    }
    WriteStartupTiming(L"dwrite factory created");
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uiWicFactory_)))) {
        return false;
    }
    WriteStartupTiming(L"wic factory created");

    dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-cn", &titleFormat_);
    dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn", &textFormat_);
    dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn", &navSelectedFormat_);
    dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-cn", &smallFormat_);
    if (!titleFormat_ || !textFormat_ || !smallFormat_) {
        return false;
    }
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    smallFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (navSelectedFormat_) {
        navSelectedFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        navSelectedFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    WriteStartupTiming(L"text formats created");

    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const POINT position = ClampWindowPosition(config_.posX, config_.posY, config_.width, config_.height);
    WriteStartupTiming(L"native main window create begin");
    const std::wstring windowTitle = AppWindowTitle();
    hwnd_ = CreateWindowExW(
        MainWindowExStyle(config_.alpha),
        wc.lpszClassName,
        windowTitle.c_str(),
        style,
        position.x,
        position.y,
        config_.width,
        config_.height,
        nullptr,
        nullptr,
        instance_,
        this);
    if (!hwnd_) {
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    if (!dpi_) dpi_ = USER_DEFAULT_SCREEN_DPI;
    WriteStartupTiming(
        L"native main window created",
        L"size=" + std::to_wstring(config_.width) + L"x" + std::to_wstring(config_.height));

    WriteStartupTiming(L"main window alpha apply begin", L"alpha=" + std::to_wstring(config_.alpha));
    ApplyMainWindowAlpha(hwnd_, config_.alpha);
    WriteStartupTiming(L"main window alpha apply end");
    if (config_.topMost) {
        if (!config_.hideOnStart) {
            startupTopMostPending_ = true;
            WriteStartupTiming(L"main window topmost deferred");
        } else {
            SetWindowPos(hwnd_,
                         MainWindowTopMostInsertAfter(config_.topMost),
                         0,
                         0,
                         0,
                         0,
                         SWP_NOMOVE | SWP_NOSIZE);
            WriteStartupTiming(L"main window topmost applied", L"hide_on_start=1");
        }
    } else {
        WriteStartupTiming(L"main window topmost skipped", L"enabled=0");
    }
    WriteStartupTiming(L"tooltip window deferred");

    if (!config_.hideOnStart) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
        WriteStartupTiming(L"main window shown");
        UpdateWindow(hwnd_);
        WriteStartupTiming(L"main window update requested");
    } else {
        WriteStartupTiming(L"main window show skipped", L"hide_on_start=1");
    }
    if (config_.hideOnStart) {
        oleDropTarget_ = new OleDropTarget(this);
        if (FAILED(RegisterDragDrop(hwnd_, oleDropTarget_))) {
            oleDropTarget_->Release();
            oleDropTarget_ = nullptr;
        }
        DragAcceptFiles(hwnd_, TRUE);
        WriteStartupTiming(L"drag drop initialized", L"ole_drop=" + std::wstring(oleDropTarget_ ? L"1" : L"0"));
    } else {
        WriteStartupTiming(L"drag drop deferred");
    }
    RegisterConfiguredHotKeys();
    WriteStartupTiming(L"hotkeys registered", L"main_hotkey=" + std::wstring(mainHotKeyRegistered_ ? L"1" : L"0"));
    if (config_.autoDock || config_.hideWhenInactive) {
        dockTimerId_ = SetTimer(hwnd_, ID_TIMER_DOCK, 250, nullptr);
    }
    WriteStartupTiming(L"dock timer configured", L"enabled=" + std::wstring(dockTimerId_ != 0 ? L"1" : L"0"));

    if (!config_.hideOnStart) {
        PostMessageW(hwnd_, WM_QUATTRO_STARTUP_DEFERRED, 0, 0);
        startupDeferredPosted_ = true;
        WriteStartupTiming(L"startup deferred services posted");
    } else {
        InitializeTrayIcon();
        WriteStartupTiming(L"tray icon initialized", L"visible=" + std::wstring(trayIconVisible_ ? L"1" : L"0"));
        if (config_.httpServerAutoStart) {
            StartHttpServer(false);
            WriteStartupTiming(L"http server startup checked", L"auto_start=1");
        } else {
            WriteStartupTiming(L"http server startup skipped", L"auto_start=0");
        }
        reminderScanTimerId_ = SetTimer(hwnd_, ID_TIMER_REMINDER_SCAN, 1000, nullptr);
        CheckTodoReminders();
        WriteStartupTiming(L"todo reminder scan completed");
    }
    windowStateSaveEnabled_ = true;
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (ThemedUi::PreTranslateMessage(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_QUATTRO_WAKEUP:
        WakeUp();
        return 0;
    case WM_QUATTRO_DOCK_PEEK_ACTIVATE:
        if (dockHidden_) {
            DockRestore();
        }
        return 0;
    case WM_QUATTRO_TEST_DOCK_HIDE:
        return QuattroTestMode() && DockHide(false) ? TRUE : FALSE;
    case WM_QUATTRO_TEST_DOCK_HIDDEN:
        return QuattroTestMode() && dockHidden_ ? TRUE : FALSE;
    case WM_QUATTRO_TEST_TODO_MENU:
        if (QuattroTestMode()) {
            RECT rect{};
            GetWindowRect(hwnd_, &rect);
            ShowTodoMenu(static_cast<int>(wParam), POINT{rect.left + 80, rect.top + 120});
            return TRUE;
        }
        return FALSE;
    case WM_QUATTRO_TEST_REMINDER_STATE:
        if (!QuattroTestMode()) {
            return 0;
        }
        if (wParam == 0) return lastReminderChannel_;
        if (wParam == 1) return lastReminderBatchCount_;
        if (wParam == 2) return static_cast<LRESULT>(pendingReminderTodoIds_.size());
        if (wParam == 3) return testReminderFailureConsumed_ ? TRUE : FALSE;
        return 0;
    case WM_QUATTRO_TEST_REMINDER_ACTION:
        if (!QuattroTestMode()) {
            return FALSE;
        }
        if (wParam == 1) MarkTodoReminderViewed(static_cast<int>(lParam));
        else if (wParam == 2) IgnoreTodoReminder(static_cast<int>(lParam));
        else if (wParam == 3) SnoozeTodoReminder(static_cast<int>(lParam), 5);
        else if (wParam == 4) SnoozeTodoReminder(static_cast<int>(lParam), 30);
        else if (wParam == 5) SnoozeTodoReminder(static_cast<int>(lParam), 60);
        else return FALSE;
        return TRUE;
    case WM_QUATTRO_TEST_COMPLETE_OVERDUE:
        if (!QuattroTestMode()) return FALSE;
        CompleteOverdueTodosInTag(static_cast<int>(wParam), false);
        return TRUE;
    case WM_QUATTRO_TEST_TODO_TAG_MENU:
        if (QuattroTestMode()) {
            RECT rect{};
            GetWindowRect(hwnd_, &rect);
            ShowTagMenu(static_cast<int>(wParam), POINT{rect.left + 80, rect.top + 120});
            return TRUE;
        }
        return FALSE;
    case WM_QUATTRO_TEST_SYSTEM_REMINDER:
        if (QuattroTestMode()) {
            std::vector<TodoItem*> items;
            const std::wstring now = CurrentTodoTimestamp();
            for (auto& item : model_.todos) {
                if (IsTodoReminderDue(item, now)) items.push_back(&item);
            }
            lastReminderChannel_ = 2;
            lastReminderBatchCount_ = static_cast<int>(items.size());
            return ShowTodoSystemNotification(items) ? TRUE : FALSE;
        }
        return FALSE;
    case WM_QUATTRO_EXIT_INSTANCE:
        WriteAppLog(L"收到同路径实例退出通知，销毁主窗口。");
        DestroyWindow(hwnd_);
        return 0;
    case WM_QUATTRO_STARTUP_ACTIVATE:
        ActivateWindow(hwnd_);
        return 0;
    case WM_QUATTRO_STARTUP_DEFERRED:
        if (startupDeferredPosted_) {
            startupDeferredPosted_ = false;
            if (startupTopMostPending_) {
                startupTopMostPending_ = false;
                SetWindowPos(hwnd_,
                             MainWindowTopMostInsertAfter(true),
                             0,
                             0,
                             0,
                             0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                WriteAppLog(L"启动延迟置顶已应用。");
            }
            if (!oleDropTarget_) {
                oleDropTarget_ = new OleDropTarget(this);
                if (FAILED(RegisterDragDrop(hwnd_, oleDropTarget_))) {
                    oleDropTarget_->Release();
                    oleDropTarget_ = nullptr;
                }
                DragAcceptFiles(hwnd_, TRUE);
            }
            InitializeTrayIcon();
            if (config_.httpServerAutoStart) {
                StartHttpServer(false);
            }
            reminderScanTimerId_ = SetTimer(hwnd_, ID_TIMER_REMINDER_SCAN, 1000, nullptr);
            CheckTodoReminders();
        }
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCPAINT:
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_QUATTRO_TRAY:
        if (LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
            ViewPendingTodoReminders();
            return 0;
        }
        if (LOWORD(lParam) == WM_LBUTTONUP) {
            WakeUp();
            return 0;
        }
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT point{};
            GetCursorPos(&point);
            ShowTrayMenu(point);
            return 0;
        }
        return 0;
    case WM_QUATTRO_URL_ICON_DOWNLOADED:
        OnUrlIconDownloaded(static_cast<int>(wParam), lParam != 0);
        return 0;
    case WM_NCHITTEST: {
        // The auto-hidden main window intentionally leaves a narrow strip on
        // screen. Normally the separate dock-peek window receives the hover,
        // but z-order changes (for example after activating from the tray) can
        // briefly leave the main window's resize frame above that window. In
        // that case treating the strip as HTLEFT/HTRIGHT only shows a resize
        // cursor and the window can never restore. Make the main strip an
        // equivalent restore target so either window reliably reveals it.
        if (dockHidden_) {
            SendMessageW(hwnd_, WM_QUATTRO_DOCK_PEEK_ACTIVATE, 0, 0);
            return HTCLIENT;
        }

        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int frame = std::max(6, GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER));
        const bool left = clientPoint.x >= 0 && clientPoint.x < frame;
        const bool right = clientPoint.x <= width && clientPoint.x >= width - frame;
        const bool top = clientPoint.y >= 0 && clientPoint.y < frame;
        const bool bottom = clientPoint.y <= height && clientPoint.y >= height - frame;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;

        const int titleHeight = config_.showTitle
            ? ClientDipToPx(Metric(theme_, L"title", L"height", 32.0f))
            : 0;
        const int titleButtonsWidth = ClientDipToPx(TitleButtonsReserveWidth());
        if (clientPoint.y >= 0 && clientPoint.y < titleHeight && clientPoint.x < width - titleButtonsWidth) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_HOTKEY:
        if (wParam == ID_HOTKEY_MAIN) {
            ToggleMainWindowFromHotKey();
            return 0;
        }
        if (wParam == ID_HOTKEY_PROCESS_LOCATOR) {
            OpenBuiltinToolEngine(L"process-locator", true);
            return 0;
        }
        if (wParam == ID_HOTKEY_COPY_SELECTED_PATHS) {
            CopySelectedPathsFromForeground(GetForegroundWindow());
            return 0;
        }
        if (wParam >= ID_HOTKEY_LINK_BASE) {
            const int linkId = LinkIdFromHotKeyId(static_cast<int>(wParam));
            if (linkId > 0) {
                RunLink(linkId);
            }
            return 0;
        }
        return 0;
    case WM_QUATTRO_DOUBLE_ALT_HOTKEY:
        if (IsDoubleAltMainHotKey(config_.mainHotKey)) {
            ToggleMainWindowFromHotKey();
        }
        return 0;
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_DISPLAYCHANGE:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED:
        ApplyDpiChange(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        return 0;
    case WM_SETTINGCHANGE:
    case WM_FONTCHANGE:
        if (menuFont_) {
            menuFont_->Reset();
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_ACTIVATEAPP:
        if (!wParam) {
            CancelNavDrag();
            CancelLinkDrag();
        }
        if (!wParam && config_.hideWhenInactive && IsWindowVisible(hwnd_) && !DockAutoHidePaused()) {
            HideMainWindow();
        } else if (wParam && dockHidden_) {
            WakeUp();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && dockHidden_) {
            WakeUp();
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_SYSCOMMAND: {
        const UINT command = static_cast<UINT>(wParam) & 0xFFF0u;
        if (dockHidden_ && (command == SC_MINIMIZE || command == SC_RESTORE)) {
            WakeUp();
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
    case WM_TIMER:
        if (embeddedUi_) {
            LRESULT handled = 0;
            if (embeddedUi_->HandleMessage(message, wParam, lParam, handled)) {
                return handled;
            }
        }
        if (wParam == ID_TIMER_DOCK) {
            // Background acceptance must not inspect the user's real cursor.
            // Dock transitions are driven through the isolated semantic test
            // messages above instead.
            if (!BackgroundAcceptanceMode()) {
                UpdateDockState();
            }
            return 0;
        }
        if (wParam == ID_TIMER_HOVER_ACTIVATE) {
            KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
            hoverActivationTimerId_ = 0;
            if (pendingHoverActivationKind_ == HitKind::Group) {
                SelectGroup(pendingHoverActivationId_);
            } else if (pendingHoverActivationKind_ == HitKind::Tag) {
                SelectTag(pendingHoverActivationId_);
            }
            pendingHoverActivationKind_ = HitKind::None;
            pendingHoverActivationId_ = 0;
            return 0;
        }
        if (wParam == ID_TIMER_NOTE_AUTOSAVE) {
            KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
            noteSaveTimerId_ = 0;
            SaveCurrentNotePage();
            return 0;
        }
        if (wParam == ID_TIMER_REMINDER_SCAN) {
            CheckTodoReminders();
            if (const Group* tag = FindGroup(currentTagId_); tag && IsTodoItemsTag(*tag)) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        return 0;
    case WM_DROPFILES: {
        SetDragOver(false);
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        int importedCount = 0;
        int failedCount = 0;
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            if (ImportPath(path, false)) {
                ++importedCount;
            } else {
                ++failedCount;
            }
        }
        DragFinish(drop);
        ShowImportFeedback(importedCount, failedCount);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOVING: {
        auto* moving = reinterpret_cast<RECT*>(lParam);
        if (moving && SnapDockWindowRect(*moving)) {
            dockHideDueTick_ = 0;
            return TRUE;
        }
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        RECT window{};
        if (GetWindowRect(hwnd_, &window) && SnapDockWindowRect(window)) {
            const int width = window.right - window.left;
            const int height = window.bottom - window.top;
            SetWindowPos(hwnd_,
                         MainWindowMoveInsertAfter(config_.topMost),
                         window.left,
                         window.top,
                         width,
                         height,
                         MainWindowMoveFlags(config_.topMost, SWP_NOZORDER | SWP_NOACTIVATE));
        }
        dockHideDueTick_ = GetTickCount64() + kDockRestoreGraceMs;
        SaveWindowState();
        return 0;
    }
    case WM_SIZE:
        OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOVE:
        HideItemTooltip();
        SaveWindowState();
        return 0;
    case WM_MOUSEMOVE: {
        POINT clientPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const POINT dipPoint = ClientPointToDip(clientPoint);
        const float x = static_cast<float>(dipPoint.x);
        const float y = static_cast<float>(dipPoint.y);
        if (navDragCandidateId_ > 0 || navDragActive_) {
            UpdateNavDrag(clientPoint);
            if (navDragActive_) {
                return 0;
            }
        }
        if (linkDragCandidateId_ > 0 || linkDragActive_) {
            UpdateLinkDrag(clientPoint);
            if (linkDragActive_) {
                return 0;
            }
        }
        HitArea next = HitTest(x, y);
        if (next.kind != hover_.kind || next.id != hover_.id) {
            hover_ = next;
            pendingHoverActivationKind_ = HitKind::None;
            pendingHoverActivationId_ = 0;
            if (hoverActivationTimerId_ != 0) {
                KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
                hoverActivationTimerId_ = 0;
            }
            if (next.kind == HitKind::Group && config_.mouseEnterActiveGroup && next.id != currentGroupId_) {
                pendingHoverActivationKind_ = HitKind::Group;
                pendingHoverActivationId_ = next.id;
                const UINT delay = static_cast<UINT>(std::max(1, config_.activeGroupDelay));
                hoverActivationTimerId_ = SetTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE, delay, nullptr);
            } else if (next.kind == HitKind::Tag && config_.mouseEnterActiveTag && next.id != currentTagId_) {
                pendingHoverActivationKind_ = HitKind::Tag;
                pendingHoverActivationId_ = next.id;
                const UINT delay = static_cast<UINT>(std::max(1, config_.activeTagDelay));
                hoverActivationTimerId_ = SetTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE, delay, nullptr);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (!trackingMouse_) {
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = hwnd_;
            TrackMouseEvent(&event);
            trackingMouse_ = true;
        }
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd_, &screenPoint);
        UpdateItemTooltip(next, screenPoint);
        return 0;
    }
    case WM_MOUSELEAVE:
        HideItemTooltip();
        if (navDragActive_) {
            return 0;
        }
        CancelNavDrag();
        trackingMouse_ = false;
        hover_ = {};
        pendingHoverActivationKind_ = HitKind::None;
        pendingHoverActivationId_ = 0;
        if (hoverActivationTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
            hoverActivationTimerId_ = 0;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        HideItemTooltip();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &point);
        point = ClientPointToDip(point);
        ScrollAtPoint(static_cast<float>(point.x), static_cast<float>(point.y), GET_WHEEL_DELTA_WPARAM(wParam), false);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        HideItemTooltip();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &point);
        point = ClientPointToDip(point);
        ScrollAtPoint(static_cast<float>(point.x), static_cast<float>(point.y), GET_WHEEL_DELTA_WPARAM(wParam), true);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        HideItemTooltip();
        SetFocus(hwnd_);
        selectionByKeyboard_ = false;
        CancelNavDrag();
        CancelLinkDrag();
        POINT clientPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const POINT dipPoint = ClientPointToDip(clientPoint);
        const float x = static_cast<float>(dipPoint.x);
        const float y = static_cast<float>(dipPoint.y);
        HitArea hit = HitTest(x, y);
        switch (hit.kind) {
        case HitKind::CloseButton:
            HideMainWindow();
            return 0;
        case HitKind::MenuButton:
            {
                POINT point{};
                GetCursorPos(&point);
                ShowMainMenu(point);
            }
            return 0;
        case HitKind::ToolButton:
            {
                POINT point{};
                GetCursorPos(&point);
                ShowToolMenu(point);
            }
            return 0;
        case HitKind::SkinButton:
            {
                POINT point{};
                GetCursorPos(&point);
                ShowThemeMenu(point);
            }
            return 0;
        case HitKind::AddButton:
            if (const Group* tag = FindGroup(currentTagId_); tag && IsTodoItemsTag(*tag)) {
                AddTodoItem();
            } else {
                AddLink();
            }
            return 0;
        case HitKind::Group:
            SelectGroup(hit.id);
            BeginNavDragCandidate(hit.kind, hit.id, clientPoint);
            return 0;
        case HitKind::Tag:
            SelectTag(hit.id);
            BeginNavDragCandidate(hit.kind, hit.id, clientPoint);
            return 0;
        case HitKind::Link:
            selectedLinkId_ = hit.id;
            BeginLinkDragCandidate(hit.id, clientPoint);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case HitKind::Todo:
            selectedTodoId_ = hit.id;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        default:
            return 0;
        }
    }
    case WM_LBUTTONUP: {
        HideItemTooltip();
        if (navDragActive_) {
            CommitNavDrag();
            return 0;
        }
        navDragCandidateId_ = 0;
        navDragId_ = 0;
        navDragKind_ = NavDragKind::None;
        navDragMode_ = NavDragMode::None;
        navDragInsertIndex_ = -1;
        if (linkDragActive_) {
            CommitLinkDrag();
            return 0;
        }
        const int clickLinkId = linkDragCandidateId_;
        linkDragCandidateId_ = 0;
        linkDragId_ = 0;
        linkDragMode_ = LinkDragMode::None;
        linkDragTargetLinkId_ = 0;
        linkDragInsertIndex_ = -1;
        if (clickLinkId > 0 && !config_.doubleClickToRun) {
            RunLink(clickLinkId);
            return 0;
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        HideItemTooltip();
        SetFocus(hwnd_);
        selectionByKeyboard_ = false;
        const POINT dipPoint = ClientPointToDip(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        const float x = static_cast<float>(dipPoint.x);
        const float y = static_cast<float>(dipPoint.y);
        HitArea hit = HitTest(x, y);
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd_, &point);
        if (hit.kind == HitKind::Link) {
            selectedLinkId_ = hit.id;
            ShowLinkMenu(hit.id, point);
        } else if (hit.kind == HitKind::Todo) {
            selectedTodoId_ = hit.id;
            ShowTodoMenu(hit.id, point);
        } else if (hit.kind == HitKind::Group) {
            ShowGroupMenu(hit.id, point);
        } else if (hit.kind == HitKind::Tag) {
            ShowTagMenu(hit.id, point);
        } else {
            D2D1_RECT_F title{}, groups{}, tags{}, links{};
            const D2D1_SIZE_F client = ClientSizeDip();
            BuildLayout(client.width, client.height, title, groups, tags, links);
            if (Width(groups) > 0.0f && Height(groups) > 0.0f && Contains(groups, x, y)) {
                ShowGroupBlankMenu(point);
            } else if (Width(tags) > 0.0f && Height(tags) > 0.0f && Contains(tags, x, y)) {
                ShowTagBlankMenu(point);
            } else {
                ShowBackgroundMenu(point);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        HideItemTooltip();
        const POINT dipPoint = ClientPointToDip(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        const float x = static_cast<float>(dipPoint.x);
        const float y = static_cast<float>(dipPoint.y);
        HitArea hit = HitTest(x, y);
        if (hit.kind == HitKind::Link) {
            RunLink(hit.id);
        } else if (hit.kind == HitKind::Todo) {
            EditTodoItem(hit.id);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        info->ptMinTrackSize.x = MulDiv(260, dpi, 96);
        info->ptMinTrackSize.y = MulDiv(320, dpi, 96);
        return 0;
    }
    case WM_CLOSE:
        if (exitingForPrivilegeRestart_) {
            DestroyWindow(hwnd_);
            return 0;
        }
        if (!trayIconVisible_) {
            config_.hideNotifyIcon = false;
            InitializeTrayIcon();
        }
        SaveWindowState();
        HideMainWindow();
        return 0;
    case WM_COMMAND:
        {
        const UINT command = LOWORD(wParam);
        if (command == ID_NOTE_EDIT && HIWORD(wParam) == EN_CHANGE) {
            noteDirty_ = true;
            if (noteSaveTimerId_ != 0) {
                KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
            }
            noteSaveTimerId_ = SetTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE, 700, nullptr);
            return 0;
        }
        if (command >= ID_MENU_MOVE_TO_BASE && command < ID_MENU_MOVE_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
            const std::size_t targetIndex = command - ID_MENU_MOVE_TO_BASE;
            std::vector<int> targets = menuMoveTargetIds_;
            if (targetIndex >= targets.size()) {
                Link* link = FindLink(CommandLinkId());
                targets = GroupedTagTargetIds(link ? link->parentGroup : 0);
            }
            if (targetIndex < targets.size()) {
                MoveLinkToTag(CommandLinkId(), targets[targetIndex]);
            }
            return 0;
        }
        if (command >= ID_MENU_COPY_TO_BASE && command < ID_MENU_COPY_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
            const std::size_t targetIndex = command - ID_MENU_COPY_TO_BASE;
            std::vector<int> targets = menuCopyTargetIds_;
            if (targetIndex >= targets.size()) {
                targets = GroupedTagTargetIds(0);
            }
            if (targetIndex < targets.size()) {
                CopyLinkToTag(CommandLinkId(), targets[targetIndex]);
            }
            return 0;
        }
        if (command >= ID_MENU_MOVE_TAG_TO_BASE && command < ID_MENU_MOVE_TAG_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
            const std::size_t targetIndex = command - ID_MENU_MOVE_TAG_TO_BASE;
            std::vector<int> targets = menuGroupTargetIds_;
            if (targetIndex >= targets.size()) {
                const Group* tag = FindGroup(CommandTagId());
                targets = GroupTargetIds(tag ? tag->parentGroup : 0);
            }
            if (targetIndex < targets.size()) {
                MoveTagToGroup(CommandTagId(), targets[targetIndex]);
            }
            return 0;
        }
        if (command >= ID_MENU_THEME_BASE && command < ID_MENU_THEME_BASE + 100) {
            const auto themeItems = DiscoverThemeMenuItems(appDirectory_ / L"theme", config_.theme);
            const std::size_t themeIndex = command - ID_MENU_THEME_BASE;
            if (themeIndex < themeItems.size()) {
                ApplyTheme(themeItems[themeIndex].name);
            }
            return 0;
        }
        if (command >= ID_MENU_SYSTEM_FUNCTION_BASE && command < ID_MENU_SYSTEM_FUNCTION_BASE + ID_MENU_SYSTEM_FUNCTION_LIMIT) {
            OpenSystemFunction(static_cast<std::size_t>(command - ID_MENU_SYSTEM_FUNCTION_BASE));
            return 0;
        }
        if (command >= ID_MENU_ADD_SYSTEM_FUNCTION_BASE && command < ID_MENU_ADD_SYSTEM_FUNCTION_BASE + ID_MENU_ADD_SYSTEM_FUNCTION_LIMIT) {
            AddSystemFunction(static_cast<std::size_t>(command - ID_MENU_ADD_SYSTEM_FUNCTION_BASE));
            return 0;
        }
        if (command >= ID_MENU_BUILTIN_SYSTEM_ACTION_BASE && command < ID_MENU_BUILTIN_SYSTEM_ACTION_BASE + ID_MENU_BUILTIN_SYSTEM_ACTION_LIMIT) {
            ExecuteBuiltinSystemContextAction(
                CommandLinkId(),
                static_cast<BuiltinSystemContextAction>(command - ID_MENU_BUILTIN_SYSTEM_ACTION_BASE));
            return 0;
        }
        if (command >= ID_MENU_TOOL_BASE && command < ID_MENU_TOOL_BASE + ID_MENU_TOOL_LIMIT) {
            OpenBuiltinTool(static_cast<std::size_t>(command - ID_MENU_TOOL_BASE));
            return 0;
        }
        if (command >= ID_MENU_TRACKED_SHELL_ACTION_BASE &&
            command < ID_MENU_TRACKED_SHELL_ACTION_BASE + ID_MENU_TRACKED_SHELL_ACTION_LIMIT) {
            ExecuteTrackedShellMenuAction(
                static_cast<std::size_t>(command - ID_MENU_TRACKED_SHELL_ACTION_BASE));
            return 0;
        }
        switch (command) {
        case ID_MENU_QUICK_IMPORT:
            QuickImport();
            return 0;
        case ID_MENU_ADD_LINK:
            AddLink();
            return 0;
        case ID_MENU_ADD_FILE:
            AddFile();
            return 0;
        case ID_MENU_ADD_FOLDER:
            AddFolder();
            return 0;
        case ID_MENU_ADD_URL:
            AddUrl();
            return 0;
        case ID_MENU_CLEAR_TAG_LINKS:
            ClearCurrentTagLinks();
            return 0;
        case ID_MENU_RUN_LINK:
            RunLink(CommandLinkId());
            return 0;
        case ID_MENU_RUN_ADMIN:
            RunLinkAsAdmin(CommandLinkId());
            return 0;
        case ID_MENU_RUN_PRIVATE:
            RunUrlPrivate(CommandLinkId());
            return 0;
        case ID_MENU_EDIT_LINK:
            EditLink(CommandLinkId());
            return 0;
        case ID_MENU_DELETE_LINK:
            DeleteLink(CommandLinkId());
            return 0;
        case ID_MENU_MOVE_UP:
            MoveMenuContext(-1);
            return 0;
        case ID_MENU_MOVE_DOWN:
            MoveMenuContext(1);
            return 0;
        case ID_MENU_OPEN_LOCATION:
            OpenContainingFolder(CommandLinkId());
            return 0;
        case ID_MENU_COPY_PATH:
            CopyLinkPath(CommandLinkId());
            return 0;
        case ID_MENU_COPY_URL:
            CopyLinkPath(CommandLinkId());
            return 0;
        case ID_MENU_WINDOWS_CONTEXT:
            {
                POINT point{};
                if (!LinkCenterScreenPoint(CommandLinkId(), point)) {
                    GetCursorPos(&point);
                }
                ShowWindowsContextMenu(CommandLinkId(), point);
            }
            return 0;
        case ID_MENU_CREATE_DESKTOP_SHORTCUT:
            CreateDesktopShortcut(CommandLinkId());
            return 0;
        case ID_MENU_PROPERTIES:
            OpenSystemProperties(CommandLinkId());
            return 0;
        case ID_MENU_REFRESH_LINK_ICON:
            RefreshLinkIcon(CommandLinkId());
            return 0;
        case ID_MENU_REFRESH_PAGE_ICONS:
            RefreshTagLinks(CommandTagId());
            return 0;
        case ID_MENU_REFRESH_GROUP_LINKS:
            RefreshGroupLinks(CommandGroupId());
            return 0;
        case ID_MENU_REPAIR_LINK:
            if (Link* link = FindLink(CommandLinkId())) {
                if (TryRepairLinkTarget(*link)) {
                    if (!storageService_.UpdateLink(*link)) {
                        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"修复启动项", MB_OK | MB_ICONWARNING);
                    } else {
                        RegisterConfiguredHotKeys();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        ShowToast(L"“" + link->name + L"”的目标已更新，可以重新启动。", ThemedToastRole::Success);
                    }
                }
            }
            return 0;
        case ID_MENU_COPY_LINK:
            CopyLinkInternal(CommandLinkId(), false);
            return 0;
        case ID_MENU_CUT_LINK:
            CopyLinkInternal(CommandLinkId(), true);
            return 0;
        case ID_MENU_PASTE_LINK:
            PasteLinkInternal();
            return 0;
        case ID_MENU_ADD_GROUP:
            AddGroup();
            return 0;
        case ID_MENU_EDIT_GROUP:
            EditGroup(CommandGroupId());
            return 0;
        case ID_MENU_DELETE_GROUP:
            DeleteGroup(CommandGroupId());
            return 0;
        case ID_MENU_ADD_TAG:
            AddTag();
            return 0;
        case ID_MENU_ADD_NOTE_TAG:
            AddNoteTag();
            return 0;
        case ID_MENU_ADD_TODO_TAG:
            AddTodoTag();
            return 0;
        case ID_MENU_EDIT_TAG:
            EditTag(CommandTagId());
            return 0;
        case ID_MENU_DELETE_TAG:
            DeleteTag(CommandTagId());
            return 0;
        case ID_MENU_ADD_TODO_ITEM:
            AddTodoItem();
            return 0;
        case ID_MENU_EDIT_TODO_ITEM:
            EditTodoItem(CommandTodoId());
            return 0;
        case ID_MENU_DELETE_TODO_ITEM:
            DeleteTodoItem(CommandTodoId());
            return 0;
        case ID_MENU_TOGGLE_TODO_DONE:
            ToggleTodoDone(CommandTodoId());
            return 0;
        case ID_MENU_TOGGLE_TODO_ENABLED:
            ToggleTodoEnabled(CommandTodoId());
            return 0;
        case ID_MENU_TODO_REMINDER_VIEWED:
            MarkTodoReminderViewed(CommandTodoId());
            return 0;
        case ID_MENU_TODO_REMINDER_IGNORE:
            IgnoreTodoReminder(CommandTodoId());
            return 0;
        case ID_MENU_TODO_REMINDER_SNOOZE_5:
            SnoozeTodoReminder(CommandTodoId(), 5);
            return 0;
        case ID_MENU_TODO_REMINDER_SNOOZE_30:
            SnoozeTodoReminder(CommandTodoId(), 30);
            return 0;
        case ID_MENU_TODO_REMINDER_SNOOZE_60:
            SnoozeTodoReminder(CommandTodoId(), 60);
            return 0;
        case ID_MENU_CLEAR_DONE_TODOS:
            ClearDoneTodos();
            return 0;
        case ID_MENU_COMPLETE_OVERDUE_TODOS:
            CompleteOverdueTodosInTag(CommandTagId());
            return 0;
        case ID_MENU_TODO_SORT_DUE:
            SetCurrentTodoSort(0);
            return 0;
        case ID_MENU_TODO_SORT_CREATED:
            SetCurrentTodoSort(1);
            return 0;
        case ID_MENU_TODO_SORT_TITLE:
            SetCurrentTodoSort(2);
            return 0;
        case ID_MENU_TODO_SORT_STATUS:
            SetCurrentTodoSort(3);
            return 0;
        case ID_MENU_SORT_POS:
            SetCurrentTagSort(0);
            return 0;
        case ID_MENU_SORT_RUNCOUNT:
            SetCurrentTagSort(1);
            return 0;
        case ID_MENU_SORT_NAME:
            SetCurrentTagSort(2);
            return 0;
        case ID_MENU_LAYOUT_LIST:
            SetCurrentTagLayout(0);
            return 0;
        case ID_MENU_LAYOUT_TILE:
            SetCurrentTagLayout(1);
            return 0;
        case ID_MENU_ICON_SMALL:
            SetCurrentTagIconSize(24);
            return 0;
        case ID_MENU_ICON_MEDIUM:
            SetCurrentTagIconSize(32);
            return 0;
        case ID_MENU_ICON_LARGE:
            SetCurrentTagIconSize(48);
            return 0;
        case ID_MENU_ALL_SORT_POS:
            SetAllTagsSort(0);
            return 0;
        case ID_MENU_ALL_SORT_RUNCOUNT:
            SetAllTagsSort(1);
            return 0;
        case ID_MENU_ALL_SORT_NAME:
            SetAllTagsSort(2);
            return 0;
        case ID_MENU_ALL_LAYOUT_LIST:
            SetAllTagsLayout(0);
            return 0;
        case ID_MENU_ALL_LAYOUT_TILE:
            SetAllTagsLayout(1);
            return 0;
        case ID_MENU_ALL_ICON_SMALL:
            SetAllTagsIconSize(24);
            return 0;
        case ID_MENU_ALL_ICON_MEDIUM:
            SetAllTagsIconSize(32);
            return 0;
        case ID_MENU_ALL_ICON_LARGE:
            SetAllTagsIconSize(48);
            return 0;
        case ID_MENU_TOGGLE_TITLE:
            ToggleConfigVisibility(&AppConfig::showTitle);
            return 0;
        case ID_MENU_TOGGLE_GROUP:
            ToggleConfigVisibility(&AppConfig::showGroup);
            return 0;
        case ID_MENU_TOGGLE_TAG:
            ToggleConfigVisibility(&AppConfig::showTag);
            return 0;
        case ID_MENU_TOGGLE_TOPMOST:
            ToggleConfigVisibility(&AppConfig::topMost);
            ShowToast(config_.topMost ? L"已置顶主窗口。" : L"已取消置顶。", ThemedToastRole::Info);
            return 0;
        case ID_MENU_SETTINGS:
            OpenSettings();
            return 0;
        case ID_MENU_RESET_LAYOUT:
            ResetLayoutToDefaults();
            return 0;
        case ID_MENU_IMPORT_CLIPBOARD:
            ImportClipboard();
            return 0;
        case ID_MENU_IMPORT_CONFIG_MERGE:
            ImportConfigPackageMerge();
            return 0;
        case ID_MENU_EXPORT_CONFIG:
            ExportConfigPackage();
            return 0;
        case ID_MENU_CLEAR_ICON_CACHE:
            ClearIconCache();
            return 0;
        case ID_MENU_REFRESH_ALL_ICONS:
            RefreshAllIcons();
            return 0;
        case ID_MENU_ABOUT:
            ShowAbout();
            return 0;
        case ID_MENU_CHECK_UPDATE:
            CheckForUpdates();
            return 0;
        case ID_MENU_HELP:
            OpenHelp();
            return 0;
        case ID_MENU_FAQ:
            OpenFaq();
            return 0;
        case ID_MENU_REWARD:
            OpenReward();
            return 0;
        case ID_MENU_RESTART_PRIVILEGE:
            RestartWithOppositePrivilege();
            return 0;
        case ID_MENU_SHOW:
            if (IsEffectivelyVisible()) {
                HideMainWindow();
            } else {
                WakeUp();
            }
            return 0;
        case ID_MENU_EXIT:
            WriteAppLog(L"收到退出命令，准备清理同路径后台实例。");
            TerminateSiblingQuattroProcesses();
            WriteAppLog(L"退出清理完成，销毁当前主窗口。");
            DestroyWindow(hwnd_);
            return 0;
        default:
            return 0;
        }
        }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            const HitArea hit = CursorHitArea();
            switch (hit.kind) {
            case HitKind::CloseButton:
            case HitKind::MenuButton:
            case HitKind::ToolButton:
            case HitKind::SkinButton:
            case HitKind::AddButton:
            case HitKind::Group:
            case HitKind::Tag:
            case HitKind::Link:
            case HitKind::Todo:
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            default:
                break;
            }
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (HandleKeyDown(wParam)) {
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_MEASUREITEM:
        if (MeasureThemedMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_DRAWITEM:
        if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        if (DrawThemedMenuItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lParam) == noteEdit_ && embeddedUi_) {
            LRESULT result = 0;
            if (embeddedUi_->HandleMessage(message, wParam, lParam, result)) {
                return result;
            }
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_DESTROY:
        WriteAppLog(L"主窗口销毁，准备退出消息循环。");
        CancelNavDrag();
        CancelLinkDrag();
        RemoveTrayIcon();
        SaveCurrentNotePage();
        urlIconDownloadService_.Shutdown();
        HideItemTooltip();
        if (dockTimerId_ != 0) {
            KillTimer(hwnd_, dockTimerId_);
            dockTimerId_ = 0;
        }
        if (hoverActivationTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
            hoverActivationTimerId_ = 0;
        }
        if (noteSaveTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
            noteSaveTimerId_ = 0;
        }
        if (reminderScanTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_REMINDER_SCAN);
            reminderScanTimerId_ = 0;
        }
        HideTodoReminderPanel();
        DragAcceptFiles(hwnd_, FALSE);
        RevokeDragDrop(hwnd_);
        if (oleDropTarget_) {
            oleDropTarget_->Release();
            oleDropTarget_ = nullptr;
        }
        UnregisterConfiguredHotKeys();
        SaveWindowState();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}

void MainWindow::SelectInitialItems() {
    const auto majors = MajorGroups();
    if (majors.empty()) {
        return;
    }

    currentGroupId_ = config_.currentGroupId;
    if (!FindGroup(currentGroupId_) || FindGroup(currentGroupId_)->parentGroup != 0) {
        currentGroupId_ = majors.front().id;
    }

    const auto tags = TagsForCurrentGroup();
    currentTagId_ = config_.currentTagId;
    Group* tag = FindGroup(currentTagId_);
    if (!tag || tag->parentGroup != currentGroupId_) {
        currentTagId_ = tags.empty() ? 0 : tags.front().id;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
}

void MainWindow::SelectGroup(int groupId) {
    Group* group = FindGroup(groupId);
    if (!group || group->parentGroup != 0 || currentGroupId_ == groupId) {
        return;
    }

    SaveCurrentNotePage();
    currentGroupId_ = groupId;
    const auto tags = TagsForCurrentGroup();
    currentTagId_ = tags.empty() ? 0 : tags.front().id;
    selectedLinkId_ = 0;
    selectedTodoId_ = 0;
    selectionByKeyboard_ = false;
    tagScrollOffset_ = 0.0f;
    linkScrollOffset_ = 0.0f;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SelectTag(int tagId) {
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup != currentGroupId_ || currentTagId_ == tagId) {
        return;
    }

    SaveCurrentNotePage();
    currentTagId_ = tagId;
    selectedLinkId_ = 0;
    selectedTodoId_ = 0;
    selectionByKeyboard_ = false;
    linkScrollOffset_ = 0.0f;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int MainWindow::CommandGroupId() const {
    if (menuContextKind_ == HitKind::Group) {
        return menuContextId_;
    }
    if (menuContextKind_ == HitKind::Tag) {
        const Group* tag = FindGroup(menuContextId_);
        if (tag && tag->parentGroup > 0) {
            return tag->parentGroup;
        }
    }
    return currentGroupId_;
}

int MainWindow::CommandTagId() const {
    if (menuContextKind_ == HitKind::Tag) {
        return menuContextId_;
    }
    return currentTagId_;
}

int MainWindow::CommandLinkId() const {
    if (menuContextKind_ == HitKind::Link) {
        return menuContextId_;
    }
    return selectedLinkId_;
}

int MainWindow::CommandTodoId() const {
    if (menuContextKind_ == HitKind::Todo) {
        return menuContextId_;
    }
    return selectedTodoId_;
}

void MainWindow::AddGroup() {
    std::wstring name = UniqueSiblingGroupName(model_.groups, 0, L"新的分组");
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"新建分组", L"分组名称", name)) {
        return;
    }

    Group group;
    group.name = name;
    group.parentGroup = 0;
    group.pos = -1;
    if (!storageService_.InsertGroup(group)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增分组", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(group);

    Group defaultTag;
    defaultTag.name = UniqueSiblingGroupName(model_.groups, group.id, L"新的标签");
    defaultTag.parentGroup = group.id;
    defaultTag.pos = -1;
    defaultTag.layout = 1;
    defaultTag.iconSize = 32;
    if (storageService_.InsertGroup(defaultTag)) {
        model_.groups.push_back(defaultTag);
        currentTagId_ = defaultTag.id;
    } else {
        currentTagId_ = 0;
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增标签", MB_OK | MB_ICONWARNING);
    }

    currentGroupId_ = group.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EditGroup(int groupId) {
    Group* group = FindGroup(groupId);
    if (!group || group->parentGroup != 0) {
        return;
    }
    std::wstring name = group->name;
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"编辑分组", L"分组名称", name)) {
        return;
    }
    Group edited = *group;
    edited.name = name;
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑分组", MB_OK | MB_ICONWARNING);
        return;
    }
    *group = edited;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteGroup(int groupId) {
    Group* group = FindGroup(groupId);
    if (!group || group->parentGroup != 0) {
        return;
    }
    SaveCurrentNotePage();
    std::wstring message = L"确定删除分组“" + group->name + L"”及其标签和启动项？";
    if (MessageBoxW(hwnd_, message.c_str(), L"删除分组", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (!storageService_.DeleteGroup(groupId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除分组", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring deletedName = group->name;
    std::unordered_set<int> deletedTagIds;
    for (const Group& item : model_.groups) {
        if (item.parentGroup == groupId) {
            deletedTagIds.insert(item.id);
        }
    }
    const int deletedContentCount = static_cast<int>(std::count_if(model_.links.begin(), model_.links.end(), [&](const Link& link) {
        return deletedTagIds.contains(link.parentGroup);
    }) + std::count_if(model_.notes.begin(), model_.notes.end(), [&](const NotePage& note) {
        return deletedTagIds.contains(note.tagId);
    }) + std::count_if(model_.todos.begin(), model_.todos.end(), [&](const TodoItem& item) {
        return deletedTagIds.contains(item.tagId);
    }));
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [&](const Link& link) {
        Group* tag = FindGroup(link.parentGroup);
        return tag && tag->parentGroup == groupId;
    }), model_.links.end());
    model_.notes.erase(std::remove_if(model_.notes.begin(), model_.notes.end(), [&](const NotePage& note) {
        Group* tag = FindGroup(note.tagId);
        return tag && tag->parentGroup == groupId;
    }), model_.notes.end());
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [&](const TodoItem& item) {
        Group* tag = FindGroup(item.tagId);
        return tag && tag->parentGroup == groupId;
    }), model_.todos.end());
    model_.groups.erase(std::remove_if(model_.groups.begin(), model_.groups.end(), [groupId](const Group& item) {
        return item.id == groupId || item.parentGroup == groupId;
    }), model_.groups.end());
    RegisterConfiguredHotKeys();
    SelectInitialItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(
        L"已删除分组“" + deletedName + L"”（" + std::to_wstring(deletedTagIds.size()) +
            L" 个标签、" + std::to_wstring(deletedContentCount) + L" 项内容）。",
        ThemedToastRole::Info,
        5000);
}

void MainWindow::AddTag() {
    const int parentGroupId = CommandGroupId();
    Group* parent = FindGroup(parentGroupId);
    if (!parent || parent->parentGroup != 0) {
        AddGroup();
        return;
    }
    std::wstring name = UniqueSiblingGroupName(model_.groups, parentGroupId, L"新的标签");
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"新建标签", L"标签名称", name)) {
        return;
    }

    Group tag;
    tag.name = name;
    tag.parentGroup = parentGroupId;
    tag.pos = -1;
    tag.layout = 1;
    tag.iconSize = 32;
    if (!storageService_.InsertGroup(tag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增标签", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(tag);
    currentGroupId_ = parentGroupId;
    currentTagId_ = tag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddNoteTag() {
    const int parentGroupId = CommandGroupId();
    Group* parent = FindGroup(parentGroupId);
    if (!parent || parent->parentGroup != 0) {
        AddGroup();
        return;
    }
    std::wstring name = UniqueSiblingGroupName(model_.groups, parentGroupId, L"便签");
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"新建便签", L"便签标题", name)) {
        return;
    }

    Group tag;
    tag.name = name;
    tag.parentGroup = parentGroupId;
    tag.pos = -1;
    tag.layout = 1;
    tag.iconSize = 32;
    tag.type = 3;
    tag.content = L"note";
    if (!storageService_.InsertGroup(tag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增便签", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(tag);
    currentGroupId_ = parentGroupId;
    currentTagId_ = tag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureTagVisible(currentTagId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddTodoTag() {
    const int parentGroupId = CommandGroupId();
    Group* parent = FindGroup(parentGroupId);
    if (!parent || parent->parentGroup != 0) {
        AddGroup();
        return;
    }
    Group tag;
    tag.name = UniqueSiblingGroupName(model_.groups, parentGroupId, L"待办事项");
    tag.parentGroup = parentGroupId;
    tag.pos = -1;
    tag.layout = 1;
    tag.iconSize = 32;
    tag.type = 4;
    tag.content = L"todoItems";
    if (!storageService_.InsertGroup(tag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增待办事项标签页", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(tag);
    currentGroupId_ = parentGroupId;
    currentTagId_ = tag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureTagVisible(currentTagId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EditTag(int tagId) {
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    std::wstring name = tag->name;
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"编辑标签", L"标签名称", name)) {
        return;
    }
    Group edited = *tag;
    edited.name = name;
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑标签", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteTag(int tagId) {
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    SaveCurrentNotePage();
    std::wstring message = L"确定删除标签“" + tag->name + L"”及其启动项？";
    if (MessageBoxW(hwnd_, message.c_str(), L"删除标签", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (!storageService_.DeleteGroup(tagId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除标签", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring deletedName = tag->name;
    const int deletedContentCount = static_cast<int>(
        std::count_if(model_.links.begin(), model_.links.end(), [tagId](const Link& link) { return link.parentGroup == tagId; }) +
        std::count_if(model_.notes.begin(), model_.notes.end(), [tagId](const NotePage& note) { return note.tagId == tagId; }) +
        std::count_if(model_.todos.begin(), model_.todos.end(), [tagId](const TodoItem& item) { return item.tagId == tagId; }));
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [tagId](const Link& link) {
        return link.parentGroup == tagId;
    }), model_.links.end());
    model_.notes.erase(std::remove_if(model_.notes.begin(), model_.notes.end(), [tagId](const NotePage& note) {
        return note.tagId == tagId;
    }), model_.notes.end());
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [tagId](const TodoItem& item) {
        return item.tagId == tagId;
    }), model_.todos.end());
    model_.groups.erase(std::remove_if(model_.groups.begin(), model_.groups.end(), [tagId](const Group& item) {
        return item.id == tagId;
    }), model_.groups.end());
    RegisterConfiguredHotKeys();
    SelectInitialItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(
        L"已删除标签“" + deletedName + L"”（" + std::to_wstring(deletedContentCount) + L" 项内容）。",
        ThemedToastRole::Info);
}

void MainWindow::SetCurrentTagSort(int sort) {
    // View options are stored per tag page; menu context may point at a non-current tag.
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    Group edited = *tag;
    sort = std::max(0, std::min(2, sort));
    if (sort == 0) {
        edited.sort = 0;
        edited.sortDirection = 0;
    } else {
        edited.sortDirection = edited.sort == sort ? (edited.sortDirection == 0 ? 1 : 0) : DefaultLinkSortDirection(sort);
        edited.sort = sort;
    }
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"排序", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCurrentTodoSort(int sort) {
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0 || !IsTodoItemsTag(*tag)) {
        return;
    }
    Group edited = *tag;
    edited.sort = std::max(0, std::min(3, sort));
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"待办排序", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCurrentTagLayout(int layout) {
    // View options are stored per tag page; menu context may point at a non-current tag.
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    Group edited = *tag;
    edited.layout = layout == 0 ? 0 : 1;
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"布局", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCurrentTagIconSize(int iconSize) {
    // View options are stored per tag page; menu context may point at a non-current tag.
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    Group edited = *tag;
    if (iconSize <= 24) {
        edited.iconSize = 24;
    } else if (iconSize >= 48) {
        edited.iconSize = 48;
    } else {
        edited.iconSize = 32;
    }
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"图标尺寸", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetAllTagsSort(int sort) {
    sort = std::max(0, std::min(2, sort));
    bool hasSameAutoSort = false;
    bool allSameAutoDirection = true;
    int currentDirection = DefaultLinkSortDirection(sort);
    if (sort != 0) {
        for (const Group& tag : model_.groups) {
            if (tag.parentGroup == 0) {
                continue;
            }
            if (tag.sort != sort) {
                allSameAutoDirection = false;
                continue;
            }
            if (!hasSameAutoSort) {
                currentDirection = tag.sortDirection == 0 ? 0 : 1;
                hasSameAutoSort = true;
            } else {
                allSameAutoDirection = allSameAutoDirection && (tag.sortDirection == currentDirection);
            }
        }
    }
    const int targetDirection = sort == 0 ? 0 :
        (hasSameAutoSort && allSameAutoDirection ? (currentDirection == 0 ? 1 : 0) : DefaultLinkSortDirection(sort));
    int updatedCount = 0;
    for (Group& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        Group edited = tag;
        edited.sort = sort;
        edited.sortDirection = targetDirection;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"统一排序方式", MB_OK | MB_ICONWARNING);
            return;
        }
        tag = edited;
        ++updatedCount;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已统一 " + std::to_wstring(updatedCount) + L" 个标签的排序方式。", ThemedToastRole::Success);
}

void MainWindow::SetAllTagsLayout(int layout) {
    int updatedCount = 0;
    for (Group& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        Group edited = tag;
        edited.layout = layout == 0 ? 0 : 1;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"统一查看方式", MB_OK | MB_ICONWARNING);
            return;
        }
        tag = edited;
        ++updatedCount;
    }
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(
        L"已将 " + std::to_wstring(updatedCount) + L" 个标签统一为" + (layout == 0 ? L"列表" : L"平铺") + L"视图。",
        ThemedToastRole::Success);
}

void MainWindow::SetAllTagsIconSize(int iconSize) {
    int normalized = 32;
    if (iconSize <= 24) {
        normalized = 24;
    } else if (iconSize >= 48) {
        normalized = 48;
    }

    int updatedCount = 0;
    for (Group& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        Group edited = tag;
        edited.iconSize = normalized;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"统一图标大小", MB_OK | MB_ICONWARNING);
            return;
        }
        tag = edited;
        ++updatedCount;
    }
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
    const wchar_t* sizeText = normalized == 24 ? L"小" : (normalized == 48 ? L"大" : L"中等");
    ShowToast(
        L"已将 " + std::to_wstring(updatedCount) + L" 个标签统一为" + sizeText + L"图标。",
        ThemedToastRole::Success);
}

void MainWindow::ToggleConfigVisibility(bool AppConfig::*field) {
    AppConfig previous = config_;
    config_.*field = !(config_.*field);
    configService_.SaveWindowState(config_);
    ApplyConfigRuntimeChanges(previous);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int MainWindow::EnsureCurrentTag() {
    Group* tag = FindGroup(currentTagId_);
    if (tag && tag->parentGroup != 0 && !IsNoteTag(*tag) && !IsTodoItemsTag(*tag)) {
        return currentTagId_;
    }

    Group* group = FindGroup(currentGroupId_);
    if (!group || group->parentGroup != 0) {
        Group createdGroup;
        createdGroup.name = UniqueSiblingGroupName(model_.groups, 0, L"默认分组");
        createdGroup.parentGroup = 0;
        createdGroup.pos = -1;
        if (!storageService_.InsertGroup(createdGroup)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"创建默认分组", MB_OK | MB_ICONWARNING);
            return 0;
        }
        model_.groups.push_back(createdGroup);
        currentGroupId_ = createdGroup.id;
    }

    Group createdTag;
    createdTag.name = UniqueSiblingGroupName(model_.groups, currentGroupId_, L"默认标签");
    createdTag.parentGroup = currentGroupId_;
    createdTag.pos = -1;
    createdTag.layout = 1;
    createdTag.iconSize = 32;
    if (!storageService_.InsertGroup(createdTag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"创建默认标签", MB_OK | MB_ICONWARNING);
        return 0;
    }
    model_.groups.push_back(createdTag);
    currentTagId_ = createdTag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    return currentTagId_;
}

void MainWindow::AddLink() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }
    Link link;
    link.parentGroup = CommandTagId();
    link.type = 0;
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (!LinkEditDialog::Show(hwnd_, instance_, theme_, link, model_.groups, true)) {
        return;
    }
    if (!storageService_.InsertLink(link)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(link);
    RequestInitialUrlIconDownload(link);
    RegisterConfiguredHotKeys();
    selectedLinkId_ = link.id;
    currentTagId_ = link.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddFile() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    CommonFileDialogOptions options{};
    options.owner = hwnd_;
    options.mode = CommonFileDialogMode::FileOnly;
    options.context = L"主窗口添加文件";
    options.defaultPath = appDirectory_.wstring();
    options.legacyFilter = L"所有文件\0*.*\0";
    CommonFileDialogResult result{};
    if (!ShowCommonFileDialog(options, result)) {
        return;
    }
    ImportPath(result.path);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddFolder() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    CommonFileDialogOptions options{};
    options.owner = hwnd_;
    options.mode = CommonFileDialogMode::FolderOnly;
    options.context = L"主窗口添加文件夹";
    options.defaultPath = appDirectory_.wstring();
    CommonFileDialogResult result{};
    if (!ShowCommonFileDialog(options, result)) {
        return;
    }
    ImportPath(result.path);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddUrl() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    Link link;
    link.parentGroup = currentTagId_;
    link.type = 2;
    link.icon = L"#url";
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (!UrlEditDialog::Show(hwnd_, instance_, theme_, link, true)) {
        return;
    }
    if (!storageService_.InsertLink(link)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"添加网址", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(link);
    selectedLinkId_ = link.id;
    RequestInitialUrlIconDownload(link);
    RegisterConfiguredHotKeys();
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int MainWindow::EnsureQuickImportTargetTag() {
    Group* currentTag = FindGroup(currentTagId_);
    if (currentTag && currentTag->parentGroup != 0 && !IsNoteTag(*currentTag) && !IsTodoItemsTag(*currentTag) && !IsAllTag(*currentTag) && !IsTodoTag(*currentTag)) {
        return currentTag->id;
    }

    int parentGroupId = currentGroupId_;
    if (currentTag && currentTag->parentGroup != 0) {
        parentGroupId = currentTag->parentGroup;
    }

    Group* parentGroup = FindGroup(parentGroupId);
    if (!parentGroup || parentGroup->parentGroup != 0) {
        Group createdGroup;
        createdGroup.name = UniqueSiblingGroupName(model_.groups, 0, L"默认分组");
        createdGroup.parentGroup = 0;
        createdGroup.pos = -1;
        if (!storageService_.InsertGroup(createdGroup)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"快速导入", MB_OK | MB_ICONWARNING);
            return 0;
        }
        model_.groups.push_back(createdGroup);
        parentGroupId = createdGroup.id;
        currentGroupId_ = parentGroupId;
    }

    for (const auto& group : model_.groups) {
        if (group.parentGroup == parentGroupId && !IsNoteTag(group) && !IsTodoItemsTag(group) && !IsAllTag(group) && !IsTodoTag(group)) {
            return group.id;
        }
    }

    Group createdTag;
    createdTag.name = UniqueSiblingGroupName(model_.groups, parentGroupId, L"默认标签");
    createdTag.parentGroup = parentGroupId;
    createdTag.pos = -1;
    createdTag.layout = 1;
    createdTag.iconSize = 32;
    if (!storageService_.InsertGroup(createdTag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"快速导入", MB_OK | MB_ICONWARNING);
        return 0;
    }
    model_.groups.push_back(createdTag);
    return createdTag.id;
}

void MainWindow::QuickImport() {
    std::vector<Link> links;
    if (!QuickImportDialog::Show(hwnd_, instance_, theme_, model_.links, links)) {
        return;
    }

    const int targetTagId = EnsureQuickImportTargetTag();
    if (targetTagId <= 0) {
        return;
    }

    int imported = 0;
    std::wstring error;
    for (auto& link : links) {
        link.parentGroup = targetTagId;
        link.pos = -1;
        if (link.showCmd == 0) {
            link.showCmd = SW_SHOWNORMAL;
        }
        if (!storageService_.InsertLink(link)) {
            error = storageService_.lastError();
            continue;
        }
        model_.links.push_back(link);
        selectedLinkId_ = link.id;
        ++imported;
        RequestInitialUrlIconDownload(link);
    }

    currentTagId_ = targetTagId;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    RegisterConfiguredHotKeys();
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    if (selectedLinkId_ > 0) {
        EnsureLinkVisible(selectedLinkId_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (!error.empty()) {
        MessageBoxW(hwnd_, (L"已导入 " + std::to_wstring(imported) + L" 项，部分项目失败：\n" + error).c_str(), L"快速导入", MB_OK | MB_ICONWARNING);
    } else {
        ShowToast(L"已导入 " + std::to_wstring(imported) + L" 项到当前标签页。", ThemedToastRole::Success);
    }
}

void MainWindow::AddSystemFunction(std::size_t index) {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    Link link;
    link.parentGroup = CommandTagId();
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (!ConfigureSystemFunctionLink(index, link)) {
        return;
    }
    link.parentGroup = CommandTagId();
    link.pos = -1;
    if (!storageService_.InsertLink(link)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"添加系统功能", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(link);
    selectedLinkId_ = link.id;
    currentTagId_ = link.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    RegisterConfiguredHotKeys();
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenSystemFunction(std::size_t index) {
    Link link;
    if (!ConfigureSystemFunctionLink(index, link)) {
        return;
    }

    std::wstring error;
    if (!launcher_.Run(link, error)) {
        const std::wstring message = error.empty() ? L"打开系统功能失败。" : error;
        MessageBoxW(hwnd_, message.c_str(), L"系统功能", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::EditLink(int linkId) {
    Link* existing = FindLink(linkId);
    if (!existing) {
        return;
    }
    Link edited = *existing;
    if (!LinkEditDialog::Show(hwnd_, instance_, theme_, edited, model_.groups, false)) {
        return;
    }
    if (!storageService_.UpdateLink(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    const bool shellTargetChanged = existing->path != edited.path || existing->type != edited.type;
    *existing = edited;
    if (shellTargetChanged) {
        shellContextMenuCache_.Remove(linkId);
    }
    RegisterConfiguredHotKeys();
    selectedLinkId_ = edited.id;
    currentTagId_ = edited.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteLink(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    if (config_.deleteConfirm) {
        std::wstring message = L"确定删除启动项“" + link->name + L"”？";
        if (MessageBoxW(hwnd_, message.c_str(), L"删除启动项", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }
    if (!storageService_.DeleteLink(linkId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring deletedName = link->name;
    shellContextMenuCache_.Remove(linkId);
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [linkId](const Link& item) {
        return item.id == linkId;
    }), model_.links.end());
    RegisterConfiguredHotKeys();
    if (selectedLinkId_ == linkId) {
        selectedLinkId_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已删除“" + deletedName + L"”。", ThemedToastRole::Info);
}

void MainWindow::CopyLinkInternal(int linkId, bool cut) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    clipboardLink_ = *link;
    clipboardSourceId_ = linkId;
    clipboardCut_ = cut;
    hasClipboardLink_ = true;
    ShowToast(
        cut ? (L"已剪切“" + link->name + L"”。") : (L"已复制“" + link->name + L"”，可粘贴到其他标签。"),
        ThemedToastRole::Info);
}

void MainWindow::PasteLinkInternal() {
    if (!hasClipboardLink_) {
        return;
    }
    int targetTagId = currentTagId_;
    if (menuContextKind_ == HitKind::Tag) {
        targetTagId = menuContextId_;
    } else if (menuContextKind_ == HitKind::Link) {
        if (Link* contextLink = FindLink(menuContextId_)) {
            targetTagId = contextLink->parentGroup;
        }
    }

    Group* tag = FindGroup(targetTagId);
    if (!tag || tag->parentGroup == 0) {
        ShowToast(L"请先选择一个标签。", ThemedToastRole::Warning);
        return;
    }

    if (clipboardCut_) {
        Link* source = FindLink(clipboardSourceId_);
        if (!source) {
            hasClipboardLink_ = false;
            ShowToast(L"原启动项已不存在，无法粘贴。", ThemedToastRole::Warning);
            return;
        }
        if (source->parentGroup != targetTagId) {
            MoveLinkToTag(source->id, targetTagId);
        } else {
            selectedLinkId_ = source->id;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        hasClipboardLink_ = false;
        clipboardCut_ = false;
        clipboardSourceId_ = 0;
        return;
    }

    Link copy = clipboardLink_;
    copy.id = 0;
    copy.parentGroup = targetTagId;
    copy.pos = -1;
    copy.hotKey = 0;
    copy.runCount = 0;
    if (!storageService_.InsertLink(copy)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"粘贴启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(copy);
    selectedLinkId_ = copy.id;
    currentTagId_ = targetTagId;
    currentGroupId_ = tag->parentGroup;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    RegisterConfiguredHotKeys();
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已粘贴“" + copy.name + L"”。", ThemedToastRole::Success);
}

void MainWindow::MoveMenuContext(int direction) {
    if (menuContextKind_ == HitKind::Link || (menuContextKind_ == HitKind::None && selectedLinkId_ > 0)) {
        MoveLinkWithinTag(CommandLinkId(), direction);
    } else if (menuContextKind_ == HitKind::Group || menuContextKind_ == HitKind::Tag) {
        MoveGroupWithinParent(menuContextId_, direction);
    }
}

bool MainWindow::ApplyManualLinkOrder(int tagId, const std::vector<int>& orderedLinkIds, const wchar_t* title) {
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup == 0 || orderedLinkIds.empty()) {
        return false;
    }

    for (int i = 0; i < static_cast<int>(orderedLinkIds.size()); ++i) {
        Link* link = FindLink(orderedLinkIds[i]);
        if (!link || link->parentGroup != tagId) {
            continue;
        }
        if (link->pos == i) {
            continue;
        }
        Link edited = *link;
        edited.pos = i;
        if (!storageService_.UpdateLink(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), title, MB_OK | MB_ICONWARNING);
            return false;
        }
        *link = edited;
    }

    if (tag->sort != 0 || tag->sortDirection != 0) {
        Group edited = *tag;
        edited.sort = 0;
        edited.sortDirection = 0;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), title, MB_OK | MB_ICONWARNING);
            return false;
        }
        *tag = edited;
        ShowToast(L"排序方式已切换为手动排序。", ThemedToastRole::Info);
    }
    return true;
}

bool MainWindow::ApplyManualGroupOrder(int parentGroup, const std::vector<int>& orderedGroupIds, const wchar_t* title) {
    if (parentGroup < 0 || orderedGroupIds.empty()) {
        return false;
    }

    for (int i = 0; i < static_cast<int>(orderedGroupIds.size()); ++i) {
        Group* group = FindGroup(orderedGroupIds[i]);
        if (!group || group->parentGroup != parentGroup) {
            continue;
        }
        if (group->pos == i) {
            continue;
        }
        Group edited = *group;
        edited.pos = i;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), title, MB_OK | MB_ICONWARNING);
            return false;
        }
        *group = edited;
    }

    return true;
}

void MainWindow::MoveLinkWithinTag(int linkId, int direction) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }

    std::vector<Link*> siblings = OrderedLinksForTag(link->parentGroup);

    auto it = std::find(siblings.begin(), siblings.end(), link);
    if (it == siblings.end()) {
        return;
    }
    const int index = static_cast<int>(std::distance(siblings.begin(), it));
    const int targetIndex = index + (direction < 0 ? -1 : 1);
    if (targetIndex < 0 || targetIndex >= static_cast<int>(siblings.size())) {
        return;
    }

    std::swap(siblings[index], siblings[targetIndex]);
    std::vector<int> orderedIds;
    orderedIds.reserve(siblings.size());
    for (const Link* item : siblings) {
        orderedIds.push_back(item->id);
    }
    if (!ApplyManualLinkOrder(link->parentGroup, orderedIds, L"移动启动项")) {
        return;
    }
    selectedLinkId_ = linkId;
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveGroupWithinParent(int groupId, int direction) {
    Group* group = FindGroup(groupId);
    if (!group) {
        return;
    }

    std::vector<Group*> siblings;
    for (auto& item : model_.groups) {
        if (item.parentGroup == group->parentGroup) {
            siblings.push_back(&item);
        }
    }
    std::sort(siblings.begin(), siblings.end(), [](const Group* left, const Group* right) {
        if (left->pos != right->pos) {
            return left->pos < right->pos;
        }
        return left->id < right->id;
    });

    auto it = std::find(siblings.begin(), siblings.end(), group);
    if (it == siblings.end()) {
        return;
    }
    const int index = static_cast<int>(std::distance(siblings.begin(), it));
    const int targetIndex = index + (direction < 0 ? -1 : 1);
    if (targetIndex < 0 || targetIndex >= static_cast<int>(siblings.size())) {
        return;
    }

    std::swap(siblings[index], siblings[targetIndex]);
    std::vector<int> orderedIds;
    orderedIds.reserve(siblings.size());
    for (const Group* item : siblings) {
        orderedIds.push_back(item->id);
    }
    if (!ApplyManualGroupOrder(group->parentGroup, orderedIds, group->parentGroup == 0 ? L"移动分组" : L"移动标签")) {
        return;
    }
    if (group->parentGroup == 0) {
        currentGroupId_ = groupId;
        EnsureGroupVisible(currentGroupId_);
    } else {
        currentTagId_ = groupId;
        EnsureTagVisible(currentTagId_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveTagToGroup(int tagId, int groupId) {
    Group* tag = FindGroup(tagId);
    Group* targetGroup = FindGroup(groupId);
    if (!tag || tag->parentGroup == 0 || !targetGroup || targetGroup->parentGroup != 0 || tag->parentGroup == groupId) {
        return;
    }

    Group edited = *tag;
    edited.parentGroup = groupId;
    edited.pos = NextModelGroupPosition(model_.groups, groupId);
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"移动标签", MB_OK | MB_ICONWARNING);
        return;
    }

    *tag = edited;
    currentGroupId_ = groupId;
    currentTagId_ = tagId;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    tagScrollOffset_ = 0.0f;
    linkScrollOffset_ = 0.0f;
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"标签已移至“" + targetGroup->name + L"”。", ThemedToastRole::Success);
}

void MainWindow::MoveLinkToTag(int linkId, int tagId) {
    Link* link = FindLink(linkId);
    Group* tag = FindGroup(tagId);
    if (!link || !tag || tag->parentGroup == 0 || link->parentGroup == tagId) {
        return;
    }

    Link edited = *link;
    edited.parentGroup = tagId;
    edited.pos = NextModelLinkPosition(model_.links, tagId);
    if (!storageService_.UpdateLink(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"移动启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    *link = edited;
    selectedLinkId_ = edited.id;
    currentGroupId_ = tag->parentGroup;
    currentTagId_ = tag->id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已移动到“" + tag->name + L"”。", ThemedToastRole::Success);
}

void MainWindow::CopyLinkToTag(int linkId, int tagId) {
    Link* link = FindLink(linkId);
    Group* tag = FindGroup(tagId);
    if (!link || !tag || tag->parentGroup == 0) {
        return;
    }

    Link copy = *link;
    copy.id = 0;
    copy.parentGroup = tagId;
    copy.pos = -1;
    copy.hotKey = 0;
    copy.runCount = 0;
    if (!storageService_.InsertLink(copy)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"复制启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(copy);
    RegisterConfiguredHotKeys();
    selectedLinkId_ = copy.id;
    currentGroupId_ = tag->parentGroup;
    currentTagId_ = tag->id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已复制到“" + tag->name + L"”。", ThemedToastRole::Success);
}

void MainWindow::RunLink(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }

    std::wstring error;
    if (!launcher_.Run(*link, error)) {
        std::wstring message = error.empty() ? L"启动失败。" : error;
        message += L"\n\n是否现在修复该启动项目标？";
        if (MessageBoxW(hwnd_, message.c_str(), L"启动项", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES &&
            TryRepairLinkTarget(*link)) {
            if (!storageService_.UpdateLink(*link)) {
                MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"修复启动项", MB_OK | MB_ICONWARNING);
            } else {
                RegisterConfiguredHotKeys();
                InvalidateRect(hwnd_, nullptr, FALSE);
                ShowToast(L"“" + link->name + L"”的目标已更新，可以重新启动。", ThemedToastRole::Success);
            }
        }
        return;
    }

    if (config_.saveRunCount) {
        ++link->runCount;
        storageService_.IncrementRunCount(link->id, link->runCount);
    }
    if (config_.hideAfterLink) {
        HideMainWindowAfterLink();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RunLinkAsAdmin(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    Link elevated = *link;
    elevated.isAdmin = true;
    std::wstring error;
    if (!launcher_.Run(elevated, error)) {
        MessageBoxW(hwnd_, error.empty() ? L"管理员运行失败。" : error.c_str(), L"以管理员身份运行", MB_OK | MB_ICONWARNING);
        return;
    }
    if (config_.saveRunCount) {
        ++link->runCount;
        storageService_.IncrementRunCount(link->id, link->runCount);
    }
    if (config_.hideAfterLink) {
        HideMainWindowAfterLink();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RunUrlPrivate(int linkId) {
    Link* link = FindLink(linkId);
    if (!link || !IsUrlLink(*link)) {
        return;
    }

    const std::wstring url = NormalizeUrl(link->path);
    const std::wstring arguments = L"-inprivate " + QuoteForCommandLine(url);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = hwnd_;
    info.lpVerb = L"open";
    info.lpFile = L"msedge.exe";
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    if (config_.saveRunCount) {
        ++link->runCount;
        storageService_.IncrementRunCount(link->id, link->runCount);
    }
    if (config_.hideAfterLink) {
        HideMainWindowAfterLink();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenContainingFolder(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    std::wstring error;
    if (!ShellItemService::OpenContainingLocation(hwnd_, *link, error)) {
        ShowToast(error.empty() ? L"无法打开所在位置。" : error, ThemedToastRole::Warning);
    }
}

bool MainWindow::LinkCenterScreenPoint(int linkId, POINT& screenPoint) const {
    POINT clientPoint{};
    if (!quattro::windows::TryMatchedAreaCenterPoint(
            hitAreas_,
            [linkId](const HitArea& area) {
                return area.kind == HitKind::Link && area.id == linkId;
            },
            clientPoint)) {
        return false;
    }
    screenPoint = clientPoint;
    return ClientToScreen(hwnd_, &screenPoint) != FALSE;
}

ShellContextMenuTrackingOptions MainWindow::TrackedShellMenuOptions() const {
    ShellContextMenuTrackingOptions options;
    for (const auto& provider : TrackedContextMenuProviders()) {
        options.*(provider.trackingMember) = config_.*(provider.configMember);
    }
    return options;
}

void MainWindow::ShowWindowsContextMenu(int linkId, POINT screenPoint) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    DockAutoHidePause dockPause(*this);
    const ShellContextMenuTrackingOptions tracking = TrackedShellMenuOptions();
    ShellContextMenuTrackingOptions nativeTracking = tracking;
    nativeTracking.terminal = false;
    ShellContextMenuSnapshot snapshot;
    BeginPopupMenuSession();
    ShellItemService::ShowNativeContextMenu(
        hwnd_,
        *link,
        screenPoint,
        nativeTracking,
        &snapshot);
    EndPopupMenuSession();
    shellContextMenuCache_.Update(*link, snapshot, nativeTracking);
    if (tracking.terminal) {
        ShellContextMenuTrackingOptions terminalOnly;
        terminalOnly.terminal = true;
        ShellContextMenuSnapshot terminalSnapshot;
        terminalSnapshot.complete = true;
        const auto terminalContext = TerminalContextMenuService::DetectAvailablePrograms();
        terminalSnapshot.items = TerminalContextMenuService::ItemsFor(*link, terminalContext);
        shellContextMenuCache_.Update(*link, terminalSnapshot, terminalOnly);
    }
}

void MainWindow::ExecuteTrackedShellMenuAction(std::size_t index) {
    if (index >= menuTrackedShellCommands_.size()) {
        return;
    }
    Link* link = FindLink(CommandLinkId());
    if (!link) {
        return;
    }
    const auto& command = menuTrackedShellCommands_[index];
    std::wstring error;
    const bool invoked = command.actionKind == ShellContextMenuActionKind::Terminal
        ? TerminalContextMenuService::Invoke(hwnd_, command, error)
        : ShellItemService::InvokeTrackedContextMenuItem(hwnd_, *link, command, error);
    if (!invoked) {
        MessageBoxW(
            hwnd_,
            error.empty() ? L"执行原生菜单命令失败。" : error.c_str(),
            L"右键菜单",
            MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::CreateDesktopShortcut(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    const std::filesystem::path desktop = DesktopDirectory();
    if (desktop.empty()) {
        MessageBoxW(hwnd_, L"无法定位桌面目录。", L"创建桌面快捷方式", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring extension = IsUrlLink(*link) ? L".url" : L".lnk";
    std::filesystem::path shortcutPath = desktop / (SafeFileName(link->name) + extension);
    for (int index = 2; FileExists(shortcutPath) && index < 1000; ++index) {
        shortcutPath = desktop / (SafeFileName(link->name) + L" (" + std::to_wstring(index) + L")" + extension);
    }

    const bool ok = IsUrlLink(*link)
        ? CreateUrlShortcutFile(*link, shortcutPath)
        : CreateShellShortcutFile(*link, shortcutPath);
    if (!ok) {
        MessageBoxW(hwnd_, L"创建桌面快捷方式失败。", L"创建桌面快捷方式", MB_OK | MB_ICONWARNING);
        return;
    }
    ShowToast(L"桌面快捷方式已创建。", ThemedToastRole::Success);
}

void MainWindow::OpenSystemProperties(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    ShellItemService::OpenProperties(hwnd_, *link);
}

void MainWindow::ClearCurrentTagLinks() {
    const int tagId = CommandTagId();
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    std::vector<int> linkIds;
    for (const auto& link : model_.links) {
        if (link.parentGroup == tagId) {
            linkIds.push_back(link.id);
        }
    }
    if (linkIds.empty()) {
        ShowToast(L"当前页面没有可清空的应用。", ThemedToastRole::Info);
        return;
    }
    std::wstring message = L"确定清空“" + tag->name + L"”中的全部应用？";
    if (MessageBoxW(hwnd_, message.c_str(), L"清空本页应用", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    for (int id : linkIds) {
        if (!storageService_.DeleteLink(id)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"清空本页应用", MB_OK | MB_ICONWARNING);
            return;
        }
        shellContextMenuCache_.Remove(id);
    }
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [tagId](const Link& link) {
        return link.parentGroup == tagId;
    }), model_.links.end());
    if (selectedLinkId_ > 0 && !FindLink(selectedLinkId_)) {
        selectedLinkId_ = 0;
    }
    RegisterConfiguredHotKeys();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(
        L"已清空“" + tag->name + L"”中的 " + std::to_wstring(linkIds.size()) + L" 个应用。",
        ThemedToastRole::Info);
}

void MainWindow::AddTodoItem() {
    Group* tag = FindGroup(currentTagId_);
    if (!tag || !IsTodoItemsTag(*tag)) {
        return;
    }

    TodoItem item;
    item.tagId = currentTagId_;
    item.title.clear();
    item.enabled = true;
    item.scheduleKind = TodoScheduleKind::None;
    item.pos = -1;
    if (!TodoEditDialog::Show(hwnd_, instance_, theme_, item, true)) {
        return;
    }
    item.tagId = currentTagId_;
    item.pos = -1;
    if (!storageService_.InsertTodoItem(item)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.todos.push_back(item);
    selectedTodoId_ = item.id;
    CheckTodoReminders();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EditTodoItem(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    TodoItem edited = *item;
    if (!TodoEditDialog::Show(hwnd_, instance_, theme_, edited, false)) {
        return;
    }
    edited.id = item->id;
    edited.tagId = item->tagId;
    edited.pos = item->pos;
    edited.createdAt = item->createdAt;
    if (edited.nextDueAt != item->nextDueAt || edited.scheduleKind != item->scheduleKind ||
        edited.cronExpression != item->cronExpression || edited.repeatMode != item->repeatMode ||
        edited.repeatInterval != item->repeatInterval) {
        ResetTodoReminderState(edited);
    }
    if (!storageService_.UpdateTodoItem(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    *item = edited;
    selectedTodoId_ = item->id;
    CheckTodoReminders();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteTodoItem(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    std::wstring message = L"确定删除待办事项“" + item->title + L"”？";
    if (MessageBoxW(hwnd_, message.c_str(), L"删除待办事项", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (!storageService_.DeleteTodoItem(todoId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring deletedTitle = item->title;
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [todoId](const TodoItem& item) {
        return item.id == todoId;
    }), model_.todos.end());
    if (selectedTodoId_ == todoId) {
        selectedTodoId_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已删除待办“" + deletedTitle + L"”。", ThemedToastRole::Info);
}

void MainWindow::ToggleTodoDone(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    const bool complete = item->completedAt.empty();
    const std::wstring now = CurrentTodoTimestamp();
    if (!storageService_.SetTodoCompleted(todoId, complete)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    if (complete) {
        const TodoCompletionOutcome outcome = CompleteTodoOccurrence(*item, now);
        if (outcome == TodoCompletionOutcome::AdvancedRecurring && !item->nextDueAt.empty()) {
            ShowToast(L"本次已完成，下次提醒：" + item->nextDueAt, ThemedToastRole::Info);
        }
    } else {
        item->completedAt.clear();
        if (IsRecurringTodoSchedule(item->scheduleKind) && item->nextDueAt.empty()) {
            item->nextDueAt = ComputeNextTodoDueAt(*item, now);
        }
        ResetTodoReminderState(*item);
        item->updatedAt = now;
    }
    selectedTodoId_ = todoId;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleTodoEnabled(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    const bool enable = !item->enabled;
    if (!storageService_.SetTodoEnabled(todoId, enable)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    item->enabled = enable;
    ResetTodoReminderState(*item);
    item->updatedAt = CurrentTodoTimestamp();
    selectedTodoId_ = todoId;
    if (enable) {
        CheckTodoReminders();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(
        enable ? (L"已启用“" + item->title + L"”的提醒。") : (L"已暂停“" + item->title + L"”的提醒。"),
        enable ? ThemedToastRole::Success : ThemedToastRole::Info);
}

bool MainWindow::SaveTodoReminderState(TodoItem& item, const wchar_t* context) {
    item.updatedAt = CurrentTodoTimestamp();
    if (storageService_.UpdateTodoReminderState(item)) {
        return true;
    }
    WriteAppLog(
        std::wstring(L"待办提醒状态保存失败。context=") + (context ? context : L"") +
        L", todo_id=" + std::to_wstring(item.id) + L", error=" + storageService_.lastError());
    return false;
}

void MainWindow::MarkTodoReminderViewed(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item || EffectiveTodoReminderDueAt(*item).empty()) {
        return;
    }
    const TodoItem previous = *item;
    ::MarkTodoReminderViewed(*item, CurrentTodoTimestamp());
    if (!SaveTodoReminderState(*item, L"标记已查看")) {
        *item = previous;
        ShowToast(L"保存提醒状态失败。", ThemedToastRole::Danger);
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已标记“" + item->title + L"”的本次提醒为已查看。", ThemedToastRole::Success);
}

void MainWindow::IgnoreTodoReminder(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    const std::wstring now = CurrentTodoTimestamp();
    if (!item || !IsTodoReminderDue(*item, now)) {
        return;
    }
    const TodoItem previous = *item;
    IgnoreTodoReminderOccurrence(*item);
    if (!SaveTodoReminderState(*item, L"忽略本次")) {
        *item = previous;
        ShowToast(L"保存提醒状态失败。", ThemedToastRole::Danger);
        return;
    }
    HideTodoReminderPanel();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已忽略“" + item->title + L"”的本次提醒。", ThemedToastRole::Info);
}

void MainWindow::SnoozeTodoReminder(int todoId, int minutes) {
    TodoItem* item = FindTodoItem(todoId);
    const std::wstring now = CurrentTodoTimestamp();
    if (!item || !IsTodoReminderDue(*item, now)) {
        return;
    }
    const TodoItem previous = *item;
    if (!::SnoozeTodoReminder(*item, now, minutes)) return;
    if (!SaveTodoReminderState(*item, L"稍后提醒")) {
        *item = previous;
        ShowToast(L"保存稍后提醒时间失败。", ThemedToastRole::Danger);
        return;
    }
    HideTodoReminderPanel();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(
        L"将在 " + item->snoozedUntil + L" 再次提醒“" + item->title + L"”。",
        ThemedToastRole::Info,
        5000);
}

void MainWindow::ViewPendingTodoReminders() {
    if (pendingReminderTodoIds_.empty()) {
        WakeUp();
        return;
    }

    const std::vector<int> todoIds = pendingReminderTodoIds_;
    pendingReminderTodoIds_.clear();
    WakeUp();
    TodoItem* first = nullptr;
    const std::wstring now = CurrentTodoTimestamp();
    for (int todoId : todoIds) {
        TodoItem* item = FindTodoItem(todoId);
        if (!item) {
            continue;
        }
        if (!first) {
            first = item;
        }
        const TodoItem previous = *item;
        ::MarkTodoReminderViewed(*item, now);
        if (!SaveTodoReminderState(*item, L"系统通知点击查看")) {
            *item = previous;
        }
    }
    if (!first) {
        return;
    }
    if (const Group* tag = FindGroup(first->tagId)) {
        if (currentGroupId_ != tag->parentGroup) {
            SelectGroup(tag->parentGroup);
        }
        if (currentTagId_ != tag->id) {
            SelectTag(tag->id);
        }
    }
    selectedTodoId_ = first->id;
    EnsureTodoVisible(selectedTodoId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ClearDoneTodos() {
    Group* tag = FindGroup(currentTagId_);
    if (!tag || !IsTodoItemsTag(*tag)) {
        return;
    }
    std::vector<int> doneIds;
    for (const auto& item : model_.todos) {
        if (item.tagId == currentTagId_ && !item.completedAt.empty()) {
            doneIds.push_back(item.id);
        }
    }
    if (doneIds.empty()) {
        ShowToast(L"没有已完成的待办事项。", ThemedToastRole::Info);
        return;
    }
    if (MessageBoxW(hwnd_, L"确定清空已完成待办事项？", L"清空已完成", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    for (int id : doneIds) {
        if (!storageService_.DeleteTodoItem(id)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"清空已完成", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [&](const TodoItem& item) {
        return std::find(doneIds.begin(), doneIds.end(), item.id) != doneIds.end();
    }), model_.todos.end());
    if (FindTodoItem(selectedTodoId_) == nullptr) {
        selectedTodoId_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"已清空 " + std::to_wstring(doneIds.size()) + L" 项已完成待办。", ThemedToastRole::Success);
}

int MainWindow::OverdueTodoCount(int tagId, int* recurringCount, const std::wstring& now) const {
    int count = 0;
    int recurring = 0;
    const std::wstring effectiveNow = now.empty() ? CurrentTodoTimestamp() : now;
    for (const auto& item : model_.todos) {
        if (item.tagId != tagId || !IsTodoOverdueAt(item, effectiveNow)) continue;
        ++count;
        if (IsRecurringTodoSchedule(item.scheduleKind)) ++recurring;
    }
    if (recurringCount) *recurringCount = recurring;
    return count;
}

void MainWindow::CompleteOverdueTodosInTag(int tagId, bool confirm) {
    Group* tag = FindGroup(tagId);
    if (!tag || !IsTodoItemsTag(*tag)) return;

    const std::wstring now = CurrentTodoTimestamp();
    int recurringCount = 0;
    const int overdueCount = OverdueTodoCount(tagId, &recurringCount, now);
    if (overdueCount <= 0) {
        ShowToast(L"当前标签没有逾期待办。", ThemedToastRole::Info);
        return;
    }
    if (confirm) {
        std::wstring message = L"将完成当前标签中的 " + std::to_wstring(overdueCount) + L" 项逾期待办。";
        if (recurringCount > 0) {
            message += L"\n其中 " + std::to_wstring(recurringCount) +
                L" 项为重复待办；未达到重复上限的项目将推进到下一次提醒。";
        }
        message += L"\n\n是否继续？";
        if (MessageBoxW(hwnd_, message.c_str(), L"完成全部逾期待办", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    TodoBatchCompleteResult result;
    if (!storageService_.CompleteOverdueTodos(tagId, now, result)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"完成全部逾期待办", MB_OK | MB_ICONWARNING);
        return;
    }
    if (result.updatedItems.empty()) {
        ShowToast(L"当前标签没有逾期待办。", ThemedToastRole::Info);
        return;
    }

    std::unordered_set<int> updatedIds;
    for (const auto& updated : result.updatedItems) {
        updatedIds.insert(updated.id);
        if (TodoItem* item = FindTodoItem(updated.id)) *item = updated;
    }
    std::erase_if(pendingReminderTodoIds_, [&](int id) { return updatedIds.contains(id); });
    std::erase_if(reminderRetryAfter_, [&](const auto& entry) {
        for (int id : updatedIds) {
            if (entry.first.starts_with(std::to_wstring(id) + L"|")) return true;
        }
        return false;
    });
    HideTodoReminderPanel();
    InvalidateRect(hwnd_, nullptr, FALSE);

    std::wstring summary = L"已处理 " + std::to_wstring(result.updatedItems.size()) + L" 项逾期待办";
    if (result.completedCount > 0 || result.advancedRecurringCount > 0) {
        summary += L"：";
        if (result.completedCount > 0) {
            summary += std::to_wstring(result.completedCount) + L" 项已完成";
        }
        if (result.advancedRecurringCount > 0) {
            if (result.completedCount > 0) summary += L"，";
            summary += std::to_wstring(result.advancedRecurringCount) + L" 项已推进到下一次提醒";
        }
    }
    summary += L"。";
    ShowToast(summary, ThemedToastRole::Success, 5000);
}

void MainWindow::CheckTodoReminders() {
    const std::wstring now = CurrentTodoTimestamp();
    const ULONGLONG tick = GetTickCount64();
    std::vector<TodoItem*> dueItems;
    for (auto& item : model_.todos) {
        if (!IsTodoReminderDue(item, now) || IsTodoReminderDelivered(item)) {
            continue;
        }
        const std::wstring key = TodoReminderKey(item);
        const auto retry = reminderRetryAfter_.find(key);
        if (retry != reminderRetryAfter_.end() && tick < retry->second) {
            continue;
        }
        dueItems.push_back(&item);
    }
    if (dueItems.empty()) {
        return;
    }

    const bool useAppToast = IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && !dockHidden_;
    lastReminderChannel_ = useAppToast ? 1 : 2;
    lastReminderBatchCount_ = static_cast<int>(dueItems.size());
    bool delivered = useAppToast
        ? ShowTodoReminderPanel(dueItems)
        : ShowTodoSystemNotification(dueItems);
    if (FailFirstReminderForIsolatedTest() && !testReminderFailureConsumed_) {
        testReminderFailureConsumed_ = true;
        delivered = false;
        WriteAppLog(L"后台验收已注入一次待办提醒发送失败。count=" + std::to_wstring(dueItems.size()));
    }
    if (!delivered) {
        const ULONGLONG retryDelayMs = ReminderRetryDelayMs();
        for (const auto* item : dueItems) {
            reminderRetryAfter_[TodoReminderKey(*item)] = tick + retryDelayMs;
        }
        WriteAppLog(L"待办提醒发送失败，将在 " + std::to_wstring(retryDelayMs) +
                    L" 毫秒后重试。count=" + std::to_wstring(dueItems.size()));
        return;
    }

    for (auto* item : dueItems) {
        reminderRetryAfter_.erase(TodoReminderKey(*item));
        const TodoItem previous = *item;
        ::MarkTodoReminderSent(*item, now);
        if (!SaveTodoReminderState(*item, L"发送提醒")) {
            *item = previous;
            reminderRetryAfter_[TodoReminderKey(*item)] = tick + ReminderRetryDelayMs();
        }
    }
}

bool MainWindow::ShowTodoReminderPanel(const std::vector<TodoItem*>& items) {
    if (items.empty()) {
        return false;
    }
    std::wstring text;
    if (items.size() == 1) {
        const TodoItem& item = *items.front();
        text = L"待办提醒\r\n" + LimitNotificationText(item.title, 80) +
            (Trim(item.content).empty() ? L"" : (L"\r\n" + LimitNotificationText(Trim(item.content), 160)));
    } else {
        text = L"有 " + std::to_wstring(items.size()) + L" 项待办已到期";
        const std::size_t previewCount = std::min<std::size_t>(3, items.size());
        for (std::size_t index = 0; index < previewCount; ++index) {
            text += L"\r\n• " + LimitNotificationText(items[index]->title, 60);
        }
        if (items.size() > previewCount) {
            text += L"\r\n另有 " + std::to_wstring(items.size() - previewCount) + L" 项";
        }
        text += L"\r\n可在待办事项中查看或稍后提醒。";
    }
    CreateTooltip();
    if (!embeddedUi_) {
        return false;
    }
    ThemedToastOptions options{};
    options.anchor = ThemedToastAnchor::ScreenBottomRight;
    options.role = ThemedToastRole::Info;
    options.multiline = true;
    options.durationMs = 12000;
    options.maxWidth = 340;
    embeddedUi_->ui().ShowToast(text, options);
    return true;
}

void MainWindow::HideTodoReminderPanel() {
    if (embeddedUi_) embeddedUi_->ui().HideToast();
}

bool MainWindow::EnsureNotificationIcon() {
    if (trayIconVisible_) {
        return true;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_QUATTRO_TRAY;
    data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wcsncpy_s(data.szTip, TrayTooltipText().c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        trayIconVisible_ = true;
    }
    return trayIconVisible_;
}

bool MainWindow::ShowTodoSystemNotification(const std::vector<TodoItem*>& items) {
    if (items.empty()) {
        return false;
    }
    auto rememberPendingItems = [&]() {
        pendingReminderTodoIds_.clear();
        pendingReminderTodoIds_.reserve(items.size());
        for (const auto* item : items) {
            pendingReminderTodoIds_.push_back(item->id);
        }
    };
    if (SuppressTrayForIsolatedTest()) {
        rememberPendingItems();
        WriteAppLog(L"后台验收已记录系统待办通知意图，未调用 Shell_NotifyIcon。count=" + std::to_wstring(items.size()));
        return true;
    }
    if (!EnsureNotificationIcon()) {
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    std::wstring title;
    std::wstring body;
    if (items.size() == 1) {
        const TodoItem& item = *items.front();
        title = LimitNotificationText(item.title, 63);
        body = LimitNotificationText(Trim(item.content).empty() ? TodoReminderText(item) : Trim(item.content), 255);
    } else {
        title = std::to_wstring(items.size()) + L" 项待办已到期";
        const std::size_t previewCount = std::min<std::size_t>(3, items.size());
        for (std::size_t index = 0; index < previewCount; ++index) {
            if (!body.empty()) body += L"；";
            body += items[index]->title;
        }
        if (items.size() > previewCount) {
            body += L"；另有 " + std::to_wstring(items.size() - previewCount) + L" 项";
        }
        body = LimitNotificationText(body, 255);
    }
    wcscpy_s(data.szInfoTitle, title.empty() ? L"待办提醒" : title.c_str());
    wcscpy_s(data.szInfo, body.empty() ? L"待办事项时间到了。" : body.c_str());
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        return false;
    }
    rememberPendingItems();
    return true;
}

void MainWindow::ShowClipboardImportNotification(int count, int failedCount, const std::wstring& pathDetail) {
    if (count <= 0 && failedCount <= 0) {
        return;
    }
    const Group* tag = FindGroup(currentTagId_);
    const ImportToastSummary summary{count, failedCount};
    const std::wstring body = pathDetail.empty() || failedCount > 0
        ? ImportToastText(summary, tag ? tag->name : L"")
        : LimitNotificationText(pathDetail, 255);
    if (IsWindowVisible(hwnd_)) {
        ShowToast(body, ImportToastRole(summary), failedCount > 0 ? 5000 : 0);
        return;
    }
    if (!EnsureNotificationIcon()) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = failedCount > 0 ? NIIF_WARNING : NIIF_INFO;
    const std::wstring title = failedCount > 0 ? L"导入启动项未全部完成" : L"已添加启动项";
    wcscpy_s(data.szInfoTitle, LimitNotificationText(title, 63).c_str());
    wcscpy_s(data.szInfo, body.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MainWindow::CopyLinkPath(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    const bool isUrl = IsUrlLink(*link);
    if (!OpenClipboard(hwnd_)) {
        ShowToast(L"复制失败，剪贴板被其他程序占用。", ThemedToastRole::Danger);
        return;
    }
    EmptyClipboard();
    const std::wstring text = ClipboardPathTextForLink(*link, isUrl);
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool copied = false;
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
            memory = nullptr;
            copied = true;
        }
    }
    if (memory) {
        GlobalFree(memory);
    }
    CloseClipboard();
    if (copied) {
        ShowToast(isUrl ? L"网址已复制到剪贴板。" : L"路径已复制到剪贴板。", ThemedToastRole::Success);
    } else {
        ShowToast(L"复制失败。", ThemedToastRole::Danger);
    }
}

void MainWindow::OpenSettings() {
    AppConfig next = config_;
    bool importedData = false;
    auto applySettings = [this, &next](const AppConfig& applied, bool appliedImportedData) -> bool {
        CommitSettingsConfig(applied, appliedImportedData);
        next = config_;
        return mainHotKeyRegistered_;
    };
    auto resetContextMenu = [this, &next]() -> bool {
        if (!shellContextMenuCache_.Reset()) {
            return false;
        }
        AppConfig reset = config_;
        for (const auto& provider : TrackedContextMenuProviders()) {
            reset.*(provider.configMember) = false;
        }
        CommitSettingsConfig(reset, false);
        next = config_;
        return true;
    };
    auto applyContextMenuRefresh = [this](const ShellContextMenuRefreshResult& result) {
        ShellContextMenuTrackingOptions nativeTracking = result.tracking;
        nativeTracking.terminal = false;
        ShellContextMenuTrackingOptions terminalTracking;
        terminalTracking.terminal = result.tracking.terminal;
        std::vector<ShellContextMenuCacheUpdate> cacheUpdates;
        cacheUpdates.reserve(result.updates.size() * 2);
        for (const auto& update : result.updates) {
            if (update.hasNativeSnapshot) {
                cacheUpdates.push_back(ShellContextMenuCacheUpdate{
                    update.link, update.nativeSnapshot, nativeTracking});
            }
            if (update.hasTerminalSnapshot) {
                cacheUpdates.push_back(ShellContextMenuCacheUpdate{
                    update.link, update.terminalSnapshot, terminalTracking});
            }
        }
        shellContextMenuCache_.UpdateBatch(cacheUpdates);
    };
    if (!ShowSettingsDialog(
            hwnd_,
            instance_,
            next,
            theme_,
            appDirectory_,
            httpRootBaseDirectory_,
            &importedData,
            &httpServerService_,
            mainHotKeyRegistered_,
            processLocatorHotKeyRegistered_,
            copySelectedPathsHotKeyRegistered_,
            applySettings,
            resetContextMenu,
            model_.links,
            {},
            applyContextMenuRefresh)) {
        if (importedData) {
            model_ = storageService_.Load();
            RestoreLegacyBuiltinSystemFunctionKeys();
            SelectInitialItems();
            RegisterConfiguredHotKeys();
            ClearUiBitmaps();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    CommitSettingsConfig(next, importedData);
    ShowToast(importedData ? L"设置和导入数据已保存。" : L"设置已保存。", ThemedToastRole::Success);
}

void MainWindow::CommitSettingsConfig(const AppConfig& next, bool importedData) {
    AppConfig previous = config_;
    config_ = next;
    configService_.Save(config_);
    for (const auto& provider : TrackedContextMenuProviders()) {
        if (previous.*(provider.configMember) && !(config_.*(provider.configMember))) {
            shellContextMenuCache_.RemoveProvider(provider.providerId);
        }
    }
    if (previous.loggingEnabled != config_.loggingEnabled) {
        SetAppLogEnabled(config_.loggingEnabled);
        WriteAppLog(config_.loggingEnabled ? L"日志已启用。" : L"日志已关闭。");
    }
    std::wstring webDavRecoveryError;
    WebDavRecoveryService().Save(config_, webDavRecoveryError);
    if (!webDavRecoveryError.empty()) {
        WriteAppLog(webDavRecoveryError);
    }
    SyncAutoRun(previous);
    ApplyConfigRuntimeChanges(previous);
    if (importedData) {
        model_ = storageService_.Load();
        RestoreLegacyBuiltinSystemFunctionKeys();
        SelectInitialItems();
        RegisterConfiguredHotKeys();
        ClearUiBitmaps();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenBuiltinTool(std::size_t index) {
    if (menuToolEngines_.empty()) {
        const auto plugins = BuiltinToolMenuPlugins(pluginRegistry_.LoadPlugins());
        for (const auto& plugin : plugins) {
            if (plugin.kind != L"builtin-tool" || plugin.engine.empty()) {
                continue;
            }
            if (menuToolEngines_.size() >= ID_MENU_TOOL_LIMIT) {
                break;
            }
            menuToolEngines_.push_back(plugin.engine);
            menuToolEnabled_.push_back(plugin.enabled);
        }
    }
    if (index >= menuToolEngines_.size()) {
        return;
    }
    if (index < menuToolEnabled_.size() && !menuToolEnabled_[index]) {
        return;
    }
    OpenBuiltinToolEngine(menuToolEngines_[index]);
}

void MainWindow::OpenBuiltinToolEngine(const std::wstring& engine, bool locateProcessOnOpen) {
    if (engine == L"app-launch-locker") {
        const EmbeddedExecutablePrepareResult prepared = PrepareEmbeddedExecutable(
            L"app-launch-locker", {QuattroEmbeddedExecutableRootDirectory()});
        if (!prepared.success) {
            WriteAppLog(L"准备自启动管理工具失败: " + prepared.message);
            ShowThemedMessageBox(hwnd_, instance_, theme_, prepared.message, L"自启动管理", MB_OK | MB_ICONWARNING);
            return;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                prepared.path.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                prepared.path.parent_path().c_str(), &startup, &process)) {
            const std::wstring error = L"无法启动自启动管理工具：" + FormatLastError(GetLastError());
            WriteAppLog(error);
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"自启动管理", MB_OK | MB_ICONWARNING);
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return;
    }
    if (engine == L"ad-block") {
        const EmbeddedExecutablePrepareResult prepared = PrepareEmbeddedExecutable(
            L"app-launch-locker", {QuattroEmbeddedExecutableRootDirectory()});
        if (!prepared.success) {
            WriteAppLog(L"准备广告拦截工具失败: " + prepared.message);
            ShowThemedMessageBox(hwnd_, instance_, theme_, prepared.message, L"广告拦截", MB_OK | MB_ICONWARNING);
            return;
        }
        // CreateProcessW 的命令行参数必须为可写缓冲区。
        std::wstring commandLine = L"\"" + prepared.path.wstring() + L"\" --ad-block";
        std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                prepared.path.c_str(), commandBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                prepared.path.parent_path().c_str(), &startup, &process)) {
            const std::wstring error = L"无法启动广告拦截工具：" + FormatLastError(GetLastError());
            WriteAppLog(error);
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"广告拦截", MB_OK | MB_ICONWARNING);
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return;
    }
    ShowBuiltinTool(hwnd_, instance_, theme_, pluginRegistry_, config_, engine, locateProcessOnOpen);
}

void MainWindow::ResetLayoutToDefaults() {
    AppConfig previous = config_;
    const AppConfig defaults;

    config_.showTitle = defaults.showTitle;
    config_.showGroup = defaults.showGroup;
    config_.showTag = defaults.showTag;
    config_.showToolboxButton = defaults.showToolboxButton;
    config_.showSkinButton = defaults.showSkinButton;
    config_.topMost = defaults.topMost;
    config_.groupRight = defaults.groupRight;
    config_.tagRight = defaults.tagRight;
    config_.tagAlign = defaults.tagAlign;
    config_.width = defaults.width;
    config_.height = defaults.height;
    config_.groupWidth = defaults.groupWidth;
    config_.autoGroupWidth = defaults.autoGroupWidth;
    config_.tagWidth = defaults.tagWidth;
    config_.autoTagHeight = defaults.autoTagHeight;
    config_.attrWidth = defaults.attrWidth;
    config_.attrHeight = defaults.attrHeight;
    config_.alpha = defaults.alpha;

    const POINT position = ClampWindowPosition(defaults.posX, defaults.posY, defaults.width, defaults.height);
    config_.posX = position.x;
    config_.posY = position.y;

    dockHidden_ = false;
    dockRestoreRect_ = {};
    dockHideDueTick_ = GetTickCount64() + kDockRestoreGraceMs;
    groupScrollOffset_ = 0.0f;
    tagScrollOffset_ = 0.0f;
    linkScrollOffset_ = 0.0f;

    configService_.SaveWindowState(config_);
    ApplyConfigRuntimeChanges(previous);

    if (IsIconic(hwnd_)) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_RESTORE);
    }
    ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
    SetWindowPos(
        hwnd_,
        MainWindowTopMostInsertAfter(config_.topMost),
        config_.posX,
        config_.posY,
        config_.width,
        config_.height,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ActivateWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowToast(L"窗口布局已恢复默认。", ThemedToastRole::Success);
}

void MainWindow::ClearIconCache() {
    if (iconService_.ClearDiskCache()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowToast(L"图标缓存已清理。", ThemedToastRole::Success);
    } else {
        WriteAppLog(L"图标缓存清理失败。");
        MessageBoxW(hwnd_, L"图标缓存清理失败，请确认 icons/cache 目录可写。", L"图标缓存", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::RefreshAllIcons() {
    BeginResourceRefresh(L"全部启动项");
    std::unordered_set<int> ordinaryTagIds;
    for (const auto& tag : model_.groups) {
        if (IsOrdinaryTag(tag)) {
            ordinaryTagIds.insert(tag.id);
        }
    }
    TerminalContextMenuRefreshContext terminalContext;
    const TerminalContextMenuRefreshContext* terminalContextPtr = nullptr;
    if (config_.trackTerminalContextMenu) {
        terminalContext = TerminalContextMenuService::DetectAvailablePrograms();
        terminalContextPtr = &terminalContext;
    }
    int completed = 0;
    int pending = 0;
    int failed = 0;
    for (auto& link : model_.links) {
        if (ordinaryTagIds.contains(link.parentGroup) && !BuiltinSystemFunctionForLink(link)) {
            switch (RefreshLinkResources(link, terminalContextPtr)) {
            case LinkResourceRefreshState::Complete: ++completed; break;
            case LinkResourceRefreshState::Pending: ++pending; break;
            case LinkResourceRefreshState::Failed: ++failed; break;
            }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    CompleteResourceRefreshStart(completed, pending, failed);
}

MainWindow::LinkResourceRefreshState MainWindow::RefreshLinkResources(
    Link& link,
    const TerminalContextMenuRefreshContext* terminalContext) {
    if (IsUrlLink(link)) {
        shellContextMenuCache_.Remove(link.id);
        if (urlIconDownloadService_.RequestManualRefresh(hwnd_, WM_QUATTRO_URL_ICON_DOWNLOADED, link)) {
            pendingUrlIconRefreshIds_.insert(link.id);
            return LinkResourceRefreshState::Pending;
        }
        return LinkResourceRefreshState::Failed;
    }
    bool success = iconService_.RefreshDiskCache(link);
    const ShellContextMenuTrackingOptions tracking = TrackedShellMenuOptions();
    if (!tracking.Any()) {
        return success ? LinkResourceRefreshState::Complete : LinkResourceRefreshState::Failed;
    }
    ShellContextMenuTrackingOptions nativeTracking = tracking;
    nativeTracking.terminal = false;
    if (nativeTracking.Any()) {
        ShellContextMenuSnapshot snapshot;
        if (ShellItemService::QueryTrackedContextMenu(hwnd_, link, nativeTracking, snapshot)) {
            shellContextMenuCache_.Update(link, snapshot, nativeTracking);
        } else {
            success = false;
        }
    }
    if (tracking.terminal) {
        TerminalContextMenuRefreshContext localContext;
        if (!terminalContext) {
            localContext = TerminalContextMenuService::DetectAvailablePrograms();
            terminalContext = &localContext;
        }
        ShellContextMenuTrackingOptions terminalOnly;
        terminalOnly.terminal = true;
        ShellContextMenuSnapshot terminalSnapshot;
        terminalSnapshot.complete = true;
        terminalSnapshot.items = TerminalContextMenuService::ItemsFor(link, *terminalContext);
        shellContextMenuCache_.Update(link, terminalSnapshot, terminalOnly);
    }
    return success ? LinkResourceRefreshState::Complete : LinkResourceRefreshState::Failed;
}

void MainWindow::BeginResourceRefresh(const std::wstring& scopeText) {
    pendingUrlIconRefreshIds_.clear();
    pendingResourceRefreshScope_ = scopeText;
    pendingResourceRefreshCompleted_ = 0;
    pendingResourceRefreshFailed_ = 0;
    ShowToast(L"正在刷新" + scopeText + L"…", ThemedToastRole::Info, 5000);
}

void MainWindow::CompleteResourceRefreshStart(int completed, int pending, int failed) {
    pendingResourceRefreshCompleted_ = completed;
    pendingResourceRefreshFailed_ = failed;
    if (pending <= 0) {
        ShowResourceRefreshResult(completed, failed);
        return;
    }
    std::wstring message = L"正在刷新" + pendingResourceRefreshScope_ + L"：" + std::to_wstring(pending) + L" 个网址图标在后台处理";
    if (completed > 0) {
        message += L"，" + std::to_wstring(completed) + L" 项已处理";
    }
    if (failed > 0) {
        message += L"，" + std::to_wstring(failed) + L" 项未能开始";
    }
    message += L"。";
    ShowToast(message, failed > 0 ? ThemedToastRole::Warning : ThemedToastRole::Info, 5000);
}

void MainWindow::ShowResourceRefreshResult(int completed, int failed) {
    if (completed <= 0 && failed <= 0) {
        ShowToast(pendingResourceRefreshScope_ + L"中没有需要刷新的启动项。", ThemedToastRole::Info);
        return;
    }
    if (failed <= 0) {
        ShowToast(
            L"已刷新" + pendingResourceRefreshScope_ + L"（" + std::to_wstring(completed) + L" 项）。",
            ThemedToastRole::Success);
        return;
    }
    ShowToast(
        L"已刷新 " + std::to_wstring(completed) + L" 项，" + std::to_wstring(failed) + L" 项失败。",
        ThemedToastRole::Warning,
        5000);
}

void MainWindow::RefreshTagLinks(int tagId) {
    const Group* tag = FindGroup(tagId);
    if (!tag || !IsOrdinaryTag(*tag)) {
        return;
    }
    BeginResourceRefresh(L"标签“" + tag->name + L"”");
    TerminalContextMenuRefreshContext terminalContext;
    const TerminalContextMenuRefreshContext* terminalContextPtr = nullptr;
    if (config_.trackTerminalContextMenu) {
        terminalContext = TerminalContextMenuService::DetectAvailablePrograms();
        terminalContextPtr = &terminalContext;
    }
    int completed = 0;
    int pending = 0;
    int failed = 0;
    for (auto& link : model_.links) {
        if (link.parentGroup == tagId && !BuiltinSystemFunctionForLink(link)) {
            switch (RefreshLinkResources(link, terminalContextPtr)) {
            case LinkResourceRefreshState::Complete: ++completed; break;
            case LinkResourceRefreshState::Pending: ++pending; break;
            case LinkResourceRefreshState::Failed: ++failed; break;
            }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    CompleteResourceRefreshStart(completed, pending, failed);
}

void MainWindow::RefreshGroupLinks(int groupId) {
    const Group* group = FindGroup(groupId);
    if (!group || group->parentGroup != 0) {
        return;
    }
    BeginResourceRefresh(L"分组“" + group->name + L"”");
    std::unordered_set<int> ordinaryTagIds;
    for (const auto& tag : model_.groups) {
        if (tag.parentGroup == groupId && IsOrdinaryTag(tag)) {
            ordinaryTagIds.insert(tag.id);
        }
    }
    TerminalContextMenuRefreshContext terminalContext;
    const TerminalContextMenuRefreshContext* terminalContextPtr = nullptr;
    if (config_.trackTerminalContextMenu) {
        terminalContext = TerminalContextMenuService::DetectAvailablePrograms();
        terminalContextPtr = &terminalContext;
    }
    int completed = 0;
    int pending = 0;
    int failed = 0;
    for (auto& link : model_.links) {
        if (ordinaryTagIds.contains(link.parentGroup) && !BuiltinSystemFunctionForLink(link)) {
            switch (RefreshLinkResources(link, terminalContextPtr)) {
            case LinkResourceRefreshState::Complete: ++completed; break;
            case LinkResourceRefreshState::Pending: ++pending; break;
            case LinkResourceRefreshState::Failed: ++failed; break;
            }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    CompleteResourceRefreshStart(completed, pending, failed);
}

void MainWindow::RefreshLinkIcon(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    BeginResourceRefresh(L"“" + link->name + L"”");
    const LinkResourceRefreshState state = RefreshLinkResources(*link);
    InvalidateRect(hwnd_, nullptr, FALSE);
    CompleteResourceRefreshStart(
        state == LinkResourceRefreshState::Complete ? 1 : 0,
        state == LinkResourceRefreshState::Pending ? 1 : 0,
        state == LinkResourceRefreshState::Failed ? 1 : 0);
}

void MainWindow::RequestInitialUrlIconDownload(const Link& link) {
    if (IsUrlLink(link)) {
        urlIconDownloadService_.RequestInitialDownload(hwnd_, WM_QUATTRO_URL_ICON_DOWNLOADED, link);
    }
}

void MainWindow::OnUrlIconDownloaded(int linkId, bool success) {
    bool refreshed = success;
    if (success) {
        if (Link* link = FindLink(linkId)) {
            refreshed = iconService_.RefreshDiskCache(*link);
        } else {
            refreshed = false;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (pendingUrlIconRefreshIds_.erase(linkId) == 0) {
        return;
    }
    if (refreshed) {
        ++pendingResourceRefreshCompleted_;
    } else {
        ++pendingResourceRefreshFailed_;
    }
    if (pendingUrlIconRefreshIds_.empty()) {
        ShowResourceRefreshResult(pendingResourceRefreshCompleted_, pendingResourceRefreshFailed_);
    }
}

void MainWindow::ShowAbout() {
    ShowAboutDialog(hwnd_, instance_, theme_, runningAsAdmin_);
}

void MainWindow::CheckForUpdates() {
    if (updateDownloadActive_) {
        ShowToast(L"更新包正在下载，请稍候。", ThemedToastRole::Info);
        return;
    }

    const std::wstring localVersion = QuattroVersionText();
    const std::wstring updateInfoUrl = UpdateCheckService::UpdateInfoUrlForConfig(config_.updateUrl);
    WriteAppLog(
        L"检查更新开始。local_version=" + localVersion +
        L"，update_info_url=" + updateInfoUrl +
        L"，configured_update_url=" + (Trim(config_.updateUrl).empty() ? L"(default)" : config_.updateUrl));

    UpdateCheckService service(appDirectory_, config_.updateUrl);
    UpdateReleaseInfo info;
    std::wstring error;
    if (!service.CheckLatest(info, error)) {
        WriteAppLog(
            L"检查更新失败。local_version=" + (info.currentVersion.empty() ? localVersion : info.currentVersion) +
            L"，update_info_url=" + updateInfoUrl +
            L"，error=" + (error.empty() ? L"未知错误" : error));
        MessageBoxW(hwnd_, error.empty() ? L"检查更新失败。" : error.c_str(), L"检查更新", MB_OK | MB_ICONWARNING);
        return;
    }

    const int versionComparison = UpdateCheckService::CompareVersions(info.currentVersion, info.latestVersion);
    WriteAppLog(
        L"检查更新完成。local_version=" + info.currentVersion +
        L"，remote_version=" + info.latestVersion +
        L"，compare=" + VersionCompareText(versionComparison) +
        L"，update_available=" + std::wstring(info.updateAvailable ? L"1" : L"0") +
        L"，asset=" + (info.assetName.empty() ? L"(none)" : info.assetName) +
        L"，asset_size=" + std::to_wstring(info.assetSizeBytes) +
        L"，asset_sha256=" + (info.expectedSha256.empty() ? L"(none)" : info.expectedSha256) +
        L"，checksum_url=" + (info.checksumDownloadUrl.empty() ? L"(none)" : info.checksumDownloadUrl) +
        L"，update_source=" + (info.sourceName.empty() ? L"(none)" : info.sourceName) +
        L"，source_manifest_url=" + (info.sourceManifestUrl.empty() ? L"(none)" : info.sourceManifestUrl) +
        L"，release_url=" + (info.releaseUrl.empty() ? L"(none)" : info.releaseUrl));

    if (!info.updateAvailable) {
        ShowToast(
            L"当前已是最新版本（" + FormatVersionForDisplay(info.currentVersion) + L"）。",
            ThemedToastRole::Info);
        return;
    }

    const UpdateCheckDialogChoice choice = ShowUpdateCheckDialog(hwnd_, instance_, theme_, info);
    if (choice == UpdateCheckDialogChoice::OpenRelease) {
        OpenConfiguredUrl(info.releaseUrl, L"检查更新");
        return;
    }
    if (choice != UpdateCheckDialogChoice::Download) {
        return;
    }

    UpdateDownloadResult download;
    updateDownloadActive_ = true;
    const bool downloaded = ShowUpdateDownloadDialog(hwnd_, instance_, theme_, appDirectory_, info, download, error);
    updateDownloadActive_ = false;
    if (!IsWindow(hwnd_)) {
        return;
    }
    if (!downloaded) {
        if (error == L"下载已取消。") {
            return;
        }
        MessageBoxW(hwnd_, error.empty() ? L"下载更新失败。" : error.c_str(), L"检查更新", MB_OK | MB_ICONWARNING);
        return;
    }
    WriteAppLog(
        L"更新包下载完成。version=" + info.latestVersion +
        L"，asset=" + info.assetName +
        L"，expected_size=" + std::to_wstring(info.assetSizeBytes) +
        L"，saved=" + AbsolutePathForLog(download.filePath).wstring() +
        L"，saved_size=" + FileSizeForLog(download.filePath) +
        L"，checksum_verified=" + std::wstring(download.checksumVerified ? L"1" : L"0") +
        (download.checksumMessage.empty() ? L"" : (L"，checksum_message=" + download.checksumMessage)));

    if (!download.checksumVerified) {
        const std::wstring warning =
            (download.checksumMessage.empty() ? L"更新包未完成 SHA256 校验。" : download.checksumMessage) +
            L"\n\n仍要继续自动覆盖更新吗？";
        if (MessageBoxW(hwnd_, warning.c_str(), L"检查更新", MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
    }

    UpdateInstallPlan plan;
    plan.downloadedExe = download.filePath;
    plan.currentExe = CurrentExecutablePath();
    if (config_.loggingEnabled) {
        plan.logPath = appDirectory_ / L"logs" / L"update.log";
    }
    plan.latestVersion = info.latestVersion;
    plan.assetName = info.assetName;
    plan.assetSizeBytes = info.assetSizeBytes;
    if (!LaunchEmbeddedUpdater(plan, error)) {
        MessageBoxW(hwnd_, error.empty() ? L"启动更新器失败。" : error.c_str(), L"检查更新", MB_OK | MB_ICONWARNING);
        return;
    }

    WriteAppLog(
        L"更新器已启动，准备退出当前进程。version=" + info.latestVersion +
        L"，downloaded=" + AbsolutePathForLog(download.filePath).wstring() +
        L"，target=" + AbsolutePathForLog(plan.currentExe).wstring());
    DestroyWindow(hwnd_);
}

void MainWindow::OpenHelp() {
    if (OpenConfiguredUrl(config_.helpUrl, L"帮助")) {
        return;
    }
    ShowToast(L"帮助链接尚未配置，可在设置窗口填写。", ThemedToastRole::Info);
}

void MainWindow::OpenFaq() {
    if (!OpenConfiguredUrl(config_.faqUrl, L"常见问题")) {
        ShowToast(L"常见问题链接尚未配置，可在设置窗口填写。", ThemedToastRole::Info);
    }
}

void MainWindow::OpenReward() {
    if (!OpenConfiguredUrl(config_.rewardUrl, L"赞助")) {
        ShowToast(L"赞助链接尚未配置，可在设置窗口填写。", ThemedToastRole::Info);
    }
}

void MainWindow::RestartWithOppositePrivilege() {
    config_.preferAdminRun = !runningAsAdmin_;
    configService_.Save(config_);

    std::wstring error;
    const bool launched = runningAsAdmin_
        ? RestartCurrentProcessUnelevated(hwnd_, error)
        : RestartCurrentProcessElevated(hwnd_, error);
    if (!launched) {
        config_.preferAdminRun = runningAsAdmin_;
        configService_.Save(config_);
        MessageBoxW(
            hwnd_,
            error.empty() ? L"重启失败。" : error.c_str(),
            runningAsAdmin_ ? L"以普通用户重启" : L"以管理员身份重启",
            MB_OK | MB_ICONWARNING);
        return;
    }

    exitingForPrivilegeRestart_ = true;
    DestroyWindow(hwnd_);
}

bool MainWindow::OpenConfiguredUrl(const std::wstring& url, const wchar_t* title) {
    const std::wstring trimmed = Trim(url);
    if (trimmed.empty()) {
        return false;
    }
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", trimmed.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(hwnd_, L"外部链接打开失败。", title, MB_OK | MB_ICONWARNING);
    }
    return true;
}

MainWindow::DockAutoHidePause::DockAutoHidePause(MainWindow& window, bool restoreHidden) : window_(window) {
    window_.BeginDockAutoHidePause(restoreHidden);
}

MainWindow::DockAutoHidePause::~DockAutoHidePause() {
    window_.EndDockAutoHidePause();
}

bool MainWindow::TryRepairLinkTarget(Link& link) {
    Link edited = link;
    if (!LinkEditDialog::Show(hwnd_, instance_, theme_, edited, model_.groups, false)) {
        return false;
    }
    const bool shellTargetChanged = link.path != edited.path || link.type != edited.type;
    link = edited;
    if (shellTargetChanged) {
        shellContextMenuCache_.Remove(link.id);
    }
    selectedLinkId_ = link.id;
    currentTagId_ = link.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    return true;
}

UINT MainWindow::TrackMainPopupMenu(
    HMENU menu,
    POINT screenPoint,
    bool returnCommand,
    ThemedPopupMenuSource source) {
    ThemedPopupMenuOptions options{};
    options.source = source;
    options.returnCommand = returnCommand;
    BeginPopupMenuSession();
    const ThemedPopupMenuResult result = ThemedUi::ShowPopupMenu(hwnd_, menu, screenPoint, options);
    EndPopupMenuSession();
    if (!result.foregroundReady && !result.foregroundSuppressed) {
        WriteAppLog(L"弹出菜单前台准备失败，菜单可能受 Windows 前台仲裁限制。");
    }
    return result.command;
}

void MainWindow::BeginPopupMenuSession() {
    ++popupMenuDepth_;
}

void MainWindow::EndPopupMenuSession() {
    if (popupMenuDepth_ > 0) {
        --popupMenuDepth_;
    }
    if (popupMenuDepth_ == 0 && popupWakePending_) {
        popupWakePending_ = false;
        WakeUp();
    }
}

void MainWindow::ShowThemeMenu(POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    HMENU menu = CreatePopupMenu();
    AppendThemeItemsToMenu(menu);
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::ApplyTheme(const std::wstring& themeName) {
    config_.theme = themeName;
    theme_ = Theme::Load(appDirectory_ / L"theme", config_.theme);
    configService_.Save(config_);
    activeMenuItems_.clear();
    if (menuFont_) {
        menuFont_->Reset();
    }
    embeddedUi_.reset();
    if (noteEdit_) {
        embeddedUi_ = std::make_unique<ThemedWindowUi>(
            instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact, 1, 1);
        ThemedEditOptions options{};
        options.mode = ThemedEditMode::MultiLine;
        embeddedUi_->RegisterEditFrame(noteEdit_, noteEditFrame_, options);
        SendMessageW(noteEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(embeddedUi_->font()), TRUE);
    }
    ApplyTooltipTheme();
    if (noteEdit_) {
        InvalidateRect(noteEdit_, nullptr, TRUE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::BeginDockAutoHidePause(bool restoreHidden) {
    ++dockAutoHidePauseDepth_;
    dockHideDueTick_ = 0;
    HideDockPeek();
    if (restoreHidden && dockHidden_) {
        DockRestore();
        ActivateWindow(hwnd_);
    }
}

void MainWindow::EndDockAutoHidePause() {
    if (dockAutoHidePauseDepth_ > 0) {
        --dockAutoHidePauseDepth_;
    }
    if (dockAutoHidePauseDepth_ == 0) {
        dockHideDueTick_ = GetTickCount64() + kDockRestoreGraceMs;
    }
}

bool MainWindow::DockAutoHidePaused() const {
    return dockAutoHidePauseDepth_ > 0 || (hwnd_ && !IsWindowEnabled(hwnd_));
}

void MainWindow::UpdateDockState() {
    if (!config_.autoDock || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
        dockHideDueTick_ = 0;
        HideDockPeek();
        return;
    }
    if (DockAutoHidePaused()) {
        dockHideDueTick_ = 0;
        HideDockPeek();
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    if (dockHidden_) {
        if (IsNearDockEdge(cursor)) {
            DockRestore();
        } else {
            EnsureDockPeekZOrder(true);
        }
        dockHideDueTick_ = 0;
        return;
    }

    if (IsFullScreenForegroundWindow()) {
        dockHideDueTick_ = 0;
        return;
    }

    RECT window{};
    if (!GetWindowRect(hwnd_, &window)) {
        return;
    }
    if (PtInRect(&window, cursor)) {
        dockHideDueTick_ = 0;
        return;
    }

    auto target = FindDockTarget(window, kDockSnapThreshold);
    if (!target) {
        dockHideDueTick_ = 0;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (dockHideDueTick_ != 0 && now < dockHideDueTick_) {
        return;
    }

    if (config_.dockDelay <= 0) {
        DockHide();
        return;
    }

    if (dockHideDueTick_ == 0) {
        dockHideDueTick_ = now + static_cast<ULONGLONG>(config_.dockDelay);
    }
    if (now >= dockHideDueTick_) {
        DockHide();
        dockHideDueTick_ = 0;
    }
}

void MainWindow::ShowDockPeek(const RECT& peekRect) {
    const int width = peekRect.right - peekRect.left;
    const int height = peekRect.bottom - peekRect.top;
    if (width <= 0 || height <= 0) {
        HideDockPeek();
        return;
    }

    if (!dockPeek_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DockPeekWindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kDockPeekWindowClass;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return;
        }

        dockPeek_ = CreateWindowExW(
            (BackgroundAcceptanceMode() ? 0u : WS_EX_TOPMOST) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kDockPeekWindowClass,
            L"",
            WS_POPUP,
            peekRect.left,
            peekRect.top,
            width,
            height,
            hwnd_,
            nullptr,
            instance_,
            hwnd_);
        if (!dockPeek_) {
            return;
        }
    }

    SetWindowPos(dockPeek_,
                 BackgroundAcceptanceMode() ? HWND_BOTTOM : HWND_TOPMOST,
                 peekRect.left,
                 peekRect.top,
                 width,
                 height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(dockPeek_, nullptr, TRUE);
}

void MainWindow::EnsureDockPeekZOrder(bool showWindow) {
    if (!dockPeek_) {
        return;
    }
    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (showWindow) {
        flags |= SWP_SHOWWINDOW;
    }
    SetWindowPos(
        dockPeek_,
        BackgroundAcceptanceMode() ? HWND_BOTTOM : HWND_TOPMOST,
        0,
        0,
        0,
        0,
        flags);
}

void MainWindow::HideDockPeek() {
    if (dockPeek_) {
        ShowWindow(dockPeek_, SW_HIDE);
    }
}

bool MainWindow::DockHide(bool persistWindowState) {
    HideItemTooltip();
    if (dockHidden_) {
        return true;
    }

    RECT window{};
    if (!GetWindowRect(hwnd_, &window)) {
        return false;
    }

    auto target = FindDockTarget(window, kDockSnapThreshold);
    if (!target) {
        return false;
    }

    dockRestoreRect_ = SnapRectToDockTarget(window, *target);
    RECT hidden = HiddenRectForDockTarget(dockRestoreRect_, *target);
    RECT peek = DockPeekRectForDockTarget(dockRestoreRect_, *target);
    if (HiddenRectIntersectsOtherMonitor(hidden, target->monitor)) {
        return false;
    }

    const int width = hidden.right - hidden.left;
    const int height = hidden.bottom - hidden.top;
    config_.posX = dockRestoreRect_.left;
    config_.posY = dockRestoreRect_.top;
    if (persistWindowState) {
        configService_.SaveWindowState(config_);
    }

    dockHidden_ = true;
    if (!SetWindowPos(hwnd_,
                      MainWindowMoveInsertAfter(config_.topMost),
                      hidden.left,
                      hidden.top,
                      width,
                      height,
                      MainWindowMoveFlags(config_.topMost, SWP_NOZORDER | SWP_NOACTIVATE))) {
        dockHidden_ = false;
        HideDockPeek();
        return false;
    } else {
        ShowDockPeek(peek);
    }
    return true;
}

void MainWindow::DockRestore() {
    if (!dockHidden_) {
        return;
    }
    HideDockPeek();
    const int width = dockRestoreRect_.right - dockRestoreRect_.left;
    const int height = dockRestoreRect_.bottom - dockRestoreRect_.top;
    SetWindowPos(hwnd_,
                 MainWindowMoveInsertAfter(config_.topMost),
                 dockRestoreRect_.left,
                 dockRestoreRect_.top,
                 width,
                 height,
                 MainWindowMoveFlags(config_.topMost, SWP_NOZORDER | SWP_NOACTIVATE));
    dockHidden_ = false;
    dockHideDueTick_ = GetTickCount64() + kDockRestoreGraceMs;
}

bool MainWindow::SnapDockWindowRect(RECT& window) const {
    if (!config_.autoDock || dockHidden_) {
        return false;
    }
    auto target = FindDockTarget(window, kDockSnapThreshold);
    if (!target) {
        return false;
    }
    RECT snapped = SnapRectToDockTarget(window, *target);
    if (EqualRect(&snapped, &window)) {
        return false;
    }
    window = snapped;
    return true;
}

bool MainWindow::IsNearDockEdge(POINT screenPoint) const {
    if (dockPeek_) {
        RECT peek{};
        if (IsWindowVisible(dockPeek_) && GetWindowRect(dockPeek_, &peek)) {
            InflateRect(&peek, 8, 8);
            if (PtInRect(&peek, screenPoint)) {
                return true;
            }
        }
    }

    RECT window{};
    if (!GetWindowRect(hwnd_, &window)) {
        return false;
    }
    InflateRect(&window, 8, 8);
    return PtInRect(&window, screenPoint) != FALSE;
}

bool MainWindow::ImportPath(const std::wstring& path, bool showError) {
    const std::wstring trimmed = Trim(path);
    if (trimmed.empty()) {
        return false;
    }
    if (EnsureCurrentTag() <= 0) {
        return false;
    }
    Link link;
    link.path = trimmed;
    link.name = std::filesystem::path(trimmed).stem().wstring();
    if (link.name.empty()) {
        link.name = trimmed;
    }
    std::error_code ec;
    const std::wstring lower = ToLower(trimmed);
    const bool isUrl = lower.rfind(L"http://", 0) == 0 ||
                       lower.rfind(L"https://", 0) == 0 ||
                       lower.rfind(L"ftp://", 0) == 0 ||
                       lower.rfind(L"www.", 0) == 0;
    if (ShellItemService::IsShellParseName(trimmed)) {
        link.type = 3;
    } else {
        link.type = std::filesystem::is_directory(trimmed, ec) ? 1 : (isUrl ? 2 : 0);
    }
    if (!isUrl) {
        if (auto item = ShellItemService::FromPathOrParseName(trimmed)) {
            link.pidl = item->pidl;
            if (item->isVirtual || ShellItemService::IsShellParseName(trimmed)) {
                link.type = 3;
                if (!Trim(item->displayName).empty()) {
                    link.name = item->displayName;
                }
            }
        }
    }
    link.parentGroup = currentTagId_;
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (storageService_.InsertLink(link)) {
        model_.links.push_back(link);
        selectedLinkId_ = link.id;
        RequestInitialUrlIconDownload(link);
        RegisterConfiguredHotKeys();
        EnsureGroupVisible(currentGroupId_);
        EnsureTagVisible(currentTagId_);
        EnsureLinkVisible(selectedLinkId_);
        return true;
    } else {
        if (showError) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"导入启动项", MB_OK | MB_ICONWARNING);
        } else {
            WriteAppLog(L"导入启动项失败：" + trimmed + L" - " + storageService_.lastError());
        }
        return false;
    }
}

void MainWindow::ShowImportFeedback(int succeeded, int failed) {
    if (succeeded <= 0 && failed <= 0) {
        ShowToast(L"没有可添加的内容。", ThemedToastRole::Info);
        return;
    }
    const Group* tag = FindGroup(currentTagId_);
    const ImportToastSummary summary{succeeded, failed};
    ShowToast(
        ImportToastText(summary, tag ? tag->name : L""),
        ImportToastRole(summary),
        failed > 0 ? 5000 : 0);
}

void MainWindow::ImportClipboard() {
    if (!OpenClipboardWithRetry(hwnd_)) {
        ShowToast(L"无法读取剪贴板，请稍后重试。", ThemedToastRole::Warning);
        return;
    }
    HANDLE handle = GetClipboardData(CF_HDROP);
    int importedCount = 0;
    int failedCount = 0;
    if (handle) {
        HDROP drop = static_cast<HDROP>(handle);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            if (ImportPath(path, false)) {
                ++importedCount;
            } else {
                ++failedCount;
            }
        }
    }

    if (importedCount == 0 && failedCount == 0) {
        handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(handle));
            if (text) {
                if (ImportPath(text, false)) {
                    ++importedCount;
                } else {
                    ++failedCount;
                }
                GlobalUnlock(handle);
            }
        }
    }
    CloseClipboard();

    if (importedCount > 0 || failedCount > 0) {
        ShowClipboardImportNotification(importedCount, failedCount);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        ShowToast(L"剪贴板中没有可添加的文件、文件夹或网址。", ThemedToastRole::Info);
    }
}

void MainWindow::ExportConfigPackage() {
    SaveCurrentNotePage();
    SaveWindowState();

    std::wstring buffer = (appDirectory_ / ConfigPackageFileName()).wstring();
    buffer.resize(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0";
    ofn.lpstrDefExt = L"q4cfg";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }

    ConfigPackageOptions options;
    options.includeConfig = true;
    options.includeData = true;
    options.includeUrlIcons = true;

    ConfigPackageService service(appDirectory_);
    const ConfigPackageReport report = service.ExportPackage(buffer.c_str(), options);
    if (report.ok) {
        ShowToast(L"配置包已导出。", ThemedToastRole::Success);
    } else {
        MessageBoxW(hwnd_, FormatConfigPackageReport(report).c_str(), L"导出配置包", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::ImportConfigPackageMerge() {
    SaveCurrentNotePage();

    CommonFileDialogOptions dialogOptions{};
    dialogOptions.owner = hwnd_;
    dialogOptions.mode = CommonFileDialogMode::FileOnly;
    dialogOptions.context = L"配置包合并导入";
    dialogOptions.defaultPath = appDirectory_.wstring();
    dialogOptions.legacyFilter = L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0";
    dialogOptions.defaultExtension = L"q4cfg";
    CommonFileDialogResult result{};
    if (!ShowCommonFileDialog(dialogOptions, result)) {
        return;
    }

    const int confirm = MessageBoxW(
        hwnd_,
        L"将把配置包中的分组、标签、启动项、便签和待办合并到当前数据。\n\n当前数据不会被覆盖，导入前会自动备份。",
        L"合并导入配置包",
        MB_OKCANCEL | MB_ICONINFORMATION);
    if (confirm != IDOK) {
        return;
    }

    ConfigPackageOptions options;
    options.includeConfig = false;
    options.includeData = true;
    options.includeUrlIcons = true;

    ConfigPackageService service(appDirectory_);
    const ConfigPackageReport report = service.ImportPackageMerge(result.path, options);
    if (report.ok) {
        model_ = storageService_.Load();
        RestoreLegacyBuiltinSystemFunctionKeys();
        SelectInitialItems();
        RegisterConfiguredHotKeys();
        ClearUiBitmaps();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowToast(L"配置包已合并导入。", ThemedToastRole::Success);
    } else {
        MessageBoxW(hwnd_, FormatConfigPackageReport(report).c_str(), L"合并导入配置包", MB_OK | MB_ICONWARNING);
    }
}

bool MainWindow::ImportDropData(IDataObject* dataObject) {
    if (!dataObject) {
        return false;
    }

    auto finish = [&](int succeeded, int failed) {
        ShowImportFeedback(succeeded, failed);
        if (succeeded > 0) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return succeeded > 0;
    };

    FORMATETC fileFormat{};
    fileFormat.cfFormat = CF_HDROP;
    fileFormat.dwAspect = DVASPECT_CONTENT;
    fileFormat.lindex = -1;
    fileFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (SUCCEEDED(dataObject->GetData(&fileFormat, &medium))) {
        int succeeded = 0;
        int failed = 0;
        UINT count = 0;
        HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
        if (drop) {
            count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length, L'\0');
                DragQueryFileW(drop, i, path.data(), length + 1);
                if (ImportPath(path, false)) {
                    ++succeeded;
                } else {
                    ++failed;
                }
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
        if (count > 0) {
            return finish(succeeded, failed);
        }
    }

    const UINT shellIdListFormatId = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    if (shellIdListFormatId != 0) {
        FORMATETC shellFormat{};
        shellFormat.cfFormat = static_cast<CLIPFORMAT>(shellIdListFormatId);
        shellFormat.dwAspect = DVASPECT_CONTENT;
        shellFormat.lindex = -1;
        shellFormat.tymed = TYMED_HGLOBAL;
        STGMEDIUM shellMedium{};
        if (SUCCEEDED(dataObject->GetData(&shellFormat, &shellMedium))) {
            int succeeded = 0;
            int failed = 0;
            bool handled = false;
            const SIZE_T bytes = shellMedium.hGlobal ? GlobalSize(shellMedium.hGlobal) : 0;
            const CIDA* cida = static_cast<const CIDA*>(GlobalLock(shellMedium.hGlobal));
            if (cida && bytes >= sizeof(UINT) * 2) {
                const UINT count = cida->cidl;
                const SIZE_T headerBytes = sizeof(UINT) * (static_cast<SIZE_T>(count) + 2);
                const auto* base = static_cast<const BYTE*>(static_cast<const void*>(cida));

                auto offsetValid = [&](UINT offset) {
                    return static_cast<SIZE_T>(offset) + sizeof(USHORT) <= bytes;
                };
                auto importAbsolutePidl = [&](PCIDLIST_ABSOLUTE pidl) {
                    handled = true;
                    if (!pidl) {
                        ++failed;
                        return;
                    }
                    auto item = ShellItemService::FromAbsolutePidl(pidl);
                    if (!item) {
                        ++failed;
                        return;
                    }
                    std::wstring target = !Trim(item->fileSystemPath).empty() ? item->fileSystemPath : item->parseName;
                    if (Trim(target).empty()) {
                        ++failed;
                        return;
                    }
                    if (!ImportPath(target, false)) {
                        ++failed;
                        return;
                    }
                    if (Link* importedLink = FindLink(selectedLinkId_)) {
                        importedLink->pidl = item->pidl;
                        if (item->isVirtual || ShellItemService::IsShellParseName(target)) {
                            importedLink->type = 3;
                        }
                        if (!Trim(item->displayName).empty()) {
                            importedLink->name = item->displayName;
                        }
                        if (!storageService_.UpdateLink(*importedLink)) {
                            WriteAppLog(L"更新 Shell 导入项信息失败：" + storageService_.lastError());
                        }
                    }
                    ++succeeded;
                };

                if (headerBytes <= bytes && offsetValid(cida->aoffset[0])) {
                    auto parent = reinterpret_cast<PCIDLIST_ABSOLUTE>(base + cida->aoffset[0]);
                    if (count == 0) {
                        importAbsolutePidl(parent);
                    } else {
                        for (UINT i = 0; i < count; ++i) {
                            if (!offsetValid(cida->aoffset[i + 1])) {
                                continue;
                            }
                            auto child = reinterpret_cast<PCUIDLIST_RELATIVE>(base + cida->aoffset[i + 1]);
                            PIDLIST_ABSOLUTE full = ILCombine(parent, child);
                            if (full) {
                                importAbsolutePidl(full);
                                CoTaskMemFree(full);
                            }
                        }
                    }
                }
            }
            if (cida) {
                GlobalUnlock(shellMedium.hGlobal);
            }
            ReleaseStgMedium(&shellMedium);
            if (handled) {
                return finish(succeeded, failed);
            }
        }
    }

    FORMATETC textFormat{};
    textFormat.cfFormat = CF_UNICODETEXT;
    textFormat.dwAspect = DVASPECT_CONTENT;
    textFormat.lindex = -1;
    textFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM textMedium{};
    if (SUCCEEDED(dataObject->GetData(&textFormat, &textMedium))) {
        int succeeded = 0;
        int failed = 0;
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(textMedium.hGlobal));
        if (text && *text) {
            if (ImportPath(text, false)) {
                ++succeeded;
            } else {
                ++failed;
            }
        }
        if (text) {
            GlobalUnlock(textMedium.hGlobal);
        }
        ReleaseStgMedium(&textMedium);
        if (succeeded > 0 || failed > 0) {
            return finish(succeeded, failed);
        }
    }
    ShowToast(L"拖入内容不是可添加的文件、文件夹或网址。", ThemedToastRole::Info);
    return false;
}

bool MainWindow::StartHttpServer(bool showMessage) {
    std::wstring error;
    const auto options = LocalHttpServerService::OptionsFromConfig(config_, httpRootBaseDirectory_);
    const bool ok = httpServerService_.Start(options, error);
    if (!ok) {
        WriteAppLog(error.empty() ? L"HTTP 服务启动失败。" : (L"HTTP 服务启动失败: " + error));
        if (showMessage) {
            MessageBoxW(hwnd_, error.empty() ? L"HTTP 服务启动失败。" : error.c_str(), L"HTTP 服务", MB_OK | MB_ICONWARNING);
        } else {
            ShowToast(error.empty() ? L"HTTP 服务启动失败。" : (L"HTTP 服务启动失败：" + error), ThemedToastRole::Danger, 6000);
        }
        return false;
    }
    WriteAppLog(L"HTTP 服务已启动: " + httpServerService_.BaseUrl(true));
    if (showMessage) {
        ShowToast(L"HTTP 服务已启动：" + httpServerService_.BaseUrl(true), ThemedToastRole::Success, 5000);
    }
    return true;
}

void MainWindow::StopHttpServer(bool showMessage) {
    const bool wasRunning = httpServerService_.IsRunning();
    httpServerService_.Stop();
    if (wasRunning) {
        WriteAppLog(L"HTTP 服务已停止。");
    }
    if (showMessage) {
        ShowToast(L"HTTP 服务已停止。", ThemedToastRole::Info);
    }
}

bool MainWindow::RestartHttpServer(bool showMessage) {
    std::wstring error;
    const auto options = LocalHttpServerService::OptionsFromConfig(config_, httpRootBaseDirectory_);
    const bool ok = httpServerService_.Restart(options, error);
    if (!ok) {
        WriteAppLog(error.empty() ? L"HTTP 服务重启失败。" : (L"HTTP 服务重启失败: " + error));
        if (showMessage) {
            MessageBoxW(hwnd_, error.empty() ? L"HTTP 服务重启失败。" : error.c_str(), L"HTTP 服务", MB_OK | MB_ICONWARNING);
        } else {
            ShowToast(error.empty() ? L"HTTP 服务重启失败。" : (L"HTTP 服务重启失败：" + error), ThemedToastRole::Danger, 6000);
        }
        return false;
    }
    WriteAppLog(L"HTTP 服务已重启: " + httpServerService_.BaseUrl(true));
    if (showMessage) {
        ShowToast(L"HTTP 服务已重启：" + httpServerService_.BaseUrl(true), ThemedToastRole::Success, 5000);
    }
    return true;
}

void MainWindow::SyncHttpServerRuntime(const AppConfig& previous) {
    const bool shouldRun = config_.httpServerEnabled;
    if (!shouldRun) {
        StopHttpServer(false);
        return;
    }

    const bool wasConfiguredToRun = previous.httpServerEnabled;
    const bool settingsChanged =
        previous.httpServerPort != config_.httpServerPort ||
        previous.httpServerLanAccess != config_.httpServerLanAccess ||
        Trim(previous.httpServerRootPath) != Trim(config_.httpServerRootPath);

    if (!httpServerService_.IsRunning()) {
        StartHttpServer(false);
        return;
    }
    if (!wasConfiguredToRun || settingsChanged) {
        RestartHttpServer(false);
    }
}

void MainWindow::ApplyConfigRuntimeChanges(const AppConfig& previous) {
    ApplyMainWindowAlpha(hwnd_, config_.alpha);
    SetWindowPos(hwnd_,
                 MainWindowTopMostInsertAfter(config_.topMost),
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (dockHidden_ && dockPeek_) {
        EnsureDockPeekZOrder(true);
    }
    if (previous.showTooltip != config_.showTooltip && !config_.showTooltip) {
        HideItemTooltip();
    }
    if (previous.hideNotifyIcon != config_.hideNotifyIcon) {
        RemoveTrayIcon();
        InitializeTrayIcon();
    }
    if (previous.globalHotKeysEnabled != config_.globalHotKeysEnabled ||
        previous.mainHotKey != config_.mainHotKey ||
        previous.processLocatorHotKey != config_.processLocatorHotKey ||
        previous.copySelectedPathsHotKey != config_.copySelectedPathsHotKey) {
        UnregisterConfiguredHotKeys();
        RegisterConfiguredHotKeys();
    }
    if ((config_.autoDock || config_.hideWhenInactive) && dockTimerId_ == 0) {
        dockTimerId_ = SetTimer(hwnd_, ID_TIMER_DOCK, 250, nullptr);
    } else if (!config_.autoDock && !config_.hideWhenInactive && dockTimerId_ != 0) {
        KillTimer(hwnd_, dockTimerId_);
        dockTimerId_ = 0;
    }
    SyncHttpServerRuntime(previous);
}

void MainWindow::SyncAutoRun(const AppConfig& previous) {
    if (previous.autoRun == config_.autoRun && (!config_.autoRun || StartupShortcutExists(appDirectory_))) {
        return;
    }

    std::wstring error;
    if (!SyncStartupShortcut(appDirectory_, config_.autoRun, error)) {
        WriteAppLog(L"开机自启动同步失败: " + error);
        MessageBoxW(hwnd_, error.empty() ? L"开机自启动同步失败。" : error.c_str(), L"开机自启动", MB_OK | MB_ICONWARNING);
        return;
    }
    WriteAppLog(config_.autoRun ? L"开机自启动已启用。" : L"开机自启动已关闭。");
    ShowToast(config_.autoRun ? L"开机自启动已启用。" : L"已关闭开机自启动。", ThemedToastRole::Success);
}

void MainWindow::CopySelectedPathsFromForeground(HWND foregroundWindow) {
    const SelectedPathCopyResult result = SelectedPathCopyService::CopySelectedPaths(foregroundWindow, hwnd_);
    switch (result.status) {
    case SelectedPathCopyStatus::Success:
        ShowToast(
            L"已复制 " + std::to_wstring(result.copiedCount) + L" 个选中对象的绝对路径。",
            ThemedToastRole::Success);
        return;
    case SelectedPathCopyStatus::UnsupportedForegroundWindow:
        ShowToast(L"请先在文件资源管理器或桌面中选择文件或文件夹。", ThemedToastRole::Warning);
        return;
    case SelectedPathCopyStatus::NoSelection:
        ShowToast(L"当前没有选中任何文件或文件夹。", ThemedToastRole::Warning);
        return;
    case SelectedPathCopyStatus::NonFileSystemItem:
        ShowToast(L"选中项包含无法取得绝对路径的系统对象。", ThemedToastRole::Warning);
        return;
    case SelectedPathCopyStatus::ClipboardUnavailable:
        WriteAppLog(L"复制选中项绝对路径失败: 剪贴板不可用 - " + result.detail);
        ShowToast(L"剪贴板暂时被其他程序占用，请重试。", ThemedToastRole::Warning);
        return;
    case SelectedPathCopyStatus::ShellUnavailable:
        WriteAppLog(L"复制选中项绝对路径失败: 无法读取 Shell 选中项 - " + result.detail);
        ShowToast(L"无法读取当前资源管理器的选中项。", ThemedToastRole::Warning);
        return;
    }
}

void MainWindow::RegisterConfiguredHotKeys() {
    UnregisterConfiguredHotKeys();
    std::wstring failures;
    auto registerHotKey = [&](int id, int key, const std::wstring& name) -> bool {
        if (key <= 0) {
            return false;
        }
        if (RegisterHotKey(hwnd_, id, MOD_CONTROL | MOD_ALT, static_cast<UINT>(key))) {
            return true;
        }
        const std::wstring line = name + L"（" + FormatHotKeyText(key) + L"）";
        failures += failures.empty() ? line : (L"\n" + line);
        WriteAppLog(L"热键注册失败: " + line + L" - " + FormatLastError(GetLastError()));
        return false;
    };

    if (config_.globalHotKeysEnabled) {
        if (IsDoubleAltMainHotKey(config_.mainHotKey)) {
            mainHotKeyRegistered_ = InstallDoubleAltHotKeyHook(hwnd_);
            if (!mainHotKeyRegistered_) {
                const std::wstring line = L"主窗口（" + FormatMainHotKeyText(config_.mainHotKey) + L"）";
                failures += failures.empty() ? line : (L"\n" + line);
                WriteAppLog(L"热键注册失败: " + line + L" - " + FormatLastError(GetLastError()));
            }
        } else if (config_.mainHotKey != 0) {
            mainHotKeyRegistered_ = registerHotKey(ID_HOTKEY_MAIN, config_.mainHotKey, L"主窗口");
        }
        processLocatorHotKeyRegistered_ = registerHotKey(
            ID_HOTKEY_PROCESS_LOCATOR, config_.processLocatorHotKey, L"进程定位器");
        copySelectedPathsHotKeyRegistered_ = registerHotKey(
            ID_HOTKEY_COPY_SELECTED_PATHS, config_.copySelectedPathsHotKey, L"复制选中项绝对路径");

        int nextHotKeyId = ID_HOTKEY_LINK_BASE;
        for (const auto& link : model_.links) {
            if (link.hotKey == 0) {
                continue;
            }
            const int hotKeyId = nextHotKeyId++;
            if (RegisterHotKey(hwnd_, hotKeyId, MOD_CONTROL | MOD_ALT, static_cast<UINT>(link.hotKey))) {
                registeredLinkHotKeys_.push_back({hotKeyId, link.id});
            } else {
                const std::wstring line = link.name + L"（" + FormatHotKeyText(link.hotKey) + L"）";
                failures += failures.empty() ? line : (L"\n" + line);
                WriteAppLog(L"启动项热键注册失败: " + line + L" - " + FormatLastError(GetLastError()));
            }
        }
    }

    if (!failures.empty()) {
        ShowHotKeyConflictWarning(failures);
    }
    hotKeysRegistered_ = true;
    UpdateTrayTooltip();
}

void MainWindow::ShowHotKeyConflictWarning(const std::wstring& failures) {
    if (failures.empty()) {
        return;
    }
    // Acceptance rule: never open a modal dialog while a test drives the app
    // (QUATTRO_TEST_NO_FOCUS). A startup modal loop would also swallow the
    // WM_QUIT posted by ID_MENU_EXIT and leave the process hanging.
    if (hotKeyConflictWarningShown_ || config_.ignoreHotKeyConflictWarning || SuppressForegroundActivation()) {
        ShowToast(L"部分热键注册失败，可能被其他程序占用。", ThemedToastRole::Warning, 5000);
        return;
    }
    hotKeyConflictWarningShown_ = true;
    bool ignoreFutureWarnings = config_.ignoreHotKeyConflictWarning;
    const std::wstring message = L"以下热键注册失败，可能已被系统保留，或已被其它软件/工具占用：\n" + failures;
    ShowHotKeyConflictDialog(hwnd_, instance_, theme_, message, ignoreFutureWarnings);
    if (ignoreFutureWarnings != config_.ignoreHotKeyConflictWarning) {
        config_.ignoreHotKeyConflictWarning = ignoreFutureWarnings;
        configService_.Save(config_);
    }
}

void MainWindow::UnregisterConfiguredHotKeys() {
    if (!hwnd_) {
        return;
    }
    UninstallDoubleAltHotKeyHook(hwnd_);
    UnregisterHotKey(hwnd_, ID_HOTKEY_MAIN);
    UnregisterHotKey(hwnd_, ID_HOTKEY_PROCESS_LOCATOR);
    UnregisterHotKey(hwnd_, ID_HOTKEY_COPY_SELECTED_PATHS);
    mainHotKeyRegistered_ = false;
    processLocatorHotKeyRegistered_ = false;
    copySelectedPathsHotKeyRegistered_ = false;
    for (const auto& [hotKeyId, _] : registeredLinkHotKeys_) {
        UnregisterHotKey(hwnd_, hotKeyId);
    }
    registeredLinkHotKeys_.clear();
    hotKeysRegistered_ = false;
}

std::wstring MainWindow::TrayTooltipText() const {
    std::wstring text = std::wstring(kAppDisplayName) + L"\n版本" + FormatVersionForDisplay(QuattroVersionText());
    if (!config_.globalHotKeysEnabled) {
        return text;
    }
    if (mainHotKeyRegistered_ && config_.mainHotKey != 0) {
        text += L"\n主窗口：" + FormatMainHotKeyText(config_.mainHotKey);
    }
    if (processLocatorHotKeyRegistered_ && config_.processLocatorHotKey != 0) {
        text += L"\n进程定位器：" + FormatGlobalHotKeyText(config_.processLocatorHotKey);
    }
    if (copySelectedPathsHotKeyRegistered_ && config_.copySelectedPathsHotKey != 0) {
        text += L"\n复制路径：" + FormatGlobalHotKeyText(config_.copySelectedPathsHotKey);
    }
    return text;
}

void MainWindow::UpdateTrayTooltip() {
    if (!trayIconVisible_) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, TrayTooltipText().c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MainWindow::InitializeTrayIcon() {
    if (trayIconVisible_ || config_.hideNotifyIcon || SuppressTrayForIsolatedTest()) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_QUATTRO_TRAY;
    data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wcsncpy_s(data.szTip, TrayTooltipText().c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        trayIconVisible_ = true;
    }
}

void MainWindow::RemoveTrayIcon() {
    if (!trayIconVisible_) {
        return;
    }
    DeleteTrayIconForWindow(hwnd_, L"托盘图标移除");
    trayIconVisible_ = false;
}

void MainWindow::ShowTrayMenu(POINT screenPoint) {
    DockAutoHidePause dockPause(*this, false);
    ResetMenuVisuals(screenPoint);
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_SHOW, IsWindowVisible(hwnd_) ? L"隐藏主窗口" : L"显示主窗口");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_RESTART_PRIVILEGE, runningAsAdmin_ ? L"以普通用户重启" : L"以管理员身份重启", false, -1, -1, MenuIconShield);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_RESET_LAYOUT, L"重置布局");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ABOUT, L"关于");
    AppendThemedMenuItem(menu, MF_STRING | (updateDownloadActive_ ? MF_GRAYED : 0), ID_MENU_CHECK_UPDATE, L"检查更新");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EXIT, L"退出");
    const UINT command = TrackMainPopupMenu(menu, screenPoint, true, ThemedPopupMenuSource::TrayIcon);
    DestroyMenu(menu);
    if (command != 0) {
        SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
}

void MainWindow::ShowMainMenu(POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    HMENU menu = CreatePopupMenu();
    HMENU themeMenu = CreatePopupMenu();
    HMENU systemMenu = CreatePopupMenu();
    HMENU toolMenu = CreatePopupMenu();
    AppendThemeItemsToMenu(themeMenu);
    AppendSystemFunctionItems(systemMenu);
    AppendToolItems(toolMenu);

    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_QUICK_IMPORT, L"快速导入");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TITLE, config_.showTitle ? L"隐藏标题栏" : L"显示标题栏", false, -1, -1, config_.showTitle ? MenuIconEyeOff : MenuIconEye);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_GROUP, config_.showGroup ? L"隐藏分组" : L"显示分组", false, -1, -1, config_.showGroup ? MenuIconEyeOff : MenuIconEye);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TAG, config_.showTag ? L"隐藏标签" : L"显示标签", false, -1, -1, config_.showTag ? MenuIconEyeOff : MenuIconEye);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TOPMOST, config_.topMost ? L"取消置顶" : L"置顶", false, -1, -1, config_.topMost ? MenuIconPinOff : MenuIconPin);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), L"皮肤", true);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_SETTINGS, L"设置");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_ALL_ICONS, L"刷新");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_ICON_CACHE, L"清理图标缓存");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(systemMenu), L"系统功能", true);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(toolMenu), L"工具箱", true);
    AppendThemedSeparator(menu);
    AppendUnifiedViewOptionItems(menu);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ABOUT, L"关于");
    AppendThemedMenuItem(menu, MF_STRING | (updateDownloadActive_ ? MF_GRAYED : 0), ID_MENU_CHECK_UPDATE, L"检查更新");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EXIT, L"关闭退出");
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::ShowToolMenu(POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    HMENU menu = CreatePopupMenu();
    AppendToolItems(menu);
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::ShowLinkMenu(int linkId, POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    HideItemTooltip();
    ResetMenuVisuals(screenPoint);
    menuTrackedShellCommands_.clear();
    HMENU menu = CreatePopupMenu();
    Link* link = FindLink(linkId);
    AppendLinkActionItems(menu, link, true);
    selectedLinkId_ = linkId;
    menuContextKind_ = HitKind::Link;
    menuContextId_ = linkId;
    const UINT command = TrackMainPopupMenu(menu, screenPoint, true);
    DestroyMenu(menu);
    if (command != 0) {
        SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
}

void MainWindow::AppendLinkActionItems(HMENU menu, Link* link, bool includeNativeMenuItem) {
    const bool isUrl = link && IsUrlLink(*link);
    const bool isBuiltinSystemFunction = link && BuiltinSystemFunctionForLink(*link);

    if (includeNativeMenuItem && isBuiltinSystemFunction) {
        AppendBuiltinSystemContextItems(menu, *link);
        AppendThemedSeparator(menu);
    }

    if (isUrl) {
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_RUN_PRIVATE, L"以隐私模式运行");
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_COPY_URL, L"复制网址(URL)");
    } else {
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_OPEN_LOCATION, L"打开文件位置");
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_RUN_ADMIN, L"以管理员身份运行");
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_COPY_PATH, L"复制路径");
        if (includeNativeMenuItem && link) {
            AppendTrackedShellMenuItems(menu, *link);
            AppendThemedMenuItem(menu, MF_STRING, ID_MENU_WINDOWS_CONTEXT, L"Windows 原生菜单", false, -1, -1, MenuIconWindows);
        }
    }
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CREATE_DESKTOP_SHORTCUT, L"创建桌面快捷方式");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_LINK_ICON, L"刷新");
    HMENU moveMenu = CreatePopupMenu();
    HMENU copyMenu = CreatePopupMenu();
    AppendGroupedTagTargetMenu(moveMenu, ID_MENU_MOVE_TO_BASE, menuMoveTargetIds_, link ? link->parentGroup : 0);
    AppendGroupedTagTargetMenu(copyMenu, ID_MENU_COPY_TO_BASE, menuCopyTargetIds_, 0);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"移动到", true);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制到", true);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_LINK, L"编辑");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_PROPERTIES, L"属性");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_LINK, L"删除");
}

void MainWindow::AppendTrackedShellMenuItems(HMENU menu, const Link& link) {
    const auto items = shellContextMenuCache_.ItemsFor(
        link,
        TrackedShellMenuOptions());
    if (items.empty()) {
        return;
    }
    AppendThemedSeparator(menu);
    // 绑定表行序即菜单内 provider 顺序，与设置页「自动跟踪」表格一致。
    bool firstProvider = true;
    for (const auto& provider : TrackedContextMenuProviders()) {
        std::vector<ShellContextMenuItem> providerItems;
        for (const auto& item : items) {
            if (item.providerId == provider.providerId) {
                providerItems.push_back(item);
            }
        }
        if (providerItems.empty()) {
            continue;
        }
        if (!firstProvider) {
            AppendThemedSeparator(menu);
        }
        std::vector<std::wstring> path;
        AppendTrackedShellMenuItems(menu, providerItems, path);
        firstProvider = false;
    }
    AppendThemedSeparator(menu);
}

void MainWindow::AppendTrackedShellMenuItems(
    HMENU menu,
    const std::vector<ShellContextMenuItem>& items,
    std::vector<std::wstring>& path) {
    bool lastWasSeparator = true;
    for (const auto& item : items) {
        if (item.separator) {
            if (!lastWasSeparator) {
                AppendThemedSeparator(menu);
                lastWasSeparator = true;
            }
            continue;
        }
        if (item.text.empty()) {
            continue;
        }
        path.push_back(item.text);
        const UINT disabled = item.enabled ? 0 : MF_GRAYED;
        if (!item.children.empty()) {
            HMENU submenu = CreatePopupMenu();
            AppendTrackedShellMenuItems(submenu, item.children, path);
            AppendThemedTrackedMenuItem(
                menu,
                MF_POPUP | disabled,
                reinterpret_cast<UINT_PTR>(submenu),
                item,
                true);
            lastWasSeparator = false;
        } else if (menuTrackedShellCommands_.size() < ID_MENU_TRACKED_SHELL_ACTION_LIMIT) {
            ShellContextMenuLocator locator;
            locator.providerId = item.providerId;
            locator.path = path;
            locator.verb = item.verb;
            locator.actionKind = item.actionKind;
            locator.actionId = item.actionId;
            locator.executable = item.executable;
            locator.arguments = item.arguments;
            locator.workingDirectory = item.workingDirectory;
            const UINT command = ID_MENU_TRACKED_SHELL_ACTION_BASE +
                static_cast<UINT>(menuTrackedShellCommands_.size());
            menuTrackedShellCommands_.push_back(std::move(locator));
            AppendThemedTrackedMenuItem(menu, MF_STRING | disabled, command, item, false);
            lastWasSeparator = false;
        }
        path.pop_back();
    }
}

void MainWindow::AppendBuiltinSystemContextItems(HMENU menu, const Link& link) {
    const auto items = BuiltinSystemContextMenuItems(link);
    if (items.empty()) {
        return;
    }
    for (const auto& item : items) {
        AppendThemedMenuItem(
            menu,
            MF_STRING,
            ID_MENU_BUILTIN_SYSTEM_ACTION_BASE + static_cast<UINT>(item.action),
            item.text,
            false,
            -1,
            -1,
            item.menuIcon);
    }
}

void MainWindow::RestoreLegacyBuiltinSystemFunctionKeys() {
    for (auto& link : model_.links) {
        if (RestoreLegacyBuiltinSystemFunctionKey(link)) {
            storageService_.UpdateLink(link);
        }
    }
}

void MainWindow::ExecuteBuiltinSystemContextAction(int linkId, BuiltinSystemContextAction action) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }

    const auto items = BuiltinSystemContextMenuItems(*link);
    const bool allowed = std::any_of(items.begin(), items.end(), [action](const auto& item) {
        return item.action == action;
    });
    if (!allowed) {
        return;
    }

    DWORD result = NO_ERROR;
    switch (action) {
    case BuiltinSystemContextAction::ManageComputer:
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", L"compmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            MessageBoxW(hwnd_, L"无法打开计算机管理。", L"系统功能", MB_OK | MB_ICONWARNING);
        }
        return;
    case BuiltinSystemContextAction::MapNetworkDrive:
        result = WNetConnectionDialog(hwnd_, RESOURCETYPE_DISK);
        break;
    case BuiltinSystemContextAction::DisconnectNetworkDrive:
        result = WNetDisconnectDialog(hwnd_, RESOURCETYPE_DISK);
        break;
    case BuiltinSystemContextAction::EmptyRecycleBin: {
        const HRESULT hr = SHEmptyRecycleBinW(hwnd_, nullptr, 0);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            MessageBoxW(hwnd_, L"清空回收站失败。", L"回收站", MB_OK | MB_ICONWARNING);
        } else if (SUCCEEDED(hr)) {
            ShowToast(L"回收站已清空。", ThemedToastRole::Success);
        }
        return;
    }
    }

    if (result != NO_ERROR && result != ERROR_CANCELLED) {
        MessageBoxW(hwnd_, (L"Windows 系统操作失败，错误代码：" + std::to_wstring(result)).c_str(), L"系统功能", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::ShowTodoMenu(int todoId, POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    HMENU menu = CreatePopupMenu();
    const TodoItem* item = FindTodoItem(todoId);
    const bool done = item && !item->completedAt.empty();
    const bool enabled = !item || item->enabled;
    const std::wstring now = CurrentTodoTimestamp();
    const bool reminderDue = item && IsTodoReminderDue(*item, now);
    const bool reminderSent = item && !EffectiveTodoReminderDueAt(*item).empty() &&
        item->lastNotifiedDueAt == EffectiveTodoReminderDueAt(*item);
    const bool reminderViewed = item && !EffectiveTodoReminderDueAt(*item).empty() &&
        item->lastViewedDueAt == EffectiveTodoReminderDueAt(*item);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_TODO_ITEM, L"编辑待办事项");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TODO_DONE, done ? L"标记为未完成" : L"标记为完成");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TODO_ENABLED, enabled ? L"禁用待办事项" : L"启用待办事项");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(
        menu,
        MF_STRING | ((!reminderSent || reminderViewed) ? MF_GRAYED : 0),
        ID_MENU_TODO_REMINDER_VIEWED,
        reminderViewed ? L"本次提醒已查看" : L"标记本次提醒为已查看");
    AppendThemedMenuItem(
        menu,
        MF_STRING | (!reminderDue ? MF_GRAYED : 0),
        ID_MENU_TODO_REMINDER_IGNORE,
        L"忽略本次提醒");
    HMENU snoozeMenu = CreatePopupMenu();
    AppendThemedMenuItem(snoozeMenu, MF_STRING, ID_MENU_TODO_REMINDER_SNOOZE_5, L"5 分钟后");
    AppendThemedMenuItem(snoozeMenu, MF_STRING, ID_MENU_TODO_REMINDER_SNOOZE_30, L"30 分钟后");
    AppendThemedMenuItem(snoozeMenu, MF_STRING, ID_MENU_TODO_REMINDER_SNOOZE_60, L"1 小时后");
    AppendThemedMenuItem(
        menu,
        MF_POPUP | (!reminderDue ? MF_GRAYED : 0),
        reinterpret_cast<UINT_PTR>(snoozeMenu),
        L"稍后提醒",
        true,
        -1,
        -1,
        MenuIconHistory);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_TODO_ITEM, L"删除待办事项");
    AppendThemedSeparator(menu);
    AppendTodoSortItems(menu, FindGroup(currentTagId_));
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_DONE_TODOS, L"清空已完成");
    selectedTodoId_ = todoId;
    menuContextKind_ = HitKind::Todo;
    menuContextId_ = todoId;
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::CreateTooltip() {
    if (embeddedUi_ || !hwnd_) {
        return;
    }
    embeddedUi_ = std::make_unique<ThemedWindowUi>(
        instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact, 1, 1);
    tooltipText_.clear();
}

void MainWindow::ShowToast(const std::wstring& text, ThemedToastRole role, int durationMs) {
    CreateTooltip();
    if (!embeddedUi_) {
        return;
    }
    ThemedToastOptions options{};
    options.role = role;
    if (durationMs > 0) {
        options.durationMs = durationMs;
    }
    embeddedUi_->ui().ShowToast(text, options);
}

void MainWindow::ApplyTooltipTheme() {
    // The embedded themed host resolves tooltip resources lazily from theme_.
}

void MainWindow::HideItemTooltip() {
    if (embeddedUi_) embeddedUi_->ui().HideTooltip();
    tooltipItemKind_ = HitKind::None;
    tooltipItemId_ = 0;
}

std::wstring MainWindow::LinkTooltipText(const Link& link) const {
    const auto appendLine = [](std::wstring& text, const std::wstring& line) {
        if (line.empty()) {
            return;
        }
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += line;
    };

    const std::wstring name = Trim(link.name);
    const std::wstring remark = Trim(link.remark);
    const SystemFunctionDefinition* systemFunction = SystemFunctionForLink(link);

    std::wstring text;
    if (!name.empty()) {
        appendLine(text, L"名称: " + name);
    }

    if (systemFunction) {
        appendLine(text, L"类型: 系统功能");
        const std::wstring description = !remark.empty() ? remark : Trim(systemFunction->remark);
        if (!description.empty()) {
            appendLine(text, L"说明: " + description);
        }
        if (systemFunction->type == 1) {
            const std::wstring location = ExpandEnvironmentStringsSafe(Trim(systemFunction->target));
            appendLine(text, L"位置: " + location);
        } else if (std::wstring(systemFunction->name) == L"hosts") {
            const std::wstring file = ExpandEnvironmentStringsSafe(Trim(systemFunction->parameter));
            appendLine(text, L"文件: " + file);
        }
    } else {
        std::wstring installLocation = Trim(link.path);
        if (!installLocation.empty()) {
            installLocation = IsUrlLink(link) ? NormalizeUrl(installLocation) : ExpandEnvironmentStringsSafe(installLocation);
        } else if (!Trim(link.workDir).empty()) {
            installLocation = ExpandEnvironmentStringsSafe(Trim(link.workDir));
        }
        if (!installLocation.empty()) {
            appendLine(text, (IsUrlLink(link) ? L"网址: " : L"安装位置: ") + installLocation);
        }
        if (!remark.empty()) {
            appendLine(text, L"备注: " + remark);
        }
    }

    if (IsVolumeMenuIcon(SystemFunctionMenuIconForLink(link))) {
        if (const auto volume = CurrentMasterVolumeText()) {
            appendLine(text, *volume);
        }
    }
    appendLine(text, L"点击次数: " + std::to_wstring(link.runCount));
    return text;
}

std::wstring MainWindow::TodoTooltipText(const TodoItem& item) const {
    const auto appendLine = [](std::wstring& text, const std::wstring& line) {
        if (line.empty()) {
            return;
        }
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += line;
    };

    std::wstring text;
    const std::wstring title = Trim(item.title);
    if (!title.empty()) {
        appendLine(text, L"标题: " + title);
    }

    std::wstring status = L"待办";
    if (!item.enabled) {
        status = L"已禁用";
    } else if (!item.completedAt.empty()) {
        status = L"已完成";
    } else if (IsTodoOverdue(item)) {
        status = L"已逾期";
    }
    appendLine(text, L"状态: " + status);

    std::wstring reminderStatus;
    switch (GetTodoReminderStatus(item, CurrentTodoTimestamp())) {
    case TodoReminderStatus::Sent: reminderStatus = L"已发送"; break;
    case TodoReminderStatus::Viewed: reminderStatus = L"已查看"; break;
    case TodoReminderStatus::Ignored: reminderStatus = L"已忽略本次"; break;
    case TodoReminderStatus::Snoozed: reminderStatus = L"稍后提醒至 " + item.snoozedUntil; break;
    case TodoReminderStatus::Completed: reminderStatus = L"已完成"; break;
    case TodoReminderStatus::Disabled: reminderStatus = L"未启用"; break;
    case TodoReminderStatus::Pending: reminderStatus = L"待提醒"; break;
    }
    appendLine(text, L"提醒: " + reminderStatus);

    const bool recurring = IsRecurringTodoSchedule(item.scheduleKind);
    if (recurring) {
        appendLine(text, L"重复: " + TodoScheduleText(item));
        const std::wstring remaining = TodoRemainingText(item);
        if (!remaining.empty()) {
            appendLine(text, L"剩余: " + remaining);
        }
        if (!item.nextDueAt.empty()) {
            appendLine(text, L"下次: " + item.nextDueAt);
        }
        appendLine(text, L"已执行: " + std::to_wstring(std::max(0, item.repeatFinished)) + L" 次");
        if (item.repeatLimit > 0) {
            appendLine(text, L"进度: " + std::to_wstring(std::max(0, item.repeatFinished)) + L"/" + std::to_wstring(item.repeatLimit));
        }
    } else if (item.scheduleKind == TodoScheduleKind::Once && !item.nextDueAt.empty()) {
        const std::wstring remaining = TodoRemainingText(item);
        if (!remaining.empty()) {
            appendLine(text, L"剩余: " + remaining);
        }
        appendLine(text, L"时间: " + item.nextDueAt);
    }

    if (!item.completedAt.empty()) {
        appendLine(text, L"完成时间: " + item.completedAt);
    }

    const auto contentLines = LimitedTooltipContentLines(item.content);
    for (std::size_t i = 0; i < contentLines.size(); ++i) {
        appendLine(text, (i == 0 ? L"内容: " : L"      ") + contentLines[i]);
    }

    return text;
}

void MainWindow::UpdateItemTooltip(const HitArea& hit, POINT screenPoint) {
    if (!config_.showTooltip) {
        HideItemTooltip();
        return;
    }

    std::wstring text;
    if (hit.kind == HitKind::Link) {
        Link* link = FindLink(hit.id);
        if (!link) {
            HideItemTooltip();
            return;
        }
        text = LinkTooltipText(*link);
    } else if (hit.kind == HitKind::Todo) {
        TodoItem* item = FindTodoItem(hit.id);
        if (!item) {
            HideItemTooltip();
            return;
        }
        text = TodoTooltipText(*item);
    } else {
        HideItemTooltip();
        return;
    }
    if (text.empty()) {
        HideItemTooltip();
        return;
    }

    if (!embeddedUi_) {
        CreateTooltip();
    }
    if (!embeddedUi_) {
        return;
    }

    if (tooltipItemKind_ != hit.kind || tooltipItemId_ != hit.id || tooltipText_ != text) {
        tooltipText_ = text;
        tooltipItemKind_ = hit.kind;
        tooltipItemId_ = hit.id;
    }
    ThemedTooltipOptions options{};
    options.placement = ThemedTooltipPlacement::Cursor;
    options.multiline = true;
    embeddedUi_->ui().ShowTooltip(tooltipText_, screenPoint, options);
}

void MainWindow::ShowGroupMenu(int groupId, POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_GROUP, L"新建分组");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_GROUP, L"重命名");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_GROUP, L"删除分组");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_GROUP_LINKS, L"刷新");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_UP, L"左移(Shift+←)");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_DOWN, L"右移(Shift+→)");
    menuContextKind_ = HitKind::Group;
    menuContextId_ = groupId;
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::ShowGroupBlankMenu(POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_GROUP, L"新建分组");
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::ShowTagMenu(int tagId, POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    HMENU menu = CreatePopupMenu();
    Group* tag = FindGroup(tagId);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TAG, L"新建普通标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_NOTE_TAG, L"新建便签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TODO_TAG, L"新建待办事项标签页");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_TAG, L"重命名标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_TAG, L"删除标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_UP, L"上移");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_DOWN, L"下移");
    HMENU moveMenu = CreatePopupMenu();
    AppendGroupTargetMenu(moveMenu, ID_MENU_MOVE_TAG_TO_BASE, menuGroupTargetIds_, tag ? tag->parentGroup : 0);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"移动到", true);
    if (tag && IsTodoItemsTag(*tag)) {
        AppendThemedSeparator(menu);
        AppendTodoPageItems(menu, tag);
    } else if (!tag || !IsNoteTag(*tag)) {
        AppendThemedSeparator(menu);
        AppendAddLinkItems(menu);
        AppendThemedSeparator(menu);
        AppendViewOptionItems(menu, tag);
        AppendThemedSeparator(menu);
        if (tag && IsOrdinaryTag(*tag)) {
            AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_PAGE_ICONS, L"刷新");
        }
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_TAG_LINKS, L"清空本页应用");
    }
    menuContextKind_ = HitKind::Tag;
    menuContextId_ = tagId;
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::ShowTagBlankMenu(POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TAG, L"新建普通标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_NOTE_TAG, L"新建便签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TODO_TAG, L"新建待办事项标签页");
    menuContextKind_ = HitKind::Group;
    menuContextId_ = currentGroupId_;
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::ShowBackgroundMenu(POINT screenPoint) {
    DockAutoHidePause dockPause(*this);
    ResetMenuVisuals(screenPoint);
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    HMENU menu = CreatePopupMenu();
    Group* tag = FindGroup(currentTagId_);
    if (tag && IsTodoItemsTag(*tag)) {
        AppendTodoPageItems(menu, tag);
    } else if (!tag || !IsNoteTag(*tag)) {
        AppendAddLinkItems(menu);
        AppendThemedSeparator(menu);
        AppendViewOptionItems(menu, tag);
        AppendThemedSeparator(menu);
        if (tag && IsOrdinaryTag(*tag)) {
            AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_PAGE_ICONS, L"刷新");
        }
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_TAG_LINKS, L"清空本页应用");
    }
    TrackMainPopupMenu(menu, screenPoint);
    DestroyMenu(menu);
}

void MainWindow::AppendThemeItemsToMenu(HMENU menu) {
    const std::filesystem::path themeDirectory = appDirectory_ / L"theme";
    const auto themeItems = DiscoverThemeMenuItems(themeDirectory, config_.theme);
    const std::wstring effectiveTheme = EffectiveThemeName(themeDirectory, config_.theme);
    const std::size_t count = std::min<std::size_t>(themeItems.size(), 100);
    for (std::size_t i = 0; i < count; ++i) {
        AppendThemedStateMenuItem(menu, MF_STRING, ID_MENU_THEME_BASE + static_cast<UINT>(i), themeItems[i].label, effectiveTheme == themeItems[i].name, MenuIconTheme);
    }
}

void MainWindow::AppendAddLinkItems(HMENU menu) {
    HMENU systemMenu = CreatePopupMenu();
    AppendSystemFunctionItems(systemMenu, ID_MENU_ADD_SYSTEM_FUNCTION_BASE);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_FILE, L"添加文件");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_FOLDER, L"添加文件夹");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_URL, L"添加网址");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(systemMenu), L"添加系统功能", true, -1, -1, MenuIconSystem);
}

void MainWindow::AppendViewOptionItems(HMENU menu, const Group* tag) {
    // These submenus reflect the selected tag page, not a global display preference.
    HMENU iconMenu = CreatePopupMenu();
    const int iconSize = EffectiveIconSize(tag);
    AppendThemedStateMenuItem(iconMenu, MF_STRING, ID_MENU_ICON_SMALL, L"小图标", iconSize == 24, MenuIconSize);
    AppendThemedStateMenuItem(iconMenu, MF_STRING, ID_MENU_ICON_MEDIUM, L"中图标", iconSize == 32, MenuIconSize);
    AppendThemedStateMenuItem(iconMenu, MF_STRING, ID_MENU_ICON_LARGE, L"大图标", iconSize == 48, MenuIconSize);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), L"图标大小", true);

    HMENU layoutMenu = CreatePopupMenu();
    const int layout = EffectiveLinkLayout(tag);
    AppendThemedStateMenuItem(layoutMenu, MF_STRING, ID_MENU_LAYOUT_LIST, L"列表", layout == 0, MenuIconList);
    AppendThemedStateMenuItem(layoutMenu, MF_STRING, ID_MENU_LAYOUT_TILE, L"平铺", layout == 1, MenuIconTile);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), L"查看方式", true);

    HMENU sortMenu = CreatePopupMenu();
    const int sort = tag ? tag->sort : 0;
    const int sortDirection = tag ? tag->sortDirection : 0;
    AppendThemedStateMenuItem(sortMenu, MF_STRING, ID_MENU_SORT_POS, L"手动排序", sort == 0, MenuIconSort);
    AppendThemedStateMenuItem(sortMenu, MF_STRING, ID_MENU_SORT_RUNCOUNT, L"按运行次数", sort == 1, SortDirectionMenuIcon(1, sort == 1 ? sortDirection : DefaultLinkSortDirection(1)));
    AppendThemedStateMenuItem(sortMenu, MF_STRING, ID_MENU_SORT_NAME, L"按名称", sort == 2, SortDirectionMenuIcon(2, sort == 2 ? sortDirection : DefaultLinkSortDirection(2)));
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式", true);
}

void MainWindow::AppendTodoSortItems(HMENU menu, const Group* tag) {
    HMENU sortMenu = CreatePopupMenu();
    const int sort = tag ? tag->sort : 0;
    AppendThemedStateMenuItem(sortMenu, MF_STRING, ID_MENU_TODO_SORT_DUE, L"按提醒时间（推荐）", sort == 0, MenuIconSort);
    AppendThemedStateMenuItem(sortMenu, MF_STRING, ID_MENU_TODO_SORT_CREATED, L"按创建时间", sort == 1, MenuIconSort);
    AppendThemedStateMenuItem(sortMenu, MF_STRING, ID_MENU_TODO_SORT_TITLE, L"按标题", sort == 2, MenuIconSort);
    AppendThemedStateMenuItem(sortMenu, MF_STRING, ID_MENU_TODO_SORT_STATUS, L"按完成状态", sort == 3, MenuIconSort);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式", true);
}

void MainWindow::AppendTodoPageItems(HMENU menu, const Group* tag) {
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TODO_ITEM, L"新增待办事项");
    AppendTodoSortItems(menu, tag);
    AppendThemedSeparator(menu);
    const int overdueCount = tag ? OverdueTodoCount(tag->id) : 0;
    AppendThemedMenuItem(
        menu,
        MF_STRING | (overdueCount <= 0 ? MF_GRAYED : 0),
        ID_MENU_COMPLETE_OVERDUE_TODOS,
        L"标记全部逾期项为完成（" + std::to_wstring(overdueCount) + L" 项）");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_DONE_TODOS, L"清空已完成");
}

void MainWindow::AppendUnifiedViewOptionItems(HMENU menu) {
    bool hasTag = false;
    bool allSmall = true;
    bool allMedium = true;
    bool allLarge = true;
    bool allList = true;
    bool allTile = true;
    bool allSortPos = true;
    bool allSortRunCount = true;
    bool allSortName = true;
    bool allSortAscending = true;
    bool allSortDescending = true;

    for (const auto& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        hasTag = true;
        const int iconSize = EffectiveIconSize(&tag);
        const int layout = EffectiveLinkLayout(&tag);
        allSmall = allSmall && iconSize == 24;
        allMedium = allMedium && iconSize == 32;
        allLarge = allLarge && iconSize == 48;
        allList = allList && layout == 0;
        allTile = allTile && layout == 1;
        allSortPos = allSortPos && tag.sort == 0;
        allSortRunCount = allSortRunCount && tag.sort == 1;
        allSortName = allSortName && tag.sort == 2;
        allSortAscending = allSortAscending && tag.sortDirection == 0;
        allSortDescending = allSortDescending && tag.sortDirection != 0;
    }

    const UINT disabled = hasTag ? 0 : MF_GRAYED;
    HMENU iconMenu = CreatePopupMenu();
    AppendThemedStateMenuItem(iconMenu, MF_STRING | disabled, ID_MENU_ALL_ICON_SMALL, L"小图标", hasTag && allSmall, MenuIconSize);
    AppendThemedStateMenuItem(iconMenu, MF_STRING | disabled, ID_MENU_ALL_ICON_MEDIUM, L"中图标", hasTag && allMedium, MenuIconSize);
    AppendThemedStateMenuItem(iconMenu, MF_STRING | disabled, ID_MENU_ALL_ICON_LARGE, L"大图标", hasTag && allLarge, MenuIconSize);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), L"统一图标大小", true);

    HMENU layoutMenu = CreatePopupMenu();
    AppendThemedStateMenuItem(layoutMenu, MF_STRING | disabled, ID_MENU_ALL_LAYOUT_LIST, L"列表", hasTag && allList, MenuIconList);
    AppendThemedStateMenuItem(layoutMenu, MF_STRING | disabled, ID_MENU_ALL_LAYOUT_TILE, L"平铺", hasTag && allTile, MenuIconTile);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), L"统一查看方式", true);

    HMENU sortMenu = CreatePopupMenu();
    const bool allSortRunCountSameDirection = allSortRunCount && (allSortAscending || allSortDescending);
    const bool allSortNameSameDirection = allSortName && (allSortAscending || allSortDescending);
    const int unifiedRunCountDirection = hasTag && allSortRunCount && allSortDescending ? 1 : DefaultLinkSortDirection(1);
    const int unifiedNameDirection = hasTag && allSortName && allSortDescending ? 1 : 0;
    AppendThemedStateMenuItem(sortMenu, MF_STRING | disabled, ID_MENU_ALL_SORT_POS, L"手动排序", hasTag && allSortPos, MenuIconSort);
    AppendThemedStateMenuItem(sortMenu, MF_STRING | disabled, ID_MENU_ALL_SORT_RUNCOUNT, L"按运行次数", hasTag && allSortRunCountSameDirection, SortDirectionMenuIcon(1, unifiedRunCountDirection));
    AppendThemedStateMenuItem(sortMenu, MF_STRING | disabled, ID_MENU_ALL_SORT_NAME, L"按名称", hasTag && allSortNameSameDirection, SortDirectionMenuIcon(2, unifiedNameDirection));
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"统一排序方式", true);
}

void MainWindow::AppendSystemFunctionItems(HMENU menu, UINT commandBase) {
    const auto functions = SystemFunctions();
    const std::size_t limit = commandBase == ID_MENU_ADD_SYSTEM_FUNCTION_BASE
        ? ID_MENU_ADD_SYSTEM_FUNCTION_LIMIT
        : ID_MENU_SYSTEM_FUNCTION_LIMIT;
    const std::size_t count = std::min<std::size_t>(functions.size(), limit);
    const std::size_t columnBreak = count > 18 ? (count + 1) / 2 : count + 1;
    for (std::size_t i = 0; i < count; ++i) {
        UINT flags = MF_STRING;
        if (i == columnBreak) {
            flags |= MF_MENUBARBREAK;
        }
        AppendThemedMenuItem(
            menu,
            flags,
            commandBase + static_cast<UINT>(i),
            functions[i].name,
            false,
            functions[i].menuIcon != MenuIconNone ? -1 : SystemFunctionImageIndex(functions[i]),
            functions[i].stockIcon,
            functions[i].menuIcon);
    }
    if (count == 0) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可用功能");
    }
}

void MainWindow::AppendToolItems(HMENU menu) {
    menuToolEngines_.clear();
    menuToolEnabled_.clear();
    const auto plugins = BuiltinToolMenuPlugins(pluginRegistry_.LoadPlugins());
    for (const auto& plugin : plugins) {
        if (plugin.kind != L"builtin-tool" || plugin.engine.empty()) {
            continue;
        }
        if (menuToolEngines_.size() >= ID_MENU_TOOL_LIMIT) {
            break;
        }
        const UINT command = ID_MENU_TOOL_BASE + static_cast<UINT>(menuToolEngines_.size());
        menuToolEngines_.push_back(plugin.engine);
        menuToolEnabled_.push_back(plugin.enabled);
        AppendThemedMenuItem(menu, MF_STRING | (plugin.enabled ? 0 : MF_GRAYED), command, plugin.name, false, -1, -1, MenuIconTools);
    }
    if (menuToolEngines_.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, ID_MENU_TOOL_BASE, L"无可用工具");
    }
}

std::vector<int> MainWindow::GroupTargetIds(int excludedGroupId) const {
    std::vector<int> targetIds;
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        if (group.id == excludedGroupId) {
            continue;
        }
        if (targetIds.size() >= ID_MENU_DYNAMIC_TARGET_LIMIT) {
            break;
        }
        targetIds.push_back(group.id);
    }
    return targetIds;
}

std::vector<int> MainWindow::GroupedTagTargetIds(int excludedTagId) const {
    std::vector<int> targetIds;
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        for (const auto& tag : model_.groups) {
            if (tag.parentGroup != group.id || tag.id == excludedTagId) {
                continue;
            }
            if (targetIds.size() >= ID_MENU_DYNAMIC_TARGET_LIMIT) {
                return targetIds;
            }
            targetIds.push_back(tag.id);
        }
    }
    return targetIds;
}

void MainWindow::AppendGroupTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedGroupId) {
    targetIds = GroupTargetIds(excludedGroupId);
    for (std::size_t i = 0; i < targetIds.size(); ++i) {
        const Group* group = FindGroup(targetIds[i]);
        if (!group) {
            continue;
        }
        AppendThemedMenuItem(menu, MF_STRING, commandBase + static_cast<UINT>(i), group->name);
    }
    if (targetIds.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可选分组");
    }
}

void MainWindow::AppendTagTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedTagId) {
    targetIds.clear();
    for (const auto& tag : model_.groups) {
        if (tag.parentGroup == 0 || tag.id == excludedTagId) {
            continue;
        }
        if (targetIds.size() >= 500) {
            break;
        }
        const UINT command = commandBase + static_cast<UINT>(targetIds.size());
        const std::wstring text = TagDisplayName(tag);
        AppendThemedMenuItem(menu, MF_STRING, command, text);
        targetIds.push_back(tag.id);
    }
    if (targetIds.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可选标签");
    }
}

void MainWindow::AppendGroupedTagTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedTagId) {
    targetIds.clear();
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        HMENU tagMenu = CreatePopupMenu();
        int count = 0;
        for (const auto& tag : model_.groups) {
            if (tag.parentGroup != group.id || tag.id == excludedTagId) {
                continue;
            }
            if (targetIds.size() >= ID_MENU_DYNAMIC_TARGET_LIMIT) {
                break;
            }
            const UINT command = commandBase + static_cast<UINT>(targetIds.size());
            AppendThemedMenuItem(tagMenu, MF_STRING, command, tag.name);
            targetIds.push_back(tag.id);
            ++count;
        }
        if (count == 0) {
            AppendThemedMenuItem(tagMenu, MF_STRING | MF_GRAYED, commandBase + static_cast<UINT>(targetIds.size()), L"无可选标签");
        }
        AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tagMenu), group.name, true);
    }
    if (groups.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可选分组");
    }
}

void MainWindow::SaveWindowState() {
    if (!windowStateSaveEnabled_ || !hwnd_ || IsIconic(hwnd_)) {
        return;
    }

    RECT rect{};
    if (!dockHidden_ && GetWindowRect(hwnd_, &rect)) {
        config_.posX = rect.left;
        config_.posY = rect.top;
    }
    RECT client{};
    if (GetClientRect(hwnd_, &client)) {
        config_.width = client.right - client.left;
        config_.height = client.bottom - client.top;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
}

void MainWindow::WakeUp() {
    if (popupMenuDepth_ > 0) {
        popupWakePending_ = true;
        return;
    }
    if (dockHidden_) {
        DockRestore();
    }
    dockHideDueTick_ = GetTickCount64() + kDockRestoreGraceMs;
    if (!IsWindowVisible(hwnd_)) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
    }
    ShowWindowRespectFocusPolicy(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_,
                 MainWindowActivationInsertAfter(config_.topMost),
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    const bool activated = ActivateWindow(hwnd_);
    WriteAppLog(
        L"主窗口唤起完成: topmost=" + std::wstring(config_.topMost ? L"1" : L"0") +
        L", dock_hidden=" + std::wstring(dockHidden_ ? L"1" : L"0") +
        L", activated=" + std::wstring(activated ? L"1" : L"0"));
}

bool MainWindow::IsEffectivelyVisible() const {
    return IsWindowVisible(hwnd_) && !dockHidden_;
}

bool MainWindow::IsMainWindowForeground() const {
    if (!IsEffectivelyVisible() || IsIconic(hwnd_)) {
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    const HWND foregroundRoot = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    const HWND mainRoot = GetAncestor(hwnd_, GA_ROOT);
    return foregroundRoot && foregroundRoot == (mainRoot ? mainRoot : hwnd_);
}

void MainWindow::ToggleMainWindowFromHotKey() {
    if (IsMainWindowForeground()) {
        HideMainWindow();
        return;
    }
    WakeUp();
}

void MainWindow::HideMainWindowAfterLink() {
    if (config_.autoDock && IsWindowVisible(hwnd_) && DockHide()) {
        return;
    }
    HideMainWindow();
}

void MainWindow::HideMainWindow() {
    HideItemTooltip();
    if (dockHidden_) {
        HideDockPeek();
        const int width = dockRestoreRect_.right - dockRestoreRect_.left;
        const int height = dockRestoreRect_.bottom - dockRestoreRect_.top;
        ShowWindow(hwnd_, SW_HIDE);
        SetWindowPos(hwnd_,
                     MainWindowMoveInsertAfter(config_.topMost),
                     dockRestoreRect_.left,
                     dockRestoreRect_.top,
                     width,
                     height,
                     MainWindowMoveFlags(config_.topMost, SWP_NOZORDER | SWP_NOACTIVATE));
        dockHidden_ = false;
        SaveWindowState();
        return;
    }
    SaveWindowState();
    ShowWindow(hwnd_, SW_HIDE);
}

void MainWindow::DiscardDeviceResources() {
    iconService_.Clear();
    ClearUiBitmaps();
    SafeRelease(renderTarget_);
}

HRESULT MainWindow::CreateDeviceResources() {
    if (renderTarget_) {
        return S_OK;
    }

    if (!startupFirstPaintLogged_) {
        WriteStartupTiming(L"d2d render target create begin");
    }
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const D2D1_SIZE_U size = D2D1::SizeU(rect.right - rect.left, rect.bottom - rect.top);
    const HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT),
        D2D1::HwndRenderTargetProperties(hwnd_, size),
        &renderTarget_);
    if (SUCCEEDED(hr) && renderTarget_) {
        renderTarget_->SetDpi(static_cast<float>(CurrentDpi()), static_cast<float>(CurrentDpi()));
    }
    if (!startupFirstPaintLogged_) {
        WriteStartupTiming(L"d2d render target create end", L"hr=" + std::to_wstring(static_cast<long>(hr)));
    }
    return hr;
}

void MainWindow::OnPaint() {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
    if (SUCCEEDED(CreateDeviceResources())) {
        Draw();
        if (!startupFirstPaintLogged_) {
            startupFirstPaintLogged_ = true;
            WriteStartupTiming(
                L"first paint completed",
                L"groups=" + std::to_wstring(model_.groups.size()) +
                    L", links=" + std::to_wstring(model_.links.size()) +
                    L", todos=" + std::to_wstring(model_.todos.size()));
            if (!config_.hideOnStart && !startupActivationPosted_) {
                startupActivationPosted_ = true;
                PostMessageW(hwnd_, WM_QUATTRO_STARTUP_ACTIVATE, 0, 0);
            }
        }
    }
    EndPaint(hwnd_, &paint);
}

void MainWindow::OnResize(UINT width, UINT height) {
    if (renderTarget_) {
        renderTarget_->Resize(D2D1::SizeU(width, height));
    }
    SaveWindowState();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::Draw() {
    const bool traceStartupPaint = !startupFirstPaintLogged_;
    if (traceStartupPaint) {
        WriteStartupTiming(L"first paint begin");
    }

    renderTarget_->BeginDraw();
    const D2D1_SIZE_F size = renderTarget_->GetSize();

    renderTarget_->Clear(theme_.color(L"window", L"normal", L"bg").d2d());
    D2D1_RECT_F title{}, groups{}, tags{}, links{};
    BuildLayout(size.width, size.height, title, groups, tags, links);
    ClampScrollOffsets();
    hitAreas_.clear();

    DrawTitle(title);
    if (traceStartupPaint) {
        WriteStartupTiming(L"first paint title drawn");
    }
    if (config_.showGroup) {
        DrawGroups(groups);
        if (traceStartupPaint) {
            WriteStartupTiming(L"first paint groups drawn");
        }
    }
    if (config_.showTag) {
        DrawTags(tags);
        if (traceStartupPaint) {
            WriteStartupTiming(L"first paint tags drawn");
        }
    }
    DrawLinks(links);
    if (traceStartupPaint) {
        WriteStartupTiming(L"first paint links drawn");
    }
    DrawRect(D2D1::RectF(0, 0, size.width, size.height), theme_.color(L"window", L"normal", L"border"));

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

void MainWindow::DrawTitle(D2D1_RECT_F rect) {
    if (Height(rect) <= 0) {
        return;
    }

    FillRect(rect, theme_.color(L"title", L"normal", L"bg"));
    FillRect(D2D1::RectF(rect.left, rect.bottom - 1.0f, rect.right, rect.bottom), theme_.color(L"title", L"normal", L"line"));

    const float iconSize = Metric(theme_, L"title", L"iconSize", 20.0f);
    const float iconLeft = Metric(theme_, L"title", L"iconLeft", 9.0f);
    const float iconTop = Metric(theme_, L"title", L"iconTop", 7.0f);
    D2D1_RECT_F appIcon = D2D1::RectF(rect.left + iconLeft, rect.top + iconTop, rect.left + iconLeft + iconSize, rect.top + iconTop + iconSize);
    if (ID2D1Bitmap* bitmap = LoadAppIconBitmap()) {
        renderTarget_->DrawBitmap(bitmap, appIcon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    const float buttonSize = ClampFloat(theme_.metric(L"titleButton", L"size", 26.0f), 18.0f, 40.0f);
    const float buttonGap = ClampFloat(theme_.metric(L"titleButton", L"gap", 2.0f), 0.0f, 12.0f);
    const float buttonRightInset = Metric(theme_, L"titleButton", L"rightInset", 4.0f);
    const float buttonTopInset = Metric(theme_, L"titleButton", L"topInset", 4.0f);
    const std::array<HitKind, 4> buttons = TitleButtonsRightToLeft();
    const float buttonReserve = TitleButtonsReserveWidth();
    const float titleTextLeft = appIcon.right + Metric(theme_, L"title", L"textGap", 7.0f);
    const float titleTextMaxEnd = rect.right - buttonReserve - Metric(theme_, L"title", L"textGap", 7.0f);
    const float titleTextEnd = std::max(titleTextLeft, titleTextMaxEnd);
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const std::wstring buildMarker = BuildMarkerDisplayText();
    if (buildMarker.empty()) {
        const D2D1_RECT_F nameRect = D2D1::RectF(titleTextLeft, rect.top, titleTextEnd, rect.bottom);
        DrawTextBlock(kAppDisplayName, titleFormat_, nameRect, theme_.color(L"title", L"normal", L"text"));
    } else {
        IDWriteTextFormat* markerFormat = navSelectedFormat_ ? navSelectedFormat_ : titleFormat_;
        const float markerWidth = MeasureTextWidth(
            buildMarker,
            markerFormat,
            std::max(1.0f, titleTextEnd - titleTextLeft));
        const float markerGap = Metric(theme_, L"title", L"textGap", 7.0f);
        const MainTitleBuildMarkerLayout textLayout = CalculateMainTitleBuildMarkerLayout(
            titleTextLeft,
            titleTextEnd,
            markerWidth,
            markerGap);
        const D2D1_RECT_F nameRect = D2D1::RectF(titleTextLeft, rect.top, textLayout.nameEnd, rect.bottom);
        const D2D1_RECT_F markerRect = D2D1::RectF(textLayout.markerLeft, rect.top, titleTextEnd, rect.bottom);
        DrawTextBlock(kAppDisplayName, titleFormat_, nameRect, theme_.color(L"title", L"normal", L"text"));
        DrawTextBlock(buildMarker, markerFormat, markerRect, theme_.color(L"global", L"danger", L"text"));
    }

    float buttonRight = rect.right - buttonRightInset;
    for (HitKind kind : buttons) {
        if (!IsTitleButtonVisible(kind)) {
            continue;
        }
        D2D1_RECT_F button = D2D1::RectF(buttonRight - buttonSize, rect.top + buttonTopInset, buttonRight, rect.top + buttonTopInset + buttonSize);
        if (IsHover(kind, 0)) {
            FillRect(button, kind == HitKind::CloseButton ? theme_.color(L"titleCloseButton", L"hover", L"bg") : theme_.color(L"titleButton", L"hover", L"bg"));
        }
        DrawButtonIcon(kind, button, kind == HitKind::CloseButton && IsHover(kind, 0) ? theme_.color(L"titleCloseButton", L"hover", L"icon") : theme_.color(L"titleButton", L"normal", L"icon"));
        hitAreas_.push_back(HitArea{kind, 0, button});
        buttonRight -= buttonSize + buttonGap;
    }
}

std::array<MainWindow::HitKind, 4> MainWindow::TitleButtonsRightToLeft() {
    return {
        HitKind::CloseButton,
        HitKind::MenuButton,
        HitKind::ToolButton,
        HitKind::SkinButton,
    };
}

bool MainWindow::IsTitleButtonVisible(HitKind kind) const {
    return !((kind == HitKind::ToolButton && !config_.showToolboxButton) ||
             (kind == HitKind::SkinButton && !config_.showSkinButton));
}

float MainWindow::TitleButtonsReserveWidth() const {
    const float buttonSize = ClampFloat(theme_.metric(L"titleButton", L"size", 26.0f), 18.0f, 40.0f);
    const float buttonGap = ClampFloat(theme_.metric(L"titleButton", L"gap", 2.0f), 0.0f, 12.0f);
    const float buttonReserveInset = Metric(theme_, L"titleButton", L"reserveInset", 16.0f);
    int visibleButtonCount = 0;
    for (HitKind kind : TitleButtonsRightToLeft()) {
        if (IsTitleButtonVisible(kind)) {
            ++visibleButtonCount;
        }
    }
    if (visibleButtonCount <= 0) {
        return buttonReserveInset;
    }
    return buttonReserveInset +
        static_cast<float>(visibleButtonCount) * buttonSize +
        static_cast<float>(visibleButtonCount - 1) * buttonGap;
}

void MainWindow::DrawGroups(D2D1_RECT_F rect) {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return;
    }

    FillRect(rect, theme_.color(L"majorNav", L"normal", L"bg"));
    const bool vertical = Height(rect) > Width(rect);
    const auto groups = MajorGroups();
    const auto itemRects = MajorGroupItemRects(rect);

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (const auto& itemRect : itemRects) {
        if (vertical && itemRect.rect.bottom < rect.top + Metric(theme_, L"tabButton", L"groupPadding", 3.0f)) {
            continue;
        }
        if (!vertical && itemRect.rect.right < rect.left + 2.0f) {
            continue;
        }
        if ((vertical && itemRect.rect.top > rect.bottom - 2.0f) ||
            (!vertical && itemRect.rect.left > rect.right - 2.0f)) {
            break;
        }
        auto groupIt = std::find_if(groups.begin(), groups.end(), [&itemRect](const Group& group) {
            return group.id == itemRect.id;
        });
        if (groupIt == groups.end()) {
            continue;
        }
        const bool selected = groupIt->id == currentGroupId_;
        const bool hovered = IsHover(HitKind::Group, groupIt->id);
        DrawMajorNavItem(itemRect.rect, groupIt->name, selected, hovered, vertical);
        hitAreas_.push_back(HitArea{HitKind::Group, groupIt->id, IntersectRectF(itemRect.rect, rect)});
    }
    if (navDragActive_ && navDragKind_ == NavDragKind::Group && navDragMode_ == NavDragMode::Insert) {
        DrawNavInsertIndicator(itemRects, navDragInsertIndex_, vertical, rect, theme_.color(L"majorNavItem", L"selected", L"accent"));
    }
    renderTarget_->PopAxisAlignedClip();
    FillRect(D2D1::RectF(rect.left, rect.bottom - 1.0f, rect.right, rect.bottom), theme_.color(L"majorNav", L"normal", L"line"));
    if (vertical) {
        DrawScrollBar(rect, groupScrollOffset_, MaxGroupScrollOffset(rect), false);
    }
}

void MainWindow::DrawTags(D2D1_RECT_F rect) {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return;
    }

    FillRect(rect, theme_.color(L"content", L"normal", L"bg"));
    DrawContentCard(rect);
    const D2D1_RECT_F content = CardContentRectFor(rect);
    const auto tags = TagsForCurrentGroup();
    const auto itemRects = TagItemRects(rect);

    renderTarget_->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (const auto& itemRect : itemRects) {
        if (itemRect.rect.bottom < content.top + Metric(theme_, L"minorNavItem", L"topInset", 2.0f)) {
            continue;
        }
        if (itemRect.rect.top > content.bottom - 2.0f) {
            break;
        }
        auto tagIt = std::find_if(tags.begin(), tags.end(), [&itemRect](const Group& tag) {
            return tag.id == itemRect.id;
        });
        if (tagIt == tags.end()) {
            continue;
        }
        const bool selected = tagIt->id == currentTagId_;
        const bool hovered = IsHover(HitKind::Tag, tagIt->id);
        DrawMinorNavItem(itemRect.rect, tagIt->name, selected, hovered);
        hitAreas_.push_back(HitArea{HitKind::Tag, tagIt->id, IntersectRectF(itemRect.rect, content)});
    }
    if (navDragActive_ && navDragKind_ == NavDragKind::Tag && navDragMode_ == NavDragMode::Insert) {
        DrawNavInsertIndicator(itemRects, navDragInsertIndex_, true, content, theme_.color(L"minorNavItem", L"selected", L"accent"));
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(content, tagScrollOffset_, MaxTagScrollOffset(rect), false);
}

void MainWindow::DrawMajorNavItem(D2D1_RECT_F rect, const std::wstring& text, bool selected, bool hovered, bool vertical) {
    const wchar_t* state = selected ? L"selected" : (hovered ? L"hover" : L"normal");
    // Hover背景：轻浮出的浅底，无边框，弱于选中态
    if (hovered && !selected) {
        const float insetX = Metric(theme_, L"majorNavItem", L"hoverInsetX", 3.0f);
        const float insetY = Metric(theme_, L"majorNavItem", L"hoverInsetY", 4.0f);
        const float radius = Metric(theme_, L"majorNavItem", L"radius", 7.0f);
        FillRoundedRect(Inset(rect, insetX, insetY), theme_.color(L"majorNavItem", L"hover", L"bg"), radius);
    }

    // 选中态：accent 字色 + 半粗字 + 边缘 accent 指示条
    IDWriteTextFormat* format = (selected && navSelectedFormat_) ? navSelectedFormat_ : textFormat_;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    const float paddingX = Metric(theme_, L"tabButton", L"paddingX", 12.0f);
    DrawTextBlock(text, format, Inset(rect, paddingX, 0.0f), theme_.color(L"majorNavItem", state, L"text"));

    if (selected) {
        const float thickness = Metric(theme_, L"majorNavItem", L"indicatorHeight", 3.0f);
        const float insetX = Metric(theme_, L"majorNavItem", L"indicatorInsetX", 10.0f);
        const float radius = Metric(theme_, L"majorNavItem", L"indicatorRadius", 2.0f);
        const Color accent = theme_.color(L"majorNavItem", L"selected", L"accent");
        D2D1_RECT_F bar;
        if (vertical) {
            // 竖排：左侧竖条
            bar = D2D1::RectF(rect.left, rect.top + insetX, rect.left + thickness, rect.bottom - insetX);
        } else {
            // 横排：底部指示条
            bar = D2D1::RectF(rect.left + insetX, rect.bottom - thickness, rect.right - insetX, rect.bottom);
        }
        FillRoundedRect(bar, accent, radius);
    }
}

void MainWindow::DrawMinorNavItem(D2D1_RECT_F rect, const std::wstring& text, bool selected, bool hovered) {
    const wchar_t* state = selected ? L"selected" : (hovered ? L"hover" : L"normal");
    const float topInset = Metric(theme_, L"minorNavItem", L"topInset", 2.0f);
    const float bottomInset = Metric(theme_, L"minorNavItem", L"bottomInset", 2.0f);
    const D2D1_RECT_F body = D2D1::RectF(rect.left, rect.top + topInset, rect.right, rect.bottom - bottomInset);
    const float radius = Metric(theme_, L"minorNavItem", L"radius", 6.0f);

    // 选中/hover 浅底，无边框
    if (selected || hovered) {
        FillRoundedRect(body, theme_.color(L"minorNavItem", state, L"bg"), radius);
    }

    // 选中态：左侧 accent 竖条
    if (selected) {
        const float accentWidth = Metric(theme_, L"minorNavItem", L"accentWidth", 3.0f);
        const float accentInsetY = Metric(theme_, L"minorNavItem", L"accentInsetY", 3.0f);
        const Color accent = theme_.color(L"minorNavItem", L"selected", L"accent");
        FillRoundedRect(D2D1::RectF(body.left, body.top + accentInsetY, body.left + accentWidth, body.bottom - accentInsetY),
                        accent, accentWidth * 0.5f);
    }

    if (config_.tagAlign == L"right") {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (config_.tagAlign == L"center") {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    const float textInsetX = Metric(theme_, L"minorNavItem", L"textInsetX", 10.0f);
    DrawTextBlock(text, textFormat_, D2D1::RectF(rect.left + textInsetX, rect.top, rect.right - textInsetX, rect.bottom),
                  theme_.color(L"minorNavItem", state, L"text"));
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

void MainWindow::DrawNavInsertIndicator(
    const std::vector<NavItemRect>& items,
    int insertIndex,
    bool vertical,
    const D2D1_RECT_F& clipRect,
    const Color& accent) {
    if (items.empty() || insertIndex < 0) {
        return;
    }

    const int count = static_cast<int>(items.size());
    const int index = std::max(0, std::min(count, insertIndex));
    const float thickness = std::max(2.0f, Metric(theme_, L"separator", L"thickness", 1.0f) + 1.0f);
    const NavItemRect& anchor = items[std::min(index, count - 1)];
    D2D1_RECT_F line{};
    if (vertical) {
        float gap = 0.0f;
        if (index > 0 && index < count) {
            gap = std::max(0.0f, items[index].rect.top - items[index - 1].rect.bottom);
        } else if (index >= count && count > 1) {
            gap = std::max(0.0f, items[count - 1].rect.top - items[count - 2].rect.bottom);
        }
        const float y = index >= count ? anchor.rect.bottom + gap * 0.5f : anchor.rect.top - gap * 0.5f;
        line = D2D1::RectF(anchor.rect.left + 4.0f, y - thickness * 0.5f, anchor.rect.right - 4.0f, y + thickness * 0.5f);
    } else {
        float gap = 0.0f;
        if (index > 0 && index < count) {
            gap = std::max(0.0f, items[index].rect.left - items[index - 1].rect.right);
        } else if (index >= count && count > 1) {
            gap = std::max(0.0f, items[count - 1].rect.left - items[count - 2].rect.right);
        }
        const float x = index >= count ? anchor.rect.right + gap * 0.5f : anchor.rect.left - gap * 0.5f;
        line = D2D1::RectF(x - thickness * 0.5f, anchor.rect.top + 5.0f, x + thickness * 0.5f, anchor.rect.bottom - 5.0f);
    }

    const D2D1_RECT_F clipped = IntersectRectF(line, clipRect);
    if (Width(clipped) > 0.0f && Height(clipped) > 0.0f) {
        FillRoundedRect(clipped, accent, thickness * 0.5f);
    }
}

D2D1_RECT_F MainWindow::CardRectFor(const D2D1_RECT_F& regionRect) const {
    const float margin = Metric(theme_, L"content", L"cardMargin", 8.0f);
    const float halfGap = Metric(theme_, L"content", L"cardGap", 8.0f) * 0.5f;
    const float topGap = Metric(theme_, L"content", L"cardTopGap", 6.0f);

    // 判断每条边是否贴着窗口外沿（用外边距），否则是与相邻卡片的中缝（用半个间距）
    float windowWidth = regionRect.right;
    float windowHeight = regionRect.bottom;
    if (hwnd_) {
        RECT client{};
        if (GetClientRect(hwnd_, &client)) {
            windowWidth = static_cast<float>(client.right - client.left);
            windowHeight = static_cast<float>(client.bottom - client.top);
        }
    }
    const float eps = 1.0f;
    const bool leftOuter = regionRect.left <= eps;
    const bool rightOuter = regionRect.right >= windowWidth - eps;
    const bool bottomOuter = regionRect.bottom >= windowHeight - eps;

    const float insetLeft = leftOuter ? margin : halfGap;
    const float insetRight = rightOuter ? margin : halfGap;
    const float insetTop = topGap;  // 顶部总是紧邻分组栏或标题栏，用较小的间距
    const float insetBottom = bottomOuter ? margin : halfGap;

    D2D1_RECT_F card = D2D1::RectF(regionRect.left + insetLeft, regionRect.top + insetTop,
                                   regionRect.right - insetRight, regionRect.bottom - insetBottom);
    if (card.right < card.left) card.right = card.left;
    if (card.bottom < card.top) card.bottom = card.top;
    return card;
}

D2D1_RECT_F MainWindow::CardContentRectFor(const D2D1_RECT_F& regionRect) const {
    const float padX = Metric(theme_, L"content", L"cardPaddingX", 6.0f);
    const float padY = Metric(theme_, L"content", L"cardPaddingY", 6.0f);
    D2D1_RECT_F card = CardRectFor(regionRect);
    D2D1_RECT_F content = D2D1::RectF(card.left + padX, card.top + padY, card.right - padX, card.bottom - padY);
    if (content.right < content.left) content.right = content.left;
    if (content.bottom < content.top) content.bottom = content.top;
    return content;
}

void MainWindow::DrawContentCard(const D2D1_RECT_F& fullRect) {
    const float radius = Metric(theme_, L"content", L"cardRadius", 8.0f);
    const float borderWidth = Metric(theme_, L"content", L"cardBorderWidth", 1.0f);

    const D2D1_RECT_F card = CardRectFor(fullRect);
    if (Width(card) <= 0.0f || Height(card) <= 0.0f) {
        return;
    }

    // 扁平干净卡片：白底 + 一根利落边框，靠对比而非阴影建立层次
    FillRoundedRect(card, theme_.color(L"content", L"card", L"bg"), radius);
    DrawRoundedRect(card, theme_.color(L"content", L"card", L"border"), radius, borderWidth);
}

void MainWindow::DrawLinks(D2D1_RECT_F rect) {
    FillRect(rect, theme_.color(L"content", L"normal", L"bg"));
    const Group* currentTag = FindGroup(currentTagId_);
    if (currentTag && IsNoteTag(*currentTag)) {
        if (dragOver_) {
            Color highlight = theme_.color(L"content", L"empty", L"text");
            highlight.a = 0.14f;
            FillRect(rect, highlight);
            Color borderColor = theme_.color(L"content", L"empty", L"text");
            borderColor.a = 0.38f;
            DrawRect(D2D1::RectF(rect.left + 1.0f, rect.top + 1.0f, rect.right - 1.0f, rect.bottom - 1.0f), borderColor, 2.0f);
        }
        DrawNotePage(rect, *currentTag);
        return;
    }
    if (currentTag && IsTodoItemsTag(*currentTag)) {
        if (dragOver_) {
            Color highlight = theme_.color(L"content", L"empty", L"text");
            highlight.a = 0.14f;
            FillRect(rect, highlight);
            Color borderColor = theme_.color(L"content", L"empty", L"text");
            borderColor.a = 0.38f;
            DrawRect(D2D1::RectF(rect.left + 1.0f, rect.top + 1.0f, rect.right - 1.0f, rect.bottom - 1.0f), borderColor, 2.0f);
        }
        HideNoteEdit();
        DrawTodoItems(rect, *currentTag);
        return;
    }
    HideNoteEdit();

    // 启动项：灰底上托一张白色圆角卡片
    DrawContentCard(rect);
    const D2D1_RECT_F content = CardContentRectFor(rect);
    if (dragOver_) {
        const float radius = Metric(theme_, L"content", L"cardRadius", 10.0f);
        const Color accent = theme_.color(L"majorNavItem", L"selected", L"accent");
        Color highlight = accent;
        highlight.a = 0.10f;
        const D2D1_RECT_F card = CardRectFor(rect);
        FillRoundedRect(card, highlight, radius);
        DrawRoundedRect(card, accent, radius, 2.0f);
    }

    auto links = LinksForCurrentTag();
    if (links.empty()) {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawEmptyState(content, L"当前标签没有启动项", L"拖入文件、文件夹或网址即可添加", L"添加启动项");
        return;
    }

    const Group* tag = FindGroup(currentTagId_);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, content, tag);

    renderTarget_->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float itemRadius = Metric(theme_, L"list", L"radius", 7.0f);
    for (std::size_t index = 0; index < links.size(); ++index) {
        Link* link = links[index];
        D2D1_RECT_F item = LinkItemRect(content, metrics, static_cast<int>(index), linkScrollOffset_);
        if (item.bottom < content.top + 2.0f) {
            continue;
        }
        if (item.top > content.bottom - 2.0f) {
            break;
        }

        const bool linkHovered = IsHover(HitKind::Link, link->id);
        const bool anyLinkHovered = hover_.kind == HitKind::Link;
        const bool linkSelected = link->id == selectedLinkId_ && !anyLinkHovered;

        if (linkDragActive_ && link->id == linkDragTargetLinkId_ && linkDragMode_ == LinkDragMode::Swap) {
            FillRoundedRect(item, theme_.color(L"linkItem", L"hover", L"bg"), itemRadius);
            DrawRoundedRect(Inset(item, 1.5f, 1.5f), theme_.color(L"linkItem", L"selected", L"accent"), itemRadius, 2.0f);
        } else if (linkHovered || linkSelected) {
            FillRoundedRect(item, theme_.color(L"linkItem", L"selected", L"bg"), itemRadius);
            if (linkSelected && selectionByKeyboard_) {
                DrawRoundedRect(Inset(item, 1.5f, 1.5f), theme_.color(L"linkItem", L"selected", L"accent"), itemRadius, 1.0f);
            }
        }
        if (link->isCustomColor) {
            if (auto color = ParseCustomColor(link->customColor)) {
                FillRect(D2D1::RectF(
                             item.left,
                             item.top + Metric(theme_, L"linkItem", L"customColorInsetY", 4.0f),
                             item.left + Metric(theme_, L"linkItem", L"customColorWidth", 3.0f),
                             item.bottom - Metric(theme_, L"linkItem", L"customColorInsetY", 4.0f)),
                         *color);
            }
        }

        D2D1_RECT_F icon{};
        D2D1_RECT_F nameRect{};
        IDWriteTextFormat* nameFormat = textFormat_;
        if (metrics.layout == 0 || metrics.compactTile) {
            const float iconTop = item.top + (Height(item) - metrics.iconSize) * 0.5f;
            const float iconLeft = item.left + Metric(theme_, L"linkItem", L"listIconLeft", 8.0f);
            icon = D2D1::RectF(iconLeft, iconTop, iconLeft + metrics.iconSize, iconTop + metrics.iconSize);
            nameRect = D2D1::RectF(icon.right + Metric(theme_, L"linkItem", L"listTextGap", 8.0f), item.top, item.right - Metric(theme_, L"linkItem", L"listTextRightInset", 6.0f), item.bottom);
            nameFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            nameFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        } else {
            const float iconLeft = item.left + (Width(item) - metrics.iconSize) * 0.5f;
            const float iconTop = item.top + Metric(theme_, L"linkItem", L"gridIconTop", 8.0f);
            icon = D2D1::RectF(iconLeft, iconTop, iconLeft + metrics.iconSize, iconTop + metrics.iconSize);
            nameRect = D2D1::RectF(
                item.left + Metric(theme_, L"linkItem", L"gridTextPaddingX", 4.0f),
                icon.bottom + Metric(theme_, L"linkItem", L"gridTextGap", 4.0f),
                item.right - Metric(theme_, L"linkItem", L"gridTextPaddingX", 4.0f),
                item.bottom - Metric(theme_, L"linkItem", L"gridTextPaddingBottom", 4.0f));
            nameFormat = metrics.itemWidth <= Metric(theme_, L"linkItem", L"smallTextWidthThreshold", 78.0f) ? smallFormat_ : textFormat_;
            nameFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            nameFormat->SetWordWrapping(config_.linkNameSingleLine ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
        }

        if (ID2D1Bitmap* bitmap = iconService_.GetBitmap(renderTarget_, *link)) {
            renderTarget_->DrawBitmap(bitmap, icon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            const float iconRadius = Metric(theme_, L"iconFallback", L"radius", 7.0f);
            FillRoundedRect(icon, theme_.color(L"iconFallback", L"normal", L"bg"), iconRadius);
            DrawRoundedRect(icon, theme_.color(L"iconFallback", L"normal", L"border"), iconRadius);
        }
        DrawLinkName(link->name, nameFormat, nameRect, theme_.color(L"linkItem", L"normal", L"text"));
        hitAreas_.push_back(HitArea{HitKind::Link, link->id, IntersectRectF(item, content)});
    }
    if (linkDragActive_ && linkDragMode_ == LinkDragMode::Insert && linkDragInsertIndex_ >= 0) {
        const int count = static_cast<int>(links.size());
        const int index = std::max(0, std::min(count, linkDragInsertIndex_));
        const float thickness = std::max(2.0f, Metric(theme_, L"separator", L"thickness", 1.0f) + 1.0f);
        const Color accent = theme_.color(L"linkItem", L"selected", L"accent");
        D2D1_RECT_F line{};
        if (metrics.layout == 0 || metrics.compactTile) {
            D2D1_RECT_F anchor = LinkItemRect(content, metrics, std::min(index, std::max(0, count - 1)), linkScrollOffset_);
            const float y = index >= count ? anchor.bottom + metrics.gapY * 0.5f : anchor.top - metrics.gapY * 0.5f;
            line = D2D1::RectF(anchor.left + 4.0f, y - thickness * 0.5f, anchor.right - 4.0f, y + thickness * 0.5f);
        } else {
            D2D1_RECT_F anchor = LinkItemRect(content, metrics, std::min(index, std::max(0, count - 1)), linkScrollOffset_);
            if (index >= count) {
                const int nextColumn = index % std::max(1, metrics.columns);
                if (nextColumn == 0) {
                    const float y = anchor.bottom + metrics.gapY * 0.5f;
                    line = D2D1::RectF(content.left + metrics.leftInset, y - thickness * 0.5f, content.left + metrics.leftInset + metrics.itemWidth, y + thickness * 0.5f);
                } else {
                    const float x = anchor.right + metrics.gapX * 0.5f;
                    line = D2D1::RectF(x - thickness * 0.5f, anchor.top + 6.0f, x + thickness * 0.5f, anchor.bottom - 6.0f);
                }
            } else {
                const float x = anchor.left - metrics.gapX * 0.5f;
                line = D2D1::RectF(x - thickness * 0.5f, anchor.top + 6.0f, x + thickness * 0.5f, anchor.bottom - 6.0f);
            }
        }
        FillRoundedRect(IntersectRectF(line, content), accent, thickness * 0.5f);
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(content, linkScrollOffset_, MaxLinkScrollOffset(rect), false);
}

void MainWindow::DrawEmptyState(const D2D1_RECT_F& contentRect, const std::wstring& title, const std::wstring& hint, const std::wstring& buttonLabel) {
    const float iconSize = Metric(theme_, L"content", L"emptyIconSize", 56.0f);
    const float iconRadius = Metric(theme_, L"content", L"emptyIconRadius", 14.0f);
    const float iconStroke = Metric(theme_, L"content", L"emptyIconStroke", 2.0f);
    const float glyphHalf = Metric(theme_, L"content", L"emptyIconGlyph", 15.0f) * 0.5f;
    const float iconGap = Metric(theme_, L"content", L"emptyIconGap", 18.0f);
    const float titleGap = Metric(theme_, L"content", L"emptyTitleGap", 8.0f);
    const float hintGap = Metric(theme_, L"content", L"emptyHintGap", 20.0f);
    const float shiftY = Metric(theme_, L"content", L"emptyBlockShiftY", -16.0f);
    const float lineHeight = Metric(theme_, L"content", L"emptyTextHeight", 30.0f);
    const float textInsetX = Metric(theme_, L"content", L"emptyTextInsetX", 20.0f);
    const float buttonHeight = Metric(theme_, L"button", L"height", 28.0f);

    const bool hasHint = !hint.empty();
    const bool hasButton = !buttonLabel.empty();
    // 计算整块高度用于垂直居中
    float blockHeight = iconSize + iconGap + lineHeight;
    if (hasHint) blockHeight += titleGap + lineHeight;
    if (hasButton) blockHeight += hintGap + buttonHeight;

    const float centerX = (contentRect.left + contentRect.right) * 0.5f;
    float y = contentRect.top + std::max(12.0f, (Height(contentRect) - blockHeight) * 0.5f + shiftY);

    // 柔和图标：accentSoft 圆角方块底 + accent 描边 + 加号意象
    const D2D1_RECT_F iconRect = D2D1::RectF(centerX - iconSize * 0.5f, y, centerX + iconSize * 0.5f, y + iconSize);
    FillRoundedRect(iconRect, theme_.color(L"content", L"empty", L"iconBg"), iconRadius);
    DrawRoundedRect(iconRect, theme_.color(L"content", L"empty", L"icon"), iconRadius, iconStroke);
    hitAreas_.push_back(HitArea{HitKind::AddButton, 1, iconRect});
    const float icx = (iconRect.left + iconRect.right) * 0.5f;
    const float icy = (iconRect.top + iconRect.bottom) * 0.5f;
    ID2D1SolidColorBrush* glyphBrush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(theme_.color(L"content", L"empty", L"icon").d2d(), &glyphBrush)) && glyphBrush) {
        renderTarget_->DrawLine(D2D1::Point2F(icx - glyphHalf, icy), D2D1::Point2F(icx + glyphHalf, icy), glyphBrush, iconStroke);
        renderTarget_->DrawLine(D2D1::Point2F(icx, icy - glyphHalf), D2D1::Point2F(icx, icy + glyphHalf), glyphBrush, iconStroke);
        glyphBrush->Release();
    }
    y = iconRect.bottom + iconGap;

    // 主标题
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    D2D1_RECT_F titleRect = D2D1::RectF(contentRect.left + textInsetX, y, contentRect.right - textInsetX, y + lineHeight);
    DrawTextBlock(title, textFormat_, titleRect, theme_.color(L"content", L"empty", L"title"));
    y = titleRect.bottom;

    // 副提示
    if (hasHint) {
        y += titleGap;
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        D2D1_RECT_F hintRect = D2D1::RectF(contentRect.left + textInsetX, y, contentRect.right - textInsetX, y + lineHeight);
        DrawTextBlock(hint, smallFormat_, hintRect, theme_.color(L"content", L"empty", L"hint"));
        y = hintRect.bottom;
    }

    // 主色按钮
    if (hasButton) {
        DrawEmptyAddButton(contentRect, y + hintGap - 14.0f, buttonLabel);
    }
}

void MainWindow::DrawEmptyAddButton(const D2D1_RECT_F& contentRect, float topY, const std::wstring& label) {
    const bool hovered = IsHover(HitKind::AddButton, 0);
    const float radius = Metric(theme_, L"button", L"radius", 7.0f);
    const float height = Metric(theme_, L"button", L"height", 28.0f);
    const float paddingX = Metric(theme_, L"button", L"paddingX", 12.0f);
    const float plusGap = 8.0f;
    const float plusHalf = 5.0f;

    const float textWidth = MeasureTextWidth(label, textFormat_);
    const float buttonWidth = std::min(Width(contentRect) - 32.0f,
                                       paddingX * 2.0f + plusHalf * 2.0f + plusGap + textWidth + 6.0f);
    const float centerX = (contentRect.left + contentRect.right) * 0.5f;
    const float top = topY + 14.0f;
    D2D1_RECT_F button = D2D1::RectF(centerX - buttonWidth * 0.5f, top, centerX + buttonWidth * 0.5f, top + height);

    // 主色实心按钮（primaryButton）
    const wchar_t* state = hovered ? L"hover" : L"normal";
    FillRoundedRect(button, theme_.color(L"primaryButton", state, L"bg"), radius);
    DrawRoundedRect(button, theme_.color(L"primaryButton", state, L"border"), radius, Metric(theme_, L"button", L"borderWidth", 1.0f));

    const Color iconColor = theme_.color(L"primaryButton", state, L"text");
    const float contentWidth = plusHalf * 2.0f + plusGap + textWidth;
    const float contentLeft = button.left + (Width(button) - contentWidth) * 0.5f;
    const float cy = (button.top + button.bottom) * 0.5f;
    const float px = contentLeft + plusHalf;
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(iconColor.d2d(), &brush)) && brush) {
        renderTarget_->DrawLine(D2D1::Point2F(px - plusHalf, cy), D2D1::Point2F(px + plusHalf, cy), brush, 1.6f);
        renderTarget_->DrawLine(D2D1::Point2F(px, cy - plusHalf), D2D1::Point2F(px, cy + plusHalf), brush, 1.6f);
        brush->Release();
    }

    D2D1_RECT_F textRect = D2D1::RectF(px + plusHalf + plusGap, button.top, button.right - paddingX, button.bottom);
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawTextBlock(label, textFormat_, textRect, iconColor);
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    hitAreas_.push_back(HitArea{HitKind::AddButton, 0, button});
}

void MainWindow::DrawNotePage(D2D1_RECT_F rect, const Group& tag) {
    const float insetX = Metric(theme_, L"linkItem", L"viewportPaddingX", 16.0f);
    const float insetY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const D2D1_RECT_F frame = D2D1::RectF(
        rect.left + insetX,
        rect.top + insetY,
        rect.right - insetX,
        rect.bottom - insetY);
    FillRoundedRect(frame, theme_.color(L"edit", L"normal", L"bg"), Metric(theme_, L"edit", L"radius", 7.0f));
    DrawRoundedRect(
        frame,
        theme_.color(L"edit", noteEdit_ == GetFocus() ? L"focused" : L"normal", L"border"),
        Metric(theme_, L"edit", L"radius", 7.0f),
        Metric(theme_, L"edit", L"borderWidth", 1.0f));
    EnsureNoteEdit(frame, tag);
}

void MainWindow::DrawTodoItems(D2D1_RECT_F rect, const Group&) {
    auto items = TodosForCurrentTag();
    if (items.empty()) {
        DrawEmptyState(rect, L"当前标签没有待办事项", L"", L"添加待办事项");
        return;
    }

    const float paddingX = Metric(theme_, L"linkItem", L"viewportPaddingX", 16.0f);
    const float paddingY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const float rowHeight = std::max(64.0f, Metric(theme_, L"listItem", L"height", 28.0f) + 36.0f);
    const float rowGap = Metric(theme_, L"linkItem", L"listGapY", 4.0f);
    const float contentInsetX = std::max(14.0f, Metric(theme_, L"listItem", L"paddingX", 8.0f));
    const float dotRadius = 4.0f;
    const float dotRightInset = 16.0f;
    const float tagHeight = 20.0f;
    const float tagGap = 6.0f;

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = rect.top + paddingY - linkScrollOffset_;
    for (TodoItem* itemPtr : items) {
        TodoItem& item = *itemPtr;
        D2D1_RECT_F row = D2D1::RectF(rect.left + paddingX, y, rect.right - paddingX, y + rowHeight);
        y += rowHeight + rowGap;
        if (row.bottom < rect.top + 2.0f) {
            continue;
        }
        if (row.top > rect.bottom - 2.0f) {
            break;
        }

        const bool selected = item.id == selectedTodoId_;
        const bool hovered = IsHover(HitKind::Todo, item.id);
        const bool done = !item.completedAt.empty();
        const bool disabled = !item.enabled;
        const bool overdue = IsTodoOverdue(item);
        const bool recurring = IsRecurringTodoSchedule(item.scheduleKind);
        const Color surface = theme_.color(L"panel", L"normal", L"bg");
        const Color textColor = theme_.color(L"text", L"normal", L"text");
        TodoVisualStyle style{};
        if (disabled) {
            style = MakeTodoStyle(theme_.color(L"text", L"disabled", L"text"), surface, textColor);
        } else if (done) {
            style = MakeTodoStyle(theme_.color(L"global", L"success", L"text"), surface, textColor);
        } else if (overdue) {
            style = MakeTodoStyle(theme_.color(L"global", L"danger", L"text"), surface, textColor);
        } else if (recurring) {
            style = MakeTodoStyle(theme_.color(L"global", L"success", L"text"), surface, textColor);
        } else if (item.scheduleKind == TodoScheduleKind::Once || !item.nextDueAt.empty()) {
            style = MakeTodoStyle(theme_.color(L"global", L"info", L"text"), surface, textColor);
        } else {
            style = MakeTodoStyle(theme_.color(L"text", L"muted", L"text"), surface, textColor);
        }

        const float radius = Metric(theme_, L"list", L"radius", 7.0f);
        FillRoundedRect(row, style.bg, radius);
        DrawRoundedRect(row, selected ? theme_.color(L"linkItem", L"selected", L"accent") : style.border, radius, selected ? 1.4f : 1.0f);
        if (selected && selectionByKeyboard_) {
            DrawRoundedRect(Inset(row, 2.0f, 2.0f), theme_.color(L"linkItem", L"selected", L"accent"), std::max(0.0f, radius - 1.0f), 1.0f);
        }
        if (hovered && !selected) {
            DrawRoundedRect(Inset(row, 1.0f, 1.0f), theme_.color(L"listItem", L"hover", L"bg"), std::max(0.0f, radius - 1.0f), 1.0f);
        }

        const float dotCx = row.right - dotRightInset;
        const float titleRight = dotCx - dotRadius - 12.0f;
        FillEllipse(dotCx, row.top + 22.0f, dotRadius, style.dot);

        D2D1_RECT_F titleRect = D2D1::RectF(row.left + contentInsetX, row.top + 9.0f, titleRight, row.top + 30.0f);
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        DrawTextBlock(item.title, textFormat_, titleRect, style.title);

        std::vector<std::wstring> tags;
        if (disabled) {
            tags.push_back(L"已禁用");
        } else if (done) {
            tags.push_back(L"已完成");
        } else if (overdue) {
            tags.push_back(L"已逾期");
        }
        if (recurring) {
            tags.push_back(L"重复");
        } else if (item.scheduleKind == TodoScheduleKind::Once || !item.nextDueAt.empty()) {
            tags.push_back(L"一次性");
        } else {
            tags.push_back(L"无时间");
        }
        const std::wstring remainingText = TodoRemainingText(item);
        if (!remainingText.empty() && !overdue) {
            tags.push_back(remainingText);
        }

        float tagX = row.left + contentInsetX;
        const float tagY = row.top + 36.0f;
        const float tagRight = row.right - contentInsetX;
        for (const auto& tagText : tags) {
            const float desiredWidth = std::min(150.0f, std::max(34.0f, MeasureTextWidth(tagText, smallFormat_) + 16.0f));
            const float availableWidth = tagRight - tagX;
            if (availableWidth < 34.0f) {
                break;
            }
            const float tagWidth = std::min(desiredWidth, availableWidth);
            const D2D1_RECT_F tagRect = D2D1::RectF(tagX, tagY, tagX + tagWidth, tagY + tagHeight);
            FillRoundedRect(tagRect, style.tagBg, 4.0f);
            DrawTextBlock(tagText, smallFormat_, Inset(tagRect, 8.0f, 1.0f), style.tagText);
            tagX += tagWidth + tagGap;
        }
        hitAreas_.push_back(HitArea{HitKind::Todo, item.id, IntersectRectF(row, rect)});
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(rect, linkScrollOffset_, MaxLinkScrollOffset(rect), false);
}

void MainWindow::DrawButtonIcon(HitKind kind, D2D1_RECT_F rect, const Color& color) {
    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(color.d2d(), &brush);
    if (!brush) {
        return;
    }

    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float stroke = Metric(theme_, L"titleButton", L"iconStrokeWidth", 1.6f);
    const float iconHalf = Metric(theme_, L"titleButton", L"iconHalf", 5.0f);
    const float menuHalfWidth = Metric(theme_, L"titleButton", L"menuHalfWidth", 6.0f);
    const float menuLineGap = Metric(theme_, L"titleButton", L"menuLineGap", 5.0f);
    if (kind == HitKind::CloseButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - iconHalf, cy - iconHalf), D2D1::Point2F(cx + iconHalf, cy + iconHalf), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx + iconHalf, cy - iconHalf), D2D1::Point2F(cx - iconHalf, cy + iconHalf), brush, stroke);
    } else if (kind == HitKind::MenuButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy - menuLineGap), D2D1::Point2F(cx + menuHalfWidth, cy - menuLineGap), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy + menuLineGap), D2D1::Point2F(cx + menuHalfWidth, cy + menuLineGap), brush, stroke);
    } else if (kind == HitKind::ToolButton) {
        const float handleHalf = menuHalfWidth * 0.45f;
        const float handleTop = cy - menuHalfWidth;
        const float handleBottom = cy - menuHalfWidth * 0.45f;
        renderTarget_->DrawRectangle(D2D1::RectF(cx - handleHalf, handleTop, cx + handleHalf, handleBottom), brush, stroke);
        renderTarget_->DrawRectangle(D2D1::RectF(cx - menuHalfWidth, cy - menuHalfWidth * 0.35f, cx + menuHalfWidth, cy + menuHalfWidth), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, stroke);
    } else if (kind == HitKind::AddButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx, cy - menuHalfWidth), D2D1::Point2F(cx, cy + menuHalfWidth), brush, stroke);
    } else if (kind == HitKind::SkinButton) {
        renderTarget_->DrawRectangle(D2D1::RectF(cx - menuHalfWidth, cy - menuHalfWidth, cx + menuHalfWidth, cy + menuHalfWidth), brush, Metric(theme_, L"titleButton", L"skinStrokeWidth", 1.4f));
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, Metric(theme_, L"titleButton", L"skinLineStrokeWidth", 1.2f));
    }
    brush->Release();
}

ID2D1Bitmap* MainWindow::LoadAppIconBitmap() {
    if (!uiWicFactory_ || !renderTarget_) {
        return nullptr;
    }

    const std::wstring key = L"#app-icon";
    auto found = uiBitmapCache_.find(key);
    if (found != uiBitmapCache_.end()) {
        return found->second;
    }

    HICON icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!icon) {
        return nullptr;
    }

    IWICBitmap* wicBitmap = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID2D1Bitmap* bitmap = nullptr;
    if (SUCCEEDED(uiWicFactory_->CreateBitmapFromHICON(icon, &wicBitmap)) &&
        SUCCEEDED(uiWicFactory_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            wicBitmap,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut)) &&
        SUCCEEDED(renderTarget_->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap)) &&
        bitmap) {
        uiBitmapCache_[key] = bitmap;
    }

    SafeRelease(converter);
    SafeRelease(wicBitmap);
    DestroyIcon(icon);
    return bitmap;
}

void MainWindow::ClearUiBitmaps() {
    for (auto& [_, bitmap] : uiBitmapCache_) {
        SafeRelease(bitmap);
    }
    uiBitmapCache_.clear();
}

void MainWindow::FillRect(const D2D1_RECT_F& rect, const Color& color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->FillRectangle(rect, brush);
        brush->Release();
    }
}

void MainWindow::DrawRect(const D2D1_RECT_F& rect, const Color& color, float strokeWidth) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->DrawRectangle(rect, brush, strokeWidth);
        brush->Release();
    }
}

void MainWindow::FillRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius) {
    if (Width(rect) <= 0 || Height(rect) <= 0) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        const float r = std::min(radius, std::min(Width(rect), Height(rect)) * 0.5f);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush);
        brush->Release();
    }
}

void MainWindow::DrawRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius, float strokeWidth) {
    if (Width(rect) <= 0 || Height(rect) <= 0) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        const float r = std::min(radius, std::min(Width(rect), Height(rect)) * 0.5f);
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush, strokeWidth);
        brush->Release();
    }
}

void MainWindow::FillEllipse(float cx, float cy, float radius, const Color& color) {
    if (radius <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush);
        brush->Release();
    }
}

void MainWindow::DrawScrollBar(const D2D1_RECT_F& rect, float offset, float maxOffset, bool horizontal) {
    if (maxOffset <= 0.5f || Width(rect) <= 0 || Height(rect) <= 0) {
        return;
    }

    const float thickness = ClampFloat(theme_.metric(L"scrollbar", L"thickness", 5.0f), 2.0f, 16.0f);
    const float inset = ClampFloat(theme_.metric(L"scrollbar", L"inset", 5.0f), 0.0f, 24.0f);
    const float viewport = horizontal ? Width(rect) : Height(rect);
    const float content = viewport + maxOffset;
    const float trackLength = std::max(20.0f, viewport - inset * 2.0f);
    const float thumbLength = std::max(Metric(theme_, L"scrollbar", L"minThumbLength", 24.0f), trackLength * viewport / std::max(viewport, content));
    const float travel = std::max(1.0f, trackLength - thumbLength);
    const float thumbStart = inset + travel * ClampFloat(offset / maxOffset, 0.0f, 1.0f);

    D2D1_RECT_F track{};
    D2D1_RECT_F thumb{};
    if (horizontal) {
        const float edgeInset = Metric(theme_, L"scrollbar", L"edgeInset", 4.0f);
        track = D2D1::RectF(rect.left + inset, rect.bottom - thickness - edgeInset, rect.right - inset, rect.bottom - edgeInset);
        thumb = D2D1::RectF(rect.left + thumbStart, track.top, rect.left + thumbStart + thumbLength, track.bottom);
    } else {
        const float edgeInset = Metric(theme_, L"scrollbar", L"edgeInset", 4.0f);
        track = D2D1::RectF(rect.right - thickness - edgeInset, rect.top + inset, rect.right - edgeInset, rect.bottom - inset);
        thumb = D2D1::RectF(track.left, rect.top + thumbStart, track.right, rect.top + thumbStart + thumbLength);
    }

    FillRoundedRect(track, theme_.color(L"scrollbar", L"normal", L"track"), thickness * 0.5f);
    FillRoundedRect(thumb, theme_.color(L"scrollbar", L"normal", L"thumb"), thickness * 0.5f);
}

float MainWindow::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format, float maxWidth) const {
    if (text.empty() || !format || !dwriteFactory_) {
        return 0.0f;
    }
    IDWriteTextLayout* layout = nullptr;
    float width = 0.0f;
    if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            std::max(1.0f, maxWidth),
            32.0f,
            &layout)) &&
        layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
            width = metrics.widthIncludingTrailingWhitespace;
        }
        layout->Release();
    }
    return width;
}

void MainWindow::DrawTextBlock(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const Color& color) {
    if (text.empty() || !format) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        brush->Release();
    }
}

void MainWindow::DrawLinkName(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const Color& color) {
    if (text.empty() || !format || !dwriteFactory_) {
        return;
    }

    IDWriteInlineObject* ellipsis = nullptr;
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        format->SetTrimming(&trimming, ellipsis);
    }

    DrawTextBlock(text, format, rect, color);

    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_NONE;
    format->SetTrimming(&trimming, nullptr);
    if (ellipsis) {
        ellipsis->Release();
    }
}

void MainWindow::ResetMenuVisuals(POINT screenPoint) {
    activeMenuItems_.clear();
    if (!menuFont_) {
        menuFont_ = std::make_unique<ThemedMenuFontCache>();
    }
    if (QuattroTestMode() && BackgroundAcceptanceMode()) {
        menuFont_->FontForDpi(CurrentDpi());
    } else {
        menuFont_->FontForScreenPoint(screenPoint, hwnd_);
    }
}

void MainWindow::AppendThemedMenuItem(HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool submenu, int systemImageIndex, int stockIcon, int menuIcon) {
    InsertThemedMenuItem(menu, static_cast<UINT>(-1), flags, id, text, submenu, systemImageIndex, stockIcon, menuIcon);
}

void MainWindow::InsertThemedMenuItem(HMENU menu, UINT position, UINT flags, UINT_PTR id, const std::wstring& text, bool submenu, int systemImageIndex, int stockIcon, int menuIcon) {
    auto item = std::make_unique<MenuItemData>();
    item->text = MenuTextFromRaw(text);
    item->icon = menuIcon != MenuIconNone ? menuIcon : MenuIconFor(id, item->text);
    item->systemImageIndex = systemImageIndex;
    item->stockIcon = stockIcon;
    item->disabled = (flags & (MF_DISABLED | MF_GRAYED)) != 0;
    item->submenu = submenu || ((flags & MF_POPUP) != 0);
    MenuItemData* raw = item.get();
    activeMenuItems_.push_back(std::move(item));
    const UINT menuFlags = (flags | MF_OWNERDRAW) & ~(MF_STRING | MF_CHECKED);
    if (position == static_cast<UINT>(-1)) {
        AppendMenuW(menu, menuFlags, id, reinterpret_cast<LPCWSTR>(raw));
    } else {
        InsertMenuW(menu, position, menuFlags, id, reinterpret_cast<LPCWSTR>(raw));
    }
}

void MainWindow::AppendThemedStateMenuItem(HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool active, int menuIcon, bool submenu) {
    auto item = std::make_unique<MenuItemData>();
    item->text = MenuTextFromRaw(text);
    item->icon = menuIcon != MenuIconNone ? menuIcon : MenuIconFor(id, item->text);
    item->disabled = (flags & (MF_DISABLED | MF_GRAYED)) != 0;
    item->submenu = submenu || ((flags & MF_POPUP) != 0);
    item->iconTone = active ? MenuIconTone::Active : MenuIconTone::Muted;
    MenuItemData* raw = item.get();
    activeMenuItems_.push_back(std::move(item));
    AppendMenuW(menu, (flags | MF_OWNERDRAW) & ~(MF_STRING | MF_CHECKED), id, reinterpret_cast<LPCWSTR>(raw));
}

void MainWindow::AppendThemedTrackedMenuItem(
    HMENU menu,
    UINT flags,
    UINT_PTR id,
    const ShellContextMenuItem& source,
    bool submenu) {
    if (source.checked) {
        AppendThemedStateMenuItem(menu, flags, id, source.text, true, MenuIconNone, submenu);
    } else {
        AppendThemedMenuItem(menu, flags, id, source.text, submenu);
    }
    if (!activeMenuItems_.empty()) {
        activeMenuItems_.back()->nativeIconBitmap = CreateTrackedMenuIconBitmap(source);
    }
}

void MainWindow::AppendThemedSeparator(HMENU menu) {
    auto item = std::make_unique<MenuItemData>();
    item->separator = true;
    MenuItemData* raw = item.get();
    activeMenuItems_.push_back(std::move(item));
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, reinterpret_cast<LPCWSTR>(raw));
}

const MainWindow::MenuItemData* MainWindow::ThemedMenuItemFromData(ULONG_PTR itemData) const {
    if (itemData == 0) {
        return nullptr;
    }
    const auto* raw = reinterpret_cast<const MenuItemData*>(itemData);
    for (const auto& item : activeMenuItems_) {
        if (item.get() == raw) {
            return raw;
        }
    }
    return nullptr;
}

bool MainWindow::MeasureThemedMenuItem(MEASUREITEMSTRUCT* measure) {
    if (!measure || measure->CtlType != ODT_MENU) {
        return false;
    }
    const auto* item = ThemedMenuItemFromData(measure->itemData);
    if (!item) {
        return false;
    }
    if (!menuFont_) {
        return false;
    }
    const auto scaledMetric = [this](const wchar_t* component, const wchar_t* name, float fallback) {
        return menuFont_->Scale(static_cast<int>(std::lround(Metric(theme_, component, name, fallback))));
    };
    if (item->separator) {
        const int thickness = std::max(1, scaledMetric(L"separator", L"thickness", 1.0f));
        measure->itemHeight = static_cast<UINT>(std::max(menuFont_->Scale(5), thickness + menuFont_->Scale(8)));
        measure->itemWidth = static_cast<UINT>(
            scaledMetric(L"menuItem", L"widthBase", 54.0f) +
            scaledMetric(L"menuItem", L"minTextWidth", 64.0f));
        return true;
    }
    const SIZE textSize = menuFont_->MeasureText(hwnd_, item->text);
    const int minTextWidth = scaledMetric(L"menuItem", L"minTextWidth", 64.0f);
    const int maxTextWidth = scaledMetric(L"menuItem", L"maxTextWidth", 360.0f);
    const int widthBase = scaledMetric(L"menuItem", L"widthBase", 54.0f);
    measure->itemHeight = static_cast<UINT>(scaledMetric(L"menuItem", L"height", 28.0f));
    measure->itemWidth = static_cast<UINT>(
        widthBase + std::min(maxTextWidth, std::max(minTextWidth, static_cast<int>(textSize.cx))));
    return true;
}

bool MainWindow::DrawThemedMenuItem(const DRAWITEMSTRUCT* draw) {
    if (!draw || draw->CtlType != ODT_MENU) {
        return false;
    }

    const auto* item = ThemedMenuItemFromData(draw->itemData);
    if (!item || !menuFont_) {
        return false;
    }
    const auto scaledMetric = [this](const wchar_t* component, const wchar_t* name, float fallback) {
        return menuFont_->Scale(static_cast<int>(std::lround(Metric(theme_, component, name, fallback))));
    };
    RECT rc = draw->rcItem;
    HDC dc = draw->hDC;
    const Color background = theme_.color(L"menu", L"normal", L"bg");

    if (item->separator) {
        HBRUSH backgroundBrush = CreateSolidBrush(ToColorRef(background));
        ::FillRect(dc, &rc, backgroundBrush);
        DeleteObject(backgroundBrush);

        const int inset = std::max(0, scaledMetric(L"separator", L"inset", 0.0f));
        const int thickness = std::max(1, scaledMetric(L"separator", L"thickness", 1.0f));
        RECT lineRect{
            rc.left + inset,
            rc.top + ((rc.bottom - rc.top) - thickness) / 2,
            rc.right - inset,
            rc.top + ((rc.bottom - rc.top) - thickness) / 2 + thickness};
        HBRUSH lineBrush = CreateSolidBrush(ToColorRef(theme_.color(L"separator", L"normal", L"line")));
        ::FillRect(dc, &lineRect, lineBrush);
        DeleteObject(lineBrush);
        return true;
    }

    const bool selected = (draw->itemState & ODS_SELECTED) != 0;

    const Color hover = selected ? theme_.color(L"menuItem", L"hover", L"bg") : background;
    HBRUSH backgroundBrush = CreateSolidBrush(ToColorRef(hover));
    ::FillRect(dc, &rc, backgroundBrush);
    DeleteObject(backgroundBrush);

    if (selected) {
        RECT hotRect = rc;
        InflateRect(&hotRect, -scaledMetric(L"menuItem", L"hoverInsetX", 4.0f), -scaledMetric(L"menuItem", L"hoverInsetY", 3.0f));
        HBRUSH hotBrush = CreateSolidBrush(ToColorRef(theme_.color(L"menuItem", L"hover", L"bg")));
        ::FillRect(dc, &hotRect, hotBrush);
        DeleteObject(hotBrush);
    }

    const int iconLeft = scaledMetric(L"menuItem", L"iconLeft", 8.0f);
    const int iconTopInset = scaledMetric(L"menuItem", L"iconInsetY", 6.0f);
    const int iconSize = scaledMetric(L"menuItem", L"iconSize", 16.0f);
    const RECT iconRect{rc.left + iconLeft, rc.top + iconTopInset, rc.left + iconLeft + iconSize, rc.bottom - iconTopInset};
    COLORREF iconColorRef = CLR_INVALID;
    if (item->iconTone == MenuIconTone::Active) {
        iconColorRef = ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : L"checked", L"icon"));
    } else if (item->iconTone == MenuIconTone::Muted) {
        iconColorRef = ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : L"normal", L"icon"));
    }
    MenuIconPalette iconPalette;
    iconPalette.accent = ToColorRef(theme_.color(L"menuItem", L"accent", L"icon"));
    iconPalette.danger = ToColorRef(theme_.color(L"menuItem", L"danger", L"icon"));
    iconPalette.warning = ToColorRef(theme_.color(L"menuItem", L"warning", L"icon"));
    iconPalette.success = ToColorRef(theme_.color(L"menuItem", L"success", L"icon"));
    iconPalette.muted = ToColorRef(theme_.color(L"menuItem", L"normal", L"icon"));
    iconPalette.disabled = ToColorRef(theme_.color(L"menuItem", L"disabled", L"icon"));
    iconPalette.neutral = ToColorRef(theme_.color(L"menu", L"normal", L"bg"));
    if (item->nativeIconBitmap && DrawTrackedMenuIcon(dc, iconRect, item->nativeIconBitmap, item->disabled)) {
        // The cached bitmap is copied from the Windows native menu.
    } else if (item->stockIcon >= 0) {
        if (!DrawStockMenuIcon(dc, iconRect, static_cast<SHSTOCKICONID>(item->stockIcon)) &&
            !DrawSystemImageListIcon(dc, iconRect, item->systemImageIndex, item->disabled)) {
            DrawMenuIcon(dc, iconRect, item->icon, item->disabled, iconColorRef, iconPalette, appDirectory_);
        }
    } else if (item->systemImageIndex >= 0) {
        if (!DrawSystemImageListIcon(dc, iconRect, item->systemImageIndex, item->disabled)) {
            DrawMenuIcon(dc, iconRect, item->icon, item->disabled, iconColorRef, iconPalette, appDirectory_);
        }
    } else {
        DrawMenuIcon(dc, iconRect, item->icon, item->disabled, iconColorRef, iconPalette, appDirectory_);
    }

    RECT textRect = rc;
    textRect.left += scaledMetric(L"menuItem", L"textLeft", 34.0f);
    textRect.right -= item->submenu
        ? scaledMetric(L"menuItem", L"submenuRight", 22.0f)
        : scaledMetric(L"menuItem", L"textRight", 8.0f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : L"normal", L"text")));
    HGDIOBJ oldFont = SelectObject(dc, menuFont_->FontForDpi(menuFont_->dpi()));
    DrawTextW(dc, item->text.c_str(), static_cast<int>(item->text.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);

    if (item->submenu) {
        const COLORREF arrowColor = ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : (selected ? L"hover" : L"normal"), L"text"));
        const int midY = (rc.top + rc.bottom) / 2;
        const int arrowRight = scaledMetric(L"menuItem", L"arrowRight", 9.0f);
        const int arrowIconSize = scaledMetric(L"menuItem", L"iconSize", 16.0f);
        const RECT arrowRect{
            rc.right - arrowRight - arrowIconSize,
            midY - arrowIconSize / 2,
            rc.right - arrowRight,
            midY - arrowIconSize / 2 + arrowIconSize};
        if (!DrawMenuChevronRight(dc, arrowRect, arrowColor, appDirectory_)) {
            HPEN pen = CreatePen(PS_SOLID, 1, arrowColor);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            const int arrowWidth = scaledMetric(L"menuItem", L"arrowWidth", 5.0f);
            const int arrowHalfHeight = scaledMetric(L"menuItem", L"arrowHalfHeight", 4.0f);
            const int arrowTip = rc.right - arrowRight;
            POINT points[] = {
                {arrowTip - arrowWidth, midY - arrowHalfHeight},
                {arrowTip, midY},
                {arrowTip - arrowWidth, midY + arrowHalfHeight},
            };
            Polyline(dc, points, static_cast<int>(std::size(points)));
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        ExcludeClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    }
    return true;
}

void MainWindow::ClampScrollOffsets() {
    if (!hwnd_) {
        groupScrollOffset_ = 0.0f;
        tagScrollOffset_ = 0.0f;
        linkScrollOffset_ = 0.0f;
        return;
    }

    D2D1_RECT_F title{}, groups{}, tags{}, links{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groups, tags, links);
    groupScrollOffset_ = ClampFloat(groupScrollOffset_, 0.0f, MaxGroupScrollOffset(groups));
    tagScrollOffset_ = ClampFloat(tagScrollOffset_, 0.0f, MaxTagScrollOffset(tags));
    linkScrollOffset_ = ClampFloat(linkScrollOffset_, 0.0f, MaxLinkScrollOffset(links));
}

void MainWindow::ScrollAtPoint(float x, float y, int wheelDelta, bool horizontal) {
    if (wheelDelta == 0) {
        return;
    }

    D2D1_RECT_F title{}, groups{}, tags{}, links{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groups, tags, links);

    const float step = horizontal ? Metric(theme_, L"scrollbar", L"wheelStepX", 72.0f) : Metric(theme_, L"scrollbar", L"wheelStepY", 80.0f);
    const float amount = -static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA) * step;
    bool changed = false;
    if (config_.showGroup && Contains(groups, x, y)) {
        groupScrollOffset_ = ClampFloat(groupScrollOffset_ + amount, 0.0f, MaxGroupScrollOffset(groups));
        changed = true;
    } else if (config_.showTag && Contains(tags, x, y)) {
        tagScrollOffset_ = ClampFloat(tagScrollOffset_ + amount, 0.0f, MaxTagScrollOffset(tags));
        changed = true;
    } else if (Contains(links, x, y)) {
        linkScrollOffset_ = ClampFloat(linkScrollOffset_ + amount, 0.0f, MaxLinkScrollOffset(links));
        changed = true;
    }

    if (changed) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

float MainWindow::MaxGroupScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0 || !config_.showGroup) {
        return 0.0f;
    }

    if (Height(rect) > Width(rect)) {
        const auto groups = MajorGroups();
        const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
        const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f));
        const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
        const float topInset = groupPadding;
        const float bottomInset = groupPadding;
        const float contentHeight = topInset + bottomInset + static_cast<float>(groups.size()) * itemHeight
            + static_cast<float>(groups.empty() ? 0 : groups.size() - 1) * itemGap;
        return std::max(0.0f, contentHeight - Height(rect));
    }

    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float itemOffsetX = groupPadding;
    const float contentRightPadding = groupPadding;
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    float contentWidth = itemOffsetX + contentRightPadding;
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        contentWidth += TabGroupItemWidth(theme_, group.name, textFormat_, MeasureTextWidth(group.name, textFormat_)) + itemGap;
    }
    if (!groups.empty()) {
        contentWidth -= itemGap;
    }
    return std::max(0.0f, contentWidth - Width(rect));
}

float MainWindow::MaxTagScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0 || !config_.showTag) {
        return 0.0f;
    }

    const auto tags = TagsForCurrentGroup();
    const D2D1_RECT_F content = CardContentRectFor(rect);
    const float topInset = Metric(theme_, L"minorNavItem", L"topInset", 2.0f);
    const float bottomInset = Metric(theme_, L"minorNavItem", L"bottomInset", 2.0f);
    const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"minorNavItem", L"height", 28.0f));
    const float itemGap = Metric(theme_, L"minorNavItem", L"gap", 2.0f);
    const float contentHeight = topInset + bottomInset + static_cast<float>(tags.size()) * itemHeight
        + static_cast<float>(tags.empty() ? 0 : tags.size() - 1) * itemGap;
    return std::max(0.0f, contentHeight - Height(content));
}

float MainWindow::MaxLinkScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return 0.0f;
    }

    const Group* currentTag = FindGroup(currentTagId_);
    if (currentTag && IsNoteTag(*currentTag)) {
        return 0.0f;
    }
    if (currentTag && IsTodoItemsTag(*currentTag)) {
        return std::max(0.0f, TodoContentHeight(rect) - Height(rect));
    }

    auto links = const_cast<MainWindow*>(this)->LinksForCurrentTag();
    if (links.empty()) {
        return 0.0f;
    }

    const Group* tag = FindGroup(currentTagId_);
    const D2D1_RECT_F content = CardContentRectFor(rect);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, content, tag);
    const float contentHeight = LinkContentHeight(links.size(), metrics);
    return std::max(0.0f, contentHeight - Height(content));
}

float MainWindow::TodoContentHeight(const D2D1_RECT_F&) const {
    std::size_t count = 0;
    for (const auto& item : model_.todos) {
        if (item.tagId == currentTagId_) {
            ++count;
        }
    }
    if (count == 0) {
        return 0.0f;
    }
    const float paddingY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const float itemHeight = std::max(64.0f, Metric(theme_, L"listItem", L"height", 28.0f) + 36.0f);
    const float itemGap = Metric(theme_, L"linkItem", L"listGapY", 4.0f);
    return paddingY * 2.0f + static_cast<float>(count) * itemHeight + static_cast<float>(count - 1) * itemGap;
}

void MainWindow::EnsureGroupVisible(int groupId) {
    if (!hwnd_) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groupsRect, tagsRect, linksRect);

    if (Height(groupsRect) > Width(groupsRect)) {
        const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
        const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f));
        const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
        const float topInset = groupPadding;
        const float visibilityPadding = Metric(theme_, L"majorNavItem", L"visibilityPadding", 8.0f);
        float y = groupsRect.top + topInset;
        for (const auto& group : MajorGroups()) {
            if (group.id == groupId) {
                if (y - groupScrollOffset_ < groupsRect.top + visibilityPadding) {
                    groupScrollOffset_ = y - groupsRect.top - visibilityPadding;
                } else if (y + itemHeight - groupScrollOffset_ > groupsRect.bottom - visibilityPadding) {
                    groupScrollOffset_ = y + itemHeight - groupsRect.bottom + visibilityPadding;
                }
                groupScrollOffset_ = ClampFloat(groupScrollOffset_, 0.0f, MaxGroupScrollOffset(groupsRect));
                return;
            }
            y += itemHeight + itemGap;
        }
        return;
    }

    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float itemOffsetX = groupPadding;
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    const float visibilityPadding = Metric(theme_, L"majorNavItem", L"visibilityPadding", 8.0f);
    float x = groupsRect.left + itemOffsetX;
    for (const auto& group : MajorGroups()) {
        const float width = TabGroupItemWidth(theme_, group.name, textFormat_, MeasureTextWidth(group.name, textFormat_));
        if (group.id == groupId) {
            if (x - groupScrollOffset_ < groupsRect.left + visibilityPadding) {
                groupScrollOffset_ = x - groupsRect.left - visibilityPadding;
            } else if (x + width - groupScrollOffset_ > groupsRect.right - visibilityPadding) {
                groupScrollOffset_ = x + width - groupsRect.right + visibilityPadding;
            }
            groupScrollOffset_ = ClampFloat(groupScrollOffset_, 0.0f, MaxGroupScrollOffset(groupsRect));
            return;
        }
        x += width + itemGap;
    }
}

void MainWindow::EnsureTagVisible(int tagId) {
    if (!hwnd_) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groupsRect, tagsRect, linksRect);
    const D2D1_RECT_F content = CardContentRectFor(tagsRect);
    const float topInset = Metric(theme_, L"minorNavItem", L"topInset", 2.0f);
    const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"minorNavItem", L"height", 28.0f));
    const float itemGap = Metric(theme_, L"minorNavItem", L"gap", 2.0f);
    const float visibilityPadding = Metric(theme_, L"minorNavItem", L"visibilityPadding", 8.0f);
    float y = content.top + topInset;
    for (const auto& tag : TagsForCurrentGroup()) {
        if (tag.id == tagId) {
            if (y - tagScrollOffset_ < content.top + visibilityPadding) {
                tagScrollOffset_ = y - content.top - visibilityPadding;
            } else if (y + itemHeight - tagScrollOffset_ > content.bottom - visibilityPadding) {
                tagScrollOffset_ = y + itemHeight - content.bottom + visibilityPadding;
            }
            tagScrollOffset_ = ClampFloat(tagScrollOffset_, 0.0f, MaxTagScrollOffset(tagsRect));
            return;
        }
        y += itemHeight + itemGap;
    }
}

void MainWindow::EnsureLinkVisible(int linkId) {
    if (linkId <= 0 || !hwnd_) {
        return;
    }
    if (!hwnd_) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groupsRect, tagsRect, linksRect);
    auto links = LinksForCurrentTag();
    auto it = std::find_if(links.begin(), links.end(), [linkId](const Link* link) {
        return link && link->id == linkId;
    });
    if (it == links.end()) {
        return;
    }

    const int index = static_cast<int>(std::distance(links.begin(), it));
    const Group* tag = FindGroup(currentTagId_);
    const D2D1_RECT_F content = CardContentRectFor(linksRect);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, content, tag);
    const D2D1_RECT_F item = LinkItemRect(content, metrics, index, 0.0f);
    const float itemTop = item.top;
    const float itemBottom = item.bottom;

    const float visibilityPadding = Metric(theme_, L"linkItem", L"visibilityPadding", 8.0f);
    if (itemTop - linkScrollOffset_ < content.top + visibilityPadding) {
        linkScrollOffset_ = itemTop - content.top - visibilityPadding;
    } else if (itemBottom - linkScrollOffset_ > content.bottom - visibilityPadding) {
        linkScrollOffset_ = itemBottom - content.bottom + visibilityPadding;
    }
    linkScrollOffset_ = ClampFloat(linkScrollOffset_, 0.0f, MaxLinkScrollOffset(linksRect));
}

void MainWindow::EnsureTodoVisible(int todoId) {
    if (todoId <= 0 || !hwnd_) {
        return;
    }
    if (!hwnd_) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groupsRect, tagsRect, linksRect);
    auto items = TodosForCurrentTag();
    auto it = std::find_if(items.begin(), items.end(), [todoId](const TodoItem* item) {
        return item && item->id == todoId;
    });
    if (it == items.end()) {
        return;
    }

    const int index = static_cast<int>(std::distance(items.begin(), it));
    const float paddingY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const float rowHeight = std::max(64.0f, Metric(theme_, L"listItem", L"height", 28.0f) + 36.0f);
    const float rowGap = Metric(theme_, L"linkItem", L"listGapY", 4.0f);
    const float itemTop = linksRect.top + paddingY + static_cast<float>(index) * (rowHeight + rowGap);
    const float itemBottom = itemTop + rowHeight;

    const float visibilityPadding = Metric(theme_, L"linkItem", L"visibilityPadding", 8.0f);
    if (itemTop - linkScrollOffset_ < linksRect.top + visibilityPadding) {
        linkScrollOffset_ = itemTop - linksRect.top - visibilityPadding;
    } else if (itemBottom - linkScrollOffset_ > linksRect.bottom - visibilityPadding) {
        linkScrollOffset_ = itemBottom - linksRect.bottom + visibilityPadding;
    }
    linkScrollOffset_ = ClampFloat(linkScrollOffset_, 0.0f, MaxLinkScrollOffset(linksRect));
}

MainWindow::HitArea MainWindow::CursorHitArea() const {
    POINT point{};
    if (!GetCursorPos(&point) || !ScreenToClient(hwnd_, &point)) {
        return {};
    }
    point = ClientPointToDip(point);
    return HitTest(static_cast<float>(point.x), static_cast<float>(point.y));
}

void MainWindow::BeginLinkDragCandidate(int linkId, POINT point) {
    const Group* tag = FindGroup(currentTagId_);
    if (!tag || tag->parentGroup == 0 || IsAllTag(*tag) || IsTodoTag(*tag) || IsNoteTag(*tag) || IsTodoItemsTag(*tag)) {
        return;
    }
    Link* link = FindLink(linkId);
    if (!link || link->parentGroup != currentTagId_) {
        return;
    }
    linkDragCandidateId_ = linkId;
    linkDragId_ = 0;
    linkDragStartPoint_ = point;
    linkDragCurrentPoint_ = point;
    linkDragActive_ = false;
    linkDragMode_ = LinkDragMode::None;
    linkDragTargetLinkId_ = 0;
    linkDragInsertIndex_ = -1;
}

void MainWindow::UpdateLinkDrag(POINT point) {
    if (linkDragCandidateId_ <= 0 && !linkDragActive_) {
        return;
    }

    linkDragCurrentPoint_ = point;
    if (!linkDragActive_) {
        const int thresholdX = std::max(2, GetSystemMetrics(SM_CXDRAG));
        const int thresholdY = std::max(2, GetSystemMetrics(SM_CYDRAG));
        if (std::abs(point.x - linkDragStartPoint_.x) < thresholdX &&
            std::abs(point.y - linkDragStartPoint_.y) < thresholdY) {
            return;
        }
        linkDragActive_ = true;
        linkDragId_ = linkDragCandidateId_;
        selectedLinkId_ = linkDragId_;
        selectionByKeyboard_ = false;
        HideItemTooltip();
        SetCapture(hwnd_);
    }

    UpdateLinkDragTarget(point);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateLinkDragTarget(POINT point) {
    linkDragMode_ = LinkDragMode::None;
    linkDragTargetLinkId_ = 0;
    linkDragInsertIndex_ = -1;

    point = ClientPointToDip(point);
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groupsRect, tagsRect, linksRect);
    const D2D1_RECT_F content = CardContentRectFor(linksRect);
    if (!Contains(content, static_cast<float>(point.x), static_cast<float>(point.y))) {
        return;
    }

    const Group* tag = FindGroup(currentTagId_);
    if (!tag) {
        return;
    }
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, content, tag);
    const std::vector<Link*> links = LinksForCurrentTag();
    if (links.empty()) {
        return;
    }

    const float x = static_cast<float>(point.x);
    const float y = static_cast<float>(point.y);
    int nearestIndex = -1;
    float nearestDistance = FLT_MAX;
    for (int i = 0; i < static_cast<int>(links.size()); ++i) {
        const D2D1_RECT_F item = LinkItemRect(content, metrics, i, linkScrollOffset_);
        const float centerX = (item.left + item.right) * 0.5f;
        const float centerY = (item.top + item.bottom) * 0.5f;
        const float dx = x - centerX;
        const float dy = y - centerY;
        const float distance = dx * dx + dy * dy;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = i;
        }

        if (!Contains(item, x, y)) {
            continue;
        }

        if (links[i]->id == linkDragId_) {
            return;
        }

        const float edgeRatio = Metric(theme_, L"linkItem", L"dragInsertEdgeRatio", 0.28f);
        if (metrics.layout == 0 || metrics.compactTile) {
            const float edgeHeight = Height(item) * edgeRatio;
            if (y < item.top + edgeHeight) {
                linkDragMode_ = LinkDragMode::Insert;
                linkDragInsertIndex_ = i;
            } else if (y > item.bottom - edgeHeight) {
                linkDragMode_ = LinkDragMode::Insert;
                linkDragInsertIndex_ = i + 1;
            } else {
                linkDragMode_ = LinkDragMode::Swap;
                linkDragTargetLinkId_ = links[i]->id;
            }
        } else {
            const float edgeWidth = Width(item) * edgeRatio;
            const float edgeHeight = Height(item) * edgeRatio;
            if (x > item.left + edgeWidth && x < item.right - edgeWidth &&
                y > item.top + edgeHeight && y < item.bottom - edgeHeight) {
                linkDragMode_ = LinkDragMode::Swap;
                linkDragTargetLinkId_ = links[i]->id;
            } else {
                linkDragMode_ = LinkDragMode::Insert;
                linkDragInsertIndex_ = (y < centerY || (std::abs(y - centerY) < 1.0f && x < centerX)) ? i : i + 1;
            }
        }
        return;
    }

    if (nearestIndex >= 0) {
        const D2D1_RECT_F item = LinkItemRect(content, metrics, nearestIndex, linkScrollOffset_);
        const float centerX = (item.left + item.right) * 0.5f;
        const float centerY = (item.top + item.bottom) * 0.5f;
        linkDragMode_ = LinkDragMode::Insert;
        linkDragInsertIndex_ = (y < centerY || (std::abs(y - centerY) < 1.0f && x < centerX)) ? nearestIndex : nearestIndex + 1;
    }
}

void MainWindow::CommitLinkDrag() {
    if (!linkDragActive_ || linkDragId_ <= 0 || linkDragMode_ == LinkDragMode::None) {
        CancelLinkDrag();
        return;
    }

    std::vector<Link*> links = LinksForCurrentTag();
    auto sourceIt = std::find_if(links.begin(), links.end(), [this](const Link* link) {
        return link && link->id == linkDragId_;
    });
    if (sourceIt == links.end()) {
        CancelLinkDrag();
        return;
    }

    if (linkDragMode_ == LinkDragMode::Swap) {
        auto targetIt = std::find_if(links.begin(), links.end(), [this](const Link* link) {
            return link && link->id == linkDragTargetLinkId_;
        });
        if (targetIt == links.end() || targetIt == sourceIt) {
            CancelLinkDrag();
            return;
        }
        std::iter_swap(sourceIt, targetIt);
    } else {
        const int sourceIndex = static_cast<int>(std::distance(links.begin(), sourceIt));
        int targetIndex = std::max(0, std::min(static_cast<int>(links.size()), linkDragInsertIndex_));
        Link* moved = *sourceIt;
        links.erase(sourceIt);
        if (sourceIndex < targetIndex) {
            --targetIndex;
        }
        targetIndex = std::max(0, std::min(static_cast<int>(links.size()), targetIndex));
        if (targetIndex == sourceIndex) {
            CancelLinkDrag();
            return;
        }
        links.insert(links.begin() + targetIndex, moved);
    }

    std::vector<int> orderedIds;
    orderedIds.reserve(links.size());
    for (const Link* link : links) {
        orderedIds.push_back(link->id);
    }
    const int draggedId = linkDragId_;
    const int tagId = currentTagId_;
    CancelLinkDrag();
    if (ApplyManualLinkOrder(tagId, orderedIds, L"拖动启动项")) {
        selectedLinkId_ = draggedId;
        EnsureLinkVisible(selectedLinkId_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::CancelLinkDrag() {
    if (linkDragActive_ && GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    linkDragCandidateId_ = 0;
    linkDragId_ = 0;
    linkDragActive_ = false;
    linkDragMode_ = LinkDragMode::None;
    linkDragTargetLinkId_ = 0;
    linkDragInsertIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::BeginNavDragCandidate(HitKind kind, int groupId, POINT point) {
    Group* group = FindGroup(groupId);
    if (!group) {
        return;
    }
    if (kind == HitKind::Group && group->parentGroup != 0) {
        return;
    }
    if (kind == HitKind::Tag && group->parentGroup == 0) {
        return;
    }

    navDragKind_ = kind == HitKind::Group ? NavDragKind::Group : NavDragKind::Tag;
    navDragCandidateId_ = groupId;
    navDragId_ = 0;
    navDragStartPoint_ = point;
    navDragCurrentPoint_ = point;
    navDragActive_ = false;
    navDragMode_ = NavDragMode::None;
    navDragInsertIndex_ = -1;
}

void MainWindow::UpdateNavDrag(POINT point) {
    if (navDragCandidateId_ <= 0 && !navDragActive_) {
        return;
    }

    navDragCurrentPoint_ = point;
    if (!navDragActive_) {
        const int thresholdX = std::max(2, GetSystemMetrics(SM_CXDRAG));
        const int thresholdY = std::max(2, GetSystemMetrics(SM_CYDRAG));
        if (std::abs(point.x - navDragStartPoint_.x) < thresholdX &&
            std::abs(point.y - navDragStartPoint_.y) < thresholdY) {
            return;
        }
        navDragActive_ = true;
        navDragId_ = navDragCandidateId_;
        navDragMode_ = NavDragMode::None;
        navDragInsertIndex_ = -1;
        HideItemTooltip();
        hover_ = {};
        pendingHoverActivationKind_ = HitKind::None;
        pendingHoverActivationId_ = 0;
        if (hoverActivationTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
            hoverActivationTimerId_ = 0;
        }
        SetCapture(hwnd_);
    }

    UpdateNavDragTarget(point);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateNavDragTarget(POINT point) {
    navDragMode_ = NavDragMode::None;
    navDragInsertIndex_ = -1;

    point = ClientPointToDip(point);
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groupsRect, tagsRect, linksRect);

    const bool groupDrag = navDragKind_ == NavDragKind::Group;
    const D2D1_RECT_F targetRect = groupDrag ? groupsRect : CardContentRectFor(tagsRect);
    if (!Contains(targetRect, static_cast<float>(point.x), static_cast<float>(point.y))) {
        return;
    }

    const std::vector<NavItemRect> items = groupDrag ? MajorGroupItemRects(groupsRect) : TagItemRects(tagsRect);
    if (items.size() < 2) {
        return;
    }

    const bool vertical = groupDrag ? Height(groupsRect) > Width(groupsRect) : true;
    const float x = static_cast<float>(point.x);
    const float y = static_cast<float>(point.y);
    int nearestIndex = -1;
    float nearestDistance = FLT_MAX;
    for (const auto& item : items) {
        const float centerX = (item.rect.left + item.rect.right) * 0.5f;
        const float centerY = (item.rect.top + item.rect.bottom) * 0.5f;
        const float dx = x - centerX;
        const float dy = y - centerY;
        const float distance = dx * dx + dy * dy;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = item.index;
        }

        if (!Contains(item.rect, x, y)) {
            continue;
        }

        navDragMode_ = NavDragMode::Insert;
        if (vertical) {
            navDragInsertIndex_ = y < centerY ? item.index : item.index + 1;
        } else {
            navDragInsertIndex_ = x < centerX ? item.index : item.index + 1;
        }
        return;
    }

    if (nearestIndex >= 0) {
        const auto nearestIt = std::find_if(items.begin(), items.end(), [nearestIndex](const NavItemRect& item) {
            return item.index == nearestIndex;
        });
        if (nearestIt == items.end()) {
            return;
        }
        const float centerX = (nearestIt->rect.left + nearestIt->rect.right) * 0.5f;
        const float centerY = (nearestIt->rect.top + nearestIt->rect.bottom) * 0.5f;
        navDragMode_ = NavDragMode::Insert;
        navDragInsertIndex_ = vertical
            ? (y < centerY ? nearestIndex : nearestIndex + 1)
            : (x < centerX ? nearestIndex : nearestIndex + 1);
    }
}

void MainWindow::CommitNavDrag() {
    if (!navDragActive_ || navDragId_ <= 0 || navDragMode_ == NavDragMode::None || navDragInsertIndex_ < 0) {
        CancelNavDrag();
        return;
    }

    Group* dragged = FindGroup(navDragId_);
    if (!dragged) {
        CancelNavDrag();
        return;
    }
    const int parentGroup = dragged->parentGroup;
    std::vector<Group*> siblings = GroupsForParent(parentGroup);
    auto sourceIt = std::find_if(siblings.begin(), siblings.end(), [this](const Group* group) {
        return group && group->id == navDragId_;
    });
    if (sourceIt == siblings.end()) {
        CancelNavDrag();
        return;
    }

    const int sourceIndex = static_cast<int>(std::distance(siblings.begin(), sourceIt));
    int targetIndex = std::max(0, std::min(static_cast<int>(siblings.size()), navDragInsertIndex_));
    Group* moved = *sourceIt;
    siblings.erase(sourceIt);
    if (sourceIndex < targetIndex) {
        --targetIndex;
    }
    targetIndex = std::max(0, std::min(static_cast<int>(siblings.size()), targetIndex));
    if (targetIndex == sourceIndex) {
        CancelNavDrag();
        return;
    }
    siblings.insert(siblings.begin() + targetIndex, moved);

    std::vector<int> orderedIds;
    orderedIds.reserve(siblings.size());
    for (const Group* group : siblings) {
        orderedIds.push_back(group->id);
    }

    const NavDragKind draggedKind = navDragKind_;
    const int draggedId = navDragId_;
    CancelNavDrag();
    if (!ApplyManualGroupOrder(parentGroup, orderedIds, draggedKind == NavDragKind::Group ? L"拖动分组" : L"拖动标签")) {
        return;
    }

    if (draggedKind == NavDragKind::Group) {
        currentGroupId_ = draggedId;
        config_.currentGroupId = currentGroupId_;
        EnsureGroupVisible(currentGroupId_);
    } else {
        currentTagId_ = draggedId;
        config_.currentTagId = currentTagId_;
        EnsureTagVisible(currentTagId_);
    }
    configService_.SaveWindowState(config_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CancelNavDrag() {
    if (navDragActive_ && GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    navDragKind_ = NavDragKind::None;
    navDragCandidateId_ = 0;
    navDragId_ = 0;
    navDragActive_ = false;
    navDragMode_ = NavDragMode::None;
    navDragInsertIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveLinkSelection(int dx, int dy) {
    auto links = LinksForCurrentTag();
    if (links.empty()) {
        return;
    }
    if (!hwnd_) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    const D2D1_SIZE_F client = ClientSizeDip();
    BuildLayout(client.width, client.height, title, groupsRect, tagsRect, linksRect);
    const Group* tag = FindGroup(currentTagId_);
    const D2D1_RECT_F content = CardContentRectFor(linksRect);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, content, tag);
    const int columns = std::max(1, metrics.columns);
    const int count = static_cast<int>(links.size());

    int current = -1;
    for (int i = 0; i < count; ++i) {
        if (links[i]->id == selectedLinkId_) {
            current = i;
            break;
        }
    }
    if (current < 0) {
        selectedLinkId_ = links.front()->id;
        selectionByKeyboard_ = true;
        EnsureLinkVisible(selectedLinkId_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    int next = current;
    if (dx != 0) {
        next = std::max(0, std::min(count - 1, current + dx));
    } else if (dy != 0) {
        next = current + dy * columns;
        if (next < 0 || next >= count) {
            next = current;
        }
    }
    if (next == current) {
        return;
    }
    selectedLinkId_ = links[next]->id;
    selectionByKeyboard_ = true;
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveTodoSelection(int delta) {
    auto items = TodosForCurrentTag();
    if (items.empty()) {
        return;
    }

    int current = -1;
    const int count = static_cast<int>(items.size());
    for (int i = 0; i < count; ++i) {
        if (items[i]->id == selectedTodoId_) {
            current = i;
            break;
        }
    }
    if (current < 0) {
        selectedTodoId_ = delta < 0 ? items.back()->id : items.front()->id;
        selectionByKeyboard_ = true;
        EnsureTodoVisible(selectedTodoId_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const int next = std::max(0, std::min(count - 1, current + delta));
    if (next == current) {
        return;
    }
    selectedTodoId_ = items[next]->id;
    selectionByKeyboard_ = true;
    EnsureTodoVisible(selectedTodoId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SelectAdjacentTag(int direction) {
    const auto tags = TagsForCurrentGroup();
    if (tags.empty()) {
        return;
    }
    int index = -1;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].id == currentTagId_) {
            index = static_cast<int>(i);
            break;
        }
    }
    int next = index < 0 ? 0 : index + direction;
    next = std::max(0, std::min(static_cast<int>(tags.size()) - 1, next));
    if (next != index) {
        SelectTag(tags[next].id);
    }
}

void MainWindow::SelectAdjacentGroup(int direction) {
    const auto groups = MajorGroups();
    if (groups.empty()) {
        return;
    }
    int index = -1;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].id == currentGroupId_) {
            index = static_cast<int>(i);
            break;
        }
    }
    int next = index < 0 ? 0 : index + direction;
    next = std::max(0, std::min(static_cast<int>(groups.size()) - 1, next));
    if (next != index) {
        SelectGroup(groups[next].id);
    }
}

bool MainWindow::HandleKeyDown(WPARAM key) {
    const Group* tag = FindGroup(currentTagId_);
    // Note pages own their edit control; leave keyboard handling to it.
    if (tag && IsNoteTag(*tag) && noteEdit_ && GetFocus() == noteEdit_) {
        return false;
    }

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool todoTag = tag && IsTodoItemsTag(*tag);

    switch (key) {
    case VK_ESCAPE:
        if (linkDragCandidateId_ > 0 || linkDragActive_) {
            CancelLinkDrag();
            return true;
        }
        HideMainWindow();
        return true;
    case VK_TAB:
        if (config_.showTag) {
            SelectAdjacentTag(shift ? -1 : 1);
        } else if (config_.showGroup) {
            SelectAdjacentGroup(shift ? -1 : 1);
        }
        return true;
    case VK_LEFT:
        if (ctrl && config_.showGroup) {
            SelectAdjacentGroup(-1);
        } else if (todoTag && config_.showTag) {
            SelectAdjacentTag(-1);
        } else if (!todoTag) {
            MoveLinkSelection(-1, 0);
        }
        return true;
    case VK_RIGHT:
        if (ctrl && config_.showGroup) {
            SelectAdjacentGroup(1);
        } else if (todoTag && config_.showTag) {
            SelectAdjacentTag(1);
        } else if (!todoTag) {
            MoveLinkSelection(1, 0);
        }
        return true;
    case VK_UP:
        if (ctrl && config_.showTag) {
            SelectAdjacentTag(-1);
        } else if (todoTag) {
            MoveTodoSelection(-1);
        } else if (!todoTag) {
            MoveLinkSelection(0, -1);
        }
        return true;
    case VK_DOWN:
        if (ctrl && config_.showTag) {
            SelectAdjacentTag(1);
        } else if (todoTag) {
            MoveTodoSelection(1);
        } else if (!todoTag) {
            MoveLinkSelection(0, 1);
        }
        return true;
    case VK_INSERT:
        if (todoTag) {
            AddTodoItem();
        } else {
            AddLink();
        }
        return true;
    case VK_RETURN:
        if (todoTag) {
            if (selectedTodoId_ > 0) {
                EditTodoItem(selectedTodoId_);
            }
        } else if (selectedLinkId_ > 0) {
            RunLink(selectedLinkId_);
        }
        return true;
    case VK_DELETE:
        if (todoTag) {
            if (selectedTodoId_ > 0) {
                DeleteTodoItem(selectedTodoId_);
            }
        } else if (selectedLinkId_ > 0) {
            DeleteLink(selectedLinkId_);
        }
        return true;
    case VK_F2:
        if (todoTag) {
            if (selectedTodoId_ > 0) {
                EditTodoItem(selectedTodoId_);
            }
        } else if (selectedLinkId_ > 0) {
            EditLink(selectedLinkId_);
        }
        return true;
    case 'N':
        if (ctrl) {
            if (todoTag) {
                AddTodoItem();
            } else {
                AddLink();
            }
            return true;
        }
        return false;
    case 'V':
        if (ctrl) {
            ImportClipboard();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void MainWindow::SetDragOver(bool active) {
    if (dragOver_ != active) {
        dragOver_ = active;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

std::vector<Group> MainWindow::MajorGroups() const {
    std::vector<Group> groups;
    for (const auto& group : model_.groups) {
        if (group.parentGroup == 0) {
            groups.push_back(group);
        }
    }
    std::sort(groups.begin(), groups.end(), [](const Group& left, const Group& right) {
        if (left.pos != right.pos) {
            return left.pos < right.pos;
        }
        return left.id < right.id;
    });
    return groups;
}

std::vector<Group> MainWindow::TagsForCurrentGroup() const {
    std::vector<Group> tags;
    for (const auto& group : model_.groups) {
        if (group.parentGroup == currentGroupId_) {
            tags.push_back(group);
        }
    }
    std::sort(tags.begin(), tags.end(), [](const Group& left, const Group& right) {
        if (left.pos != right.pos) {
            return left.pos < right.pos;
        }
        return left.id < right.id;
    });
    return tags;
}

std::vector<Group*> MainWindow::GroupsForParent(int parentGroup) {
    std::vector<Group*> groups;
    for (auto& group : model_.groups) {
        if (group.parentGroup == parentGroup) {
            groups.push_back(&group);
        }
    }
    std::sort(groups.begin(), groups.end(), [](const Group* left, const Group* right) {
        if (left->pos != right->pos) {
            return left->pos < right->pos;
        }
        return left->id < right->id;
    });
    return groups;
}

std::vector<MainWindow::NavItemRect> MainWindow::MajorGroupItemRects(const D2D1_RECT_F& rect) const {
    std::vector<NavItemRect> items;
    if (Height(rect) <= 0.0f || Width(rect) <= 0.0f || !config_.showGroup) {
        return items;
    }

    const auto groups = MajorGroups();
    items.reserve(groups.size());
    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    if (Height(rect) > Width(rect)) {
        const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f));
        const float itemGap = Metric(theme_, L"majorNavItem", L"verticalGap", 2.0f);
        float y = rect.top + groupPadding - groupScrollOffset_;
        for (int index = 0; index < static_cast<int>(groups.size()); ++index) {
            D2D1_RECT_F item = D2D1::RectF(rect.left + groupPadding, y, rect.right - groupPadding, y + itemHeight);
            items.push_back(NavItemRect{groups[index].id, index, item});
            y += itemHeight + itemGap;
        }
        return items;
    }

    const float itemHeight = Metric(theme_, L"tabButton", L"height", 28.0f);
    const float y = rect.top + std::max(0.0f, (Height(rect) - itemHeight) * 0.5f);
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    float x = rect.left + groupPadding - groupScrollOffset_;
    for (int index = 0; index < static_cast<int>(groups.size()); ++index) {
        const auto& group = groups[index];
        const float itemWidth = TabGroupItemWidth(theme_, group.name, textFormat_, MeasureTextWidth(group.name, textFormat_));
        D2D1_RECT_F item = D2D1::RectF(x, y, x + itemWidth, y + itemHeight);
        items.push_back(NavItemRect{group.id, index, item});
        x += itemWidth + itemGap;
    }
    return items;
}

std::vector<MainWindow::NavItemRect> MainWindow::TagItemRects(const D2D1_RECT_F& rect) const {
    std::vector<NavItemRect> items;
    if (Height(rect) <= 0.0f || Width(rect) <= 0.0f || !config_.showTag) {
        return items;
    }

    const auto tags = TagsForCurrentGroup();
    items.reserve(tags.size());
    const D2D1_RECT_F content = CardContentRectFor(rect);
    const float topInset = Metric(theme_, L"minorNavItem", L"topInset", 2.0f);
    const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"minorNavItem", L"height", 28.0f));
    const float itemGap = Metric(theme_, L"minorNavItem", L"gap", 2.0f);
    float y = content.top + topInset - tagScrollOffset_;
    for (int index = 0; index < static_cast<int>(tags.size()); ++index) {
        D2D1_RECT_F item = D2D1::RectF(content.left, y, content.right, y + itemHeight);
        items.push_back(NavItemRect{tags[index].id, index, item});
        y += itemHeight + itemGap;
    }
    return items;
}

std::wstring MainWindow::TagDisplayName(const Group& tag) const {
    const Group* parent = FindGroup(tag.parentGroup);
    if (parent && !parent->name.empty()) {
        return parent->name + L" / " + tag.name;
    }
    return tag.name;
}

std::vector<Link*> MainWindow::OrderedLinksForTag(int tagId) {
    std::vector<Link*> links;
    const Group* tag = FindGroup(tagId);
    const bool showAllInGroup = tag && IsAllTag(*tag);
    const bool showTodoInGroup = tag && IsTodoTag(*tag);
    const int groupId = tag ? tag->parentGroup : 0;
    for (auto& link : model_.links) {
        if (showAllInGroup) {
            const Group* parentTag = FindGroup(link.parentGroup);
            if (parentTag && parentTag->parentGroup == groupId) {
                links.push_back(&link);
            }
        } else if (showTodoInGroup) {
            const Group* parentTag = FindGroup(link.parentGroup);
            if (parentTag && parentTag->parentGroup == groupId && LooksLikeTodoLink(link)) {
                links.push_back(&link);
            }
        } else if (link.parentGroup == tagId) {
            links.push_back(&link);
        }
    }
    const int sortMode = tag ? tag->sort : 0;
    const int sortDirection = tag ? tag->sortDirection : 0;
    std::sort(links.begin(), links.end(), [sortMode, sortDirection](const Link* left, const Link* right) {
        return LinkSortLess(left, right, sortMode, sortDirection);
    });
    return links;
}

std::vector<Link*> MainWindow::LinksForCurrentTag() {
    return OrderedLinksForTag(currentTagId_);
}

std::vector<TodoItem*> MainWindow::TodosForCurrentTag() {
    std::vector<TodoItem*> items;
    for (auto& item : model_.todos) {
        if (item.tagId == currentTagId_) {
            items.push_back(&item);
        }
    }
    const Group* tag = FindGroup(currentTagId_);
    const int sortMode = tag ? tag->sort : 0;
    std::sort(items.begin(), items.end(), [sortMode](const TodoItem* left, const TodoItem* right) {
        const bool leftDone = !left->completedAt.empty();
        const bool rightDone = !right->completedAt.empty();
        if (leftDone != rightDone) {
            return !leftDone;
        }
        if (left->enabled != right->enabled) {
            return left->enabled;
        }
        if (sortMode == 1) {
            if (left->createdAt != right->createdAt) {
                return left->createdAt < right->createdAt;
            }
        } else if (sortMode == 2) {
            const std::wstring leftTitle = InitialSortKey(left->title);
            const std::wstring rightTitle = InitialSortKey(right->title);
            if (leftTitle != rightTitle) {
                return leftTitle < rightTitle;
            }
        } else if (sortMode != 3) {
            if (left->nextDueAt != right->nextDueAt) {
                if (left->nextDueAt.empty()) {
                    return false;
                }
                if (right->nextDueAt.empty()) {
                    return true;
                }
                return left->nextDueAt < right->nextDueAt;
            }
        }
        if (left->pos != right->pos) {
            return left->pos < right->pos;
        }
        return left->id < right->id;
    });
    return items;
}

Group* MainWindow::FindGroup(int id) {
    for (auto& group : model_.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

const Group* MainWindow::FindGroup(int id) const {
    for (const auto& group : model_.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

TodoItem* MainWindow::FindTodoItem(int id) {
    for (auto& item : model_.todos) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

NotePage* MainWindow::FindNotePage(int tagId) {
    for (auto& note : model_.notes) {
        if (note.tagId == tagId) {
            return &note;
        }
    }
    return nullptr;
}

const NotePage* MainWindow::FindNotePage(int tagId) const {
    for (const auto& note : model_.notes) {
        if (note.tagId == tagId) {
            return &note;
        }
    }
    return nullptr;
}

void MainWindow::SaveCurrentNotePage() {
    if (noteSaveTimerId_ != 0 && hwnd_) {
        KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
        noteSaveTimerId_ = 0;
    }
    const int tagId = noteEditTagId_ > 0 ? noteEditTagId_ : currentTagId_;
    const Group* tag = FindGroup(tagId);
    if (!noteDirty_ || !noteEdit_ || tagId <= 0 || !tag || !IsNoteTag(*tag)) {
        return;
    }

    const int textLength = GetWindowTextLengthW(noteEdit_);
    std::wstring content(static_cast<std::size_t>(std::max(0, textLength)) + 1, L'\0');
    if (textLength > 0) {
        GetWindowTextW(noteEdit_, content.data(), textLength + 1);
    }
    content.resize(static_cast<std::size_t>(std::max(0, textLength)));

    if (!storageService_.SaveNotePage(tagId, content)) {
        WriteAppLog(L"Save note page failed: " + storageService_.lastError());
        ShowToast(L"便签保存失败，内容可能丢失，请复制备份便签内容。", ThemedToastRole::Danger, 8000);
        return;
    }

    const std::wstring updatedAt = CurrentTodoTimestamp();
    if (NotePage* note = FindNotePage(tagId)) {
        note->content = content;
        note->updatedAt = updatedAt;
    } else {
        NotePage newNote;
        newNote.tagId = tagId;
        newNote.content = content;
        newNote.updatedAt = updatedAt;
        model_.notes.push_back(std::move(newNote));
    }
    noteDirty_ = false;
}

void MainWindow::HideNoteEdit() {
    SaveCurrentNotePage();
    if (noteEdit_) {
        ShowWindow(noteEdit_, SW_HIDE);
    }
}

void MainWindow::EnsureNoteEdit(const D2D1_RECT_F& rect, const Group& tag) {
    RECT frame{
        ClientDipToPx(rect.left),
        ClientDipToPx(rect.top),
        ClientDipToPx(rect.right),
        ClientDipToPx(rect.bottom),
    };
    const NotePage* note = FindNotePage(tag.id);
    const std::wstring content = note ? note->content : L"";
    if (!embeddedUi_) {
        embeddedUi_ = std::make_unique<ThemedWindowUi>(
            instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact, 1, 1);
    }

    if (!noteEdit_) {
        ThemedEditOptions options{};
        options.mode = ThemedEditMode::MultiLine;
        noteEdit_ = embeddedUi_->ui().Edit(ID_NOTE_EDIT, frame, content, options);
        if (noteEdit_) {
            noteEditTagId_ = tag.id;
            noteEditFrame_ = frame;
            noteDirty_ = false;
        }
    }

    if (!noteEdit_) {
        return;
    }
    if (noteEditTagId_ != tag.id) {
        SaveCurrentNotePage();
        SetWindowTextW(noteEdit_, content.c_str());
        noteEditTagId_ = tag.id;
        noteDirty_ = false;
    }
    if (!EqualRect(&noteEditFrame_, &frame)) {
        embeddedUi_->MoveEditFrame(noteEdit_, frame);
        noteEditFrame_ = frame;
    }
    ShowWindow(noteEdit_, SW_SHOWNA);
}

Link* MainWindow::FindLink(int id) {
    for (auto& link : model_.links) {
        if (link.id == id) {
            return &link;
        }
    }
    return nullptr;
}

bool MainWindow::IsUrlLink(const Link& link) const {
    return link.type == 2 || LooksLikeUrlText(link.path);
}

int MainWindow::LinkIdFromHotKeyId(int hotKeyId) const {
    for (const auto& [registeredHotKeyId, linkId] : registeredLinkHotKeys_) {
        if (registeredHotKeyId == hotKeyId) {
            return linkId;
        }
    }
    return 0;
}

UINT MainWindow::CurrentDpi() const {
    if (dpi_) return dpi_;
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : USER_DEFAULT_SCREEN_DPI;
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

float MainWindow::ClientPxToDip(int value) const {
    return static_cast<float>(value) * static_cast<float>(USER_DEFAULT_SCREEN_DPI) /
        static_cast<float>(CurrentDpi());
}

LONG MainWindow::ClientDipToPx(float value) const {
    return static_cast<LONG>(std::lround(
        value * static_cast<float>(CurrentDpi()) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)));
}

POINT MainWindow::ClientPointToDip(POINT point) const {
    point.x = static_cast<LONG>(std::lround(ClientPxToDip(point.x)));
    point.y = static_cast<LONG>(std::lround(ClientPxToDip(point.y)));
    return point;
}

D2D1_SIZE_F MainWindow::ClientSizeDip() const {
    RECT client{};
    if (!hwnd_ || !GetClientRect(hwnd_, &client)) return D2D1::SizeF();
    return D2D1::SizeF(
        ClientPxToDip(client.right - client.left),
        ClientPxToDip(client.bottom - client.top));
}

void MainWindow::ApplyDpiChange(UINT newDpi, const RECT* suggestedWindowRect) {
    if (!newDpi) return;
    dpi_ = newDpi;
    if (renderTarget_) {
        renderTarget_->SetDpi(static_cast<float>(newDpi), static_cast<float>(newDpi));
    }
    iconService_.Clear();
    ClearUiBitmaps();
    if (suggestedWindowRect) {
        SetWindowPos(
            hwnd_,
            nullptr,
            suggestedWindowRect->left,
            suggestedWindowRect->top,
            suggestedWindowRect->right - suggestedWindowRect->left,
            suggestedWindowRect->bottom - suggestedWindowRect->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
    if (embeddedUi_) {
        LRESULT ignored = 0;
        embeddedUi_->HandleMessage(
            WM_DPICHANGED,
            MAKELONG(newDpi, newDpi),
            reinterpret_cast<LPARAM>(suggestedWindowRect),
            ignored);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::BuildLayout(float width, float height, D2D1_RECT_F& title, D2D1_RECT_F& groups, D2D1_RECT_F& tags, D2D1_RECT_F& links) const {
    const float titleHeight = config_.showTitle ? Metric(theme_, L"title", L"height", 32.0f) : 0.0f;
    title = D2D1::RectF(0, 0, width, titleHeight);

    float left = 0.0f;
    float right = width;
    float top = titleHeight;
    const float bottom = height;

    groups = D2D1::RectF(0, top, width, top);
    tags = D2D1::RectF(0, top, 0, bottom);
    if (config_.showGroup) {
        if (config_.groupRight) {
            const float groupWidth = ClampFloat(static_cast<float>(config_.groupWidth), 40.0f, std::max(40.0f, width * 0.42f));
            groups = D2D1::RectF(std::max(left, right - groupWidth), top, right, bottom);
            right = groups.left;
        } else {
            const float tabGroupHeight = Metric(theme_, L"tabButton", L"height", 28.0f)
                + Metric(theme_, L"tabButton", L"groupPadding", 3.0f) * 2.0f;
            const float groupHeight = std::max(Metric(theme_, L"majorNav", L"height", 32.0f), tabGroupHeight);
            groups = D2D1::RectF(0.0f, top, width, std::min(bottom, top + groupHeight));
            top = groups.bottom;
        }
    }

    if (config_.showTag) {
        // Configured tag width maps 1:1 to rendered pixels; the ratio-based
        // maxTagWidth clamp keeps it from overwhelming narrow windows.
        const float configuredTagWidth = config_.tagWidth > 0 ? static_cast<float>(config_.tagWidth) : Metric(theme_, L"minorNav", L"width", 72.0f);
        const float minTagWidth = Metric(theme_, L"minorNav", L"minWidth", 40.0f);
        const float maxTagRatio = Metric(theme_, L"minorNav", L"maxWidthRatio", 0.42f);
        const float maxTagWidth = std::max(minTagWidth, width * maxTagRatio);
        const float tagWidth = std::min(std::max(minTagWidth, configuredTagWidth), maxTagWidth);
        if (config_.tagRight) {
            tags = D2D1::RectF(std::max(left, right - tagWidth), top, right, bottom);
            right = tags.left;
        } else {
            tags = D2D1::RectF(left, top, std::min(right, left + tagWidth), bottom);
            left = tags.right;
        }
    }

    links = D2D1::RectF(left, top, right, bottom);
}

MainWindow::HitArea MainWindow::HitTest(float x, float y) const {
    for (auto it = hitAreas_.rbegin(); it != hitAreas_.rend(); ++it) {
        if (Contains(it->rect, x, y)) {
            return *it;
        }
    }
    return {};
}

bool MainWindow::IsHover(HitKind kind, int id) const {
    return hover_.kind == kind && hover_.id == id;
}

bool MainWindow::Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}
