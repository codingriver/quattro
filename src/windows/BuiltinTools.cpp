#include "BuiltinTools.h"

#include "../../resources/resource.h"

#include "AppLog.h"
#include "DialogLayout.h"
#include "FileDialog.h"
#include "MainHotKey.h"
#include "SimpleDialogs.h"
#include "FileLockQueryService.h"
#include "ThemedControls.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"

#include <commdlg.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <tcpmib.h>
#include <tlhelp32.h>
#include <udpmib.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int ID_CLICK_COORD = 7001;
constexpr int ID_CLICK_COUNT = 7003;
constexpr int ID_CLICK_INTERVAL = 7004;
constexpr int ID_CLICK_BUTTON = 7005;
constexpr int ID_CLICK_PICK = 7006;
constexpr int ID_CLICK_TOGGLE = 7007;
constexpr UINT_PTR ID_CLICK_COUNTDOWN_TIMER = 7010;
constexpr UINT_PTR ID_CLICK_TIMER = 7011;
constexpr int ID_CLICK_TOGGLE_HOTKEY = 7012;
constexpr int ID_CLICK_COUNTDOWN = 7013;
constexpr int ID_CLICK_HOTKEY = 7014;
constexpr int ID_CLICK_PICK_HOTKEY_CONTROL = 7017;
constexpr int ID_CLICK_PICK_HOTKEY = 7018;
constexpr int ID_CLICK_PROGRESS = 7019;

constexpr int ID_TIMER_HOURS = 7101;
constexpr int ID_TIMER_MINUTES = 7107;
constexpr int ID_TIMER_SECONDS = 7108;
constexpr int ID_TIMER_START = 7102;
constexpr int ID_TIMER_PAUSE = 7103;
constexpr int ID_TIMER_RESET = 7104;
constexpr int ID_TIMER_STATUS = 7105;
constexpr UINT_PTR ID_TIMER_TICK = 7106;
constexpr int ID_TIMER_SOUND = 7109;
constexpr int ID_TIMER_TOPMOST = 7110;

constexpr int ID_SW_START = 7201;
constexpr int ID_SW_PAUSE = 7202;
constexpr int ID_SW_RESET = 7203;
constexpr int ID_SW_LAP = 7204;
constexpr int ID_SW_COPY = 7205;
constexpr int ID_SW_DISPLAY = 7206;
constexpr int ID_SW_LAPS = 7207;
constexpr UINT_PTR ID_SW_TICK = 7208;
constexpr int ID_SW_EXPORT = 7209;
constexpr int ID_PORT_VALUE = 7301;
constexpr int ID_PORT_SCAN = 7302;
constexpr int ID_PORT_KILL_BASE = 7310;
constexpr int ID_PROCESS_VALUE = 7401;
constexpr int ID_PROCESS_QUERY = 7402;
constexpr int ID_PROCESS_KILL_BASE = 7410;
constexpr int ID_LOCATOR_PICK = 7502;
constexpr int ID_LOCATOR_KILL = 7503;
constexpr int ID_LOCATOR_OPEN = 7504;
constexpr int ID_LOCATOR_PID_VALUE = 7506;
constexpr int ID_LOCATOR_PATH_VALUE = 7507;
constexpr int ID_FILE_LOCK_PATH = 7601;
constexpr int ID_FILE_LOCK_PICK_FILE = 7602;
constexpr int ID_FILE_LOCK_PICK_DIR = 7603;
constexpr int ID_FILE_LOCK_SCAN = 7604;
constexpr int ID_FILE_LOCK_KILL_ALL = 7605;
constexpr int ID_FILE_LOCK_PICK_MENU = 7606;
constexpr int ID_FILE_LOCK_KILL_BASE = 7610;
constexpr int ID_PROCESS_TOOLS_TAB = 7700;
constexpr int ID_PROCESS_TOOLS_TAB_BASE = 7710;
constexpr int ID_PROCESS_TOOLS_PORT_TABLE = 7720;
constexpr int ID_PROCESS_TOOLS_PID_TABLE = 7721;
constexpr int ID_PROCESS_TOOLS_FILE_TABLE = 7722;
constexpr int ID_FILE_LOCK_PROGRESS_BAR = 7730;
constexpr int ID_FILE_LOCK_PROGRESS_STOP = 7731;
constexpr int ID_FILE_LOCK_PROGRESS_CLOSE = 7732;
constexpr UINT WM_QUATTRO_TOOL_AUTOMATION = WM_APP + 0x80;
constexpr UINT WM_QUATTRO_TOOL_TIMER_AUTOMATION = WM_APP + 0x81;
constexpr UINT WM_QUATTRO_PROCESS_TOOLS_ACTIVATE = WM_APP + 0x82;
constexpr UINT WM_QUATTRO_FILE_LOCK_COMPLETE = WM_APP + 0x83;
constexpr wchar_t kProcessToolsWindowClass[] = L"QuattroProcessTools";
constexpr wchar_t kFileLockProgressWindowTitle[] = L"文件占用检查进度";
constexpr UINT kTimerDisplayIntervalMs = 33;
std::atomic<HWND> gProcessToolsWindow{nullptr};
#ifndef AF_INET6
constexpr ULONG AF_INET6 = 23;
#endif

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

int ReadIntField(HWND hwnd, int fallback, int minValue, int maxValue) {
    const std::optional<int> parsed = ParseInt(Trim(GetText(hwnd)));
    return std::max(minValue, std::min(maxValue, parsed.value_or(fallback)));
}

void SetText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

int HotKeyFromName(const std::wstring& value) {
    if (value == L"F6") return VK_F6;
    if (value == L"F7") return VK_F7;
    if (value == L"F9") return VK_F9;
    if (value == L"F10") return VK_F10;
    if (value == L"F11") return VK_F11;
    if (value == L"F12") return VK_F12;
    return VK_F8;
}

std::wstring HotKeyName(int key) {
    switch (key) {
    case VK_F6: return L"F6";
    case VK_F7: return L"F7";
    case VK_F9: return L"F9";
    case VK_F10: return L"F10";
    case VK_F11: return L"F11";
    case VK_F12: return L"F12";
    default: return L"F8";
    }
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
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
    return copied;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring FormatElapsed(ULONGLONG milliseconds) {
    const ULONGLONG totalSeconds = milliseconds / 1000;
    const ULONGLONG ms = milliseconds % 1000;
    const ULONGLONG seconds = totalSeconds % 60;
    const ULONGLONG minutes = (totalSeconds / 60) % 60;
    const ULONGLONG hours = totalSeconds / 3600;
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu.%03llu", hours, minutes, seconds, ms);
    return buffer;
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 >= path.size()) {
        return path;
    }
    return path.substr(slash + 1);
}

std::wstring QuerySnapshotProcessName(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {};
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring name;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                name = entry.szExeFile;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return name;
}

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;
    std::wstring path;
    DWORD error = ERROR_SUCCESS;
};

ProcessInfo QueryProcessInfo(DWORD pid) {
    ProcessInfo info{};
    info.pid = pid;
    if (pid == 0) {
        info.name = L"System Idle Process";
        return info;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            path.resize(length);
            info.path = path;
            info.name = FileNameFromPath(path);
        } else {
            info.error = GetLastError();
        }
        CloseHandle(process);
    } else {
        info.error = GetLastError();
    }

    if (info.name.empty()) {
        info.name = QuerySnapshotProcessName(pid);
    }
    if (info.name.empty()) {
        info.name = L"未知进程";
    }
    return info;
}

unsigned short NetworkOrderPort(DWORD value) {
    return static_cast<unsigned short>(((value & 0x00ff) << 8) | ((value & 0xff00) >> 8));
}

std::wstring TcpStateText(DWORD state) {
    switch (state) {
    case MIB_TCP_STATE_CLOSED: return L"CLOSED";
    case MIB_TCP_STATE_LISTEN: return L"LISTENING";
    case MIB_TCP_STATE_SYN_SENT: return L"SYN_SENT";
    case MIB_TCP_STATE_SYN_RCVD: return L"SYN_RCVD";
    case MIB_TCP_STATE_ESTAB: return L"ESTABLISHED";
    case MIB_TCP_STATE_FIN_WAIT1: return L"FIN_WAIT1";
    case MIB_TCP_STATE_FIN_WAIT2: return L"FIN_WAIT2";
    case MIB_TCP_STATE_CLOSE_WAIT: return L"CLOSE_WAIT";
    case MIB_TCP_STATE_CLOSING: return L"CLOSING";
    case MIB_TCP_STATE_LAST_ACK: return L"LAST_ACK";
    case MIB_TCP_STATE_TIME_WAIT: return L"TIME_WAIT";
    case MIB_TCP_STATE_DELETE_TCB: return L"DELETE_TCB";
    default: return L"UNKNOWN";
    }
}

std::wstring JoinStrings(const std::set<std::wstring>& values, const wchar_t* separator) {
    std::wstring joined;
    for (const auto& value : values) {
        if (!joined.empty()) {
            joined += separator;
        }
        joined += value;
    }
    return joined;
}

struct ProcessDisplayRow {
    DWORD pid = 0;
    std::wstring title;
    std::wstring detail;
};

struct PortProcessBucket {
    DWORD pid = 0;
    std::set<std::wstring> endpoints;
};

struct Tcp6RowOwnerPidCompat {
    UCHAR localAddr[16]{};
    DWORD localScopeId = 0;
    DWORD localPort = 0;
    UCHAR remoteAddr[16]{};
    DWORD remoteScopeId = 0;
    DWORD remotePort = 0;
    DWORD state = 0;
    DWORD owningPid = 0;
};

struct Tcp6TableOwnerPidCompat {
    DWORD entryCount = 0;
    Tcp6RowOwnerPidCompat table[1]{};
};

struct Udp6RowOwnerPidCompat {
    UCHAR localAddr[16]{};
    DWORD localScopeId = 0;
    DWORD localPort = 0;
    DWORD owningPid = 0;
};

struct Udp6TableOwnerPidCompat {
    DWORD entryCount = 0;
    Udp6RowOwnerPidCompat table[1]{};
};

void AddPortBucket(std::map<DWORD, PortProcessBucket>& buckets, DWORD pid, const std::wstring& endpoint) {
    auto& bucket = buckets[pid];
    bucket.pid = pid;
    bucket.endpoints.insert(endpoint);
}

void CollectTcp4Port(unsigned short port, std::map<DWORD, PortProcessBucket>& buckets) {
    DWORD size = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }
    std::vector<BYTE> buffer(size);
    result = GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != NO_ERROR) {
        return;
    }
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (NetworkOrderPort(row.dwLocalPort) == port) {
            AddPortBucket(buckets, row.dwOwningPid, L"TCP " + TcpStateText(row.dwState));
        }
    }
}

void CollectTcp6Port(unsigned short port, std::map<DWORD, PortProcessBucket>& buckets) {
    DWORD size = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }
    std::vector<BYTE> buffer(size);
    result = GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != NO_ERROR) {
        return;
    }
    auto* table = reinterpret_cast<Tcp6TableOwnerPidCompat*>(buffer.data());
    for (DWORD i = 0; i < table->entryCount; ++i) {
        const auto& row = table->table[i];
        if (NetworkOrderPort(row.localPort) == port) {
            AddPortBucket(buckets, row.owningPid, L"TCP6 " + TcpStateText(row.state));
        }
    }
}

void CollectUdp4Port(unsigned short port, std::map<DWORD, PortProcessBucket>& buckets) {
    DWORD size = 0;
    DWORD result = GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }
    std::vector<BYTE> buffer(size);
    result = GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (result != NO_ERROR) {
        return;
    }
    auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (NetworkOrderPort(row.dwLocalPort) == port) {
            AddPortBucket(buckets, row.dwOwningPid, L"UDP");
        }
    }
}

void CollectUdp6Port(unsigned short port, std::map<DWORD, PortProcessBucket>& buckets) {
    DWORD size = 0;
    DWORD result = GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }
    std::vector<BYTE> buffer(size);
    result = GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    if (result != NO_ERROR) {
        return;
    }
    auto* table = reinterpret_cast<Udp6TableOwnerPidCompat*>(buffer.data());
    for (DWORD i = 0; i < table->entryCount; ++i) {
        const auto& row = table->table[i];
        if (NetworkOrderPort(row.localPort) == port) {
            AddPortBucket(buckets, row.owningPid, L"UDP6");
        }
    }
}

std::vector<ProcessDisplayRow> QueryPortRows(unsigned short port) {
    std::map<DWORD, PortProcessBucket> buckets;
    CollectTcp4Port(port, buckets);
    CollectTcp6Port(port, buckets);
    CollectUdp4Port(port, buckets);
    CollectUdp6Port(port, buckets);

    std::vector<ProcessDisplayRow> rows;
    for (const auto& [pid, bucket] : buckets) {
        const ProcessInfo info = QueryProcessInfo(pid);
        ProcessDisplayRow row{};
        row.pid = pid;
        row.title = JoinStrings(bucket.endpoints, L" / ") + L"  PID " + std::to_wstring(pid) + L"  " + info.name;
        row.detail = info.path.empty() ? L"进程路径不可读，仍可尝试结束进程" : info.path;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<ProcessDisplayRow> FileLockRowsFromResult(const FileLockQueryResult& query) {
    std::vector<ProcessDisplayRow> rows;
    for (unsigned long rawPid : query.processIds) {
        const DWORD pid = static_cast<DWORD>(rawPid);
        const ProcessInfo info = QueryProcessInfo(pid);
        ProcessDisplayRow row{};
        row.pid = pid;
        row.title = L"PID " + std::to_wstring(pid) + L"  " + info.name;
        row.detail = info.path.empty() ? L"进程路径不可读，仍可尝试结束进程" : info.path;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<ProcessDisplayRow> QueryFileLockRows(const std::wstring& rawPath, std::wstring& statusSuffix) {
    statusSuffix.clear();
    const FileLockQueryResult query = QueryFileLocks(rawPath);
    if (!query.error.empty()) {
        statusSuffix = query.error;
        return {};
    }
    if (query.cancelled) {
        statusSuffix = L"检查已停止。";
        return {};
    }

    std::vector<ProcessDisplayRow> rows = FileLockRowsFromResult(query);
    statusSuffix = L"已检查 " + std::to_wstring(query.checkedPaths) + L" 个路径";
    if (!query.warning.empty()) {
        statusSuffix += L"，" + query.warning;
    }
    statusSuffix += L"。";
    return rows;
}

std::wstring KillProcessById(DWORD pid) {
    if (pid == 0 || pid == 4) {
        return L"系统进程不能结束。";
    }
    if (pid == GetCurrentProcessId()) {
        return L"不能结束当前 Quattro 进程。";
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return L"打开进程失败：" + FormatLastError(GetLastError());
    }
    if (!TerminateProcess(process, 1)) {
        const std::wstring error = L"结束进程失败：" + FormatLastError(GetLastError());
        CloseHandle(process);
        return error;
    }
    WaitForSingleObject(process, 1500);
    CloseHandle(process);
    return {};
}

bool ClassNameEquals(HWND hwnd, const wchar_t* expected) {
    wchar_t className[128]{};
    return hwnd && GetClassNameW(hwnd, className, static_cast<int>(_countof(className))) > 0
        && _wcsicmp(className, expected) == 0;
}

bool IsTrayWindow(HWND hwnd) {
    for (HWND current = hwnd; current; current = GetParent(current)) {
        if (ClassNameEquals(current, L"Shell_TrayWnd") || ClassNameEquals(current, L"NotifyIconOverflowWindow")) {
            return true;
        }
    }
    return false;
}

HWND FindTrayToolbar(HWND hwnd) {
    for (HWND current = hwnd; current; current = GetParent(current)) {
        if (ClassNameEquals(current, L"ToolbarWindow32")) {
            return current;
        }
    }
    return nullptr;
}

DWORD QueryClassicTrayProcessId(HWND toolbar, POINT screenPoint, DWORD& error) {
    error = ERROR_NOT_FOUND;
    DWORD explorerPid = 0;
    GetWindowThreadProcessId(toolbar, &explorerPid);
    if (!explorerPid) {
        error = GetLastError();
        return 0;
    }

    HANDLE explorer = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, explorerPid);
    if (!explorer) {
        error = GetLastError();
        return 0;
    }

    const SIZE_T remoteSize = std::max(sizeof(TBBUTTON), sizeof(RECT));
    void* remote = VirtualAllocEx(explorer, nullptr, remoteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        error = GetLastError();
        CloseHandle(explorer);
        return 0;
    }

    DWORD resultPid = 0;
    const LRESULT buttonCount = SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0);
    for (int index = 0; index < buttonCount; ++index) {
        if (!SendMessageW(toolbar, TB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(remote))) {
            continue;
        }
        RECT itemRect{};
        if (!ReadProcessMemory(explorer, remote, &itemRect, sizeof(itemRect), nullptr)) {
            continue;
        }
        POINT corners[2]{{itemRect.left, itemRect.top}, {itemRect.right, itemRect.bottom}};
        MapWindowPoints(toolbar, nullptr, corners, 2);
        RECT screenRect{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
        if (!PtInRect(&screenRect, screenPoint)) {
            continue;
        }
        if (!SendMessageW(toolbar, TB_GETBUTTON, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(remote))) {
            break;
        }
        TBBUTTON button{};
        if (!ReadProcessMemory(explorer, remote, &button, sizeof(button), nullptr) || !button.dwData) {
            break;
        }
        ULONG_PTR ownerValue = 0;
        if (!ReadProcessMemory(explorer, reinterpret_cast<const void*>(button.dwData), &ownerValue, sizeof(ownerValue), nullptr)) {
            error = GetLastError();
            break;
        }
        HWND ownerWindow = reinterpret_cast<HWND>(ownerValue);
        if (!IsWindow(ownerWindow)) {
            error = ERROR_INVALID_WINDOW_HANDLE;
            break;
        }
        GetWindowThreadProcessId(ownerWindow, &resultPid);
        error = resultPid ? ERROR_SUCCESS : GetLastError();
        break;
    }

    VirtualFreeEx(explorer, remote, 0, MEM_RELEASE);
    CloseHandle(explorer);
    return resultPid;
}

bool IsExplorerProcess(DWORD pid) {
    if (!pid) {
        return false;
    }
    const ProcessInfo info = QueryProcessInfo(pid);
    return _wcsicmp(info.name.c_str(), L"explorer.exe") == 0;
}

std::wstring LowerAscii(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool IsBatchKillProtectedSystemProcess(DWORD pid, const ProcessInfo& info) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
        return true;
    }
    const std::wstring name = LowerAscii(info.name);
    static const std::set<std::wstring> protectedNames{
        L"system",
        L"system idle process",
        L"registry",
        L"smss.exe",
        L"csrss.exe",
        L"wininit.exe",
        L"winlogon.exe",
        L"services.exe",
        L"lsass.exe",
        L"lsm.exe",
        L"svchost.exe",
        L"explorer.exe",
        L"dwm.exe",
        L"sihost.exe",
        L"shellhost.exe",
        L"fontdrvhost.exe",
    };
    return protectedNames.find(name) != protectedNames.end();
}

struct HoveredProcessResult {
    DWORD pid = 0;
    DWORD error = ERROR_SUCCESS;
    bool trayTarget = false;
};

HoveredProcessResult QueryHoveredProcess(HWND locatorWindow) {
    HoveredProcessResult result{};
    POINT point{};
    if (!GetCursorPos(&point)) {
        result.error = GetLastError();
        return result;
    }
    HWND target = WindowFromPoint(point);
    if (!target) {
        result.error = ERROR_NOT_FOUND;
        return result;
    }
    if (target == locatorWindow || IsChild(locatorWindow, target)) {
        result.error = ERROR_INVALID_TARGET_HANDLE;
        return result;
    }

    result.trayTarget = IsTrayWindow(target);
    if (result.trayTarget) {
        HWND toolbar = FindTrayToolbar(target);
        if (!toolbar) {
            result.error = ERROR_NOT_SUPPORTED;
            return result;
        }
        result.pid = QueryClassicTrayProcessId(toolbar, point, result.error);
        if (IsExplorerProcess(result.pid)) {
            result.pid = 0;
            result.error = ERROR_NOT_SUPPORTED;
        }
        return result;
    }

    HWND root = GetAncestor(target, GA_ROOT);
    GetWindowThreadProcessId(root ? root : target, &result.pid);
    if (!result.pid) {
        result.error = GetLastError();
    }
    return result;
}

std::wstring DirectoryFromPath(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring OpenProcessLocation(const std::wstring& path) {
    if (path.empty()) {
        return L"没有可打开的程序路径。";
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const std::wstring directory = DirectoryFromPath(path);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        const std::wstring parameters = L"/select,\"" + path + L"\"";
        HINSTANCE opened = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) > 32) {
            return {};
        }
    }
    if (!directory.empty() && GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES) {
        HINSTANCE opened = ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) > 32) {
            return {};
        }
    }
    return L"打开程序所在目录失败，路径可能已经失效。";
}

