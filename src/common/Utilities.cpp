#include "Utilities.h"

#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <dwmapi.h>

std::filesystem::path GetModuleDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD copied = 0;
    for (;;) {
        copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return std::filesystem::current_path();
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path UserHomeDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD copied = GetEnvironmentVariableW(L"USERPROFILE", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied > 0 && copied < buffer.size()) {
        buffer.resize(copied);
        return buffer;
    }

    std::wstring drive(MAX_PATH, L'\0');
    std::wstring path(MAX_PATH, L'\0');
    DWORD driveLength = GetEnvironmentVariableW(L"HOMEDRIVE", drive.data(), static_cast<DWORD>(drive.size()));
    DWORD pathLength = GetEnvironmentVariableW(L"HOMEPATH", path.data(), static_cast<DWORD>(path.size()));
    if (driveLength > 0 && driveLength < drive.size() && pathLength > 0 && pathLength < path.size()) {
        drive.resize(driveLength);
        path.resize(pathLength);
        return std::filesystem::path(drive + path);
    }

    return std::filesystem::current_path();
}

std::filesystem::path QuattroUserConfigDirectory() {
    std::wstring overridePath(MAX_PATH, L'\0');
    DWORD copied = GetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", overridePath.data(), static_cast<DWORD>(overridePath.size()));
    if (copied > 0 && copied < overridePath.size()) {
        overridePath.resize(copied);
        return overridePath;
    }
    return UserHomeDirectory() / L".quattro";
}

std::wstring Trim(const std::wstring& value) {
    auto begin = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }).base();
    if (begin >= end) {
        return {};
    }
    return std::wstring(begin, end);
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring FormatVersionForDisplay(const std::wstring& version) {
    std::wstring result = Trim(version);
    if (result.empty()) {
        return result;
    }
    if (result.front() == L'v' || result.front() == L'V') {
        result.front() = L'v';
        return result;
    }
    return L"v" + result;
}

std::wstring FormatByteSizeForDisplay(std::uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }

    std::wstringstream stream;
    if (unit == 0) {
        stream << bytes << L" " << units[unit];
    } else {
        stream << std::fixed << std::setprecision(value >= 10.0 ? 1 : 2) << value << L" " << units[unit];
    }
    return stream.str();
}

std::wstring FormatLastError(DWORD error) {
    if (error == 0) {
        error = GetLastError();
    }

    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    if (length == 0 || message == nullptr) {
        return L"Windows error " + std::to_wstring(error);
    }

    std::wstring result(message, length);
    LocalFree(message);
    return Trim(result);
}

std::wstring LoadUtf8File(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return {};
    }

    int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    return text;
}

bool FileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool DirectoryExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::wstring QuoteForCommandLine(const std::wstring& value) {
    if (value.empty()) {
        return L"\"\"";
    }
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }

    std::wstring quoted = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') {
            quoted += L'\\';
        }
        quoted += ch;
    }
    quoted += L'"';
    return quoted;
}

std::wstring ReplaceAll(std::wstring value, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) {
        return value;
    }

    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::wstring::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::uint32_t StablePathHash(const std::wstring& value) {
    const std::wstring normalized = ToLower(value);
    std::uint32_t hash = 2166136261u;
    for (wchar_t ch : normalized) {
        hash ^= static_cast<std::uint32_t>(ch);
        hash *= 16777619u;
    }
    return hash;
}

std::wstring Hex8(std::uint32_t value) {
    std::wstringstream stream;
    stream << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << value;
    return stream.str();
}

bool HasUrlScheme(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    const std::size_t colon = lower.find(L':');
    if (colon == std::wstring::npos || colon == 0) {
        return false;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        const wchar_t ch = lower[i];
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'+' || ch == L'-' || ch == L'.')) {
            return false;
        }
    }
    return true;
}

std::wstring NormalizeUrl(std::wstring value) {
    value = Trim(value);
    if (value.empty() || HasUrlScheme(value)) {
        return value;
    }
    return L"https://" + value;
}

std::wstring ExpandEnvironmentStringsSafe(const std::wstring& value) {
    if (value.empty()) {
        return value;
    }

    DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }

    std::wstring output(needed, L'\0');
    DWORD copied = ExpandEnvironmentStringsW(value.c_str(), output.data(), needed);
    if (copied == 0 || copied > needed) {
        return value;
    }
    output.resize(copied - 1);
    return output;
}

std::optional<int> ParseInt(const std::wstring& value) {
    try {
        std::size_t parsed = 0;
        int result = std::stoi(Trim(value), &parsed, 10);
        if (parsed == Trim(value).size()) {
            return result;
        }
    } catch (...) {
    }
    return std::nullopt;
}

namespace {
bool EnvironmentFlagEnabled(const wchar_t* name) {
    wchar_t value[16]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(value) / sizeof(value[0]));
    const DWORD length = GetEnvironmentVariableW(name, value, capacity);
    if (length == 0 || length >= capacity) {
        return false;
    }
    const std::wstring normalized = ToLower(Trim(value));
    return normalized == L"1" || normalized == L"true" || normalized == L"yes" || normalized == L"on";
}

void DisableWindowShowTransitions(HWND hwnd) {
    if (!hwnd) {
        return;
    }

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const DwmSetWindowAttributeFn setWindowAttribute = []() -> DwmSetWindowAttributeFn {
        HMODULE module = LoadLibraryW(L"dwmapi.dll");
        return module
            ? reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(module, "DwmSetWindowAttribute"))
            : nullptr;
    }();
    if (!setWindowAttribute) {
        return;
    }

    // DWMWA_TRANSITIONS_FORCEDISABLED. Keep modal dialogs out of DWM's
    // translucent show transition so an uncommitted first client frame can
    // never expose the window behind it.
    constexpr DWORD transitionsForcedDisabled = 3;
    const BOOL disabled = TRUE;
    setWindowAttribute(hwnd, transitionsForcedDisabled, &disabled, sizeof(disabled));
}

void PaintWindowSynchronously(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    RedrawWindow(
        hwnd,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    UpdateWindow(hwnd);
}
}

bool SuppressForegroundActivation() {
    return EnvironmentFlagEnabled(L"QUATTRO_TEST_NO_FOCUS");
}

bool QuattroTestMode() {
    return EnvironmentFlagEnabled(L"QUATTRO_TEST_MODE");
}

bool BackgroundAcceptanceMode() {
    wchar_t value[32]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(value) / sizeof(value[0]));
    const DWORD length = GetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", value, capacity);
    if (length == 0 || length >= capacity) {
        return false;
    }
    return ToLower(Trim(value)) == L"background";
}