int VisibleProcessRowCount(const Theme& theme, RECT frame) {
    const int padding = 8;
    const int rowHeight = ThemedControls::ListBoxTwoLineItemHeight(theme);
    const int availableHeight = static_cast<int>(frame.bottom - frame.top) - padding * 2;
    return std::max(1, availableHeight / rowHeight);
}

void DrawProcessRows(
    const Theme& theme,
    HDC dc,
    RECT frame,
    const std::vector<ProcessDisplayRow>& rows,
    const std::wstring& emptyText,
    HFONT font) {
    ThemedControls::DrawListFrame(theme, dc, frame, nullptr);
    const int padding = 8;
    RECT content = frame;
    InflateRect(&content, -padding, -padding);
    if (rows.empty()) {
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(theme.color(L"text", L"muted", L"text")));
        DrawTextW(dc, emptyText.c_str(), -1, &content, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        return;
    }

    const int rowHeight = ThemedControls::ListBoxTwoLineItemHeight(theme);
    const int visibleCount = std::min<int>(static_cast<int>(rows.size()), VisibleProcessRowCount(theme, frame));
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < visibleCount; ++i) {
        RECT rowRect{content.left, content.top + i * rowHeight, content.right, content.top + (i + 1) * rowHeight - 2};
        RECT titleRect{rowRect.left + 2, rowRect.top + 2, rowRect.right - 76, rowRect.top + 22};
        RECT detailRect{rowRect.left + 2, rowRect.top + 22, rowRect.right - 76, rowRect.bottom};
        SetTextColor(dc, ToColorRef(theme.color(L"listItem", L"normal", L"text")));
        DrawTextW(dc, rows[static_cast<std::size_t>(i)].title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        SetTextColor(dc, ToColorRef(theme.color(L"text", L"muted", L"text")));
        DrawTextW(dc, rows[static_cast<std::size_t>(i)].detail.c_str(), -1, &detailRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }
    SelectObject(dc, oldFont);
}

class ToolDialogBase {
public:
    ToolDialogBase(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry, std::wstring title, int width, int height)
        : owner_(owner), instance_(instance), theme_(theme), registry_(registry), title_(std::move(title)), width_(width), height_(height) {}

    bool Run() {
        const std::wstring className = L"QuattroBuiltinTool_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        auto options = ThemedWindowUi::DialogOptions(
            instance_,
            owner_,
            className.c_str(),
            title_.c_str(),
            ToolDialogBase::Proc,
            this,
            LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)),
            LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)));
        options.clientWidth = width_;
        options.clientHeight = height_;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 70;
        options.offsetY = 70;
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(title_ + L"窗口创建失败: " + error);
            return false;
        }

        windowUi_->ShowModal();
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (IsToolKeyMessage(message) && OnShortcutKey(message)) {
                continue;
            }
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return true;
    }

protected:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ToolDialogBase* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ToolDialogBase*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ToolDialogBase*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            if (message == WM_DESTROY) {
                OnDestroy();
                done_ = true;
            }
            return commonResult;
        }

        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact, width_, height_);
            OnCreate();
            return 0;
        case WM_COMMAND:
            if (OnCommand(LOWORD(wParam), HIWORD(wParam))) {
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                Close();
                return 0;
            }
            return 0;
        case WM_QUATTRO_TOOL_AUTOMATION:
            if (OnCommand(static_cast<int>(wParam), 0)) {
                return 1;
            }
            return 0;
        case WM_QUATTRO_TOOL_TIMER_AUTOMATION:
            if (OnAutomationTimer(static_cast<UINT_PTR>(wParam))) {
                return 1;
            }
            return 0;
        case WM_TIMER:
            if (OnTimer(static_cast<UINT_PTR>(wParam))) {
                return 0;
            }
            return 0;
        case WM_HOTKEY:
            if (OnHotKey(static_cast<int>(wParam))) {
                return 0;
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredEditFrames(dc);
            OnPaint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_CLOSE:
            Close();
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    virtual void OnCreate() = 0;
    virtual bool OnCommand(int id, int notify) = 0;
    virtual bool OnShortcutKey(const MSG& message) { (void)message; return false; }
    virtual bool OnTimer(UINT_PTR id) { (void)id; return false; }
    virtual bool OnAutomationTimer(UINT_PTR id) { return OnTimer(id); }
    virtual bool OnHotKey(int id) { (void)id; return false; }
    virtual void OnPaint(HDC dc) { (void)dc; }
    virtual void OnDestroy() {}

    void Close() {
        done_ = true;
        DestroyWindow(hwnd_);
    }

    HFONT font() const {
        return windowUi_ ? windowUi_->font() : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    HFONT editFont() const {
        return font();
    }

    DialogLayoutMetrics CompactLayout() const {
        return GetDialogLayoutMetrics(theme_, DialogLayoutKind::Compact);
    }

    ThemedUi MakeUi() const {
        return windowUi_->ui();
    }

    void ShowToast(const std::wstring& text, ThemedToastRole role, int durationMs = 0) {
        if (!windowUi_) {
            return;
        }
        ThemedToastOptions options{};
        options.role = role;
        if (durationMs > 0) {
            options.durationMs = durationMs;
        }
        windowUi_->ui().ShowToast(text, options);
    }

    HWND CreateEdit(int id, int x, int y, int width, const std::wstring& value, DWORD extraStyle = ES_AUTOHSCROLL) {
        ThemedEditOptions options{};
        options.content = (extraStyle & ES_NUMBER) != 0 ? ThemedEditContent::Integer : ThemedEditContent::Text;
        options.readOnly = (extraStyle & ES_READONLY) != 0;
        return MakeUi().Edit(id, MakeUi().editFrame(x, y, width), value, options);
    }

    bool IsToolKeyMessage(const MSG& message) const {
        if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
            return false;
        }
        return message.hwnd == hwnd_ || IsChild(hwnd_, message.hwnd);
    }

    bool CtrlOnly() const {
        return (GetKeyState(VK_CONTROL) & 0x8000) != 0
            && (GetKeyState(VK_MENU) & 0x8000) == 0
            && (GetKeyState(VK_SHIFT) & 0x8000) == 0;
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    HWND hwnd_ = nullptr;
    std::wstring title_;
    int width_ = 0;
    int height_ = 0;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool done_ = false;
};

class ClickerDialog final : public ToolDialogBase {
public:
    ClickerDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"连点器", 390, 246) {}

private:
    void OnCreate() override {
        const wchar_t* pluginId = L"quattro.builtin.clicker";
        const std::wstring savedX = registry_.GetSetting(pluginId, L"x", L"0");
        const std::wstring savedY = registry_.GetSetting(pluginId, L"y", L"0");

        const DialogLayoutMetrics layout = CompactLayout();
        const int editHeight = ThemedControls::EditFrameHeight(theme_);
        const int labelHeight = ThemedControls::LabelHeight(theme_);
        const int bh = ThemedControls::ButtonHeight(theme_);
        const int rowStep = layout.RowStep(bh);
        const int labelOffsetY = std::max(0, (editHeight - labelHeight) / 2);
        const int left = layout.contentInsetX;
        const int fieldX = layout.fieldX;
        const int fieldW = 70;
        const int rightLabelX = fieldX + fieldW + layout.controlGapX + layout.labelGap;
        const int rightFieldX = rightLabelX + layout.labelWidth + layout.labelGap;
        const int row0 = layout.contentInsetY;
        const int row1 = row0 + rowStep;
        const int row2 = row1 + rowStep;
        const int row3 = row2 + rowStep;

        MakeUi().Label(L"坐标（x，y）", left, row0 + labelOffsetY, layout.labelWidth);
        coord_ = CreateEdit(ID_CLICK_COORD, fieldX, row0, 100, savedX + L", " + savedY);
        MakeUi().Button(ID_CLICK_PICK, L"拾取(&P)", fieldX + 100 + layout.controlGapX, row0 + 1, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, layout.footerButtonWidth);

        MakeUi().Label(L"点击次数", left, row1 + labelOffsetY, layout.labelWidth);
        count_ = CreateEdit(ID_CLICK_COUNT, fieldX, row1, fieldW, registry_.GetSetting(pluginId, L"count", L"10"), ES_NUMBER);
        MakeUi().Label(L"间隔(ms)", rightLabelX, row1 + labelOffsetY, layout.labelWidth);
        interval_ = CreateEdit(ID_CLICK_INTERVAL, rightFieldX, row1, fieldW, registry_.GetSetting(pluginId, L"interval", L"1000"), ES_NUMBER);

        MakeUi().Label(L"鼠标按键", left, row2 + labelOffsetY, layout.labelWidth);
        button_ = MakeUi().ComboBox(ID_CLICK_BUTTON, fieldX, row2 + 2, layout.footerButtonWidth);
        SendMessageW(button_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"左键"));
        SendMessageW(button_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"右键"));
        SendMessageW(button_, CB_SETCURSEL, registry_.GetSetting(pluginId, L"button", L"left") == L"right" ? 1 : 0, 0);

        MakeUi().Label(L"倒计时(s)", rightLabelX, row2 + labelOffsetY, layout.labelWidth);
        countdownEdit_ = CreateEdit(ID_CLICK_COUNTDOWN, rightFieldX, row2, fieldW, registry_.GetSetting(pluginId, L"countdown", L"3"), ES_NUMBER);

        const wchar_t* hotKeys[] = {L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"};

        MakeUi().Label(L"启动停止热键", left, row3 + labelOffsetY, layout.labelWidth + layout.labelGap);
        toggleHotKey_ = MakeUi().ComboBox(ID_CLICK_HOTKEY, fieldX, row3 + 2, fieldW);
        FillHotKeyCombo(toggleHotKey_, registry_.GetSetting(pluginId, L"toggleHotKey", registry_.GetSetting(pluginId, L"stopHotKey", L"F8")), hotKeys, 7);

        MakeUi().Label(L"拾取热键", rightLabelX, row3 + labelOffsetY, layout.labelWidth);
        pickHotKey_ = MakeUi().ComboBox(ID_CLICK_PICK_HOTKEY_CONTROL, rightFieldX, row3 + 2, fieldW);
        FillHotKeyCombo(pickHotKey_, registry_.GetSetting(pluginId, L"pickHotKey", L"F9"), hotKeys, 7);

        const int toggleY = row3 + rowStep + layout.footerGap;
        toggle_ = MakeUi().Button(
            ID_CLICK_TOGGLE,
            L"启动(&S)",
            layout.FooterButtonX(width_, 0, 1),
            toggleY,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            layout.footerButtonWidth,
            true);
        const int statusY = toggleY + bh + layout.rowGap;
        ThemedStatusTextOptions startAligned{};
        startAligned.align = ThemedTextAlign::Start;
        status_ = MakeUi().StatusText(L"就绪。", left, statusY, width_ - left * 2 - 120, startAligned);
        ThemedStatusTextOptions endAligned{};
        endAligned.align = ThemedTextAlign::End;
        progress_ = MakeUi().StatusText(L"当前点击：0 / 0", width_ - left - 120, statusY, 120, endAligned);
        RegisterToolHotKeys();
    }

    bool OnCommand(int id, int notify) override {
        if (id == ID_CLICK_PICK) {
            PickCurrentPosition();
            return true;
        }
        if (id == ID_CLICK_TOGGLE) {
            ToggleRunning();
            return true;
        }
        if ((id == ID_CLICK_HOTKEY || id == ID_CLICK_PICK_HOTKEY_CONTROL) && notify == CBN_SELCHANGE) {
            SaveHotKeySettings();
            RegisterToolHotKeys();
            return true;
        }
        return false;
    }

    bool OnShortcutKey(const MSG& message) override {
        if (!CtrlOnly()) {
            return false;
        }
        switch (message.wParam) {
        case 'P':
            PickCurrentPosition();
            return true;
        case 'S':
            ToggleRunning();
            return true;
        case 'T':
            Stop(L"已停止。");
            return true;
        default:
            return false;
        }
    }

    bool OnTimer(UINT_PTR id) override {
        if (id == ID_CLICK_COUNTDOWN_TIMER) {
            --countdown_;
            if (countdown_ <= 0) {
                KillTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER);
                BeginClicking();
            } else {
                SetText(status_, std::to_wstring(countdown_) + L" 秒后开始...");
            }
            return true;
        }
        if (id == ID_CLICK_TIMER) {
            DoClick();
            if (ReachedClickLimit()) {
                Stop(L"点击完成。");
            } else {
                SetText(status_, L"正在点击，按 " + HotKeyName(toggleHotKeyCode_) + L" 停止。");
            }
            return true;
        }
        return false;
    }

    bool OnHotKey(int id) override {
        if (id == ID_CLICK_TOGGLE_HOTKEY) {
            ToggleRunning();
            return true;
        }
        if (id == ID_CLICK_PICK_HOTKEY) {
            PickCurrentPosition();
            return true;
        }
        return false;
    }

    void OnDestroy() override {
        Stop(L"");
        UnregisterToolHotKeys();
    }

    void FillHotKeyCombo(HWND combo, const std::wstring& selected, const wchar_t* const* hotKeys, int count) {
        int selectedIndex = 0;
        for (int i = 0; i < count; ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(hotKeys[i]));
            if (selected == hotKeys[i]) {
                selectedIndex = i;
            }
        }
        SendMessageW(combo, CB_SETCURSEL, selectedIndex, 0);
    }

    std::wstring SelectedHotKeyName(HWND combo, const std::wstring& fallback) const {
        const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selected < 0) {
            return fallback;
        }
        wchar_t text[32]{};
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(selected), reinterpret_cast<LPARAM>(text));
        return text[0] ? std::wstring(text) : fallback;
    }

    void SaveHotKeySettings() {
        registry_.SetSetting(L"quattro.builtin.clicker", L"toggleHotKey", SelectedHotKeyName(toggleHotKey_, L"F8"));
        registry_.SetSetting(L"quattro.builtin.clicker", L"pickHotKey", SelectedHotKeyName(pickHotKey_, L"F9"));
    }

    void RegisterToolHotKeys() {
        UnregisterToolHotKeys();
        toggleHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(toggleHotKey_, L"F8"));
        pickHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(pickHotKey_, L"F9"));
        toggleHotKeyRegistered_ = RegisterHotKey(hwnd_, ID_CLICK_TOGGLE_HOTKEY, MOD_NOREPEAT, toggleHotKeyCode_) != FALSE;
        pickHotKeyRegistered_ = RegisterHotKey(hwnd_, ID_CLICK_PICK_HOTKEY, MOD_NOREPEAT, pickHotKeyCode_) != FALSE;
        if (!toggleHotKeyRegistered_ || !pickHotKeyRegistered_) {
            SetText(status_, L"全局热键注册失败，请换一个 F 键。");
            ShowToast(L"全局热键注册失败，请换一个 F 键。", ThemedToastRole::Warning);
            return;
        }
        SetText(status_, L"就绪。按 " + HotKeyName(toggleHotKeyCode_) + L" 启动/停止，" + HotKeyName(pickHotKeyCode_) + L" 拾取坐标。");
    }

    void UnregisterToolHotKeys() {
        if (toggleHotKeyRegistered_) {
            UnregisterHotKey(hwnd_, ID_CLICK_TOGGLE_HOTKEY);
            toggleHotKeyRegistered_ = false;
        }
        if (pickHotKeyRegistered_) {
            UnregisterHotKey(hwnd_, ID_CLICK_PICK_HOTKEY);
            pickHotKeyRegistered_ = false;
        }
    }

    std::pair<int, int> ReadCoordinateField() const {
        std::wstring text = GetText(coord_);
        for (wchar_t& ch : text) {
            if (ch == L'，' || ch == L'(' || ch == L')' || ch == L'（' || ch == L'）') {
                ch = L',';
            }
        }
        const std::size_t comma = text.find(L',');
        if (comma == std::wstring::npos) {
            return {targetX_, targetY_};
        }
        const int x = std::max(0, std::min(99999, ParseInt(Trim(text.substr(0, comma))).value_or(targetX_)));
        const int y = std::max(0, std::min(99999, ParseInt(Trim(text.substr(comma + 1))).value_or(targetY_)));
        return {x, y};
    }

    void SetCoordinateText(int x, int y) {
        SetText(coord_, std::to_wstring(x) + L", " + std::to_wstring(y));
    }

    void ToggleRunning() {
        if (running_) {
            Stop(L"已停止。");
        } else {
            Start();
        }
    }

    void Start() {
        if (running_) {
            return;
        }
        const auto [x, y] = ReadCoordinateField();
        targetX_ = x;
        targetY_ = y;
        SetCoordinateText(targetX_, targetY_);
        totalClicks_ = ReadIntField(count_, 10, 0, 99999);
        clickedCount_ = 0;
        intervalMs_ = ReadIntField(interval_, 1000, 100, 3600000);
        countdown_ = ReadIntField(countdownEdit_, 3, 0, 60);
        SetText(count_, std::to_wstring(totalClicks_));
        SetText(interval_, std::to_wstring(intervalMs_));
        SetText(countdownEdit_, std::to_wstring(countdown_));
        const bool rightButton = SendMessageW(button_, CB_GETCURSEL, 0, 0) == 1;
        toggleHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(toggleHotKey_, L"F8"));
        pickHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(pickHotKey_, L"F9"));

        registry_.SetSetting(L"quattro.builtin.clicker", L"x", std::to_wstring(targetX_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"y", std::to_wstring(targetY_));
        SetText(status_, L"已拾取坐标。");
        registry_.SetSetting(L"quattro.builtin.clicker", L"count", std::to_wstring(totalClicks_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"interval", std::to_wstring(intervalMs_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"countdown", std::to_wstring(countdown_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"toggleHotKey", HotKeyName(toggleHotKeyCode_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"pickHotKey", HotKeyName(pickHotKeyCode_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"button", rightButton ? L"right" : L"left");

        rightButton_ = rightButton;
        running_ = true;
        SetText(toggle_, L"停止(&S)");
        UpdateProgress();
        if (countdown_ <= 0) {
            BeginClicking();
        } else {
            SetText(status_, std::to_wstring(countdown_) + L" 秒后开始...");
            SetTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER, 1000, nullptr);
        }
    }

    void PickCurrentPosition() {
        POINT point{};
        GetCursorPos(&point);
        targetX_ = point.x;
        targetY_ = point.y;
        SetCoordinateText(targetX_, targetY_);
        registry_.SetSetting(L"quattro.builtin.clicker", L"x", std::to_wstring(targetX_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"y", std::to_wstring(targetY_));
    }

    void Stop(const std::wstring& status) {
        KillTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER);
        KillTimer(hwnd_, ID_CLICK_TIMER);
        running_ = false;
        if (toggle_) {
            SetText(toggle_, L"启动(&S)");
        }
        if (!status.empty() && status_) {
            SetText(status_, status);
        }
    }

    void BeginClicking() {
        SetText(status_, L"正在点击，按 " + HotKeyName(toggleHotKeyCode_) + L" 或按钮停止。");
        DoClick();
        if (ReachedClickLimit()) {
            Stop(L"点击完成。");
            return;
        }
        SetTimer(hwnd_, ID_CLICK_TIMER, static_cast<UINT>(intervalMs_), nullptr);
    }

    void DoClick() {
        SetCursorPos(targetX_, targetY_);
        INPUT inputs[2]{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = rightButton_ ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = rightButton_ ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        SendInput(2, inputs, sizeof(INPUT));
        ++clickedCount_;
        UpdateProgress();
    }

    bool ReachedClickLimit() const {
        return totalClicks_ > 0 && clickedCount_ >= totalClicks_;
    }

    void UpdateProgress() {
        if (progress_) {
            const std::wstring totalText = totalClicks_ > 0 ? std::to_wstring(totalClicks_) : L"不限";
            SetText(progress_, std::to_wstring(clickedCount_) + L" / " + totalText);
        }
    }

    HWND coord_ = nullptr;
    HWND count_ = nullptr;
    HWND interval_ = nullptr;
    HWND button_ = nullptr;
    HWND countdownEdit_ = nullptr;
    HWND toggleHotKey_ = nullptr;
    HWND pickHotKey_ = nullptr;
    HWND progress_ = nullptr;
    HWND toggle_ = nullptr;
    HWND status_ = nullptr;
    int targetX_ = 0;
    int targetY_ = 0;
    int totalClicks_ = 0;
    int clickedCount_ = 0;
    int intervalMs_ = 1000;
    int countdown_ = 0;
    int toggleHotKeyCode_ = VK_F8;
    int pickHotKeyCode_ = VK_F9;
    bool rightButton_ = false;
    bool running_ = false;
    bool toggleHotKeyRegistered_ = false;
    bool pickHotKeyRegistered_ = false;
};

class ProcessRowsDialogBase : public ToolDialogBase {
public:
    ProcessRowsDialogBase(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry, std::wstring title, int width, int height)
        : ToolDialogBase(owner, instance, theme, registry, std::move(title), width, height) {}

protected:
    void ClearRowButtons() {
        for (HWND button : rowButtons_) {
            if (button) {
                DestroyWindow(button);
            }
        }
        rowButtons_.clear();
    }

    void RebuildRowButtons(int baseId) {
        ClearRowButtons();
        const int visibleCount = std::min<int>(static_cast<int>(rows_.size()), VisibleProcessRowCount(theme_, resultsFrame_));
        const int padding = 8;
        const int rowHeight = ThemedControls::ListBoxTwoLineItemHeight(theme_);
        const int buttonWidth = 62;
        const int buttonHeight = ThemedControls::CompactButtonHeight(theme_);
        for (int i = 0; i < visibleCount; ++i) {
            const int rowTop = resultsFrame_.top + padding + i * rowHeight;
            HWND button = MakeUi().Button(
                baseId + i,
                L"结束",
                resultsFrame_.right - padding - buttonWidth,
                rowTop + std::max(0, (rowHeight - buttonHeight) / 2) - 1,
                ThemedButtonRole::Normal,
                ThemedButtonSize::Compact,
                ThemedButtonWidthMode::Fixed,
                buttonWidth);
            if (button) {
                RedrawWindow(button, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            rowButtons_.push_back(button);
        }
        InvalidateRect(hwnd_, &resultsFrame_, TRUE);
    }

    bool ConfirmAndKillRow(std::size_t index, const wchar_t* title) {
        if (index >= rows_.size()) {
            return false;
        }
        const ProcessDisplayRow& row = rows_[index];
        const ProcessInfo info = QueryProcessInfo(row.pid);
        const std::wstring name = info.name.empty() ? L"未知进程" : info.name;
        const std::wstring message = L"确认结束进程 " + name + L" (PID " + std::to_wstring(row.pid) + L")？";
        if (ShowThemedMessageBox(hwnd_, instance_, theme_, message, title, MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            return true;
        }
        const std::wstring error = KillProcessById(row.pid);
        if (!error.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, title, MB_OK | MB_ICONWARNING);
        }
        RefreshAfterKill();
        return true;
    }

    void OnPaint(HDC dc) override {
        DrawProcessRows(theme_, dc, resultsFrame_, rows_, emptyText_, font());
    }

    void OnDestroy() override {
        ClearRowButtons();
    }

    virtual void RefreshAfterKill() = 0;

    RECT resultsFrame_{};
    HWND status_ = nullptr;
    std::wstring emptyText_;
    std::vector<ProcessDisplayRow> rows_;
    std::vector<HWND> rowButtons_;
};

class PortInspectorDialog final : public ProcessRowsDialogBase {
public:
    PortInspectorDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ProcessRowsDialogBase(owner, instance, theme, registry, L"端口占用检查", 620, 380) {}

private:
    void OnCreate() override {
        const DialogLayoutMetrics layout = CompactLayout();
        const int editHeight = ThemedControls::EditFrameHeight(theme_);
        const int labelHeight = ThemedControls::LabelHeight(theme_);
        const int bh = ThemedControls::ButtonHeight(theme_);
        const int labelOffsetY = std::max(0, (editHeight - labelHeight) / 2);
        const int left = layout.contentInsetX;
        const int row0 = layout.contentInsetY;
        const int fieldX = layout.fieldX;
        const int scanW = layout.footerButtonWidth;
        const int fieldW = width_ - fieldX - scanW - layout.controlGapX - left;

        MakeUi().Label(L"端口号", left, row0 + labelOffsetY, layout.labelWidth);
        port_ = CreateEdit(ID_PORT_VALUE, fieldX, row0, fieldW, registry_.GetSetting(L"quattro.builtin.port-inspector", L"port", L""), ES_NUMBER);
        MakeUi().Button(ID_PORT_SCAN, L"扫描(&S)", fieldX + fieldW + layout.controlGapX, row0 + 1, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, scanW, true);

        const int frameTop = row0 + layout.RowStep(bh) + layout.rowGap;
        const int statusY = height_ - layout.contentInsetY - labelHeight;
        resultsFrame_ = RECT{left, frameTop, width_ - left, statusY - layout.rowGap};
        ThemedStatusTextOptions statusOptions{};
        statusOptions.align = ThemedTextAlign::Start;
        status_ = MakeUi().StatusText(L"输入端口号后点击扫描。", left, statusY, width_ - left * 2, statusOptions);
        emptyText_ = L"暂无占用进程";
    }

    bool OnCommand(int id, int) override {
        if (id == ID_PORT_SCAN) {
            Scan();
            return true;
        }
        if (id >= ID_PORT_KILL_BASE && id < ID_PORT_KILL_BASE + 100) {
            return ConfirmAndKillRow(static_cast<std::size_t>(id - ID_PORT_KILL_BASE), L"端口占用检查");
        }
        return false;
    }

    bool OnShortcutKey(const MSG& message) override {
        if (CtrlOnly() && message.wParam == 'S') {
            Scan();
            return true;
        }
        return false;
    }

    void RefreshAfterKill() override {
        Scan();
    }

    void Scan() {
        const std::optional<int> parsedPort = ParseInt(Trim(GetText(port_)));
        if (!parsedPort || *parsedPort <= 0 || *parsedPort > 65535) {
            rows_.clear();
            ClearRowButtons();
            SetText(status_, L"请输入 1-65535 之间的端口号。");
            InvalidateRect(hwnd_, &resultsFrame_, TRUE);
            return;
        }

        const int portValue = *parsedPort;
        registry_.SetSetting(L"quattro.builtin.port-inspector", L"port", std::to_wstring(portValue));
        rows_ = QueryPortRows(static_cast<unsigned short>(portValue));
        RebuildRowButtons(ID_PORT_KILL_BASE);
        if (rows_.empty()) {
            SetText(status_, L"未发现占用进程。");
            return;
        }
        const int visibleCount = VisibleProcessRowCount(theme_, resultsFrame_);
        std::wstring status = L"发现 " + std::to_wstring(rows_.size()) + L" 个占用进程。";
        if (static_cast<int>(rows_.size()) > visibleCount) {
            status += L" 当前显示前 " + std::to_wstring(visibleCount) + L" 个。";
        }
        SetText(status_, status);
    }

    HWND port_ = nullptr;
};

class ProcessInspectorDialog final : public ProcessRowsDialogBase {
public:
    ProcessInspectorDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ProcessRowsDialogBase(owner, instance, theme, registry, L"进程ID查询", 560, 230) {}

private:
    void OnCreate() override {
        const DialogLayoutMetrics layout = CompactLayout();
        const int editHeight = ThemedControls::EditFrameHeight(theme_);
        const int labelHeight = ThemedControls::LabelHeight(theme_);
        const int bh = ThemedControls::ButtonHeight(theme_);
        const int labelOffsetY = std::max(0, (editHeight - labelHeight) / 2);
        const int left = layout.contentInsetX;
        const int row0 = layout.contentInsetY;
        const int fieldX = layout.fieldX;
        const int queryW = layout.footerButtonWidth;
        const int fieldW = width_ - fieldX - queryW - layout.controlGapX - left;

        MakeUi().Label(L"进程ID", left, row0 + labelOffsetY, layout.labelWidth);
        pid_ = CreateEdit(ID_PROCESS_VALUE, fieldX, row0, fieldW, registry_.GetSetting(L"quattro.builtin.process-inspector", L"pid", L""), ES_NUMBER);
        MakeUi().Button(ID_PROCESS_QUERY, L"查询(&Q)", fieldX + fieldW + layout.controlGapX, row0 + 1, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, queryW, true);

        const int frameTop = row0 + layout.RowStep(bh) + layout.rowGap;
        const int statusY = height_ - layout.contentInsetY - labelHeight;
        resultsFrame_ = RECT{left, frameTop, width_ - left, statusY - layout.rowGap};
        ThemedStatusTextOptions statusOptions{};
        statusOptions.align = ThemedTextAlign::Start;
        status_ = MakeUi().StatusText(L"输入进程ID后点击查询。", left, statusY, width_ - left * 2, statusOptions);
        emptyText_ = L"暂无进程条目";
    }

    bool OnCommand(int id, int) override {
        if (id == ID_PROCESS_QUERY) {
            Query();
            return true;
        }
        if (id >= ID_PROCESS_KILL_BASE && id < ID_PROCESS_KILL_BASE + 100) {
            return ConfirmAndKillRow(static_cast<std::size_t>(id - ID_PROCESS_KILL_BASE), L"进程ID查询");
        }
        return false;
    }

    bool OnShortcutKey(const MSG& message) override {
        if (CtrlOnly() && message.wParam == 'Q') {
            Query();
            return true;
        }
        return false;
    }

    void RefreshAfterKill() override {
        Query();
    }

    void Query() {
        const std::optional<int> parsedPid = ParseInt(Trim(GetText(pid_)));
        if (!parsedPid || *parsedPid <= 0) {
            rows_.clear();
            ClearRowButtons();
            SetText(status_, L"请输入有效的进程ID。");
            InvalidateRect(hwnd_, &resultsFrame_, TRUE);
            return;
        }

        const int pidValue = *parsedPid;
        registry_.SetSetting(L"quattro.builtin.process-inspector", L"pid", std::to_wstring(pidValue));
        const ProcessInfo info = QueryProcessInfo(static_cast<DWORD>(pidValue));
        rows_.clear();
        if (!info.name.empty() && info.name != L"未知进程") {
            ProcessDisplayRow row{};
            row.pid = static_cast<DWORD>(pidValue);
            row.title = L"PID " + std::to_wstring(pidValue) + L"  " + info.name;
            row.detail = info.path.empty() ? L"进程路径不可读，仍可尝试结束进程" : info.path;
            rows_.push_back(std::move(row));
        }
        RebuildRowButtons(ID_PROCESS_KILL_BASE);
        SetText(status_, rows_.empty() ? L"未找到该进程，或进程已经退出。" : L"找到 1 个进程。");
    }

    HWND pid_ = nullptr;
};

class FileLockInspectorDialog final {
public:
    FileLockInspectorDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : owner_(owner), instance_(instance), theme_(theme), registry_(registry) {}

    bool Run() {
        const std::wstring className = L"QuattroFileLockInspector_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), L"文件占用检查", FileLockInspectorDialog::Proc, this, icon, icon);
        options.clientWidth = width_;
        options.clientHeight = height_;
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(L"文件占用检查窗口创建失败: " + error);
            return false;
        }
        windowUi_->ShowModal();
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return true;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        FileLockInspectorDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<FileLockInspectorDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<FileLockInspectorDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            if (message == WM_DESTROY) {
                done_ = true;
            }
            return commonResult;
        }
        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, kThemedDialogLayoutKind, width_, height_);
            CreateControls();
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wParam));
            return 0;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            ClearRowButtons();
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        const ThemedUi ui = windowUi_->ui();
        const DialogLayoutMetrics& layout = ui.layout();
        const int editHeight = ThemedControls::EditFrameHeight(theme_);
        const int labelHeight = ui.labelHeight();
        const int bh = ui.buttonHeight();
        const int labelOffsetY = std::max(0, (editHeight - labelHeight) / 2);
        const int left = layout.contentInsetX;
        const int row0 = layout.contentInsetY;
        const int fieldX = layout.fieldX;
        const int pickW = ui.splitButtonWidth(
            L"文件", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int scanW = layout.footerButtonWidth;
        const int actionsW = pickW + scanW + layout.controlGapX * 2;
        const int fieldW = width_ - fieldX - actionsW - left;

        ui.Label(L"路径", left, row0 + labelOffsetY, layout.labelWidth);
        pathFrame_ = ui.editFrame(fieldX, row0, fieldW);
        path_ = ui.Edit(ID_FILE_LOCK_PATH, pathFrame_, registry_.GetSetting(L"quattro.builtin.file-lock-inspector", L"path", L""));
        pickSplit_ = ui.SplitButton(
            ID_FILE_LOCK_PICK_FILE,
            ID_FILE_LOCK_PICK_MENU,
            L"文件",
            fieldX + fieldW + layout.controlGapX,
            row0 + 1,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            pickW);
        ui.Button(ID_FILE_LOCK_SCAN, L"检查(&C)", fieldX + fieldW + layout.controlGapX * 2 + pickW, row0 + 1, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, scanW, true);

        const int frameTop = row0 + layout.RowStep(bh) + layout.rowGap;
        const int statusY = height_ - layout.contentInsetY - labelHeight;
        resultsFrame_ = RECT{left, frameTop, width_ - left, statusY - layout.rowGap};
        status_ = ui.StatusText(
            L"输入文件或目录路径后点击检查。",
            left,
            statusY,
            width_ - left * 2,
            ThemedStatusTextOptions{ThemedStatusRole::Normal, ThemedTextAlign::Start});
        emptyText_ = L"暂无占用进程";
    }

    void HandleCommand(int id) {
        if (id == ID_FILE_LOCK_PICK_FILE) {
            PickFile();
            return;
        }
        if (id == ID_FILE_LOCK_PICK_DIR) {
            PickDirectory();
            return;
        }
        if (id == ID_FILE_LOCK_PICK_MENU) {
            const UINT command = ThemedUi::ShowSplitButtonMenu(
                hwnd_,
                pickSplit_.menu,
                {{ID_FILE_LOCK_PICK_DIR, L"选择目录"}});
            if (command != 0) {
                SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
            }
            return;
        }
        if (id == ID_FILE_LOCK_SCAN) {
            Scan();
            return;
        }
        if (id >= ID_FILE_LOCK_KILL_BASE && id < ID_FILE_LOCK_KILL_BASE + 100) {
            ConfirmAndKillRow(static_cast<std::size_t>(id - ID_FILE_LOCK_KILL_BASE));
        }
    }

    void ClearRowButtons() {
        for (HWND button : rowButtons_) {
            if (button) {
                DestroyWindow(button);
            }
        }
        rowButtons_.clear();
    }

    void RebuildRowButtons() {
        ClearRowButtons();
        const ThemedUi ui = windowUi_->ui();
        const int visibleCount = std::min<int>(static_cast<int>(rows_.size()), VisibleProcessRowCount(theme_, resultsFrame_));
        const int padding = 8;
        const int rowHeight = ThemedControls::ListBoxTwoLineItemHeight(theme_);
        const int buttonWidth = 62;
        const int buttonHeight = ThemedControls::CompactButtonHeight(theme_);
        for (int i = 0; i < visibleCount; ++i) {
            const int rowTop = resultsFrame_.top + padding + i * rowHeight;
            HWND button = ui.Button(
                ID_FILE_LOCK_KILL_BASE + i,
                L"结束",
                resultsFrame_.right - padding - buttonWidth,
                rowTop + std::max(0, (rowHeight - buttonHeight) / 2) - 1,
                ThemedButtonRole::Normal,
                ThemedButtonSize::Compact,
                ThemedButtonWidthMode::Fixed,
                buttonWidth);
            rowButtons_.push_back(button);
        }
        InvalidateRect(hwnd_, &resultsFrame_, TRUE);
    }

    void ConfirmAndKillRow(std::size_t index) {
        if (index >= rows_.size()) {
            return;
        }
        const ProcessDisplayRow& row = rows_[index];
        const ProcessInfo info = QueryProcessInfo(row.pid);
        const std::wstring name = info.name.empty() ? L"未知进程" : info.name;
        const std::wstring message = L"确认结束进程 " + name + L" (PID " + std::to_wstring(row.pid) + L")？";
        if (ShowThemedMessageBox(hwnd_, instance_, theme_, message, L"文件占用检查", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            return;
        }
        const std::wstring error = KillProcessById(row.pid);
        if (!error.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"文件占用检查", MB_OK | MB_ICONWARNING);
            return;
        }
        if (index < rowButtons_.size() && rowButtons_[index]) {
            windowUi_->ui().SetEnabled(rowButtons_[index], false);
        }
        SetText(status_, L"进程已结束。");
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        windowUi_->DrawRegisteredEditFrames(dc);
        DrawProcessRows(theme_, dc, resultsFrame_, rows_, emptyText_, windowUi_->font());
        EndPaint(hwnd_, &ps);
    }

    void PickFile() {
        CommonFileDialogOptions options{};
        options.owner = hwnd_;
        options.mode = CommonFileDialogMode::FileOnly;
        options.context = L"文件占用检查文件";
        options.defaultPath = GetText(path_);
        options.legacyFilter = L"所有文件\0*.*\0";
        CommonFileDialogResult result{};
        if (ShowCommonFileDialog(options, result)) {
            SetText(path_, result.path);
        }
    }

    void PickDirectory() {
        CommonFileDialogOptions options{};
        options.owner = hwnd_;
        options.mode = CommonFileDialogMode::FolderOnly;
        options.context = L"文件占用检查目录";
        options.title = L"选择待检查目录";
        options.defaultPath = GetText(path_);
        CommonFileDialogResult result{};
        if (!ShowCommonFileDialog(options, result)) {
            return;
        }
        SetText(path_, result.path);
    }

    void Scan() {
        const std::wstring path = Trim(GetText(path_));
        if (path.empty()) {
            rows_.clear();
            ClearRowButtons();
            SetText(status_, L"请输入文件或目录路径。");
            InvalidateRect(hwnd_, &resultsFrame_, TRUE);
            return;
        }

        registry_.SetSetting(L"quattro.builtin.file-lock-inspector", L"path", path);
        std::wstring detail;
        rows_ = QueryFileLockRows(path, detail);
        RebuildRowButtons();
        if (rows_.empty()) {
            if (detail.starts_with(L"已检查")) {
                SetText(status_, L"未发现占用进程。 " + detail);
            } else {
                SetText(status_, detail.empty() ? L"未发现占用进程。" : detail);
            }
            return;
        }
        const int visibleCount = VisibleProcessRowCount(theme_, resultsFrame_);
        std::wstring status = L"发现 " + std::to_wstring(rows_.size()) + L" 个占用进程。";
        if (!detail.empty()) {
            status += L" " + detail;
        }
        if (static_cast<int>(rows_.size()) > visibleCount) {
            status += L" 当前显示前 " + std::to_wstring(visibleCount) + L" 个。";
        }
        SetText(status_, status);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    HWND hwnd_ = nullptr;
    HWND path_ = nullptr;
    HWND status_ = nullptr;
    ThemedSplitButton pickSplit_{};
    RECT pathFrame_{};
    RECT resultsFrame_{};
    std::wstring emptyText_;
    std::vector<ProcessDisplayRow> rows_;
    std::vector<HWND> rowButtons_;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool done_ = false;
    static constexpr int width_ = 660;
    static constexpr int height_ = 380;
};

class TimerDialog final : public ToolDialogBase {
public:
    TimerDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"计时器", 300, 206) {}

private:
    void OnCreate() override {
        const int fallbackSeconds = ParseInt(registry_.GetSetting(L"quattro.builtin.timer", L"seconds", L"300")).value_or(300);
        const std::wstring hours = registry_.GetSetting(L"quattro.builtin.timer", L"hours", std::to_wstring(fallbackSeconds / 3600));
        const std::wstring minutes = registry_.GetSetting(L"quattro.builtin.timer", L"minutes", std::to_wstring((fallbackSeconds / 60) % 60));
        const std::wstring seconds = registry_.GetSetting(L"quattro.builtin.timer", L"secondsPart", std::to_wstring(fallbackSeconds % 60));

        const DialogLayoutMetrics layout = CompactLayout();
        const ThemedUi timerUi = MakeUi();
        const int editHeight = ThemedControls::EditFrameHeight(theme_);
        const int labelHeight = ThemedControls::LabelHeight(theme_);
        const int labelOffsetY = std::max(0, (editHeight - labelHeight) / 2);
        const int unitLabelW = 22;
        const int unitFieldW = 46;
        const int unitStep = unitLabelW + unitFieldW + layout.controlGapX + layout.labelGap / 2;
        const int unitGroupW = unitStep * 2 + unitLabelW + layout.controlGapX / 2 + unitFieldW;
        const int unitGroupX = layout.CenteredGroupX(width_, unitGroupW);
        const int row0 = layout.contentInsetY;
        const int row1 = row0 + layout.RowStep(ThemedControls::ButtonHeight(theme_));
        const int row2 = row1 + layout.RowStep(ThemedControls::ButtonHeight(theme_));
        timerUi.Label(L"时", unitGroupX, row0 + labelOffsetY, unitLabelW);
        hours_ = CreateEdit(ID_TIMER_HOURS, unitGroupX + unitLabelW + layout.controlGapX / 2, row0, unitFieldW, hours, ES_NUMBER);
        timerUi.Label(L"分", unitGroupX + unitStep, row0 + labelOffsetY, unitLabelW);
        minutes_ = CreateEdit(ID_TIMER_MINUTES, unitGroupX + unitStep + unitLabelW + layout.controlGapX / 2, row0, unitFieldW, minutes, ES_NUMBER);
        timerUi.Label(L"秒", unitGroupX + unitStep * 2, row0 + labelOffsetY, unitLabelW);
        seconds_ = CreateEdit(ID_TIMER_SECONDS, unitGroupX + unitStep * 2 + unitLabelW + layout.controlGapX / 2, row0, unitFieldW, seconds, ES_NUMBER);
        const int checkBoxW = 100;
        const int checkGroupW = checkBoxW * 2 + layout.controlGapX + layout.labelGap;
        const int checkGroupX = layout.CenteredGroupX(width_, checkGroupW);
        ThemedCheckBoxOptions soundOptions{};
        soundOptions.checked = registry_.GetSetting(L"quattro.builtin.timer", L"sound", L"1") != L"0";
        sound_ = timerUi.CheckBox(ID_TIMER_SOUND, L"声音提醒", checkGroupX, row1, checkBoxW, soundOptions);
        ThemedCheckBoxOptions topMostOptions{};
        topMostOptions.checked = registry_.GetSetting(L"quattro.builtin.timer", L"topMost", L"1") != L"0";
        topMost_ = timerUi.CheckBox(
            ID_TIMER_TOPMOST,
            L"置顶提醒",
            checkGroupX + checkBoxW + layout.controlGapX + layout.labelGap,
            row1,
            checkBoxW,
            topMostOptions);
        display_ = timerUi.StatusText(L"00:05:00.000", layout.contentInsetX, row2, width_ - layout.contentInsetX * 2);
        const int bh = ThemedControls::ButtonHeight(theme_);
        const int buttonWidth = layout.footerButtonWidth;
        const int buttonY = row2 + 26 + layout.sectionGap;
        const int buttonsX = layout.CenteredGroupX(width_, buttonWidth * 2 + layout.footerButtonGap);
        start_ = timerUi.Button(ID_TIMER_START, L"开始(&S)", buttonsX, buttonY, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth, true);
        pause_ = timerUi.Button(ID_TIMER_PAUSE, L"暂停(&P)", buttonsX, buttonY, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth);
        reset_ = timerUi.Button(ID_TIMER_RESET, L"重置(&R)", buttonsX + buttonWidth + layout.footerButtonGap, buttonY, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth);
        status_ = timerUi.StatusText(L"", layout.contentInsetX, buttonY + bh + layout.rowGap, width_ - layout.contentInsetX * 2);
        UpdateDisplay(ReadDurationMs());
        UpdateButtons();
    }

    bool OnCommand(int id, int) override {
        if (id == ID_TIMER_START) {
            StartTimer();
            return true;
        }
        if (id == ID_TIMER_PAUSE) {
            PauseTimer();
            return true;
        }
        if (id == ID_TIMER_RESET) {
            ResetTimer();
            return true;
        }
        return false;
    }

    bool OnShortcutKey(const MSG& message) override {
        if (!CtrlOnly()) {
            return false;
        }
        switch (message.wParam) {
        case 'S':
            StartTimer();
            return true;
        case 'P':
            PauseTimer();
            return true;
        case 'R':
            ResetTimer();
            return true;
        default:
            return false;
        }
    }

    bool OnTimer(UINT_PTR id) override {
        if (id != ID_TIMER_TICK) {
            return false;
        }
        remainingMs_ = CurrentRemainingMs();
        UpdateDisplay(remainingMs_);
        FinishIfExpired();
        return true;
    }

    bool OnAutomationTimer(UINT_PTR id) override {
        if (id != ID_TIMER_TICK) {
            return false;
        }
        if (running_) {
            remainingMs_ = CurrentRemainingMs();
            if (remainingMs_ > 1000) {
                remainingMs_ -= 1000;
                finishAt_ = GetTickCount64() + remainingMs_;
                UpdateDisplay(remainingMs_);
            } else {
                remainingMs_ = 0;
                UpdateDisplay(remainingMs_);
                FinishTimer();
            }
        }
        return true;
    }

    void OnDestroy() override {
        KillTimer(hwnd_, ID_TIMER_TICK);
    }

    void StartTimer() {
        if (running_) {
            return;
        }
        if (!paused_ || remainingMs_ <= 0) {
            remainingMs_ = ReadDurationMs();
        }
        SaveSettings();
        running_ = true;
        paused_ = false;
        finishAt_ = GetTickCount64() + remainingMs_;
        SetText(status_, L"");
        SetTimer(hwnd_, ID_TIMER_TICK, kTimerDisplayIntervalMs, nullptr);
        UpdateDisplay(remainingMs_);
        UpdateButtons();
    }

    void PauseTimer() {
        if (running_) {
            remainingMs_ = CurrentRemainingMs();
            running_ = false;
            paused_ = remainingMs_ > 0;
            KillTimer(hwnd_, ID_TIMER_TICK);
            UpdateDisplay(remainingMs_);
            UpdateButtons();
        }
    }

    void ResetTimer() {
        running_ = false;
        paused_ = false;
        KillTimer(hwnd_, ID_TIMER_TICK);
        remainingMs_ = ReadDurationMs();
        finishAt_ = 0;
        SetText(status_, L"");
        UpdateDisplay(remainingMs_);
        UpdateButtons();
    }

    void FinishIfExpired() {
        if (running_ && remainingMs_ <= 0) {
            FinishTimer();
        }
    }

    void FinishTimer() {
        running_ = false;
        paused_ = false;
        remainingMs_ = 0;
        finishAt_ = 0;
        KillTimer(hwnd_, ID_TIMER_TICK);
        UpdateButtons();
        if (SendMessageW(sound_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            MessageBeep(MB_ICONEXCLAMATION);
        }
        SetText(status_, L"计时结束。");
        if (SendMessageW(topMost_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            SetWindowPos(
                hwnd_,
                BackgroundAcceptanceMode() ? HWND_BOTTOM : HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ActivateWindow(hwnd_);
        }
    }

    ULONGLONG CurrentRemainingMs() const {
        if (!running_) {
            return remainingMs_;
        }
        const ULONGLONG now = GetTickCount64();
        return finishAt_ > now ? finishAt_ - now : 0;
    }

    void UpdateDisplay(ULONGLONG milliseconds) {
        remainingMs_ = milliseconds;
        SetText(display_, FormatElapsed(milliseconds));
    }

    void UpdateButtons() {
        if (start_) {
            ShowWindow(start_, running_ ? SW_HIDE : SW_SHOW);
            SetText(start_, paused_ ? L"继续(&S)" : L"开始(&S)");
        }
        if (pause_) {
            ShowWindow(pause_, running_ ? SW_SHOW : SW_HIDE);
        }
        if (reset_) {
            EnableWindow(reset_, TRUE);
        }
    }

    int ReadDurationFields() {
        const int hours = ReadIntField(hours_, 0, 0, 23);
        const int minutes = ReadIntField(minutes_, 5, 0, 59);
        const int seconds = ReadIntField(seconds_, 0, 0, 59);
        return std::max(1, std::min(86400, hours * 3600 + minutes * 60 + seconds));
    }

    ULONGLONG ReadDurationMs() {
        return static_cast<ULONGLONG>(ReadDurationFields()) * 1000;
    }

    void SaveSettings() {
        const int hours = ReadIntField(hours_, 0, 0, 23);
        const int minutes = ReadIntField(minutes_, 5, 0, 59);
        const int seconds = ReadIntField(seconds_, 0, 0, 59);
        registry_.SetSetting(L"quattro.builtin.timer", L"hours", std::to_wstring(hours));
        registry_.SetSetting(L"quattro.builtin.timer", L"minutes", std::to_wstring(minutes));
        registry_.SetSetting(L"quattro.builtin.timer", L"secondsPart", std::to_wstring(seconds));
        registry_.SetSetting(L"quattro.builtin.timer", L"seconds", std::to_wstring(hours * 3600 + minutes * 60 + seconds));
        registry_.SetSetting(L"quattro.builtin.timer", L"sound", SendMessageW(sound_, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
        registry_.SetSetting(L"quattro.builtin.timer", L"topMost", SendMessageW(topMost_, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    }

    HWND hours_ = nullptr;
    HWND minutes_ = nullptr;
    HWND seconds_ = nullptr;
    HWND display_ = nullptr;
    HWND status_ = nullptr;
    HWND sound_ = nullptr;
    HWND topMost_ = nullptr;
    HWND start_ = nullptr;
    HWND pause_ = nullptr;
    HWND reset_ = nullptr;
    ULONGLONG remainingMs_ = 0;
    ULONGLONG finishAt_ = 0;
    bool running_ = false;
    bool paused_ = false;
};

class StopwatchDialog final : public ToolDialogBase {
public:
    StopwatchDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"秒表", 220, 310) {}

private:
    void OnCreate() override {
        const DialogLayoutMetrics layout = CompactLayout();
        const int left = layout.contentInsetX;
        const int contentWidth = width_ - left * 2;
        const int displayY = layout.contentInsetY;
        const int displayHeight = 34;
        display_ = MakeUi().StatusText(L"00:00:00.000", left, displayY, contentWidth);
        const int bh = ThemedControls::ButtonHeight(theme_);
        const int buttonWidth = (contentWidth - layout.controlGapX) / 2;
        const int buttonRow0 = displayY + displayHeight + layout.rowGap;
        const int buttonRow1 = buttonRow0 + bh + layout.rowGap;
        const int buttonRow2 = buttonRow1 + bh + layout.rowGap;
        const int rightButtonX = left + buttonWidth + layout.controlGapX;
        const ThemedUi swUi = MakeUi();
        start_ = swUi.Button(ID_SW_START, L"开始(&S)", left, buttonRow0, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth, true);
        pause_ = swUi.Button(ID_SW_PAUSE, L"暂停(&P)", left, buttonRow0, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth);
        swUi.Button(ID_SW_LAP, L"计次(&L)", rightButtonX, buttonRow0, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth);
        swUi.Button(ID_SW_RESET, L"重置(&R)", left, buttonRow1, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth);
        swUi.Button(ID_SW_COPY, L"复制(&C)", rightButtonX, buttonRow1, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, buttonWidth);
        swUi.Button(ID_SW_EXPORT, L"导出(&E)", left, buttonRow2, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, contentWidth);
        const int lapsTop = buttonRow2 + bh + layout.sectionGap;
        lapsFrame_ = RECT{left, lapsTop, width_ - left, height_ - layout.contentInsetY - layout.sectionGap - layout.rowGap};
        laps_ = swUi.ListBox(
            ID_SW_LAPS,
            lapsFrame_.left + 2,
            lapsFrame_.top + 2,
            lapsFrame_.right - lapsFrame_.left - 4,
            lapsFrame_.bottom - lapsFrame_.top - 4);
        LoadLapHistory();
        UpdateControls();
    }

    void OnPaint(HDC dc) override {
        ThemedControls::DrawListFrame(theme_, dc, lapsFrame_, laps_);
    }

    bool OnCommand(int id, int) override {
        if (id == ID_SW_START) {
            Start();
            return true;
        }
        if (id == ID_SW_PAUSE) {
            Pause();
            return true;
        }
        if (id == ID_SW_RESET) {
            Reset();
            return true;
        }
        if (id == ID_SW_LAP) {
            AddLap();
            return true;
        }
        if (id == ID_SW_COPY) {
            if (CopyTextToClipboard(hwnd_, ExportText())) {
                ShowToast(L"秒表记录已复制到剪贴板。", ThemedToastRole::Success);
            } else {
                ShowToast(L"复制失败，剪贴板被其他程序占用。", ThemedToastRole::Danger);
            }
            return true;
        }
        if (id == ID_SW_EXPORT) {
            ExportToFile();
            return true;
        }
        return false;
    }

    bool OnShortcutKey(const MSG& message) override {
        if (message.wParam == VK_DELETE && GetFocus() == laps_) {
            DeleteSelectedLap();
            return true;
        }
        if (!CtrlOnly()) {
            return false;
        }
        switch (message.wParam) {
        case 'S':
            Start();
            return true;
        case 'P':
            Pause();
            return true;
        case 'R':
            Reset();
            return true;
        case 'L':
            AddLap();
            return true;
        case 'C':
            if (CopyTextToClipboard(hwnd_, ExportText())) {
                ShowToast(L"秒表记录已复制到剪贴板。", ThemedToastRole::Success);
            } else {
                ShowToast(L"复制失败，剪贴板被其他程序占用。", ThemedToastRole::Danger);
            }
            return true;
        case 'E':
            ExportToFile();
            return true;
        default:
            return false;
        }
    }

    bool OnTimer(UINT_PTR id) override {
        if (id == ID_SW_TICK) {
            UpdateDisplay();
            return true;
        }
        return false;
    }

    void OnDestroy() override {
        KillTimer(hwnd_, ID_SW_TICK);
        SaveLapHistory();
    }

    ULONGLONG ElapsedMs() const {
        return accumulatedMs_ + (running_ ? GetTickCount64() - startedAt_ : 0);
    }

    std::wstring CurrentText() const {
        return FormatElapsed(ElapsedMs());
    }

    void Start() {
        if (!running_) {
            running_ = true;
            startedAt_ = GetTickCount64();
            SetTimer(hwnd_, ID_SW_TICK, kTimerDisplayIntervalMs, nullptr);
            UpdateControls();
            UpdateDisplay();
        }
    }

    void Pause() {
        if (running_) {
            accumulatedMs_ += GetTickCount64() - startedAt_;
            running_ = false;
            KillTimer(hwnd_, ID_SW_TICK);
            UpdateControls();
            UpdateDisplay();
        }
    }

    void Reset() {
        running_ = false;
        accumulatedMs_ = 0;
        startedAt_ = 0;
        lastLapMs_ = 0;
        KillTimer(hwnd_, ID_SW_TICK);
        SendMessageW(laps_, LB_RESETCONTENT, 0, 0);
        SaveLapHistory();
        UpdateControls();
        UpdateDisplay();
    }

    void AddLap() {
        const ULONGLONG current = ElapsedMs();
        const ULONGLONG split = current >= lastLapMs_ ? current - lastLapMs_ : current;
        lastLapMs_ = current;
        const std::wstring text = std::to_wstring(static_cast<int>(SendMessageW(laps_, LB_GETCOUNT, 0, 0)) + 1) +
            L". +" + FormatElapsed(split) + L" / " + FormatElapsed(current);
        SendMessageW(laps_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SaveLapHistory();
    }

    void DeleteSelectedLap() {
        const int selected = static_cast<int>(SendMessageW(laps_, LB_GETCURSEL, 0, 0));
        if (selected == LB_ERR) {
            return;
        }
        SendMessageW(laps_, LB_DELETESTRING, static_cast<WPARAM>(selected), 0);
        SaveLapHistory();
    }

    void UpdateDisplay() {
        SetText(display_, CurrentText());
    }

    void UpdateControls() {
        if (start_) {
            ShowWindow(start_, running_ ? SW_HIDE : SW_SHOW);
        }
        if (pause_) {
            ShowWindow(pause_, running_ ? SW_SHOW : SW_HIDE);
        }
    }

    std::wstring ExportText() const {
        std::wstring text = L"当前时间: " + CurrentText();
        const int count = static_cast<int>(SendMessageW(laps_, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            wchar_t buffer[128]{};
            SendMessageW(laps_, LB_GETTEXT, i, reinterpret_cast<LPARAM>(buffer));
            text += L"\r\n";
            text += buffer;
        }
        return text;
    }

    void SaveLapHistory() {
        if (!laps_) {
            return;
        }
        std::wstring history;
        const int count = static_cast<int>(SendMessageW(laps_, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            wchar_t buffer[128]{};
            SendMessageW(laps_, LB_GETTEXT, i, reinterpret_cast<LPARAM>(buffer));
            if (!history.empty()) {
                history += L"\n";
            }
            history += buffer;
        }
        registry_.SetSetting(L"quattro.builtin.stopwatch", L"laps", history);
    }

    void LoadLapHistory() {
        const auto lines = SplitLines(registry_.GetSetting(L"quattro.builtin.stopwatch", L"laps", L""));
        for (const auto& line : lines) {
            SendMessageW(laps_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }
    }

    void ExportToFile() {
        std::wstring path;
        wchar_t testExportPath[32768]{};
        const DWORD testExportLength = GetEnvironmentVariableW(L"QUATTRO_TEST_STOPWATCH_EXPORT", testExportPath, static_cast<DWORD>(_countof(testExportPath)));
        if (testExportLength > 0 && testExportLength < _countof(testExportPath)) {
            path.assign(testExportPath, testExportLength);
        } else {
            std::wstring buffer = L"quattro-stopwatch.txt";
            buffer.resize(32768, L'\0');
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFile = buffer.data();
            ofn.nMaxFile = static_cast<DWORD>(buffer.size());
            ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件\0*.*\0";
            ofn.lpstrDefExt = L"txt";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&ofn)) {
                return;
            }
            path = buffer.c_str();
        }
        std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
        const std::wstring text = ExportText();
        if (!file) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"导出失败。", L"秒表", MB_OK | MB_ICONWARNING);
            return;
        }
        const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), bytes.data(), length, nullptr, nullptr);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file.good()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"导出失败。", L"秒表", MB_OK | MB_ICONWARNING);
            return;
        }
        ShowToast(L"秒表记录已导出。", ThemedToastRole::Success);
    }

    HWND display_ = nullptr;
    HWND laps_ = nullptr;
    RECT lapsFrame_{};
    HWND start_ = nullptr;
    HWND pause_ = nullptr;
    bool running_ = false;
    ULONGLONG startedAt_ = 0;
    ULONGLONG accumulatedMs_ = 0;
    ULONGLONG lastLapMs_ = 0;
};

class ProcessLocatorDialog final {
public:
    ProcessLocatorDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        PluginRegistry& registry,
        const AppConfig& config,
        bool locateOnOpen)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          registry_(registry),
          config_(config),
          locateOnOpen_(locateOnOpen) {}

    bool Run() {
        const std::wstring className = L"QuattroProcessLocator_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), L"进程定位器", ProcessLocatorDialog::Proc, this, icon, icon);
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(L"进程定位器窗口创建失败: " + error);
            return false;
        }
        windowUi_->ShowModal();
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return true;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ProcessLocatorDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ProcessLocatorDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ProcessLocatorDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            if (message == WM_DESTROY) {
                done_ = true;
            }
            return commonResult;
        }
        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, kThemedDialogLayoutKind, kThemedDialogClientWidth, kThemedDialogClientHeight);
            CreateControls();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_LOCATOR_PICK) {
                LocateHoveredProcess();
            } else if (LOWORD(wParam) == ID_LOCATOR_KILL) {
                KillCurrentProcess();
            } else if (LOWORD(wParam) == ID_LOCATOR_OPEN) {
                OpenCurrentDirectory();
            } else if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd_);
            }
            return 0;
        case WM_PAINT:
            PaintFields();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        const ThemedUi ui = windowUi_->ui();
        const DialogLayoutMetrics& layout = ui.layout();
        const int labelHeight = ui.labelHeight();
        const int fieldHeight = ThemedControls::FieldFrameHeight(theme_);
        const int buttonHeight = ui.buttonHeight();
        const int rowHeight = std::max(fieldHeight, buttonHeight);
        const int labelOffsetY = std::max(0, (rowHeight - labelHeight) / 2);
        const int fieldOffsetY = std::max(0, (rowHeight - fieldHeight) / 2);
        const int buttonOffsetY = std::max(0, (rowHeight - buttonHeight) / 2);
        const int left = ui.contentLeft();
        const int actionWidth = ui.buttonWidth(
            L"打开所在目录", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int fieldX = left + layout.labelWidth + layout.labelGap;
        const int fieldWidth = ui.clientWidth() - left - fieldX - layout.controlGapX - actionWidth;
        const int row0 = ui.contentTop();
        const int row1 = ui.nextRowY(row0, rowHeight);
        const int row2 = ui.nextRowY(row1, rowHeight) + layout.sectionGap;

        ui.Label(L"进程 ID", left, row0 + labelOffsetY, layout.labelWidth);
        pidFrame_ = ui.rect(fieldX, row0 + fieldOffsetY, fieldWidth, fieldHeight);
        ThemedEditOptions readOnlyOptions{};
        readOnlyOptions.readOnly = true;
        pidValue_ = ui.Edit(ID_LOCATOR_PID_VALUE, pidFrame_, L"等待获取", readOnlyOptions);
        killButton_ = ui.Button(
            ID_LOCATOR_KILL, L"结束进程", fieldX + fieldWidth + layout.controlGapX, row0 + buttonOffsetY,
            ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, actionWidth);

        ui.Label(L"绝对路径", left, row1 + labelOffsetY, layout.labelWidth);
        pathFrame_ = ui.rect(fieldX, row1 + fieldOffsetY, fieldWidth, fieldHeight);
        pathValue_ = ui.Edit(ID_LOCATOR_PATH_VALUE, pathFrame_, L"等待获取", readOnlyOptions);
        openButton_ = ui.Button(
            ID_LOCATOR_OPEN, L"打开所在目录", fieldX + fieldWidth + layout.controlGapX, row1 + buttonOffsetY,
            ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, actionWidth);

        const int hotKeyWidth = 180;
        const int pickWidth = ui.buttonWidth(
            L"立即获取", ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int groupWidth = layout.labelWidth + layout.labelGap + hotKeyWidth + layout.controlGapX + pickWidth;
        const int groupX = ui.centeredGroupX(groupWidth);
        ui.Label(L"全局快捷键", groupX, row2 + labelOffsetY, layout.labelWidth);
        hotKeyFrame_ = ui.rect(
            groupX + layout.labelWidth + layout.labelGap,
            row2 + fieldOffsetY,
            hotKeyWidth,
            fieldHeight);
        hotKeyText_ = ui.Edit(nextGeneratedControlId_++, hotKeyFrame_, LocatorHotKeyText(), readOnlyOptions);
        ui.Button(
            ID_LOCATOR_PICK, L"立即获取",
            groupX + layout.labelWidth + layout.labelGap + hotKeyWidth + layout.controlGapX,
            row2 + buttonOffsetY,
            ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, pickWidth, true);

        status_ = ui.StatusText(
            LocatorStatusText(),
            left,
            ui.footerButtonY(labelHeight),
            ui.contentWidth());
        UpdateActionButtons();
        if (locateOnOpen_) {
            PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(ID_LOCATOR_PICK, BN_CLICKED), 0);
        }
    }

    void PaintFields() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        windowUi_->DrawRegisteredEditFrames(dc);
        EndPaint(hwnd_, &ps);
    }

    std::wstring LocatorHotKeyText() const {
        if (!config_.globalHotKeysEnabled) {
            return L"已关闭";
        }
        if (config_.processLocatorHotKey == 0) {
            return L"未设置";
        }
        return FormatGlobalHotKeyText(config_.processLocatorHotKey);
    }

    std::wstring LocatorStatusText() const {
        if (!config_.globalHotKeysEnabled) {
            return L"全局快捷键已关闭。";
        }
        if (config_.processLocatorHotKey == 0) {
            return L"将鼠标移到目标程序上，然后点击立即获取。";
        }
        return L"将鼠标移到目标程序上，然后按 " + FormatGlobalHotKeyText(config_.processLocatorHotKey) + L"。";
    }

    void LocateHoveredProcess() {
        const HoveredProcessResult hovered = QueryHoveredProcess(hwnd_);
        if (!hovered.pid) {
            currentPid_ = 0;
            currentPath_.clear();
            SetText(pidValue_, L"获取失败");
            SetText(pathValue_, L"无法获取");
            UpdateActionButtons();
            SetStatus(
                hovered.trayTarget
                    ? L"无法识别该托盘图标所属的进程。"
                    : L"获取进程 ID 失败：" + FormatLastError(hovered.error),
                ThemedStatusRole::Danger);
            return;
        }
        currentPid_ = hovered.pid;
        const ProcessInfo info = QueryProcessInfo(currentPid_);
        currentPath_ = info.path;
        SetText(pidValue_, std::to_wstring(currentPid_));
        SetText(pathValue_, currentPath_.empty() ? L"无法获取" : currentPath_);
        UpdateActionButtons();
        SetStatus(
            currentPath_.empty()
                ? L"已获取进程 ID，但程序路径不可读或权限不足。"
                : (hovered.trayTarget ? L"已识别托盘图标所属进程。" : L"已获取鼠标位置对应的进程信息。"),
            currentPath_.empty() ? ThemedStatusRole::Warning : ThemedStatusRole::Success);
    }

    void KillCurrentProcess() {
        if (!currentPid_) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"当前没有可结束的目标进程。", L"进程定位器", MB_OK | MB_ICONWARNING);
            return;
        }
        const ProcessInfo info = QueryProcessInfo(currentPid_);
        const std::wstring message = L"确认结束进程 " + info.name + L" (PID " + std::to_wstring(currentPid_) + L")？";
        if (ShowThemedMessageBox(hwnd_, instance_, theme_, message, L"进程定位器", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            return;
        }
        const std::wstring error = KillProcessById(currentPid_);
        if (!error.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"进程定位器", MB_OK | MB_ICONWARNING);
            SetStatus(L"结束进程失败，目标可能已退出、受保护或权限不足。", ThemedStatusRole::Danger);
            return;
        }
        SetStatus(L"目标进程已结束。", ThemedStatusRole::Success);
        currentPid_ = 0;
        currentPath_.clear();
        UpdateActionButtons();
    }

    void OpenCurrentDirectory() {
        const std::wstring error = OpenProcessLocation(currentPath_);
        if (!error.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"进程定位器", MB_OK | MB_ICONWARNING);
            SetStatus(L"打开所在目录失败。", ThemedStatusRole::Danger);
            return;
        }
        SetStatus(L"已在资源管理器中定位程序文件。", ThemedStatusRole::Success);
    }

    void SetStatus(const std::wstring& text, ThemedStatusRole role) {
        SetText(status_, text);
        windowUi_->ui().SetStatusTextRole(status_, role);
    }

    void UpdateActionButtons() {
        const ThemedUi ui = windowUi_->ui();
        if (killButton_) {
            ui.SetEnabled(killButton_, currentPid_ != 0);
        }
        if (openButton_) {
            ui.SetEnabled(openButton_, !currentPath_.empty());
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    const AppConfig& config_;
    HWND hwnd_ = nullptr;
    HWND pidValue_ = nullptr;
    HWND pathValue_ = nullptr;
    RECT pidFrame_{};
    RECT pathFrame_{};
    RECT hotKeyFrame_{};
    int nextGeneratedControlId_ = 8500;
    HWND hotKeyText_ = nullptr;
    HWND killButton_ = nullptr;
    HWND openButton_ = nullptr;
    HWND status_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    DWORD currentPid_ = 0;
    std::wstring currentPath_;
    bool locateOnOpen_ = false;
    bool done_ = false;
};

struct FileLockScanState {
    struct Snapshot {
        FileLockQueryProgress progress;
        bool finished = false;
        bool stopRequested = false;
        std::wstring error;
    };

    Snapshot ReadSnapshot() const {
        std::lock_guard lock(mutex);
        return Snapshot{progress, finished, cancelRequested.load(), result.error};
    }

    void UpdateProgress(const FileLockQueryProgress& value) {
        std::lock_guard lock(mutex);
        progress = value;
    }

    void Complete(FileLockQueryResult value) {
        std::lock_guard lock(mutex);
        result = std::move(value);
        finished = true;
    }

    FileLockQueryResult ReadResult() const {
        std::lock_guard lock(mutex);
        return result;
    }

    mutable std::mutex mutex;
    FileLockQueryProgress progress{};
    FileLockQueryResult result{};
    bool finished = false;
    std::atomic_bool cancelRequested{false};
};

FileLockQueryOptions BackgroundFileLockQueryOptions() {
    FileLockQueryOptions options;
    if (!QuattroTestMode()) {
        return options;
    }
    wchar_t delayText[32]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_TEST_FILE_LOCK_BATCH_DELAY_MS",
            delayText,
            static_cast<DWORD>(std::size(delayText))) == 0) {
        return options;
    }
    const std::optional<int> delay = ParseInt(delayText);
    if (delay && *delay > 0) {
        options.batchDelay = std::chrono::milliseconds(std::min(*delay, 1000));
    }
    return options;
}