void ApplyWindowBackgroundPolicy(HWND hwnd) {
    if (!hwnd || !BackgroundAcceptanceMode()) {
        return;
    }
    HWND target = GetAncestor(hwnd, GA_ROOTOWNER);
    if (!target) {
        target = hwnd;
    }
    SetWindowPos(
        target,
        HWND_BOTTOM,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

bool ActivateWindow(HWND hwnd) {
    if (!hwnd || SuppressForegroundActivation()) {
        return false;
    }
    const HWND target = GetAncestor(hwnd, GA_ROOT);
    if (!SetForegroundWindow(target ? target : hwnd)) {
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    const HWND foregroundRoot = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    return foregroundRoot == (target ? target : hwnd);
}

WindowFrontness EvaluateWindowFrontness(
    HWND target, const std::vector<WindowZOrderEntry>& entries) {
    const auto found = std::find_if(entries.begin(), entries.end(),
        [target](const auto& entry) { return entry.window == target; });
    if (!target || found == entries.end() || !found->valid || !found->rootOwner ||
        !found->visible || found->minimized || found->cloaked) {
        return WindowFrontness::Unknown;
    }
    bool behind = false;
    for (auto it = entries.begin(); it != found; ++it) {
        if (!it->valid) {
            return WindowFrontness::Unknown;
        }
        if (it->visible && !it->minimized && !it->cloaked && !it->auxiliary &&
            it->rootOwner != found->rootOwner && it->topMost == found->topMost) {
            behind = true;
        }
    }
    return behind ? WindowFrontness::Behind : WindowFrontness::Front;
}

namespace {
WindowZOrderEntry ReadZOrderEntry(HWND hwnd) {
    WindowZOrderEntry entry;
    entry.window = hwnd;
    WINDOWINFO info{};
    info.cbSize = sizeof(info);
    if (!IsWindow(hwnd) || !GetWindowInfo(hwnd, &info)) {
        return entry;
    }
    entry.rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
    entry.visible = (info.dwStyle & WS_VISIBLE) != 0;
    entry.minimized = (info.dwStyle & WS_MINIMIZE) != 0;
    entry.topMost = (info.dwExStyle & WS_EX_TOPMOST) != 0;
    entry.valid = entry.rootOwner != nullptr;
    if (!entry.visible || entry.minimized) {
        return entry;
    }
    wchar_t name[128]{};
    if (!GetClassNameW(hwnd, name, static_cast<int>(std::size(name)))) {
        entry.valid = false;
        return entry;
    }
    entry.auxiliary = hwnd == GetShellWindow() || hwnd == GetDesktopWindow() ||
        wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
        wcscmp(name, L"#32768") == 0 || wcscmp(name, L"tooltips_class32") == 0 ||
        ((info.dwExStyle & WS_EX_APPWINDOW) == 0 &&
            (info.dwExStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) ||
        IsRectEmpty(&info.rcWindow);
    if (!entry.auxiliary) {
        using GetAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
        static const auto getAttribute = []() -> GetAttribute {
            const HMODULE module = LoadLibraryW(L"dwmapi.dll");
            return module ? reinterpret_cast<GetAttribute>(
                GetProcAddress(module, "DwmGetWindowAttribute")) : nullptr;
        }();
        DWORD cloaked = 0;
        if (!getAttribute || FAILED(getAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
            entry.valid = false;
        }
        entry.cloaked = cloaked != 0;
    }
    entry.valid = entry.valid && IsWindow(hwnd);
    return entry;
}

std::vector<WindowZOrderEntry> ReadZOrderThrough(HWND target) {
    std::vector<WindowZOrderEntry> entries;
    std::unordered_set<HWND> visited;
    HWND window = GetTopWindow(nullptr);
    while (window && entries.size() < 4096 && visited.insert(window).second) {
        entries.push_back(ReadZOrderEntry(window));
        if (!entries.back().valid || window == target) {
            return entries;
        }
        window = GetWindow(window, GW_HWNDNEXT);
    }
    return entries;
}
}

WindowPresentation QueryWindowPresentation(HWND hwnd) {
    WindowPresentation state;
    if (!hwnd || !IsWindow(hwnd)) {
        return state;
    }
    state.visible = IsWindowVisible(hwnd) != FALSE;
    state.minimized = IsIconic(hwnd) != FALSE;
    state.topMost = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    state.foregroundWindow = GetForegroundWindow();
    const HWND owner = GetAncestor(hwnd, GA_ROOTOWNER);
    state.foreground = owner && state.foregroundWindow &&
        GetAncestor(state.foregroundWindow, GA_ROOTOWNER) == owner;
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    const DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
    if (state.foreground && thread && GetGUIThreadInfo(thread, &gui) && gui.hwndFocus) {
        state.focused = GetAncestor(gui.hwndFocus, GA_ROOTOWNER) == owner;
    }
    // Background acceptance uses synthetic snapshots, never the user's Z order.
    if (!state.visible || state.minimized || BackgroundAcceptanceMode()) {
        return state;
    }
    const auto first = ReadZOrderThrough(hwnd);
    const auto second = ReadZOrderThrough(hwnd);
    if (first == second && IsWindow(hwnd)) {
        state.frontness = EvaluateWindowFrontness(hwnd, second);
    }
    const HWND finalForeground = GetForegroundWindow();
    if (finalForeground != state.foregroundWindow) {
        state.foregroundWindow = finalForeground;
        state.foreground = false;
        state.focused = false;
        state.frontness = WindowFrontness::Unknown;
    }
    return state;
}

WindowActivationResult PerformWindowActivationAttempt(
    const WindowActivationOperations& operations, bool suppressed) {
    WindowActivationResult result;
    result.suppressed = suppressed;
    const auto continueRequest = [&]() {
        result.cancelled = operations.continueRequest && !operations.continueRequest();
        return !result.cancelled;
    };
    if (continueRequest()) {
        operations.restore();
        if (!suppressed && continueRequest()) {
            result.foregroundRequested = operations.requestForeground();
            if (continueRequest()) {
                result.positionError = operations.raise();
            }
        }
    }
    result.presentation = operations.query();
    if (!result.cancelled) {
        continueRequest();
    }
    return result;
}

WindowActivationResult RequestWindowForeground(
    HWND hwnd, bool topMost, std::function<bool()> continueRequest) {
    if (!hwnd || !IsWindow(hwnd)) {
        WindowActivationResult result;
        result.positionError = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }
    const bool suppressed = SuppressForegroundActivation() || BackgroundAcceptanceMode();
    const WindowActivationOperations operations{
        [=]() {
            if (suppressed) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                ApplyWindowBackgroundPolicy(hwnd);
            } else {
                ShowWindowRespectFocusPolicy(hwnd, SW_RESTORE);
            }
        },
        [=]() { return ActivateWindow(hwnd); },
        [=]() -> DWORD {
            // A wake is an activation request, unlike passive moves and layout.
            const HWND after = topMost ? HWND_TOPMOST :
                ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_NOTOPMOST : HWND_TOP);
            SetLastError(ERROR_SUCCESS);
            if (SetWindowPos(hwnd, after, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
                return ERROR_SUCCESS;
            }
            const DWORD error = GetLastError();
            return error ? error : ERROR_GEN_FAILURE;
        },
        [=]() { return QueryWindowPresentation(hwnd); },
        std::move(continueRequest),
    };
    return PerformWindowActivationAttempt(operations, suppressed);
}

bool WindowActivationRetry::Schedule(
    UINT_PTR generation, const WindowActivationResult& result, HWND previousForeground) {
    if (generation != generation_ || scheduled_ || result.Succeeded() || result.suppressed || result.cancelled ||
        !result.presentation.visible || result.presentation.minimized ||
        !previousForeground ||
        (!result.presentation.foreground && result.presentation.foregroundWindow != previousForeground)) {
        return false;
    }
    previousForeground_ = previousForeground;
    pending_ = true;
    scheduled_ = true;
    return true;
}

bool WindowActivationRetry::Consume(
    UINT_PTR generation, HWND currentForeground, bool targetForeground, bool visible) {
    if (generation != generation_ || !pending_) {
        return false;
    }
    pending_ = false;
    return visible && currentForeground &&
        (targetForeground || currentForeground == previousForeground_);
}

void ShowWindowRespectFocusPolicy(HWND hwnd, int showCommand) {
    if (!hwnd) {
        return;
    }
    if (SuppressForegroundActivation() && (showCommand == SW_SHOWNORMAL || showCommand == SW_RESTORE)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        // Keep acceptance-mode windows at the bottom of the z-order so tests
        // never cover the user's desktop.
        ApplyWindowBackgroundPolicy(hwnd);
        return;
    }
    ShowWindow(hwnd, showCommand);
}

POINT ClampWindowToOwnerMonitor(HWND owner, int x, int y, int width, int height) {
    RECT proposed{x, y, x + width, y + height};
    HMONITOR monitor = owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                             : MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return POINT{x, y};
    }

    const RECT work = info.rcWork;
    const int workLeft = static_cast<int>(work.left);
    const int workTop = static_cast<int>(work.top);
    const int workRight = static_cast<int>(work.right);
    const int workBottom = static_cast<int>(work.bottom);
    if (width >= work.right - work.left) {
        x = workLeft;
    } else {
        x = std::max(workLeft, std::min(x, workRight - width));
    }
    if (height >= work.bottom - work.top) {
        y = workTop;
    } else {
        y = std::max(workTop, std::min(y, workBottom - height));
    }
    return POINT{x, y};
}

POINT CenterWindowOnOwnerMonitor(HWND owner, int width, int height) {
    RECT ownerRect{};
    if (!owner || !GetWindowRect(owner, &ownerRect)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    }
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    return ClampWindowToOwnerMonitor(owner, x, y, width, height);
}

POINT OffsetWindowFromOwnerOnMonitor(HWND owner, int width, int height, int offsetX, int offsetY) {
    RECT ownerRect{};
    if (!owner || !GetWindowRect(owner, &ownerRect)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    }
    return ClampWindowToOwnerMonitor(owner, ownerRect.left + offsetX, ownerRect.top + offsetY, width, height);
}

bool ShowModalWindow(HWND owner, HWND hwnd) {
    if (!hwnd) {
        return false;
    }

    DisableWindowShowTransitions(hwnd);
    // Build the complete themed client surface while the top-level window is
    // still hidden, then synchronously refresh once more immediately after it
    // becomes visible. This prevents DWM from presenting the class' empty
    // initial backing surface before WM_ERASEBKGND/WM_PAINT and child paints.
    PaintWindowSynchronously(hwnd);
    ShowWindowRespectFocusPolicy(hwnd, SW_SHOWNORMAL);
    PaintWindowSynchronously(hwnd);
    ActivateWindow(hwnd);

    if (!owner || !IsWindow(owner) || !IsWindowEnabled(owner)) {
        return false;
    }

    EnableWindow(owner, FALSE);
    return true;
}

void RestoreModalOwner(HWND owner, bool ownerWasEnabled, bool& ownerRestored) {
    if (ownerRestored) {
        return;
    }
    ownerRestored = true;

    if (!ownerWasEnabled || !owner || !IsWindow(owner)) {
        return;
    }

    EnableWindow(owner, TRUE);
    ActivateWindow(owner);
}