class FileLockProgressDialog final {
public:
    FileLockProgressDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        std::shared_ptr<FileLockScanState> state)
        : owner_(owner), instance_(instance), theme_(theme), state_(std::move(state)) {}

    ~FileLockProgressDialog() {
        Close();
    }

    bool Show() {
        if (IsWindow(hwnd_)) {
            ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
            ActivateWindow(hwnd_);
            return true;
        }

        const std::wstring className = L"QuattroFileLockProgress_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), kFileLockProgressWindowTitle,
            FileLockProgressDialog::Proc, this, icon, icon);
        options.clientWidth = kClientWidth;
        options.clientHeight = kClientHeight;
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(L"文件占用检查进度窗口创建失败: " + error);
            return false;
        }
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        return true;
    }

    void Close() {
        if (IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }

private:
    static constexpr int kClientWidth = 420;
    static constexpr int kClientHeight = 164;
    static constexpr UINT_PTR kRefreshTimer = 1;

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        FileLockProgressDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<FileLockProgressDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<FileLockProgressDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            if (message == WM_DESTROY) {
                KillTimer(hwnd_, kRefreshTimer);
                hwnd_ = nullptr;
            }
            return commonResult;
        }
        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact, kClientWidth, kClientHeight);
            CreateControls();
            SetTimer(hwnd_, kRefreshTimer, 80, nullptr);
            Refresh();
            return 0;
        case WM_TIMER:
            if (wParam == kRefreshTimer) {
                Refresh();
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_FILE_LOCK_PROGRESS_STOP) {
                state_->cancelRequested.store(true);
                Refresh();
                return 0;
            }
            if (LOWORD(wParam) == ID_FILE_LOCK_PROGRESS_CLOSE || LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd_);
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd_, &paint);
            windowUi_->FillBackground(dc);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kRefreshTimer);
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    void CreateControls() {
        const ThemedUi ui = windowUi_->ui();
        // A progress dialog can be closed and reopened for the same scan.
        // New controls start enabled, so reset the cached state before the
        // first Refresh() applies the current snapshot.
        stopEnabled_ = true;
        const DialogLayoutMetrics& layout = ui.layout();
        const int left = ui.contentLeft();
        int y = layout.contentInsetY;
        ThemedStatusTextOptions statusOptions{};
        statusOptions.role = ThemedStatusRole::Info;
        statusOptions.align = ThemedTextAlign::Center;
        status_ = ui.StatusText(L"正在准备检查…", left, y, ui.contentWidth(), statusOptions);
        y = ui.nextRowY(y, ui.labelHeight());
        detail_ = ui.Label(L"正在读取路径信息。", left, y, ui.contentWidth());
        y += ui.labelHeight() + layout.sectionGap;
        progress_ = ui.ProgressBar(
            ID_FILE_LOCK_PROGRESS_BAR,
            left,
            y,
            ui.contentWidth(),
            ThemedProgressBarOptions{0.0, true, true});
        stop_ = ui.FooterButton(ID_FILE_LOCK_PROGRESS_STOP, L"停止", 0, 2, false, false);
        close_ = ui.FooterButton(ID_FILE_LOCK_PROGRESS_CLOSE, L"关闭", 1, 2, true, true);
    }

    void Refresh() {
        if (!windowUi_ || !state_) {
            return;
        }
        const FileLockScanState::Snapshot snapshot = state_->ReadSnapshot();
        const ThemedUi ui = windowUi_->ui();
        std::wstring status;
        std::wstring detail;
        ThemedStatusRole role = ThemedStatusRole::Info;
        bool indeterminate = false;
        double value = 0.0;

        if (snapshot.finished && !snapshot.error.empty()) {
            status = L"检查失败";
            detail = snapshot.error;
            role = ThemedStatusRole::Danger;
        } else {
            switch (snapshot.progress.phase) {
            case FileLockQueryPhase::Validating:
                status = snapshot.stopRequested ? L"正在停止检查…" : L"正在准备检查…";
                detail = L"正在读取路径信息。";
                role = snapshot.stopRequested ? ThemedStatusRole::Warning : ThemedStatusRole::Info;
                indeterminate = true;
                break;
            case FileLockQueryPhase::Enumerating:
                status = snapshot.stopRequested ? L"正在停止检查…" : L"正在统计目录内容…";
                detail = L"已发现 " + std::to_wstring(snapshot.progress.discoveredPaths) + L" 个路径。";
                role = snapshot.stopRequested ? ThemedStatusRole::Warning : ThemedStatusRole::Info;
                indeterminate = true;
                break;
            case FileLockQueryPhase::Querying:
                status = snapshot.stopRequested ? L"正在停止检查…" : L"正在并行检查目录占用…";
                detail = L"已检查 " + std::to_wstring(snapshot.progress.checkedPaths) + L" / " +
                    std::to_wstring(snapshot.progress.totalPaths) + L" 个路径，" +
                    std::to_wstring(snapshot.progress.workerCount) + L" 个工作线程。";
                role = snapshot.stopRequested ? ThemedStatusRole::Warning : ThemedStatusRole::Info;
                value = snapshot.progress.totalPaths == 0
                    ? 0.0
                    : static_cast<double>(snapshot.progress.checkedPaths) /
                        static_cast<double>(snapshot.progress.totalPaths);
                break;
            case FileLockQueryPhase::ScanningHandles:
                status = snapshot.stopRequested ? L"正在停止检查…" : L"正在检查目录句柄…";
                detail = L"已检查 " + std::to_wstring(snapshot.progress.checkedProcesses) + L" / " +
                    std::to_wstring(snapshot.progress.totalProcesses) + L" 个进程";
                if (snapshot.progress.inaccessibleProcesses > 0) {
                    detail += L"，" + std::to_wstring(snapshot.progress.inaccessibleProcesses) +
                        L" 个高权限进程无法访问";
                }
                detail += L"。";
                role = snapshot.stopRequested ? ThemedStatusRole::Warning : ThemedStatusRole::Info;
                value = snapshot.progress.totalProcesses == 0
                    ? 1.0
                    : static_cast<double>(snapshot.progress.checkedProcesses) /
                        static_cast<double>(snapshot.progress.totalProcesses);
                break;
            case FileLockQueryPhase::Completed:
                status = L"检查完成";
                detail = L"已检查 " + std::to_wstring(snapshot.progress.checkedPaths) + L" 个路径、" +
                    std::to_wstring(snapshot.progress.checkedProcesses) + L" 个进程。";
                role = ThemedStatusRole::Success;
                value = 1.0;
                break;
            case FileLockQueryPhase::Cancelled:
                status = L"检查已停止";
                detail = L"已检查 " + std::to_wstring(snapshot.progress.checkedPaths) + L" / " +
                    std::to_wstring(snapshot.progress.totalPaths) + L" 个路径。";
                role = ThemedStatusRole::Warning;
                value = snapshot.progress.totalPaths == 0
                    ? 0.0
                    : static_cast<double>(snapshot.progress.checkedPaths) /
                        static_cast<double>(snapshot.progress.totalPaths);
                break;
            }
        }

        ui.SetStatusTextRole(status_, role);
        ThemedUi::SetText(status_, status);
        ThemedUi::SetText(detail_, detail);
        ThemedUi::SetProgress(progress_, value, indeterminate);
        const bool stopEnabled = !snapshot.finished && !snapshot.stopRequested;
        // Refresh runs on a short timer. Avoid re-disabling/repainting the
        // button on every tick after a stop request; doing so repeatedly
        // invalidates the owner-draw button and makes its disabled state flash.
        if (stopEnabled_ != stopEnabled) {
            stopEnabled_ = stopEnabled;
            if (!stopEnabled && GetFocus() == stop_) {
                SetFocus(close_);
            }
            ui.SetEnabled(stop_, stopEnabled);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::shared_ptr<FileLockScanState> state_;
    HWND hwnd_ = nullptr;
    HWND status_ = nullptr;
    HWND detail_ = nullptr;
    HWND progress_ = nullptr;
    HWND stop_ = nullptr;
    HWND close_ = nullptr;
    bool stopEnabled_ = true;
    std::unique_ptr<ThemedWindowUi> windowUi_;
};

class ProcessToolsDialog final {
public:
    enum class Page {
        Locator = 0,
        ProcessId = 1,
        Port = 2,
        FileLock = 3,
    };

    ProcessToolsDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        PluginRegistry& registry,
        const AppConfig& config,
        Page initialPage,
        bool locateOnOpen)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          registry_(registry),
          config_(config),
          initialPage_(initialPage),
          locateOnOpen_(locateOnOpen) {}

    bool Run() {
        const HWND existingWindow = gProcessToolsWindow.load();
        if (IsWindow(existingWindow)) {
            PostMessageW(
                existingWindow,
                WM_QUATTRO_PROCESS_TOOLS_ACTIVATE,
                static_cast<WPARAM>(initialPage_),
                locateOnOpen_ ? 1 : 0);
            return true;
        }
        gProcessToolsWindow.store(nullptr);

        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, kProcessToolsWindowClass, L"进程工具", ProcessToolsDialog::Proc, this, icon, icon);
        options.clientWidth = kClientWidth;
        options.clientHeight = kClientHeight;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 60;
        options.offsetY = 60;
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(L"进程工具窗口创建失败: " + error);
            return false;
        }
        gProcessToolsWindow.store(hwnd_);

        windowUi_->ShowModal();
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return true;
    }

private:
    static constexpr int kClientWidth = 680;
    static constexpr int kClientHeight = 440;
    static constexpr int kPageCount = 4;

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ProcessToolsDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ProcessToolsDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ProcessToolsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            if (message == WM_DESTROY) {
                CancelFileLockQueryAndWait();
                if (gProcessToolsWindow.load() == hwnd_) {
                    gProcessToolsWindow.store(nullptr);
                }
                done_ = true;
            }
            return commonResult;
        }

        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact, kClientWidth, kClientHeight);
            CreateControls();
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_NOTIFY:
            if (HandleTableEvent(lParam)) {
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_QUATTRO_FILE_LOCK_COMPLETE:
            FinishDirectoryFileLockQuery();
            return 0;
        case WM_QUATTRO_PROCESS_TOOLS_ACTIVATE: {
            const int requestedPage = std::clamp(static_cast<int>(wParam), 0, kPageCount - 1);
            ShowPage(static_cast<Page>(requestedPage));
            if (IsIconic(hwnd_)) {
                ShowWindowRespectFocusPolicy(hwnd_, SW_RESTORE);
            } else {
                ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
            }
            ActivateWindow(hwnd_);
            if (lParam != 0) {
                PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(ID_LOCATOR_PICK, BN_CLICKED), 0);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredEditFrames(dc);
            windowUi_->DrawRegisteredTableFrames(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_CLOSE:
            CancelFileLockQueryAndWait();
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    ThemedUi Ui() const {
        return windowUi_->ui();
    }

    void AddPageControl(Page page, HWND control) {
        if (!control) {
            return;
        }
        pageControls_[static_cast<std::size_t>(page)].push_back(control);
    }

    HWND AddLabel(Page page, const std::wstring& text, int x, int y, int width, ThemedLabelOptions options = {}) {
        HWND control = Ui().Label(text, x, y, width, options);
        AddPageControl(page, control);
        return control;
    }

    HWND AddStatus(Page page, const std::wstring& text, int x, int y, int width) {
        ThemedStatusTextOptions options{};
        options.align = ThemedTextAlign::Start;
        HWND control = Ui().StatusText(text, x, y, width, options);
        AddPageControl(page, control);
        return control;
    }

    HWND AddButton(
        Page page,
        int id,
        const std::wstring& text,
        int x,
        int y,
        ThemedButtonRole role,
        ThemedButtonSize size,
        ThemedButtonWidthMode widthMode,
        int width,
        bool defaultButton = false) {
        HWND control = Ui().Button(id, text, x, y, role, size, widthMode, width, defaultButton);
        AddPageControl(page, control);
        return control;
    }

    ThemedSplitButton AddSplitButton(
        Page page,
        int primaryId,
        int menuId,
        const std::wstring& text,
        int x,
        int y,
        ThemedButtonRole role,
        ThemedButtonSize size,
        ThemedButtonWidthMode widthMode,
        int width,
        bool defaultButton = false) {
        ThemedSplitButton split = Ui().SplitButton(primaryId, menuId, text, x, y, role, size, widthMode, width, defaultButton);
        AddPageControl(page, split.primary);
        AddPageControl(page, split.menu);
        return split;
    }

    HWND AddEdit(Page page, int id, RECT frame, const std::wstring& value, ThemedEditOptions options = {}) {
        HWND control = Ui().Edit(id, frame, value, options);
        AddPageControl(page, control);
        return control;
    }

    HWND AddProcessTable(Page page, int id, RECT frame) {
        const ThemedUi ui = Ui();
        const int nameWidth = ui.tableColumnWidth({L"进程名称", L"Application.exe"});
        const int pidWidth = ui.tableColumnWidth({L"PID", L"999999"});
        const int actionWidth = ui.tableColumnWidth({L"操作", L"结束进程"});
        std::vector<ThemedTableColumn> columns{
            ThemedTableColumn{L"name", L"进程名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, nameWidth},
            ThemedTableColumn{L"pid", L"PID", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, pidWidth},
            ThemedTableColumn{L"path", L"程序路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
            ThemedTableColumn{L"action", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, actionWidth},
        };
        ThemedTableOptions options{};
        options.selection = ThemedTableSelection::Single;
        options.reserveScrollBarGutter = true;
        HWND table = ui.Table(id, frame, columns, options);
        AddPageControl(page, table);
        return table;
    }

    void CreateControls() {
        const ThemedUi ui = Ui();
        const int left = ui.contentLeft();
        const int right = ui.clientWidth() - left;
        RECT tabBounds{left, ui.contentTop(), right, ui.contentTop() + ui.tabButtonHeight()};
        tabStripRect_ = ui.tabStripRect(tabBounds);

        const std::vector<ThemedTabItem> tabs{
            ThemedTabItem{ID_PROCESS_TOOLS_TAB_BASE, L"进程定位", true},
            ThemedTabItem{ID_PROCESS_TOOLS_TAB_BASE + 1, L"PID 查询", true},
            ThemedTabItem{ID_PROCESS_TOOLS_TAB_BASE + 2, L"端口占用", true},
            ThemedTabItem{ID_PROCESS_TOOLS_TAB_BASE + 3, L"文件占用", true},
        };
        ThemedTabControlOptions tabOptions{};
        tabOptions.activeIndex = static_cast<int>(initialPage_);
        tabOptions.equalWidth = true;
        tabOptions.appearance = ThemedTabControlAppearance::EmphasizedSegmented;
        tabOptions.orientation = ThemedTabControlOrientation::Horizontal;
        tabs_ = ui.TabControl(ID_PROCESS_TOOLS_TAB, tabStripRect_, tabs, tabOptions);

        const int pageTop = ui.tabPageTop(tabStripRect_);
        CreateLocatorPage(pageTop);
        CreateProcessIdPage(pageTop);
        CreatePortPage(pageTop);
        CreateFileLockPage(pageTop);

        for (int page = 0; page < kPageCount; ++page) {
            ThemedUi::BindTabPage(tabs_, page, pageControls_[static_cast<std::size_t>(page)]);
        }
        ShowPage(initialPage_);
        if (initialPage_ == Page::Locator && locateOnOpen_) {
            PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(ID_LOCATOR_PICK, BN_CLICKED), 0);
        }
    }

    int StatusY() const {
        const ThemedUi ui = Ui();
        return ui.clientHeight() - ui.layout().contentInsetY - ui.labelHeight();
    }

    RECT ResultsFrame(int top) const {
        const ThemedUi ui = Ui();
        const DialogLayoutMetrics& layout = ui.layout();
        return RECT{
            ui.contentLeft(),
            top,
            ui.clientWidth() - ui.contentLeft(),
            StatusY() - layout.rowGap,
        };
    }

    void CreateLocatorPage(int pageTop) {
        const Page page = Page::Locator;
        const ThemedUi ui = Ui();
        const DialogLayoutMetrics& layout = ui.layout();
        const int labelHeight = ui.labelHeight();
        const int fieldHeight = ui.editHeight();
        const int buttonHeight = ui.buttonHeight();
        const int rowHeight = std::max(fieldHeight, buttonHeight);
        const int labelOffsetY = std::max(0, (rowHeight - labelHeight) / 2);
        const int fieldOffsetY = std::max(0, (rowHeight - fieldHeight) / 2);
        const int buttonOffsetY = std::max(0, (rowHeight - buttonHeight) / 2);
        const int left = ui.contentLeft();
        const int contentWidth = ui.contentWidth();

        AddLabel(page, L"将鼠标移到目标窗口或托盘图标上，然后获取进程信息。", left, pageTop, contentWidth);

        const int actionRowY = pageTop + labelHeight + layout.sectionGap;
        const int hotKeyWidth = ui.scale(180);
        const int pickWidth = ui.buttonWidth(
            L"立即获取", ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int actionGroupWidth = layout.labelWidth + layout.labelGap + hotKeyWidth + layout.controlGapX + pickWidth;
        const int actionGroupX = ui.centeredGroupX(actionGroupWidth);
        AddLabel(page, L"全局快捷键", actionGroupX, actionRowY + labelOffsetY, layout.labelWidth);
        ThemedEditOptions readOnlyOptions{};
        readOnlyOptions.readOnly = true;
        locatorHotKey_ = AddEdit(
            page,
            nextGeneratedControlId_++,
            ui.rect(actionGroupX + layout.labelWidth + layout.labelGap, actionRowY + fieldOffsetY, hotKeyWidth, fieldHeight),
            LocatorHotKeyText(),
            readOnlyOptions);
        AddButton(
            page,
            ID_LOCATOR_PICK,
            L"立即获取",
            actionGroupX + layout.labelWidth + layout.labelGap + hotKeyWidth + layout.controlGapX,
            actionRowY + buttonOffsetY,
            ThemedButtonRole::Primary,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            pickWidth,
            true);

        const int detailsTop = actionRowY + rowHeight + layout.sectionGap;
        const int actionWidth = ui.buttonWidth(
            L"打开所在目录", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int fieldX = left + layout.labelWidth + layout.labelGap;
        const int fieldWidth = ui.clientWidth() - left - fieldX - layout.controlGapX - actionWidth;

        AddLabel(page, L"进程 ID", left, detailsTop + labelOffsetY, layout.labelWidth);
        locatorPid_ = AddEdit(
            page,
            ID_LOCATOR_PID_VALUE,
            ui.rect(fieldX, detailsTop + fieldOffsetY, fieldWidth, fieldHeight),
            L"等待获取",
            readOnlyOptions);
        locatorKill_ = AddButton(
            page,
            ID_LOCATOR_KILL,
            L"结束进程",
            fieldX + fieldWidth + layout.controlGapX,
            detailsTop + buttonOffsetY,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            actionWidth);

        const int pathRowY = detailsTop + rowHeight + layout.rowGap;
        AddLabel(page, L"绝对路径", left, pathRowY + labelOffsetY, layout.labelWidth);
        locatorPath_ = AddEdit(
            page,
            ID_LOCATOR_PATH_VALUE,
            ui.rect(fieldX, pathRowY + fieldOffsetY, fieldWidth, fieldHeight),
            L"等待获取",
            readOnlyOptions);
        locatorOpen_ = AddButton(
            page,
            ID_LOCATOR_OPEN,
            L"打开所在目录",
            fieldX + fieldWidth + layout.controlGapX,
            pathRowY + buttonOffsetY,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            actionWidth);

        locatorStatus_ = AddStatus(page, LocatorStatusText(), left, StatusY(), contentWidth);
        UpdateLocatorButtons();
    }

    void CreateProcessIdPage(int pageTop) {
        const Page page = Page::ProcessId;
        const ThemedUi ui = Ui();
        const DialogLayoutMetrics& layout = ui.layout();
        const int left = ui.contentLeft();
        const int fieldX = left + layout.labelWidth + layout.labelGap;
        const int buttonWidth = ui.buttonWidth(
            L"查询(&Q)", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int fieldWidth = ui.clientWidth() - left - fieldX - layout.controlGapX - buttonWidth;
        const int rowHeight = std::max(ui.editHeight(), ui.buttonHeight());
        const int labelOffsetY = std::max(0, (rowHeight - ui.labelHeight()) / 2);
        const int fieldOffsetY = std::max(0, (rowHeight - ui.editHeight()) / 2);
        const int buttonOffsetY = std::max(0, (rowHeight - ui.buttonHeight()) / 2);

        AddLabel(page, L"进程 ID", left, pageTop + labelOffsetY, layout.labelWidth);
        ThemedEditOptions options{};
        options.content = ThemedEditContent::Integer;
        processIdInput_ = AddEdit(
            page,
            ID_PROCESS_VALUE,
            ui.rect(fieldX, pageTop + fieldOffsetY, fieldWidth, ui.editHeight()),
            registry_.GetSetting(
                L"quattro.builtin.process-tools",
                L"pid",
                registry_.GetSetting(L"quattro.builtin.process-inspector", L"pid", L"")),
            options);
        AddButton(
            page,
            ID_PROCESS_QUERY,
            L"查询(&Q)",
            fieldX + fieldWidth + layout.controlGapX,
            pageTop + buttonOffsetY,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            buttonWidth,
            true);

        processIdTable_ = AddProcessTable(
            page,
            ID_PROCESS_TOOLS_PID_TABLE,
            ResultsFrame(pageTop + rowHeight + layout.sectionGap));
        processIdStatus_ = AddStatus(page, L"输入进程 ID 后点击查询。", left, StatusY(), ui.contentWidth());
    }

    void CreatePortPage(int pageTop) {
        const Page page = Page::Port;
        const ThemedUi ui = Ui();
        const DialogLayoutMetrics& layout = ui.layout();
        const int left = ui.contentLeft();
        const int fieldX = left + layout.labelWidth + layout.labelGap;
        const int buttonWidth = ui.buttonWidth(
            L"检查(&C)", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int fieldWidth = ui.clientWidth() - left - fieldX - layout.controlGapX - buttonWidth;
        const int rowHeight = std::max(ui.editHeight(), ui.buttonHeight());
        const int labelOffsetY = std::max(0, (rowHeight - ui.labelHeight()) / 2);
        const int fieldOffsetY = std::max(0, (rowHeight - ui.editHeight()) / 2);
        const int buttonOffsetY = std::max(0, (rowHeight - ui.buttonHeight()) / 2);

        AddLabel(page, L"端口号", left, pageTop + labelOffsetY, layout.labelWidth);
        ThemedEditOptions options{};
        options.content = ThemedEditContent::Integer;
        portInput_ = AddEdit(
            page,
            ID_PORT_VALUE,
            ui.rect(fieldX, pageTop + fieldOffsetY, fieldWidth, ui.editHeight()),
            registry_.GetSetting(
                L"quattro.builtin.process-tools",
                L"port",
                registry_.GetSetting(L"quattro.builtin.port-inspector", L"port", L"")),
            options);
        AddButton(
            page,
            ID_PORT_SCAN,
            L"检查(&C)",
            fieldX + fieldWidth + layout.controlGapX,
            pageTop + buttonOffsetY,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            buttonWidth,
            true);

        portTable_ = AddProcessTable(
            page,
            ID_PROCESS_TOOLS_PORT_TABLE,
            ResultsFrame(pageTop + rowHeight + layout.sectionGap));
        portStatus_ = AddStatus(page, L"输入端口号后点击检查。", left, StatusY(), ui.contentWidth());
    }

    void CreateFileLockPage(int pageTop) {
        const Page page = Page::FileLock;
        const ThemedUi ui = Ui();
        const DialogLayoutMetrics& layout = ui.layout();
        const int left = ui.contentLeft();
        const int fieldX = left + layout.labelWidth + layout.labelGap;
        const int pickWidth = ui.splitButtonWidth(
            L"文件", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int checkWidth = ui.buttonWidth(
            L"检查(&C)", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int actionWidth = pickWidth + checkWidth + layout.controlGapX * 2;
        const int fieldWidth = ui.clientWidth() - left - fieldX - actionWidth;
        const int rowHeight = std::max(ui.editHeight(), ui.buttonHeight());
        const int labelOffsetY = std::max(0, (rowHeight - ui.labelHeight()) / 2);
        const int fieldOffsetY = std::max(0, (rowHeight - ui.editHeight()) / 2);
        const int buttonOffsetY = std::max(0, (rowHeight - ui.buttonHeight()) / 2);

        AddLabel(page, L"路径", left, pageTop + labelOffsetY, layout.labelWidth);
        filePathInput_ = AddEdit(
            page,
            ID_FILE_LOCK_PATH,
            ui.rect(fieldX, pageTop + fieldOffsetY, fieldWidth, ui.editHeight()),
            registry_.GetSetting(
                L"quattro.builtin.process-tools",
                L"path",
                registry_.GetSetting(L"quattro.builtin.file-lock-inspector", L"path", L"")));
        int buttonX = fieldX + fieldWidth + layout.controlGapX;
        filePickSplit_ = AddSplitButton(
            page,
            ID_FILE_LOCK_PICK_FILE,
            ID_FILE_LOCK_PICK_MENU,
            L"文件",
            buttonX,
            pageTop + buttonOffsetY,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            pickWidth);
        buttonX += pickWidth + layout.controlGapX;
        AddButton(
            page, ID_FILE_LOCK_SCAN, L"检查(&C)", buttonX, pageTop + buttonOffsetY,
            ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, checkWidth, true);

        const int killAllWidth = ui.buttonWidth(
            L"结束全部", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int killAllY = StatusY() - layout.rowGap - ui.buttonHeight();
        RECT tableFrame = ResultsFrame(pageTop + rowHeight + layout.sectionGap);
        tableFrame.bottom = killAllY - layout.rowGap;
        fileTable_ = AddProcessTable(
            page,
            ID_PROCESS_TOOLS_FILE_TABLE,
            tableFrame);
        fileKillAll_ = AddButton(
            page,
            ID_FILE_LOCK_KILL_ALL,
            L"结束全部",
            ui.clientWidth() - left - killAllWidth,
            killAllY,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            killAllWidth);
        fileStatus_ = AddStatus(page, L"输入文件或目录路径后点击检查。", left, StatusY(), ui.contentWidth());
        UpdateFileKillAllButton();
    }

    void ShowPage(Page page) {
        const int index = static_cast<int>(page);
        if (index < 0 || index >= kPageCount) {
            return;
        }
        ThemedUi::SetActiveTab(tabs_, index, false);
        registry_.SetSetting(L"quattro.builtin.process-tools", L"active-tab", std::to_wstring(index));
        RECT content{};
        GetClientRect(hwnd_, &content);
        content.top = tabStripRect_.bottom;
        RedrawWindow(hwnd_, &content, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void HandleCommand(int id, int notify) {
        if (id == ID_PROCESS_TOOLS_TAB && notify == CBN_SELCHANGE) {
            ShowPage(static_cast<Page>(ThemedUi::ActiveTab(tabs_)));
            return;
        }
        switch (id) {
        case ID_LOCATOR_PICK:
            LocateHoveredProcess();
            break;
        case ID_LOCATOR_KILL:
            KillLocatedProcess();
            break;
        case ID_LOCATOR_OPEN:
            OpenLocatedProcessDirectory();
            break;
        case ID_PROCESS_QUERY:
            QueryProcessId();
            break;
        case ID_PORT_SCAN:
            QueryPort();
            break;
        case ID_FILE_LOCK_PICK_FILE:
            PickFile();
            break;
        case ID_FILE_LOCK_PICK_DIR:
            PickDirectory();
            break;
        case ID_FILE_LOCK_PICK_MENU: {
            const UINT command = ThemedUi::ShowSplitButtonMenu(
                hwnd_,
                filePickSplit_.menu,
                {{ID_FILE_LOCK_PICK_DIR, L"选择目录"}});
            if (command != 0) {
                SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
            }
            break;
        }
        case ID_FILE_LOCK_SCAN:
            QueryFileLock();
            break;
        case ID_FILE_LOCK_KILL_ALL:
            KillAllFileLockProcesses();
            break;
        case IDCANCEL:
            DestroyWindow(hwnd_);
            break;
        default:
            break;
        }
    }

    bool HandleTableEvent(LPARAM lParam) {
        ThemedTableEvent event{};
        if (processIdTable_ && ThemedUi::DecodeTableEvent(processIdTable_, lParam, event)) {
            if (event.kind == ThemedTableEventKind::ActionInvoked) {
                if (ConfirmAndKill(static_cast<DWORD>(event.rowKey), L"PID 查询")) {
                    QueryProcessId();
                }
            }
            return true;
        }
        if (portTable_ && ThemedUi::DecodeTableEvent(portTable_, lParam, event)) {
            if (event.kind == ThemedTableEventKind::ActionInvoked) {
                if (ConfirmAndKill(static_cast<DWORD>(event.rowKey), L"端口占用")) {
                    QueryPort();
                }
            }
            return true;
        }
        if (fileTable_ && ThemedUi::DecodeTableEvent(fileTable_, lParam, event)) {
            if (event.kind == ThemedTableEventKind::ActionInvoked) {
                const DWORD pid = static_cast<DWORD>(event.rowKey);
                if (fileProtectedPids_.find(pid) != fileProtectedPids_.end()) {
                    SetStatus(fileStatus_, L"系统关键进程已保护，不能结束。", ThemedStatusRole::Warning);
                } else if (ConfirmAndKill(pid, L"文件占用")) {
                    fileTerminatedPids_.insert(pid);
                    SetFileProcessRows();
                    SetStatus(fileStatus_, L"进程已结束。", ThemedStatusRole::Success);
                }
            }
            return true;
        }
        return false;
    }

    void SetStatus(HWND status, const std::wstring& text, ThemedStatusRole role = ThemedStatusRole::Normal) {
        SetText(status, text);
        Ui().SetStatusTextRole(status, role);
    }

    void SetProcessRows(
        HWND table,
        const std::vector<ProcessDisplayRow>& rows,
        const std::set<DWORD>& disabledPids = {},
        const std::set<DWORD>& protectedPids = {}) {
        std::vector<ThemedTableRow> tableRows;
        tableRows.reserve(rows.size());
        for (const auto& row : rows) {
            const ProcessInfo info = QueryProcessInfo(row.pid);
            const std::wstring name = info.name.empty() ? L"未知进程" : info.name;
            std::wstring path = info.path.empty() ? row.detail : info.path;
            if (protectedPids.find(row.pid) != protectedPids.end()) {
                path += L"（系统关键进程，已保护）";
            }
            tableRows.push_back(ThemedTableRow{
                static_cast<std::intptr_t>(row.pid),
                {
                    ThemedTableCell{name},
                    ThemedTableCell{std::to_wstring(row.pid)},
                    ThemedTableCell{path},
                    ThemedTableCell{L"结束", -1, ThemedTableCellRole::DestructiveAction, 1},
                },
                false,
                disabledPids.find(row.pid) == disabledPids.end(),
            });
        }
        ThemedUi::SetTableRows(table, tableRows);
    }

    void SetFileProcessRows() {
        std::set<DWORD> disabledPids = fileTerminatedPids_;
        disabledPids.insert(fileProtectedPids_.begin(), fileProtectedPids_.end());
        SetProcessRows(fileTable_, fileRows_, disabledPids, fileProtectedPids_);
        UpdateFileKillAllButton();
    }

    void RefreshFileProtectedPids() {
        fileProtectedPids_.clear();
        for (const auto& row : fileRows_) {
            if (!row.pid || fileProtectedPids_.find(row.pid) != fileProtectedPids_.end()) {
                continue;
            }
            const ProcessInfo info = QueryProcessInfo(row.pid);
            if (IsBatchKillProtectedSystemProcess(row.pid, info)) {
                fileProtectedPids_.insert(row.pid);
            }
        }
    }

    bool HasFileLockKillCandidate() const {
        for (const auto& row : fileRows_) {
            if (fileTerminatedPids_.find(row.pid) == fileTerminatedPids_.end() &&
                fileProtectedPids_.find(row.pid) == fileProtectedPids_.end()) {
                return true;
            }
        }
        return false;
    }

    void UpdateFileKillAllButton() {
        if (fileKillAll_) {
            Ui().SetEnabled(fileKillAll_, HasFileLockKillCandidate());
        }
    }

    bool ConfirmAndKill(DWORD pid, const wchar_t* title) {
        if (!pid) {
            return false;
        }
        const ProcessInfo info = QueryProcessInfo(pid);
        const std::wstring name = info.name.empty() ? L"未知进程" : info.name;
        const std::wstring message = L"确认结束进程 " + name + L" (PID " + std::to_wstring(pid) + L")？";
        if (ShowThemedMessageBox(hwnd_, instance_, theme_, message, title, MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            return false;
        }
        const std::wstring error = KillProcessById(pid);
        if (!error.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, title, MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    void KillAllFileLockProcesses() {
        std::map<DWORD, ProcessInfo> candidates;
        std::set<DWORD> skippedProtected = fileProtectedPids_;
        for (const auto& row : fileRows_) {
            if (!row.pid || fileTerminatedPids_.find(row.pid) != fileTerminatedPids_.end()) {
                continue;
            }
            if (candidates.find(row.pid) != candidates.end() ||
                skippedProtected.find(row.pid) != skippedProtected.end()) {
                continue;
            }
            const ProcessInfo info = QueryProcessInfo(row.pid);
            if (IsBatchKillProtectedSystemProcess(row.pid, info)) {
                skippedProtected.insert(row.pid);
                continue;
            }
            candidates.emplace(row.pid, info);
        }

        if (candidates.empty()) {
            SetStatus(
                fileStatus_,
                skippedProtected.empty()
                    ? L"当前结果中没有可结束的进程。"
                    : L"当前结果中没有可结束的非系统关键进程。",
                ThemedStatusRole::Warning);
            UpdateFileKillAllButton();
            return;
        }

        std::wstring message = L"确认结束当前结果中的 " + std::to_wstring(candidates.size()) + L" 个进程？";
        if (!skippedProtected.empty()) {
            message += L"\n已忽略 " + std::to_wstring(skippedProtected.size()) + L" 个系统关键进程。";
        }
        message += L"\n此操作不会自动重新检查。";
        if (ShowThemedMessageBox(hwnd_, instance_, theme_, message, L"文件占用", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            return;
        }

        std::size_t successCount = 0;
        std::size_t failureCount = 0;
        for (const auto& [pid, info] : candidates) {
            const std::wstring error = KillProcessById(pid);
            if (error.empty()) {
                fileTerminatedPids_.insert(pid);
                ++successCount;
            } else {
                ++failureCount;
                WriteAppLog(
                    L"文件占用批量结束进程失败: pid=" + std::to_wstring(pid) +
                    L", name=" + info.name + L", error=" + error);
            }
        }

        SetFileProcessRows();
        std::wstring status = L"已结束 " + std::to_wstring(successCount) + L" 个进程";
        if (failureCount > 0) {
            status += L"，失败 " + std::to_wstring(failureCount) + L" 个";
        }
        if (!skippedProtected.empty()) {
            status += L"，已忽略 " + std::to_wstring(skippedProtected.size()) + L" 个系统关键进程";
        }
        status += L"。";
        SetStatus(fileStatus_, status, failureCount > 0 ? ThemedStatusRole::Warning : ThemedStatusRole::Success);
    }

    std::wstring LocatorHotKeyText() const {
        if (!config_.globalHotKeysEnabled) {
            return L"已关闭";
        }
        if (config_.processLocatorHotKey == 0) {
            return L"未设置";
        }
        return FormatGlobalHotKeyText(config_.processLocatorHotKey);
    }

    std::wstring LocatorStatusText() const {
        if (!config_.globalHotKeysEnabled) {
            return L"全局快捷键已关闭。";
        }
        if (config_.processLocatorHotKey == 0) {
            return L"将鼠标移到目标程序上，然后点击立即获取。";
        }
        return L"将鼠标移到目标程序上，然后按 " + FormatGlobalHotKeyText(config_.processLocatorHotKey) + L"。";
    }

    void LocateHoveredProcess() {
        const HoveredProcessResult hovered = QueryHoveredProcess(hwnd_);
        if (!hovered.pid) {
            locatedPid_ = 0;
            locatedPath_.clear();
            SetText(locatorPid_, L"获取失败");
            SetText(locatorPath_, L"无法获取");
            UpdateLocatorButtons();
            SetStatus(
                locatorStatus_,
                hovered.trayTarget
                    ? L"无法识别该托盘图标所属的进程。"
                    : L"获取进程 ID 失败：" + FormatLastError(hovered.error),
                ThemedStatusRole::Danger);
            return;
        }
        locatedPid_ = hovered.pid;
        const ProcessInfo info = QueryProcessInfo(locatedPid_);
        locatedPath_ = info.path;
        SetText(locatorPid_, std::to_wstring(locatedPid_));
        SetText(locatorPath_, locatedPath_.empty() ? L"无法获取" : locatedPath_);
        UpdateLocatorButtons();
        SetStatus(
            locatorStatus_,
            locatedPath_.empty()
                ? L"已获取进程 ID，但程序路径不可读或权限不足。"
                : (hovered.trayTarget ? L"已识别托盘图标所属进程。" : L"已获取鼠标位置对应的进程信息。"),
            locatedPath_.empty() ? ThemedStatusRole::Warning : ThemedStatusRole::Success);
    }

    void KillLocatedProcess() {
        if (!locatedPid_) {
            return;
        }
        if (!ConfirmAndKill(locatedPid_, L"进程定位")) {
            return;
        }
        locatedPid_ = 0;
        locatedPath_.clear();
        SetText(locatorPid_, L"进程已结束");
        SetText(locatorPath_, L"等待获取");
        UpdateLocatorButtons();
        SetStatus(locatorStatus_, L"目标进程已结束。", ThemedStatusRole::Success);
    }

    void OpenLocatedProcessDirectory() {
        const std::wstring error = OpenProcessLocation(locatedPath_);
        if (!error.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"进程定位", MB_OK | MB_ICONWARNING);
            SetStatus(locatorStatus_, L"打开所在目录失败。", ThemedStatusRole::Danger);
            return;
        }
        SetStatus(locatorStatus_, L"已在资源管理器中定位程序文件。", ThemedStatusRole::Success);
    }

    void UpdateLocatorButtons() {
        if (locatorKill_) {
            Ui().SetEnabled(locatorKill_, locatedPid_ != 0);
        }
        if (locatorOpen_) {
            Ui().SetEnabled(locatorOpen_, !locatedPath_.empty());
        }
    }

    void QueryProcessId() {
        const std::optional<int> parsedPid = ParseInt(Trim(GetText(processIdInput_)));
        if (!parsedPid || *parsedPid <= 0) {
            ThemedUi::ClearTable(processIdTable_);
            SetStatus(processIdStatus_, L"请输入有效的进程 ID。", ThemedStatusRole::Warning);
            return;
        }

        const int pid = *parsedPid;
        registry_.SetSetting(L"quattro.builtin.process-tools", L"pid", std::to_wstring(pid));
        const ProcessInfo info = QueryProcessInfo(static_cast<DWORD>(pid));
        std::vector<ProcessDisplayRow> rows;
        if (!info.name.empty() && info.name != L"未知进程") {
            rows.push_back(ProcessDisplayRow{
                static_cast<DWORD>(pid),
                info.name,
                info.path.empty() ? L"进程路径不可读，仍可尝试结束进程" : info.path,
            });
        }
        SetProcessRows(processIdTable_, rows);
        SetStatus(
            processIdStatus_,
            rows.empty() ? L"未找到该进程，或进程已经退出。" : L"找到 1 个进程。",
            rows.empty() ? ThemedStatusRole::Warning : ThemedStatusRole::Success);
    }

    void QueryPort() {
        const std::optional<int> parsedPort = ParseInt(Trim(GetText(portInput_)));
        if (!parsedPort || *parsedPort <= 0 || *parsedPort > 65535) {
            ThemedUi::ClearTable(portTable_);
            SetStatus(portStatus_, L"请输入 1–65535 之间的端口号。", ThemedStatusRole::Warning);
            return;
        }

        const int port = *parsedPort;
        registry_.SetSetting(L"quattro.builtin.process-tools", L"port", std::to_wstring(port));
        const std::vector<ProcessDisplayRow> rows = QueryPortRows(static_cast<unsigned short>(port));
        SetProcessRows(portTable_, rows);
        SetStatus(
            portStatus_,
            rows.empty()
                ? L"未发现占用进程。"
                : L"发现 " + std::to_wstring(rows.size()) + L" 个占用端口 " + std::to_wstring(port) + L" 的进程。",
            rows.empty() ? ThemedStatusRole::Normal : ThemedStatusRole::Success);
    }

    void PickFile() {
        CommonFileDialogOptions options{};
        options.owner = hwnd_;
        options.mode = CommonFileDialogMode::FileOnly;
        options.context = L"进程工具文件占用文件";
        options.defaultPath = GetText(filePathInput_);
        options.legacyFilter = L"所有文件\0*.*\0";
        CommonFileDialogResult result{};
        if (ShowCommonFileDialog(options, result)) {
            SetText(filePathInput_, result.path);
        }
    }

    void PickDirectory() {
        CommonFileDialogOptions options{};
        options.owner = hwnd_;
        options.mode = CommonFileDialogMode::FolderOnly;
        options.context = L"进程工具文件占用目录";
        options.title = L"选择待检查目录";
        options.defaultPath = GetText(filePathInput_);
        CommonFileDialogResult result{};
        if (!ShowCommonFileDialog(options, result)) {
            return;
        }
        SetText(filePathInput_, result.path);
    }

    void QueryFileLock() {
        const std::wstring path = Trim(GetText(filePathInput_));
        if (path.empty()) {
            ThemedUi::ClearTable(fileTable_);
            fileRows_.clear();
            fileTerminatedPids_.clear();
            fileProtectedPids_.clear();
            UpdateFileKillAllButton();
            SetStatus(fileStatus_, L"请输入文件或目录路径。", ThemedStatusRole::Warning);
            return;
        }

        registry_.SetSetting(L"quattro.builtin.process-tools", L"path", path);
        if (fileLockScanState_) {
            const FileLockScanState::Snapshot snapshot = fileLockScanState_->ReadSnapshot();
            if (!snapshot.finished) {
                if (fileLockProgressDialog_) {
                    fileLockProgressDialog_->Show();
                }
                SetStatus(fileStatus_, L"目录检查正在后台运行，可在进度窗口中查看或停止。", ThemedStatusRole::Info);
                return;
            }
            FinishDirectoryFileLockQuery();
        }

        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            StartDirectoryFileLockQuery(path);
            return;
        }

        std::wstring detail;
        const std::vector<ProcessDisplayRow> rows = QueryFileLockRows(path, detail);
        fileRows_ = rows;
        fileTerminatedPids_.clear();
        RefreshFileProtectedPids();
        SetFileProcessRows();
        std::wstring status;
        if (rows.empty()) {
            status = detail.starts_with(L"已检查")
                ? L"未发现占用进程。 " + detail
                : (detail.empty() ? L"未发现占用进程。" : detail);
        } else {
            status = L"发现 " + std::to_wstring(rows.size()) + L" 个占用进程。";
            if (fileProtectedPids_.size() == rows.size()) {
                status = L"发现 " + std::to_wstring(rows.size()) + L" 个占用进程，均为系统关键进程，已保护。";
            } else if (!fileProtectedPids_.empty()) {
                status += L" 其中 " + std::to_wstring(fileProtectedPids_.size()) + L" 个系统关键进程已保护。";
            }
            if (!detail.empty()) {
                status += L" " + detail;
            }
        }
        SetStatus(
            fileStatus_,
            status,
            rows.empty()
                ? ThemedStatusRole::Normal
                : (fileProtectedPids_.size() == rows.size() ? ThemedStatusRole::Warning : ThemedStatusRole::Success));
    }

    void StartDirectoryFileLockQuery(const std::wstring& path) {
        if (fileLockThread_.joinable()) {
            fileLockThread_.join();
        }
        if (fileLockProgressDialog_) {
            fileLockProgressDialog_->Close();
        }

        ThemedUi::ClearTable(fileTable_);
        fileRows_.clear();
        fileTerminatedPids_.clear();
        fileProtectedPids_.clear();
        UpdateFileKillAllButton();
        SetStatus(fileStatus_, L"正在后台检查目录占用…", ThemedStatusRole::Info);
        fileLockScanState_ = std::make_shared<FileLockScanState>();
        fileLockProgressDialog_ = std::make_unique<FileLockProgressDialog>(
            hwnd_, instance_, theme_, fileLockScanState_);
        fileLockProgressDialog_->Show();

        const std::shared_ptr<FileLockScanState> state = fileLockScanState_;
        const HWND notifyWindow = hwnd_;
        const FileLockQueryOptions options = BackgroundFileLockQueryOptions();
        fileLockThread_ = std::thread([state, path, notifyWindow, options]() {
            FileLockQueryResult result = QueryFileLocks(
                path,
                [state]() { return state->cancelRequested.load(); },
                [state](const FileLockQueryProgress& progress) { state->UpdateProgress(progress); },
                options);
            state->Complete(std::move(result));
            PostMessageW(notifyWindow, WM_QUATTRO_FILE_LOCK_COMPLETE, 0, 0);
        });
    }

    void FinishDirectoryFileLockQuery() {
        if (!fileLockScanState_) {
            return;
        }
        const FileLockScanState::Snapshot snapshot = fileLockScanState_->ReadSnapshot();
        if (!snapshot.finished) {
            return;
        }
        if (fileLockThread_.joinable()) {
            fileLockThread_.join();
        }

        const FileLockQueryResult result = fileLockScanState_->ReadResult();
        const std::vector<ProcessDisplayRow> rows = FileLockRowsFromResult(result);
        fileRows_ = rows;
        fileTerminatedPids_.clear();
        RefreshFileProtectedPids();
        SetFileProcessRows();

        if (!result.error.empty()) {
            SetStatus(fileStatus_, result.error, ThemedStatusRole::Danger);
            return;
        }

        std::wstring detail = L"已检查 " + std::to_wstring(result.checkedPaths) + L" / " +
            std::to_wstring(result.totalPaths) + L" 个路径";
        if (result.workerCount > 0) {
            detail += L"，使用 " + std::to_wstring(result.workerCount) + L" 个工作线程";
        }
        if (result.totalProcesses > 0) {
            detail += L"，已检查 " + std::to_wstring(result.checkedProcesses) + L" / " +
                std::to_wstring(result.totalProcesses) + L" 个进程";
        }
        if (!result.warning.empty()) {
            detail += L"，" + result.warning;
        }
        detail += L"。";

        if (result.cancelled) {
            std::wstring status = L"检查已停止。";
            if (!rows.empty()) {
                status += L" 已发现 " + std::to_wstring(rows.size()) + L" 个占用进程。";
                if (fileProtectedPids_.size() == rows.size()) {
                    status += L" 均为系统关键进程，已保护。";
                } else if (!fileProtectedPids_.empty()) {
                    status += L" 其中 " + std::to_wstring(fileProtectedPids_.size()) + L" 个系统关键进程已保护。";
                }
            }
            status += L" " + detail;
            SetStatus(fileStatus_, status, ThemedStatusRole::Warning);
            return;
        }

        std::wstring status = rows.empty()
            ? L"未发现占用进程。 " + detail
            : L"发现 " + std::to_wstring(rows.size()) + L" 个占用进程。";
        if (!rows.empty()) {
            if (fileProtectedPids_.size() == rows.size()) {
                status = L"发现 " + std::to_wstring(rows.size()) + L" 个占用进程，均为系统关键进程，已保护。";
            } else if (!fileProtectedPids_.empty()) {
                status += L" 其中 " + std::to_wstring(fileProtectedPids_.size()) + L" 个系统关键进程已保护。";
            }
            status += L" " + detail;
        }
        SetStatus(
            fileStatus_,
            status,
            !result.warning.empty() || (!rows.empty() && fileProtectedPids_.size() == rows.size())
                ? ThemedStatusRole::Warning
                : (rows.empty() ? ThemedStatusRole::Normal : ThemedStatusRole::Success));
    }

    void CancelFileLockQueryAndWait() {
        if (fileLockScanState_) {
            fileLockScanState_->cancelRequested.store(true);
        }
        if (fileLockThread_.joinable()) {
            fileLockThread_.join();
        }
        if (fileLockProgressDialog_) {
            fileLockProgressDialog_->Close();
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    const AppConfig& config_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND tabs_ = nullptr;
    RECT tabStripRect_{};
    std::vector<HWND> pageControls_[kPageCount];
    Page initialPage_ = Page::Locator;
    bool locateOnOpen_ = false;
    bool done_ = false;
    int nextGeneratedControlId_ = 7780;

    HWND locatorHotKey_ = nullptr;
    HWND locatorPid_ = nullptr;
    HWND locatorPath_ = nullptr;
    HWND locatorKill_ = nullptr;
    HWND locatorOpen_ = nullptr;
    HWND locatorStatus_ = nullptr;
    DWORD locatedPid_ = 0;
    std::wstring locatedPath_;

    HWND processIdInput_ = nullptr;
    HWND processIdTable_ = nullptr;
    HWND processIdStatus_ = nullptr;
    HWND portInput_ = nullptr;
    HWND portTable_ = nullptr;
    HWND portStatus_ = nullptr;
    HWND filePathInput_ = nullptr;
    ThemedSplitButton filePickSplit_{};
    HWND fileTable_ = nullptr;
    HWND fileKillAll_ = nullptr;
    HWND fileStatus_ = nullptr;
    std::vector<ProcessDisplayRow> fileRows_;
    std::set<DWORD> fileTerminatedPids_;
    std::set<DWORD> fileProtectedPids_;
    std::shared_ptr<FileLockScanState> fileLockScanState_;
    std::unique_ptr<FileLockProgressDialog> fileLockProgressDialog_;
    std::thread fileLockThread_;
};
}

bool ShowBuiltinTool(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    PluginRegistry& registry,
    const AppConfig& config,
    const std::wstring& engine,
    bool locateProcessOnOpen) {
    if (engine == L"clicker") {
        ClickerDialog dialog(owner, instance, theme, registry);
        return dialog.Run();
    }
    if (engine == L"timer") {
        TimerDialog dialog(owner, instance, theme, registry);
        return dialog.Run();
    }
    if (engine == L"stopwatch") {
        StopwatchDialog dialog(owner, instance, theme, registry);
        return dialog.Run();
    }
    if (engine == L"process-tools" ||
        engine == L"port-inspector" ||
        engine == L"process-inspector" ||
        engine == L"process-locator" ||
        engine == L"file-lock-inspector") {
        ProcessToolsDialog::Page page = ProcessToolsDialog::Page::Locator;
        if (engine == L"process-tools") {
            const std::optional<int> stored = ParseInt(registry.GetSetting(
                L"quattro.builtin.process-tools", L"active-tab", L"0"));
            const int index = std::clamp(stored.value_or(0), 0, 3);
            page = static_cast<ProcessToolsDialog::Page>(index);
        } else if (engine == L"process-inspector") {
            page = ProcessToolsDialog::Page::ProcessId;
        } else if (engine == L"port-inspector") {
            page = ProcessToolsDialog::Page::Port;
        } else if (engine == L"file-lock-inspector") {
            page = ProcessToolsDialog::Page::FileLock;
        }
        ProcessToolsDialog dialog(owner, instance, theme, registry, config, page, locateProcessOnOpen);
        return dialog.Run();
    }
    ShowThemedMessageBox(owner, instance, theme, L"这个内置工具暂不可用。", L"工具箱", MB_OK | MB_ICONINFORMATION);
    return false;
}
