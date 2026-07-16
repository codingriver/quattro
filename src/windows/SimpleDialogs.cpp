#include "SimpleDialogs.h"

#include "../../resources/resource.h"

#include "AppLog.h"
#include "ConfigPackageService.h"
#include "ContextMenuProviderIconService.h"
#include "DialogLayout.h"
#include "HotKeyEditor.h"
#include "JsonValue.h"
#include "LocalHttpServerService.h"
#include "MainHotKey.h"
#include "ShellContextMenuCacheService.h"
#include "ShellContextMenuRefreshService.h"
#include "ShellItemService.h"
#include "Storage.h"
#include "ThemedControls.h"
#include "ThemedFormLayout.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "TodoSchedule.h"
#include "TrackedContextMenuProviders.h"
#include "Utilities.h"
#include "WebDavBackupService.h"
#include "WebDavCredentialService.h"

#include <commdlg.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <richedit.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr int ID_SETTINGS_TAB_BASE = 280;
constexpr int ID_SETTINGS_TAB_CONTROL = 279;
constexpr int ID_GLOBAL_HOTKEYS_ENABLED = 300;
constexpr int ID_MAIN_HOTKEY_CAPTURE = 301;
constexpr int ID_MAIN_HOTKEY_CLEAR = 302;
constexpr int ID_PROCESS_LOCATOR_HOTKEY_CAPTURE = 303;
constexpr int ID_HOTKEY_TABLE = 304;
constexpr int ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE = 305;
constexpr int ID_GROUP_WIDTH = 401;
constexpr int ID_TAG_WIDTH = 402;
constexpr int ID_DOCK_DELAY = 403;
constexpr int ID_GROUP_DELAY = 404;
constexpr int ID_TAG_DELAY = 405;
constexpr int ID_TAG_ALIGN_LEFT = 407;
constexpr int ID_TAG_ALIGN_CENTER = 408;
constexpr int ID_TAG_ALIGN_RIGHT = 409;
constexpr int ID_WEBDAV_TEST = 410;
constexpr int ID_WEBDAV_CLEAR_PASSWORD = 411;
constexpr int ID_WEBDAV_UPLOAD = 412;
constexpr int ID_WEBDAV_DOWNLOAD = 413;
constexpr int ID_WEBDAV_BACKUP_LIST = 414;
constexpr int ID_CONFIG_EXPORT = 415;
constexpr int ID_CONFIG_IMPORT = 416;
constexpr int ID_TODO_EXPORT = 417;
constexpr int ID_TODO_IMPORT = 418;
constexpr int ID_TODO_INCLUDE_COMPLETED = 419;
constexpr int ID_TODO_INCLUDE_DISABLED = 420;
constexpr int ID_TODO_ONLY_FUTURE = 421;
constexpr int ID_HTTP_START = 422;
constexpr int ID_HTTP_STOP = 423;
constexpr int ID_HTTP_RESTART = 424;
constexpr int ID_HTTP_OPEN_HOME = 425;
constexpr int ID_HTTP_OPEN_CONFIG_DIR = 426;
constexpr int ID_HTTP_BROWSE_ROOT = 427;
constexpr int ID_HTTP_COPY_URL = 428;
constexpr int ID_HTTP_OPEN_ROOT = 429;
constexpr int ID_SETTINGS_APPLY = 430;
constexpr int ID_HTTP_ADDRESS = 431;
constexpr int ID_LOGGING_ENABLED = 432;
// 433-439 与 441-446 由 TrackedContextMenuProviders() 表内的行键占用。
constexpr int ID_RESET_CONTEXT_MENU = 440;
constexpr int ID_REFRESH_CONTEXT_MENU_FROM_NATIVE = 449;
constexpr int ID_CONTEXT_MENU_TABLE = 447;
constexpr int ID_MESSAGE_TEXT = 501;
constexpr int ID_HOTKEY_CONFLICT_IGNORE = 502;
constexpr int ID_MAIN_HOTKEY_PROBE = 0x5148;
constexpr UINT WM_SETTINGS_WEBDAV_DONE = WM_APP + 0x81;
constexpr UINT WM_CONTEXT_MENU_REFRESH_DONE = WM_APP + 0x82;
constexpr UINT WM_CONTEXT_MENU_ICON_LOAD_REQUEST = WM_APP + 0x83;
constexpr UINT WM_CONTEXT_MENU_ICON_LOAD_DONE = WM_APP + 0x84;

enum class SettingsWebDavOperation {
    Test,
    Upload,
    List,
    Download,
};

struct SettingsWebDavResult {
    SettingsWebDavOperation operation = SettingsWebDavOperation::Test;
    bool ok = false;
    std::wstring message;
    WebDavBackupReport report;
    std::vector<WebDavRemoteFile> backups;
    AppConfig config;
};

struct SettingsContextMenuIconAsyncState {
    std::mutex mutex;
    std::stop_source stopSource;
    std::optional<std::vector<ContextMenuProviderIconInfo>> result;
    std::uintptr_t generation = 0;
    std::atomic_bool abandoned{false};
};

std::atomic_uintptr_t gContextMenuIconGeneration{1};

bool IsValidCachedIcon(const ShellContextMenuCachedIcon& icon) {
    return icon.width > 0 && icon.height > 0 && icon.width <= 64 && icon.height <= 64 &&
        icon.pixels.size() == static_cast<std::size_t>(icon.width * icon.height);
}

HICON CreateIconFromCachedPixels(const ShellContextMenuCachedIcon& icon) {
    if (!IsValidCachedIcon(icon)) {
        return nullptr;
    }
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = icon.width;
    bitmapInfo.bmiHeader.biHeight = -icon.height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    std::memcpy(bits, icon.pixels.data(), icon.pixels.size() * sizeof(std::uint32_t));
    const std::size_t maskStride = static_cast<std::size_t>(((icon.width + 15) / 16) * 2);
    std::vector<std::uint8_t> maskBits(maskStride * static_cast<std::size_t>(icon.height), 0);
    HBITMAP mask = CreateBitmap(icon.width, icon.height, 1, 1, maskBits.data());
    if (!mask) {
        DeleteObject(color);
        return nullptr;
    }
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON result = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return result;
}

std::wstring CurrentLanIpv4Address() {
    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW || size == 0) {
        return L"127.0.0.1";
    }
    std::vector<BYTE> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
    if (GetAdaptersInfo(adapters, &size) != NO_ERROR) {
        return L"127.0.0.1";
    }
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->Type == MIB_IF_TYPE_LOOPBACK) {
            continue;
        }
        for (IP_ADDR_STRING* address = &adapter->IpAddressList; address; address = address->Next) {
            const std::string value = address->IpAddress.String ? address->IpAddress.String : "";
            if (value.empty() || value == "0.0.0.0" || value.rfind("127.", 0) == 0) {
                continue;
            }
            std::wstring result;
            result.reserve(value.size());
            for (char ch : value) {
                result.push_back(static_cast<unsigned char>(ch));
            }
            return result;
        }
    }
    return L"127.0.0.1";
}

std::wstring HttpHostForLan(bool lanAccess) {
    return lanAccess ? CurrentLanIpv4Address() : L"127.0.0.1";
}

int ParseHttpPortText(const std::wstring& text, int fallback) {
    std::wstring value = Trim(text);
    if (value.empty()) {
        return fallback;
    }
    const std::size_t scheme = value.find(L"://");
    if (scheme != std::wstring::npos) {
        value = value.substr(scheme + 3);
    }
    const std::size_t slash = value.find_first_of(L"/\\?#");
    if (slash != std::wstring::npos) {
        value = value.substr(0, slash);
    }
    const std::size_t at = value.rfind(L'@');
    if (at != std::wstring::npos) {
        value = value.substr(at + 1);
    }
    const std::size_t colon = value.rfind(L':');
    if (colon != std::wstring::npos) {
        value = value.substr(colon + 1);
    }
    value = Trim(value);
    if (!value.empty() && value.back() == L'/') {
        value.pop_back();
    }
    auto port = ParseInt(value);
    if (!port) {
        return fallback;
    }
    return std::max(1, std::min(65535, *port));
}

std::wstring HttpAddressText(bool lanAccess, int port, bool trailingSlash) {
    std::wstring value = L"http://" + HttpHostForLan(lanAccess) + L":" + std::to_wstring(std::max(1, std::min(65535, port)));
    if (trailingSlash) {
        value += L"/";
    }
    return value;
}

struct HotKeyAvailability {
    bool available = false;
    DWORD error = ERROR_SUCCESS;
    std::wstring reason;
};

std::wstring ReservedMainHotKeyReason(int key) {
    switch (key) {
    case VK_DELETE:
        return L"Ctrl+Alt+Delete 是 Windows 安全按键，不能作为普通全局热键。";
    case VK_TAB:
        return L"Ctrl+Alt+Tab 是 Windows 窗口切换相关按键，建议换一个。";
    default:
        return {};
    }
}

HotKeyAvailability CheckMainHotKeyAvailability(HWND hwnd, int key, int currentRegisteredKey) {
    if (IsDoubleAltMainHotKey(key)) {
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }
    if (key <= 0 || key == currentRegisteredKey) {
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }

    std::wstring reservedReason = ReservedMainHotKeyReason(key);
    if (!reservedReason.empty()) {
        return HotKeyAvailability{false, ERROR_SUCCESS, std::move(reservedReason)};
    }

    SetLastError(ERROR_SUCCESS);
    if (RegisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE, MOD_CONTROL | MOD_ALT, static_cast<UINT>(key))) {
        UnregisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE);
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }

    const DWORD error = GetLastError();
    std::wstring reason = L"该热键无法注册，可能已被系统、输入法或其它软件占用。";
    if (error != ERROR_SUCCESS) {
        reason += L"\n\n系统返回: " + FormatLastError(error);
    }
    return HotKeyAvailability{false, error, std::move(reason)};
}

HotKeyAvailability CheckCtrlAltHotKeyAvailability(HWND hwnd, int key, int currentRegisteredKey) {
    if (key <= 0 || key == currentRegisteredKey) {
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }
    std::wstring reservedReason = ReservedMainHotKeyReason(key);
    if (!reservedReason.empty()) {
        return HotKeyAvailability{false, ERROR_SUCCESS, std::move(reservedReason)};
    }
    SetLastError(ERROR_SUCCESS);
    if (RegisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE, MOD_CONTROL | MOD_ALT, static_cast<UINT>(key))) {
        UnregisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE);
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }
    const DWORD error = GetLastError();
    std::wstring reason = L"该热键无法注册，可能已被系统、输入法或其它软件占用。";
    if (error != ERROR_SUCCESS) {
        reason += L"\n\n系统返回: " + FormatLastError(error);
    }
    return HotKeyAvailability{false, error, std::move(reason)};
}

std::wstring MainHotKeyConflictMessage(int key, const HotKeyAvailability& availability) {
    return FormatMainHotKeyText(key) + L" 不可用。\n\n" + availability.reason;
}

std::wstring MainHotKeyStatusText(int key, const HotKeyAvailability& availability) {
    if (IsDoubleAltMainHotKey(key)) {
        return availability.available ? L"当前热键可用。" : L"热键冲突：双击 Alt 不可用。";
    }
    if (key <= 0) {
        return L"未设置主窗口热键。";
    }
    if (availability.available) {
        return L"当前热键可用。";
    }
    return L"热键冲突：" + FormatMainHotKeyText(key) + L" 可能已被系统、输入法或其它软件占用。";
}

std::wstring ProcessLocatorHotKeyStatusText(int key, const HotKeyAvailability& availability) {
    if (key <= 0) {
        return L"进程定位器快捷键未设置。";
    }
    if (availability.available) {
        return L"当前快捷键可用。";
    }
    return L"进程定位器快捷键 " + FormatGlobalHotKeyText(key) + L" 已被占用。";
}

std::wstring CopySelectedPathsHotKeyStatusText(int key, const HotKeyAvailability& availability) {
    if (key <= 0) {
        return L"复制选中项绝对路径快捷键未设置。";
    }
    if (availability.available) {
        return L"当前快捷键可用。";
    }
    return L"复制选中项绝对路径快捷键 " + FormatGlobalHotKeyText(key) + L" 已被占用。";
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

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

void FillRoundRect(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int borderWidth) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

std::wstring FormatConfigPackageReportText(const ConfigPackageReport& report) {
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

std::wstring FormatFileSize(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::to_wstring((bytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull)) + L" MB";
    }
    if (bytes >= 1024ull) {
        return std::to_wstring((bytes + 1023ull) / 1024ull) + L" KB";
    }
    return std::to_wstring(bytes) + L" B";
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
    return bytes;
}

bool SaveUtf8File(const std::filesystem::path& path, const std::wstring& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    const std::string bytes = WideToUtf8(text);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': escaped += L"\\\\"; break;
        case L'"': escaped += L"\\\""; break;
        case L'\b': escaped += L"\\b"; break;
        case L'\f': escaped += L"\\f"; break;
        case L'\n': escaped += L"\\n"; break;
        case L'\r': escaped += L"\\r"; break;
        case L'\t': escaped += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t buffer[7]{};
                swprintf_s(buffer, L"\\u%04X", static_cast<unsigned int>(ch));
                escaped += buffer;
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }
    return escaped;
}

std::wstring BoolJson(bool value) {
    return value ? L"true" : L"false";
}

std::wstring LocalIsoOffsetText() {
    TIME_ZONE_INFORMATION info{};
    const DWORD state = GetTimeZoneInformation(&info);
    LONG bias = info.Bias;
    if (state == TIME_ZONE_ID_DAYLIGHT) {
        bias += info.DaylightBias;
    } else if (state == TIME_ZONE_ID_STANDARD) {
        bias += info.StandardBias;
    }
    const int offsetMinutes = static_cast<int>(-bias);
    const wchar_t sign = offsetMinutes >= 0 ? L'+' : L'-';
    const int absolute = std::abs(offsetMinutes);
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%c%02d:%02d", sign, absolute / 60, absolute % 60);
    return buffer;
}

std::wstring TodoTimestampToIso8601(const std::wstring& value) {
    SYSTEMTIME time{};
    if (!TryParseTodoTimestamp(value, time)) {
        return {};
    }
    wchar_t buffer[32]{};
    swprintf_s(
        buffer,
        L"%04u-%02u-%02uT%02u:%02u:%02u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
    return std::wstring(buffer) + LocalIsoOffsetText();
}

std::wstring ImportableTodoTimestamp(const std::wstring& value) {
    std::wstring normalized = NormalizeTodoTimestamp(value);
    if (!normalized.empty()) {
        return normalized;
    }

    std::wstring text = ReplaceAll(Trim(value), L"T", L" ");
    if (text.size() >= 19) {
        normalized = NormalizeTodoTimestamp(text.substr(0, 19));
        if (!normalized.empty()) {
            return normalized;
        }
    }
    if (text.size() >= 16) {
        return NormalizeTodoTimestamp(text.substr(0, 16));
    }
    return {};
}

std::wstring CurrentIso8601Timestamp() {
    return TodoTimestampToIso8601(CurrentTodoTimestamp());
}

const JsonValue* ObjectField(const JsonValue& object, const std::wstring& key) {
    const JsonValue* value = object.get(key);
    return value && value->isObject() ? value : nullptr;
}

std::wstring JsonStringField(const JsonValue& object, const std::wstring& key, const std::wstring& fallback = L"") {
    const JsonValue* value = object.get(key);
    return value ? value->stringOr(fallback) : fallback;
}

bool JsonBoolField(const JsonValue& object, const std::wstring& key, bool fallback = false) {
    const JsonValue* value = object.get(key);
    return value ? value->boolOr(fallback) : fallback;
}

int JsonIntField(const JsonValue& object, const std::wstring& key, int fallback = 0) {
    const JsonValue* value = object.get(key);
    return value ? value->intOr(fallback) : fallback;
}

std::wstring JsonStringField(const JsonValue* object, const std::wstring& key, const std::wstring& fallback = L"") {
    return object ? JsonStringField(*object, key, fallback) : fallback;
}

int JsonIntField(const JsonValue* object, const std::wstring& key, int fallback = 0) {
    return object ? JsonIntField(*object, key, fallback) : fallback;
}

bool JsonBoolField(const JsonValue* object, const std::wstring& key, bool fallback = false) {
    return object ? JsonBoolField(*object, key, fallback) : fallback;
}

std::wstring ConfigPackageFileName() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"quattro-%04u%02u%02u-%02u%02u.q4cfg",
        static_cast<unsigned>(local.wYear),
        static_cast<unsigned>(local.wMonth),
        static_cast<unsigned>(local.wDay),
        static_cast<unsigned>(local.wHour),
        static_cast<unsigned>(local.wMinute));
    return buffer;
}

std::wstring TodoJsonFileName() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"quattro-todos-%04u%02u%02u-%02u%02u.json",
        static_cast<unsigned>(local.wYear),
        static_cast<unsigned>(local.wMonth),
        static_cast<unsigned>(local.wDay),
        static_cast<unsigned>(local.wHour),
        static_cast<unsigned>(local.wMinute));
    return buffer;
}

bool SelectSavePath(HWND owner, const std::wstring& initialPath, const wchar_t* filter, const wchar_t* defExt, std::wstring& selectedPath) {
    std::wstring buffer = initialPath;
    buffer.resize(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        return false;
    }
    selectedPath = buffer.c_str();
    return true;
}

bool SelectOpenPath(HWND owner, const wchar_t* filter, const wchar_t* defExt, std::wstring& selectedPath) {
    std::wstring buffer(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }
    selectedPath = buffer.c_str();
    return true;
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
        const std::wstring candidate = baseName + L" " + std::to_wstring(index);
        if (!HasSiblingGroupName(groups, parentGroup, candidate)) {
            return candidate;
        }
    }
    return baseName + L" " + std::to_wstring(static_cast<int>(groups.size()) + 1);
}

bool IsTodoItemsTag(const Group& tag) {
    return tag.type == 4 || ToLower(tag.content) == L"todoitems";
}

const Group* FindGroupById(const std::vector<Group>& groups, int id) {
    for (const auto& group : groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

const Group* FindRootGroupByName(const std::vector<Group>& groups, const std::wstring& groupName) {
    const std::wstring normalized = ToLower(Trim(groupName));
    for (const auto& group : groups) {
        if (group.parentGroup == 0 && ToLower(Trim(group.name)) == normalized) {
            return &group;
        }
    }
    return nullptr;
}

const Group* FindTodoTagByName(const std::vector<Group>& groups, int parentGroupId, const std::wstring& tagName) {
    const std::wstring normalized = ToLower(Trim(tagName));
    for (const auto& group : groups) {
        if (group.parentGroup == parentGroupId && IsTodoItemsTag(group) && ToLower(Trim(group.name)) == normalized) {
            return &group;
        }
    }
    return nullptr;
}

struct TodoExportOptions {
    bool includeCompleted = true;
    bool includeDisabled = true;
    bool onlyFuture = false;
};

bool ShouldExportTodo(const TodoItem& todo, const TodoExportOptions& options) {
    if (!options.includeCompleted && !todo.completedAt.empty()) {
        return false;
    }
    if (!options.includeDisabled && !todo.enabled) {
        return false;
    }
    if (!options.onlyFuture) {
        return true;
    }

    const std::wstring dueAt = todo.nextDueAt.empty() ? todo.anchorAt : todo.nextDueAt;
    SYSTEMTIME due{};
    SYSTEMTIME now{};
    if (!TryParseTodoTimestamp(dueAt, due)) {
        return true;
    }
    if (!TryParseTodoTimestamp(CurrentTodoTimestamp(), now)) {
        return true;
    }
    FILETIME dueFile{};
    FILETIME nowFile{};
    if (!SystemTimeToFileTime(&due, &dueFile) || !SystemTimeToFileTime(&now, &nowFile)) {
        return true;
    }
    return CompareFileTime(&dueFile, &nowFile) >= 0;
}

std::wstring BuildTodoExportJson(const AppModel& model, const TodoExportOptions& options) {
    std::wstringstream out;
    out << L"{\n";
    out << L"  \"app\": \"Quattro\",\n";
    out << L"  \"exportType\": \"todo-backup\",\n";
    out << L"  \"formatVersion\": 2,\n";
    out << L"  \"exportedAt\": \"" << JsonEscape(CurrentIso8601Timestamp()) << L"\",\n";
    out << L"  \"exportOptions\": {\n";
    out << L"    \"includeCompleted\": " << BoolJson(options.includeCompleted) << L",\n";
    out << L"    \"includeDisabled\": " << BoolJson(options.includeDisabled) << L",\n";
    out << L"    \"onlyFuture\": " << BoolJson(options.onlyFuture) << L"\n";
    out << L"  },\n";
    out << L"  \"todos\": [\n";
    bool first = true;
    for (const auto& todo : model.todos) {
        if (!ShouldExportTodo(todo, options)) {
            continue;
        }
        const Group* tag = FindGroupById(model.groups, todo.tagId);
        const Group* parent = tag ? FindGroupById(model.groups, tag->parentGroup) : nullptr;
        const std::wstring dueAt = todo.nextDueAt.empty() ? todo.anchorAt : todo.nextDueAt;
        if (!first) {
            out << L",\n";
        }
        first = false;
        out << L"    {\n";
        out << L"      \"id\": " << todo.id << L",\n";
        out << L"      \"title\": \"" << JsonEscape(todo.title) << L"\",\n";
        out << L"      \"notes\": \"" << JsonEscape(todo.content) << L"\",\n";
        out << L"      \"enabled\": " << BoolJson(todo.enabled) << L",\n";
        out << L"      \"completed\": " << BoolJson(!todo.completedAt.empty()) << L",\n";
        out << L"      \"dueAt\": \"" << JsonEscape(TodoTimestampToIso8601(dueAt)) << L"\",\n";
        out << L"      \"groupName\": \"" << JsonEscape(parent ? parent->name : L"默认分组") << L"\",\n";
        out << L"      \"tagName\": \"" << JsonEscape(tag ? tag->name : L"待办事项") << L"\",\n";
        out << L"      \"source\": \"Quattro\",\n";
        out << L"      \"quattro\": {\n";
        out << L"        \"originalId\": " << todo.id << L",\n";
        out << L"        \"content\": \"" << JsonEscape(todo.content) << L"\",\n";
        out << L"        \"scheduleKind\": " << static_cast<int>(todo.scheduleKind) << L",\n";
        out << L"        \"repeatMode\": " << static_cast<int>(todo.repeatMode) << L",\n";
        out << L"        \"repeatInterval\": " << todo.repeatInterval << L",\n";
        out << L"        \"repeatLimit\": " << todo.repeatLimit << L",\n";
        out << L"        \"repeatFinished\": " << todo.repeatFinished << L",\n";
        out << L"        \"cronExpression\": \"" << JsonEscape(todo.cronExpression) << L"\",\n";
        out << L"        \"anchorAt\": \"" << JsonEscape(todo.anchorAt) << L"\",\n";
        out << L"        \"nextDueAt\": \"" << JsonEscape(todo.nextDueAt) << L"\",\n";
        out << L"        \"completedAt\": \"" << JsonEscape(todo.completedAt) << L"\",\n";
        out << L"        \"createdAt\": \"" << JsonEscape(todo.createdAt) << L"\",\n";
        out << L"        \"updatedAt\": \"" << JsonEscape(todo.updatedAt) << L"\"\n";
        out << L"      },\n";
        out << L"      \"apple\": {\n";
        out << L"        \"list\": \"提醒事项\",\n";
        out << L"        \"priority\": \"normal\",\n";
        out << L"        \"skipIfCompleted\": true\n";
        out << L"      }\n";
        out << L"    }";
    }
    out << L"\n  ]\n";
    out << L"}\n";
    return out.str();
}

struct TodoJsonImportReport {
    bool ok = false;
    int importedCount = 0;
    int createdGroups = 0;
    int createdTags = 0;
    std::wstring message;
};

TodoJsonImportReport ImportTodoJsonFile(const std::filesystem::path& appDirectory, const std::filesystem::path& jsonPath) {
    TodoJsonImportReport report;
    const std::wstring text = LoadUtf8File(jsonPath);
    if (text.empty()) {
        report.message = L"读取待办 JSON 失败，或文件内容为空。";
        return report;
    }

    JsonValue root;
    std::wstring error;
    if (!ParseJson(text, root, error)) {
        report.message = L"待办 JSON 解析失败: " + error;
        return report;
    }
    const JsonValue* todos = root.get(L"todos");
    if (!todos || !todos->isArray()) {
        report.message = L"待办 JSON 缺少 todos 数组。";
        return report;
    }

    StorageService storage(appDirectory);
    AppModel model = storage.Load();
    std::map<std::wstring, int> groupIdCache;
    std::map<std::wstring, int> tagIdCache;

    for (const auto& entry : todos->arrayValue) {
        if (!entry.isObject()) {
            continue;
        }
        const JsonValue* quattro = ObjectField(entry, L"quattro");
        const std::wstring groupName = Trim(JsonStringField(entry, L"groupName", L"默认分组"));
        const std::wstring tagName = Trim(JsonStringField(entry, L"tagName", L"待办事项"));
        const std::wstring groupKey = ToLower(groupName);
        const std::wstring tagKey = groupKey + L"\n" + ToLower(tagName);

        int parentGroupId = 0;
        auto groupIt = groupIdCache.find(groupKey);
        if (groupIt != groupIdCache.end()) {
            parentGroupId = groupIt->second;
        } else {
            const Group* existing = FindRootGroupByName(model.groups, groupName);
            if (existing) {
                parentGroupId = existing->id;
            } else {
                Group group;
                group.name = UniqueSiblingGroupName(model.groups, 0, groupName.empty() ? L"默认分组" : groupName);
                if (!storage.InsertGroup(group)) {
                    report.message = L"创建分组失败: " + storage.lastError();
                    return report;
                }
                model.groups.push_back(group);
                parentGroupId = group.id;
                ++report.createdGroups;
            }
            groupIdCache[groupKey] = parentGroupId;
        }

        int tagId = 0;
        auto tagIt = tagIdCache.find(tagKey);
        if (tagIt != tagIdCache.end()) {
            tagId = tagIt->second;
        } else {
            const Group* existingTag = FindTodoTagByName(model.groups, parentGroupId, tagName);
            if (existingTag) {
                tagId = existingTag->id;
            } else {
                Group tag;
                tag.parentGroup = parentGroupId;
                tag.name = UniqueSiblingGroupName(model.groups, parentGroupId, tagName.empty() ? L"待办事项" : tagName);
                tag.type = 4;
                tag.content = L"todoItems";
                if (!storage.InsertGroup(tag)) {
                    report.message = L"创建待办标签失败: " + storage.lastError();
                    return report;
                }
                model.groups.push_back(tag);
                tagId = tag.id;
                ++report.createdTags;
            }
            tagIdCache[tagKey] = tagId;
        }

        TodoItem item;
        item.tagId = tagId;
        item.title = JsonStringField(entry, L"title");
        item.content = JsonStringField(quattro, L"content", JsonStringField(entry, L"notes", JsonStringField(entry, L"content")));
        item.enabled = JsonBoolField(entry, L"enabled", true);
        item.scheduleKind = static_cast<TodoScheduleKind>(JsonIntField(quattro, L"scheduleKind", JsonIntField(entry, L"scheduleKind", 0)));
        item.repeatMode = static_cast<TodoRepeatMode>(JsonIntField(quattro, L"repeatMode", JsonIntField(entry, L"repeatMode", 0)));
        item.repeatInterval = std::max(1, JsonIntField(quattro, L"repeatInterval", JsonIntField(entry, L"repeatInterval", 1)));
        item.repeatLimit = std::max(0, JsonIntField(quattro, L"repeatLimit", JsonIntField(entry, L"repeatLimit", 0)));
        item.repeatFinished = std::max(0, JsonIntField(quattro, L"repeatFinished", JsonIntField(entry, L"repeatFinished", 0)));
        item.cronExpression = JsonStringField(quattro, L"cronExpression", JsonStringField(entry, L"cronExpression"));
        item.anchorAt = JsonStringField(quattro, L"anchorAt", JsonStringField(entry, L"anchorAt"));
        item.nextDueAt = JsonStringField(quattro, L"nextDueAt", JsonStringField(entry, L"nextDueAt"));
        item.completedAt = JsonStringField(quattro, L"completedAt", JsonStringField(entry, L"completedAt"));
        item.createdAt = JsonStringField(quattro, L"createdAt", JsonStringField(entry, L"createdAt"));
        item.updatedAt = JsonStringField(quattro, L"updatedAt", JsonStringField(entry, L"updatedAt"));
        if (!quattro && item.scheduleKind == TodoScheduleKind::None && !JsonStringField(entry, L"dueAt").empty()) {
            const std::wstring dueAt = ImportableTodoTimestamp(JsonStringField(entry, L"dueAt"));
            if (!dueAt.empty()) {
                item.scheduleKind = TodoScheduleKind::Once;
                item.anchorAt = dueAt;
                item.nextDueAt = item.anchorAt;
            }
        }
        if (!quattro && item.completedAt.empty() && JsonBoolField(entry, L"completed", false)) {
            item.completedAt = CurrentTodoTimestamp();
        }
        if (!storage.InsertTodoItem(item)) {
            report.message = L"导入待办失败: " + storage.lastError();
            return report;
        }
        model.todos.push_back(item);
        ++report.importedCount;
    }

    report.ok = true;
    report.message = L"待办 JSON 导入完成。\n\n新增待办: " + std::to_wstring(report.importedCount) +
        L"\n新增分组: " + std::to_wstring(report.createdGroups) +
        L"\n新增待办标签: " + std::to_wstring(report.createdTags);
    return report;
}

int EnglishMonthIndex(const std::wstring& month) {
    static constexpr const wchar_t* kMonths[] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    for (int i = 0; i < 12; ++i) {
        if (_wcsicmp(month.c_str(), kMonths[i]) == 0) {
            return i + 1;
        }
    }
    return 0;
}

std::wstring ChineseDateTimeText(int year, int month, int day, int hour, int minute) {
    if (year <= 0 || month <= 0 || day <= 0) {
        return {};
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%d年%d月%d日 %02d:%02d", year, month, day, hour, minute);
    return buffer;
}

std::wstring LocalBackupDateTimeTextFromUtc(int year, int month, int day, int hour, int minute, int second) {
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);

    FILETIME utcFile{};
    FILETIME localFile{};
    SYSTEMTIME local{};
    if (!SystemTimeToFileTime(&utc, &utcFile) ||
        !FileTimeToLocalFileTime(&utcFile, &localFile) ||
        !FileTimeToSystemTime(&localFile, &local)) {
        return {};
    }
    return ChineseDateTimeText(local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
}

std::wstring FormatBackupModifiedDate(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    wchar_t monthText[8]{};
    if (swscanf_s(value.c_str(), L"%*3ls, %d %7ls %d %d:%d:%d", &day, monthText, static_cast<unsigned>(std::size(monthText)), &year, &hour, &minute, &second) == 6) {
        month = EnglishMonthIndex(monthText);
        const std::wstring formatted = LocalBackupDateTimeTextFromUtc(year, month, day, hour, minute, second);
        if (!formatted.empty()) {
            return formatted;
        }
    }
    if (swscanf_s(value.c_str(), L"%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) == 5) {
        const std::wstring formatted = ChineseDateTimeText(year, month, day, hour, minute);
        if (!formatted.empty()) {
            return formatted;
        }
    }
    return value;
}

std::wstring FormatWebDavLastSyncText(const std::wstring& value) {
    SYSTEMTIME time{};
    if (!TryParseTodoTimestamp(value, time)) {
        return {};
    }
    const std::wstring formatted = ChineseDateTimeText(time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    return formatted.empty() ? std::wstring{} : L"最后同步：" + formatted;
}

std::wstring FormatBackupListItem(const WebDavRemoteFile& backup) {
    std::wstring text = backup.name;
    if (backup.size > 0) {
        text += L"    " + FormatFileSize(backup.size);
    }
    if (!backup.lastModified.empty()) {
        text += L"    " + FormatBackupModifiedDate(backup.lastModified);
    }
    return text;
}

std::wstring WrapLongToken(const std::wstring& value, std::size_t maxCharsPerLine) {
    if (value.size() <= maxCharsPerLine || maxCharsPerLine == 0) {
        return value;
    }
    std::wstring text;
    for (std::size_t i = 0; i < value.size(); i += maxCharsPerLine) {
        if (!text.empty()) {
            text += L"\n";
        }
        text += value.substr(i, maxCharsPerLine);
    }
    return text;
}

std::wstring FormatBackupConfirmationText(const WebDavRemoteFile& backup) {
    std::wstring text =
        L"请确认要下载并合并以下 WebDAV 备份：\n\n"
        L"文件名:\n" + WrapLongToken(backup.name, 42) + L"\n"
        L"文件大小: " + FormatFileSize(backup.size);
    const std::wstring modified = FormatBackupModifiedDate(backup.lastModified);
    if (!modified.empty()) {
        text += L"\n备份时间: " + modified;
    }
    text +=
        L"\n\n将把该备份中的分组、标签、启动项、便签和待办合并到当前数据。"
        L"\n当前数据不会被覆盖，导入前会自动备份。";
    return text;
}

int EstimateMessageRows(const std::wstring& message, int width, int averageCharWidth) {
    int rows = 1;
    int lineLength = 0;
    const int charsPerRow = std::max(12, width / std::max(1, averageCharWidth));
    for (wchar_t ch : message) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            ++rows;
            lineLength = 0;
            continue;
        }
        ++lineLength;
        if (lineLength >= charsPerRow) {
            ++rows;
            lineLength = 0;
        }
    }
    return std::max(1, rows);
}

int MeasureMessageTextHeight(const std::wstring& message, int width) {
    HFONT font = ThemedControls::CreateDialogFont();
    if (!font) {
        font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    HDC dc = GetDC(nullptr);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    TEXTMETRICW textMetric{};
    GetTextMetricsW(dc, &textMetric);
    const int lineHeight = std::max(20, static_cast<int>(textMetric.tmHeight + textMetric.tmExternalLeading));
    const int averageCharWidth = std::max(1, static_cast<int>(textMetric.tmAveCharWidth));
    RECT rect{0, 0, std::max(1, width), 0};
    DrawTextW(dc, message.c_str(), static_cast<int>(message.size()), &rect, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);
    if (font && font != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font);
    }
    const int editRowHeight = lineHeight + std::max(4, lineHeight / 4);
    const int controlPadding = lineHeight + std::max(8, lineHeight / 2);
    const int estimatedHeight = EstimateMessageRows(message, width, averageCharWidth) * editRowHeight + controlPadding;
    return std::max(lineHeight, std::max(static_cast<int>(rect.bottom - rect.top), estimatedHeight));
}

HMODULE RichEditLibrary() {
    static HMODULE module = LoadLibraryW(L"Msftedit.dll");
    return module;
}

class ThemedMessageDialog {
public:
    ThemedMessageDialog(HWND owner, HINSTANCE instance, const Theme& theme, std::wstring message, std::wstring title, UINT flags)
        : owner_(owner), instance_(instance), theme_(theme), message_(std::move(message)), title_(std::move(title)), flags_(flags) {}

    int Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ThemedMessageDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QuattroThemedMessageDialog";
        RegisterClassExW(&wc);

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

        width_ = 430;
        const DialogLayoutMetrics layout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Mini);
        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        const int textWidth = width_ - layout.contentInsetX * 2;
        const int textHeight = MeasureMessageTextHeight(message_, textWidth);
        const int availableHeight = std::max(260, static_cast<int>(workArea.bottom - workArea.top) * 3 / 4);
        const int maxTextHeight = std::max(80, availableHeight - layout.contentInsetY - layout.footerGap - buttonHeight - layout.footerInsetY);
        textNeedsScroll_ = textHeight + 4 > maxTextHeight;
        textHeight_ = std::min(std::max(32, textHeight + 4), maxTextHeight);
        const int clientHeight = std::max(150, layout.contentInsetY + textHeight_ + layout.footerGap + buttonHeight + layout.footerInsetY);
        const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
        const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN;
        RECT windowRect{0, 0, width_, clientHeight};
        AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
        const int windowWidth = windowRect.right - windowRect.left;
        const int windowHeight = windowRect.bottom - windowRect.top;

        const POINT position = CenterWindowOnOwnerMonitor(owner_, windowWidth, windowHeight);

        hwnd_ = CreateWindowExW(
            exStyle,
            wc.lpszClassName,
            title_.empty() ? L"提示" : title_.c_str(),
            style,
            position.x,
            position.y,
            windowWidth,
            windowHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return MessageBoxW(owner_, message_.c_str(), title_.c_str(), flags_);
        }

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return result_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ThemedMessageDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ThemedMessageDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ThemedMessageDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool YesNo() const {
        return (flags_ & MB_TYPEMASK) == MB_YESNO;
    }

    bool YesNoCancel() const {
        return (flags_ & MB_TYPEMASK) == MB_YESNOCANCEL;
    }

    bool OkCancel() const {
        return (flags_ & MB_TYPEMASK) == MB_OKCANCEL;
    }

    void Close(int result) {
        result_ = result;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    static LRESULT CALLBACK TextControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
        switch (message) {
        case WM_KEYDOWN:
            if ((wParam == L'A' || wParam == L'a') && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                SendMessageW(hwnd, EM_SETSEL, 0, -1);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, TextControlProc, subclassId);
            break;
        default:
            break;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    RECT MessageTextRect(const DialogLayoutMetrics& layout, int clientWidth) const {
        return RECT{
            layout.contentInsetX,
            layout.contentInsetY,
            clientWidth - layout.contentInsetX,
            layout.contentInsetY + textHeight_};
    }

    DWORD MessageTextStyle() const {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL;
        if (textNeedsScroll_) {
            style |= WS_VSCROLL;
        }
        return style;
    }

    std::wstring MessageControlText() const {
        std::wstring text;
        text.reserve(message_.size() + 8);
        for (std::size_t i = 0; i < message_.size(); ++i) {
            const wchar_t ch = message_[i];
            if (ch == L'\r') {
                text += L"\r\n";
                if (i + 1 < message_.size() && message_[i + 1] == L'\n') {
                    ++i;
                }
            } else if (ch == L'\n') {
                text += L"\r\n";
            } else {
                text += ch;
            }
        }
        return text;
    }

    void ConfigureMessageText(HWND hwnd, bool richEdit) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        SetWindowSubclass(hwnd, TextControlProc, 1, 0);

        if (richEdit) {
            const COLORREF background = ToColorRef(theme_.color(L"dialog", L"normal", L"bg"));
            SendMessageW(hwnd, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(background));

            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_COLOR;
            format.crTextColor = ToColorRef(theme_.color(L"label", L"normal", L"text"));
            SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));

            SendMessageW(hwnd, EM_AUTOURLDETECT, TRUE, 0);
            const LRESULT mask = SendMessageW(hwnd, EM_GETEVENTMASK, 0, 0);
            SendMessageW(hwnd, EM_SETEVENTMASK, 0, static_cast<LPARAM>(mask | ENM_LINK));
            const std::wstring text = MessageControlText();
            SetWindowTextW(hwnd, text.c_str());
        }
    }

    bool CreateRichMessageText(const RECT& rect) {
        if (!RichEditLibrary()) {
            return false;
        }
        messageEdit_ = CreateWindowExW(
            0,
            MSFTEDIT_CLASS,
            nullptr,
            MessageTextStyle(),
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MESSAGE_TEXT)),
            instance_,
            nullptr);
        if (!messageEdit_) {
            return false;
        }
        messageTextIsRichEdit_ = true;
        ConfigureMessageText(messageEdit_, true);
        return true;
    }

    void CreateFallbackMessageText(const RECT& rect) {
        const std::wstring text = MessageControlText();
        messageEdit_ = CreateWindowExW(
            0,
            L"EDIT",
            text.c_str(),
            MessageTextStyle(),
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MESSAGE_TEXT)),
            instance_,
            nullptr);
        if (messageEdit_) {
            ConfigureMessageText(messageEdit_, false);
        }
    }

    void CreateMessageTextControl(const DialogLayoutMetrics& layout, int clientWidth) {
        const RECT rect = MessageTextRect(layout, clientWidth);
        if (!CreateRichMessageText(rect)) {
            CreateFallbackMessageText(rect);
        }
    }

    std::wstring LinkText(const CHARRANGE& range) const {
        if (!messageEdit_ || range.cpMax <= range.cpMin) {
            return {};
        }
        std::wstring text(static_cast<std::size_t>(range.cpMax - range.cpMin) + 1, L'\0');
        TEXTRANGEW textRange{};
        textRange.chrg = range;
        textRange.lpstrText = text.data();
        SendMessageW(messageEdit_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&textRange));
        text.resize(std::wcslen(text.c_str()));
        while (!text.empty() && (text.back() == L'.' || text.back() == L',' || text.back() == L';' ||
                                 text.back() == L':' || text.back() == L')' || text.back() == L'）' ||
                                 text.back() == L'。' || text.back() == L'，')) {
            text.pop_back();
        }
        return Trim(text);
    }

    bool HandleMessageTextNotify(LPARAM lParam) {
        if (!messageTextIsRichEdit_ || !messageEdit_) {
            return false;
        }
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (!header || header->hwndFrom != messageEdit_ || header->code != EN_LINK) {
            return false;
        }
        auto* link = reinterpret_cast<ENLINK*>(lParam);
        if (link->msg != WM_LBUTTONUP) {
            return false;
        }
        const std::wstring url = LinkText(link->chrg);
        if (url.empty()) {
            return false;
        }
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = ThemedControls::CreateDialogFont();
            if (!font_) {
                font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            } else {
                ownsFont_ = true;
            }
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            const DialogLayoutMetrics layout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Mini);
            RECT client{};
            GetClientRect(hwnd_, &client);
            const int clientWidth = client.right - client.left;
            const int clientHeight = client.bottom - client.top;
            CreateMessageTextControl(layout, clientWidth);
            const ThemedUi ui(instance_, hwnd_, theme_, font_, DialogLayoutKind::Mini, clientWidth, clientHeight);
            if (YesNoCancel()) {
                ui.FooterButton(IDYES, L"是", 0, 3, true, true);
                ui.FooterButton(IDNO, L"否", 1, 3);
                ui.FooterButton(IDCANCEL, L"取消", 2, 3);
            } else if (YesNo()) {
                ui.FooterButton(IDYES, L"是", 0, 2, true, true);
                ui.FooterButton(IDNO, L"否", 1, 2);
            } else if (OkCancel()) {
                ui.FooterButton(IDOK, L"确定", 0, 2, true, true);
                ui.FooterButton(IDCANCEL, L"取消", 1, 2);
            } else {
                ui.FooterButton(IDOK, L"确定", 0, 1, true, true);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NOTIFY:
            if (HandleMessageTextNotify(lParam)) {
                return TRUE;
            }
            {
                LRESULT result = 0;
                if (ThemedUi::HandleParentMessage(theme_, message, wParam, lParam, result)) {
                    return result;
                }
            }
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == messageEdit_) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
                return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_DRAWITEM: {
            LRESULT result = 0;
            if (ThemedUi::HandleParentMessage(theme_, message, wParam, lParam, result)) {
                return result;
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDYES:
                Close(IDYES);
                return 0;
            case IDNO:
                Close(IDNO);
                return 0;
            case IDOK:
                Close(IDOK);
                return 0;
            case IDCANCEL:
                Close(IDCANCEL);
                return 0;
            default:
                return 0;
            }
        case WM_CLOSE:
            Close(YesNo() ? IDNO : IDCANCEL);
            return 0;
        case WM_DESTROY:
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (ownsFont_ && font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND messageEdit_ = nullptr;
    const Theme& theme_;
    std::wstring message_;
    std::wstring title_;
    UINT flags_ = MB_OK;
    int width_ = 430;
    int height_ = 150;
    int textHeight_ = 32;
    int result_ = IDOK;
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    bool messageTextIsRichEdit_ = false;
    bool textNeedsScroll_ = false;
    bool ownsFont_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
};

class TextDialog {
public:
    TextDialog(HWND owner, HINSTANCE instance, const Theme& theme, std::wstring title, std::wstring label, std::wstring& value)
        : owner_(owner), instance_(instance), theme_(theme), title_(std::move(title)), label_(std::move(label)), value_(value) {}

    bool Run() {
        const std::wstring className = L"QuattroTextInputDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        constexpr int kClientWidth = 390;
        constexpr int kClientHeight = 162;
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), title_.c_str(), TextDialog::Proc, this, icon, icon);
        options.clientWidth = kClientWidth;
        options.clientHeight = kClientHeight;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 80;
        options.offsetY = 100;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            const DWORD error = GetLastError();
            WriteAppLog(L"文本输入窗口创建失败: " + FormatLastError(error));
            return false;
        }
        if (windowUi_) windowUi_->ShowModal();
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) windowUi_->RestoreModalOwner();
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        TextDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<TextDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<TextDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE: {
            constexpr int kClientWidth = 390;
            constexpr int kClientHeight = 162;
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Mini, kClientWidth, kClientHeight);
            const DialogLayoutMetrics layout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Mini);
            RECT client{};
            GetClientRect(hwnd_, &client);
            const int clientWidth = client.right - client.left;
            const int contentWidth = clientWidth - layout.contentInsetX * 2;
            const int labelY = layout.contentInsetY;
            const ThemedUi ui = windowUi_->ui();
            ui.Label(label_, layout.contentInsetX, labelY, contentWidth);
            const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
            const int editY = labelY + ThemedControls::LabelHeight(theme_) + layout.rowGap;
            editFrame_ = RECT{layout.contentInsetX, editY, layout.contentInsetX + contentWidth, editY + fieldHeight};
            edit_ = ui.Edit(100, editFrame_, value_);
            ui.FooterButton(IDOK, L"确定", 0, 2, true, true);
            ui.FooterButton(IDCANCEL, L"取消", 1, 2);
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredEditFrames(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                std::wstring next = Trim(GetText(edit_));
                if (next.empty()) {
                    ShowThemedMessageBox(hwnd_, instance_, theme_, L"名称不能为空。", title_, MB_OK | MB_ICONWARNING);
                    return 0;
                }
                value_ = next;
                accepted_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    const Theme& theme_;
    std::wstring title_;
    std::wstring label_;
    std::wstring& value_;
    RECT editFrame_{};
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool accepted_ = false;
    bool done_ = false;
};

class HotKeyConflictDialog {
public:
    HotKeyConflictDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        std::wstring message,
        bool& ignoreFutureWarnings)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          message_(std::move(message)),
          ignoreFutureWarnings_(ignoreFutureWarnings) {}

    bool Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_,
            owner_,
            L"QuattroHotKeyConflictDialog",
            L"热键冲突",
            HotKeyConflictDialog::Proc,
            this,
            icon,
            icon);
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return false;
        }
        if (windowUi_) {
            windowUi_->ShowModal();
        }
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        HotKeyConflictDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<HotKeyConflictDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<HotKeyConflictDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Close(bool accepted) {
        accepted_ = accepted;
        if (ignoreToggle_) {
            ignoreFutureWarnings_ = ThemedUi::IsChecked(ignoreToggle_);
        }
        done_ = true;
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        DestroyWindow(hwnd_);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }

        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_,
                owner_,
                hwnd_,
                theme_,
                kThemedDialogLayoutKind,
                kThemedDialogClientWidth,
                kThemedDialogClientHeight);
            const ThemedUi ui = windowUi_->ui();
            const ThemedFormLayout form(ui);
            int y = ui.contentTop();

            ThemedLabelOptions messageOptions{};
            messageOptions.lines = ThemedLabelLines::Three;
            const auto messageRow = form.row(y, ThemedRowAlign::Left, {form.item(ui.contentWidth(), ui.labelHeight() * 3)});
            ui.Label(message_, messageRow[0].left, messageRow[0].top, messageRow[0].right - messageRow[0].left, messageOptions);

            y = form.nextRowY(y, {form.item(ui.contentWidth(), ui.labelHeight() * 3)});
            ThemedToggleOptions toggleOptions{};
            toggleOptions.checked = ignoreFutureWarnings_;
            const auto toggleRow = form.row(y, ThemedRowAlign::Left, {form.item(ui.contentWidth(), ThemedControls::CheckBoxHeight(theme_))});
            ignoreToggle_ = ui.Toggle(
                ID_HOTKEY_CONFLICT_IGNORE,
                L"以后忽略启动时的热键冲突提示",
                toggleRow[0].left,
                toggleRow[0].top,
                toggleRow[0].right - toggleRow[0].left,
                toggleOptions);

            ui.FooterButton(IDOK, L"知道了", 0, 1, true, true);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK:
                Close(true);
                return 0;
            case IDCANCEL:
                Close(false);
                return 0;
            default:
                break;
            }
            break;
        case WM_CLOSE:
            Close(false);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::wstring message_;
    bool& ignoreFutureWarnings_;
    HWND hwnd_ = nullptr;
    HWND ignoreToggle_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool accepted_ = false;
    bool done_ = false;
};

class WebDavBackupSelectionDialog {
public:
    WebDavBackupSelectionDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        const std::vector<WebDavRemoteFile>& backups,
        std::wstring& selectedName)
        : owner_(owner), instance_(instance), theme_(theme), backups_(backups), selectedName_(selectedName) {}

    bool Run() {
        const std::wstring className = L"QuattroWebDavBackupSelectionDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WebDavBackupSelectionDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = className.c_str();
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(L"WebDAV 备份选择窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        const POINT position = OffsetWindowFromOwnerOnMonitor(owner_, 560, 390, 60, 80);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            L"选择 WebDAV 备份",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
            position.x,
            position.y,
            560,
            390,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(L"WebDAV 备份选择窗口创建失败: " + FormatLastError(GetLastError()));
            return false;
        }

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        WebDavBackupSelectionDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<WebDavBackupSelectionDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<WebDavBackupSelectionDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    int TextWidth(HDC dc, const std::wstring& text) const {
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        return size.cx;
    }

    int BackupSizeColumnWidth(HDC dc) const {
        int width = TextWidth(dc, L"888 KB");
        for (const auto& backup : backups_) {
            width = std::max(width, TextWidth(dc, FormatFileSize(backup.size)));
        }
        return width + 12;
    }

    int BackupDateColumnWidth(HDC dc) const {
        int width = TextWidth(dc, L"2026年12月30日 23:59");
        for (const auto& backup : backups_) {
            width = std::max(width, TextWidth(dc, FormatBackupModifiedDate(backup.lastModified)));
        }
        return width + 12;
    }

    bool DrawBackupListItem(const DRAWITEMSTRUCT* draw) {
        if (!draw || draw->CtlID != ID_WEBDAV_BACKUP_LIST) {
            return false;
        }

        RECT rect = draw->rcItem;
        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        const bool focused = (draw->itemState & ODS_FOCUS) != 0;
        const wchar_t* state = selected ? L"selected" : (focused ? L"focused" : L"normal");
        HBRUSH brush = CreateSolidBrush(ToColorRef(theme_.color(selected ? L"listItem" : L"list", state, L"bg")));
        FillRect(draw->hDC, &rect, brush);
        DeleteObject(brush);

        if (draw->itemID == static_cast<UINT>(-1) || draw->itemID >= backups_.size()) {
            return true;
        }

        const auto& backup = backups_[draw->itemID];
        const std::wstring sizeText = FormatFileSize(backup.size);
        const std::wstring dateText = FormatBackupModifiedDate(backup.lastModified);
        RECT textRect = ThemedControls::ListItemTextRect(theme_, rect);
        const int gap = 10;
        const int dateWidth = BackupDateColumnWidth(draw->hDC);
        const int sizeWidth = BackupSizeColumnWidth(draw->hDC);
        RECT dateRect{textRect.right - dateWidth, textRect.top, textRect.right, textRect.bottom};
        RECT sizeRect{dateRect.left - gap - sizeWidth, textRect.top, dateRect.left - gap, textRect.bottom};
        RECT nameRect{textRect.left, textRect.top, sizeRect.left - gap, textRect.bottom};
        if (nameRect.right < nameRect.left) {
            nameRect.right = nameRect.left;
        }

        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, ToColorRef(theme_.color(selected ? L"listItem" : L"list", state, L"text")));
        DrawTextW(draw->hDC, backup.name.c_str(), static_cast<int>(backup.name.size()), &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextW(draw->hDC, sizeText.c_str(), static_cast<int>(sizeText.size()), &sizeRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(draw->hDC, dateText.c_str(), static_cast<int>(dateText.size()), &dateRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        return true;
    }

    void AcceptSelection() {
        const int selected = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (selected < 0 || selected >= static_cast<int>(backups_.size())) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"请选择一个备份文件。", L"选择 WebDAV 备份", MB_OK | MB_ICONWARNING);
            return;
        }
        selectedName_ = backups_[static_cast<std::size_t>(selected)].name;
        accepted_ = true;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = ThemedControls::CreateDialogFont();
            if (!font_) {
                font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            } else {
                ownsFont_ = true;
            }
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            listBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"list", L"normal", L"bg")));
            RECT client{};
            GetClientRect(hwnd_, &client);
            const ThemedUi ui(instance_, hwnd_, theme_, font_, DialogLayoutKind::Standard, client.right - client.left, client.bottom - client.top);
            ui.Label(L"云端备份记录", 24, 20, 180);
            list_ = ui.ListBox(ID_WEBDAV_BACKUP_LIST, 24, 48, 500, 238);
            for (const auto& backup : backups_) {
                SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(FormatBackupListItem(backup).c_str()));
            }
            if (!backups_.empty()) {
                SendMessageW(list_, LB_SETCURSEL, 0, 0);
            }

            ui.Button(IDOK, L"下载", 360, 310, ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, 76, true);
            ui.Button(IDCANCEL, L"取消", 452, 310, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, 76);
            SetFocus(list_);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            const RECT listFrame{22, 46, 526, 288};
            ThemedControls::DrawListFrame(theme_, dc, listFrame, list_);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"list", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(listBrush_ ? listBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (DrawBackupListItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_WEBDAV_BACKUP_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                AcceptSelection();
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                AcceptSelection();
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (listBrush_) {
                DeleteObject(listBrush_);
                listBrush_ = nullptr;
            }
            if (ownsFont_ && font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HFONT font_ = nullptr;
    const Theme& theme_;
    const std::vector<WebDavRemoteFile>& backups_;
    std::wstring& selectedName_;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH listBrush_ = nullptr;
    bool ownsFont_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool accepted_ = false;
    bool done_ = false;
};

class SettingsDialog {
public:
    SettingsDialog(
        HWND owner,
        HINSTANCE instance,
        AppConfig& config,
        const Theme& theme,
        std::filesystem::path appDirectory,
        std::filesystem::path httpRootBaseDirectory,
        LocalHttpServerService* httpServer,
        bool mainHotKeyRegistered,
        bool processLocatorHotKeyRegistered,
        bool copySelectedPathsHotKeyRegistered,
        SettingsApplyCallback applyCallback,
        SettingsResetContextMenuCallback resetContextMenuCallback,
        std::vector<Link> contextMenuLinks,
        SettingsContextMenuRefreshRunner contextMenuRefreshRunner,
        SettingsContextMenuRefreshApplyCallback contextMenuRefreshApplyCallback,
        SettingsContextMenuProviderIconRunner contextMenuProviderIconRunner)
        : owner_(owner),
          instance_(instance),
          config_(config),
          draft_(config),
          theme_(theme),
          appDirectory_(std::move(appDirectory)),
          httpRootBaseDirectory_(std::move(httpRootBaseDirectory)),
          httpServer_(httpServer),
          mainHotKeyRegistered_(mainHotKeyRegistered),
          processLocatorHotKeyRegistered_(processLocatorHotKeyRegistered),
          copySelectedPathsHotKeyRegistered_(copySelectedPathsHotKeyRegistered),
          applyCallback_(std::move(applyCallback)),
          resetContextMenuCallback_(std::move(resetContextMenuCallback)),
          contextMenuLinks_(std::move(contextMenuLinks)),
          contextMenuRefreshRunner_(std::move(contextMenuRefreshRunner)),
          contextMenuRefreshApplyCallback_(std::move(contextMenuRefreshApplyCallback)),
          contextMenuProviderIconRunner_(std::move(contextMenuProviderIconRunner)) {}

    ~SettingsDialog() {
        AbandonContextMenuIconLoad();
        DestroyContextMenuImageList();
    }

    bool Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroSettingsDialog", L"设置", SettingsDialog::Proc, this, icon, icon);
        options.clientWidth = 656;
        options.clientHeight = 441;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 60;
        options.offsetY = 70;
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(L"设置窗口创建失败: " + error);
            return false;
        }
        windowUi_->ShowModal();
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        if (contextMenuRefreshThread_.joinable()) {
            contextMenuRefreshThread_.request_stop();
            contextMenuRefreshThread_.join();
        }
        return accepted_;
    }

    bool webDavDataImported() const {
        return importedData_;
    }

private:
    enum TabIndex {
        TabDisplay = 0,
        TabBehavior = 1,
        TabContextMenu = 2,
        TabInteraction = 3,
        TabHotKeys = 4,
        TabLinks = 5,
        TabWebDav = 6,
        TabHttp = 7,
        TabBackup = 8,
        TabCount = 9,
    };

    struct TabChild {
        HWND hwnd = nullptr;
        int tab = 0;
    };

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        SettingsDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<SettingsDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void AddTabChild(HWND hwnd, int tab) {
        if (!hwnd) {
            return;
        }
        tabChildren_.push_back(TabChild{hwnd, tab});
    }

    int ContentY(int y) const {
        return y + tabContentOffsetY_;
    }

    HWND Label(int tab, const wchar_t* text, int x, int y, int width = 110, ThemedLabelOptions options = {}) {
        HWND hwnd = MakeUi().Label(text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND StatusBadge(int tab, const wchar_t* text, int x, int y, int width, ThemedStatusRole role) {
        HWND hwnd = MakeUi().StatusBadge(text, x, ContentY(y), width, role);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND CheckBox(int tab, int id, const wchar_t* text, int x, int y, bool checked, int width = 210) {
        ThemedCheckBoxOptions options{};
        options.checked = checked;
        HWND hwnd = MakeUi().CheckBox(id, text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND Toggle(int tab, int id, const wchar_t* text, int x, int y, bool checked, int width) {
        ThemedToggleOptions options{};
        options.checked = checked;
        HWND hwnd = MakeUi().Toggle(id, text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    ThemedUi MakeUi() const {
        return windowUi_->ui();
    }

    HWND Button(int tab, int id, const wchar_t* text, int x, int y, int width) {
        HWND hwnd = MakeUi().Button(id, text, x, ContentY(y), ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Fixed, width);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND FramedEdit(int tab, int id, int x, int y, int width, const std::wstring& text, ThemedEditOptions options = {}) {
        const ThemedUi ui = MakeUi();
        const int fieldHeight = ui.editHeight();
        y = ContentY(y);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ui.Edit(id, frame, text, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND FramedStatic(int tab, int x, int y, int width, const std::wstring& text) {
        const ThemedUi ui = MakeUi();
        const int fieldHeight = ui.editHeight();
        y = ContentY(y);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ui.FramedStatic(text, frame);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND NumberEdit(int tab, int id, int x, int y, int width, int value) {
        ThemedEditOptions options{};
        options.content = ThemedEditContent::Integer;
        return FramedEdit(tab, id, x, y, width, std::to_wstring(value), options);
    }

    int ClampNumber(HWND edit, int minValue, int maxValue, int fallback) const {
        auto value = ParseInt(GetText(edit));
        if (!value) {
            return fallback;
        }
        return std::max(minValue, std::min(maxValue, *value));
    }

    void SelectTagAlign() {
        tagAlignIndex_ = 1;
        if (draft_.tagAlign == L"left") {
            tagAlignIndex_ = 0;
        } else if (draft_.tagAlign == L"right") {
            tagAlignIndex_ = 2;
        }
        UpdateTagAlignButtons();
    }

    void UpdateTagAlignButtons() {
        const HWND buttons[] = {tagAlignLeft_, tagAlignCenter_, tagAlignRight_};
        for (int i = 0; i < 3; ++i) {
            if (buttons[i]) {
                ThemedUi::SetTabSelected(buttons[i], i == tagAlignIndex_);
            }
        }
    }

    void CreateTabs() {
        const wchar_t* titles[] = {L"显示", L"行为", L"右键菜单", L"交互", L"热键", L"链接", L"WebDAV", L"HTTP", L"备份"};
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = std::max(1, static_cast<int>(client.right - client.left));
        const ThemedUi ui = MakeUi();
        const DialogLayoutMetrics& layout = ui.layout();
        const int startY = layout.contentInsetY;
        std::vector<ThemedTabItem> items;
        items.reserve(TabCount);
        for (int i = 0; i < TabCount; ++i) {
            items.push_back(ThemedTabItem{ID_SETTINGS_TAB_BASE + i, titles[i], true});
        }
        const int itemHeight = ui.tabButtonHeight();
        tabStripRect_ = RECT{
            layout.contentInsetX,
            startY,
            clientWidth - layout.contentInsetX,
            startY + itemHeight + layout.rowGap};
        ThemedTabControlOptions options{};
        options.activeIndex = TabDisplay;
        options.equalWidth = false;
        options.appearance = ThemedTabControlAppearance::EmphasizedSegmented;
        settingsTabs_ = MakeUi().TabControl(ID_SETTINGS_TAB_CONTROL, tabStripRect_, items, options);
        tabContentOffsetY_ = 0;
    }

    void BindTabPages() {
        if (!settingsTabs_) return;
        for (int tab = 0; tab < TabCount; ++tab) {
            std::vector<HWND> children;
            for (const auto& child : tabChildren_) {
                if (child.hwnd && child.tab == tab) children.push_back(child.hwnd);
            }
            ThemedUi::BindTabPage(settingsTabs_, tab, children);
        }
    }

    void ShowTab(int tab) {
        if (tab < 0 || tab >= TabCount || tab == currentTab_) {
            return;
        }

        RECT contentRect{};
        GetClientRect(hwnd_, &contentRect);
        contentRect.top = std::max(contentRect.top, tabStripRect_.bottom);
        if (okButton_) {
            RECT footerRect{};
            GetWindowRect(okButton_, &footerRect);
            MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT*>(&footerRect), 2);
            contentRect.bottom = std::min(contentRect.bottom, footerRect.top);
        }

        const HWND focus = GetFocus();
        bool focusMovesToTab = false;
        for (const auto& child : tabChildren_) {
            if (child.hwnd && child.tab != tab && child.hwnd == focus) {
                focusMovesToTab = true;
            }
        }

        currentTab_ = tab;
        ThemedUi::SetActiveTab(settingsTabs_, tab, false);
        if (focusMovesToTab && settingsTabs_) SetFocus(settingsTabs_);
        if (contentRect.bottom > contentRect.top) {
            RedrawWindow(hwnd_, &contentRect, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        if (currentTab_ == TabHttp) {
            UpdateHttpButtons();
        }
        if (currentTab_ == TabWebDav) {
            UpdateWebDavLastSyncLabel();
        }
        if (currentTab_ == TabContextMenu) {
            RequestContextMenuIconLoadOnce();
        }
    }

    HWND AddSectionFrame(int tab, const std::wstring& title, RECT rect) {
        rect.top = ContentY(rect.top);
        rect.bottom = ContentY(rect.bottom);
        HWND group = MakeUi().GroupBox(nextGeneratedControlId_++, title, rect);
        AddTabChild(group, tab);
        return group;
    }

    void AddHotKeyTableRows() {
        if (!hotKeyTable_) return;
        ThemedUi::SetTableRows(hotKeyTable_, {
            ThemedTableRow{
                ID_MAIN_HOTKEY_CAPTURE,
                {
                    ThemedTableCell{L"主窗口显隐"},
                    ThemedTableCell{FormatMainHotKeyText(draft_.mainHotKey)},
                    ThemedTableCell{L"录入", -1, ThemedTableCellRole::Action, ID_MAIN_HOTKEY_CAPTURE},
                },
            },
            ThemedTableRow{
                ID_PROCESS_LOCATOR_HOTKEY_CAPTURE,
                {
                    ThemedTableCell{L"进程定位器"},
                    ThemedTableCell{FormatGlobalHotKeyText(draft_.processLocatorHotKey)},
                    ThemedTableCell{L"录入", -1, ThemedTableCellRole::Action, ID_PROCESS_LOCATOR_HOTKEY_CAPTURE},
                },
            },
            ThemedTableRow{
                ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE,
                {
                    ThemedTableCell{L"复制选中项绝对路径"},
                    ThemedTableCell{FormatGlobalHotKeyText(draft_.copySelectedPathsHotKey)},
                    ThemedTableCell{L"录入", -1, ThemedTableCellRole::Action, ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE},
                },
            },
        });
    }

    void EnsureContextMenuProviderState() {
        const auto providers = TrackedContextMenuProviders();
        if (contextMenuProviderIcons_.size() == providers.size()) {
            return;
        }
        contextMenuProviderIcons_.clear();
        contextMenuProviderIcons_.reserve(providers.size());
        for (const auto& provider : providers) {
            ContextMenuProviderIconInfo info;
            info.providerId = provider.providerId;
            contextMenuProviderIcons_.push_back(std::move(info));
        }
        contextMenuProviderImageIndexes_.assign(providers.size(), 0);
    }

    void DestroyContextMenuImageList() {
        if (contextMenuTable_ && IsWindow(contextMenuTable_)) {
            ThemedUi::SetTableImageLists(contextMenuTable_, nullptr, nullptr);
        }
        if (contextMenuImages_) {
            ImageList_Destroy(contextMenuImages_);
            contextMenuImages_ = nullptr;
        }
    }

    void RebuildContextMenuImageList() {
        EnsureContextMenuProviderState();
        const auto providers = TrackedContextMenuProviders();
        const int iconSize = std::max(1, MakeUi().scale(16));
        HIMAGELIST images = ImageList_Create(
            iconSize, iconSize, ILC_COLOR32 | ILC_MASK,
            static_cast<int>(providers.size()) + 1, 4);
        if (!images) {
            return;
        }

        SHSTOCKICONINFO stockInfo{};
        stockInfo.cbSize = sizeof(stockInfo);
        HICON fallback = nullptr;
        bool destroyFallback = false;
        if (SUCCEEDED(SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_SMALLICON, &stockInfo))) {
            fallback = stockInfo.hIcon;
            destroyFallback = fallback != nullptr;
        }
        if (!fallback) {
            fallback = LoadIconW(nullptr, IDI_APPLICATION);
        }
        int fallbackIndex = fallback ? ImageList_AddIcon(images, fallback) : -1;
        if (destroyFallback) {
            DestroyIcon(fallback);
        }
        if (fallbackIndex < 0) {
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = iconSize;
            bitmapInfo.bmiHeader.biHeight = -iconSize;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP blank = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (blank && bits) {
                ZeroMemory(bits, static_cast<SIZE_T>(iconSize) * static_cast<SIZE_T>(iconSize) * 4);
                fallbackIndex = ImageList_Add(images, blank, nullptr);
            }
            if (blank) DeleteObject(blank);
        }
        if (fallbackIndex < 0) {
            ImageList_Destroy(images);
            return;
        }

        contextMenuProviderImageIndexes_.assign(providers.size(), fallbackIndex);
        for (std::size_t index = 0; index < contextMenuProviderIcons_.size(); ++index) {
            HICON icon = CreateIconFromCachedPixels(contextMenuProviderIcons_[index].icon);
            if (!icon) {
                continue;
            }
            const int imageIndex = ImageList_AddIcon(images, icon);
            DestroyIcon(icon);
            if (imageIndex >= 0) {
                contextMenuProviderImageIndexes_[index] = imageIndex;
            }
        }

        HIMAGELIST oldImages = contextMenuImages_;
        contextMenuImages_ = images;
        if (contextMenuTable_) {
            ThemedUi::SetTableImageLists(contextMenuTable_, contextMenuImages_, nullptr);
        }
        if (oldImages) {
            ImageList_Destroy(oldImages);
        }
    }

    void AddContextMenuTableRows() {
        if (!contextMenuTable_) return;
        EnsureContextMenuProviderState();
        const auto providers = TrackedContextMenuProviders();
        std::vector<bool> installed(providers.size(), true);
        if (contextMenuProviderLoadCompleted_) {
            for (std::size_t index = 0; index < contextMenuProviderIcons_.size(); ++index) {
                installed[index] = contextMenuProviderIcons_[index].installed;
            }
        }

        const int selected = ThemedUi::TableSelectedIndex(contextMenuTable_);
        const std::intptr_t selectedKey = selected >= 0
            ? ThemedUi::TableRowKey(contextMenuTable_, selected)
            : 0;
        contextMenuTableOrder_ = TrackedContextMenuDisplayOrder(installed);
        std::vector<ThemedTableRow> rows;
        rows.reserve(contextMenuTableOrder_.size());

        for (std::size_t bindingIndex : contextMenuTableOrder_) {
            const auto& provider = providers[bindingIndex];
            const auto& iconInfo = contextMenuProviderIcons_[bindingIndex];
            ThemedTableRow row{};
            row.key = provider.checkBoxControlId;

            std::wstring statusText;
            if (!contextMenuProviderLoadCompleted_) {
                statusText = L"检测中...";
            } else if (contextMenuProviderLoadFailed_) {
                statusText = L"获取失败";
            } else if (iconInfo.installedViaProbe) {
                statusText = L"已安装(注册表)";
            } else if (iconInfo.installedInNativeShell) {
                statusText = L"已安装(菜单)";
            } else {
                statusText = L"未安装";
            }

            row.cells = {
                ThemedTableCell{
                    provider.displayName,
                    bindingIndex < contextMenuProviderImageIndexes_.size()
                        ? contextMenuProviderImageIndexes_[bindingIndex]
                        : 0},
                ThemedTableCell{statusText},
            };
            row.checked = draft_.*(provider.configMember);
            // 安装状态只用于提示当前能否立即抓取菜单。跟踪配置始终可编辑，
            // 这样软件后续安装后可在刷新 Windows 菜单时自动开始抓取。
            row.enabled = true;
            rows.push_back(std::move(row));
        }
        ThemedUi::SetTableRows(contextMenuTable_, rows);
        if (selectedKey != 0) {
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (rows[index].key == selectedKey) {
                    ThemedUi::SetTableSelectedIndex(contextMenuTable_, static_cast<int>(index));
                    break;
                }
            }
        }
    }

    bool AllInstalledContextMenuProvidersAttempted() const {
        if (!contextMenuProviderLoadCompleted_) {
            return false;
        }
        return std::all_of(
            contextMenuProviderIcons_.begin(), contextMenuProviderIcons_.end(),
            [](const ContextMenuProviderIconInfo& info) {
                return !info.installed || info.attempted;
            });
    }

    void UpdateContextMenuRefreshButtonState() {
        if (!refreshContextMenuButton_) {
            return;
        }
        const wchar_t* text = contextMenuRefreshBusy_
            ? L"扫描中..."
            : (contextMenuIconLoadBusy_ ? L"获取图标中..." : L"从Windows菜单刷新");
        SetWindowTextW(refreshContextMenuButton_, text);
        MakeUi().SetEnabled(refreshContextMenuButton_, !contextMenuRefreshBusy_ && !contextMenuIconLoadBusy_);
    }

    void SetContextMenuIconLoadBusy(bool busy) {
        contextMenuIconLoadBusy_ = busy;
        UpdateContextMenuRefreshButtonState();
    }

    void RequestContextMenuIconLoadOnce() {
        if (contextMenuIconAutoRequested_) {
            return;
        }
        contextMenuIconAutoRequested_ = true;
        PostMessageW(hwnd_, WM_CONTEXT_MENU_ICON_LOAD_REQUEST, 0, 0);
    }

    bool StartContextMenuIconLoad(bool force) {
        if (contextMenuIconLoadBusy_ || (!force && AllInstalledContextMenuProvidersAttempted())) {
            return false;
        }
        AbandonContextMenuIconLoad();
        auto state = std::make_shared<SettingsContextMenuIconAsyncState>();
        state->generation = gContextMenuIconGeneration.fetch_add(1);
        contextMenuIconAsyncState_ = state;
        contextMenuIconLoadGeneration_ = state->generation;
        const HWND target = hwnd_;
        const SettingsContextMenuProviderIconRunner runner = contextMenuProviderIconRunner_;
        SetContextMenuIconLoadBusy(true);
        std::thread([state, target, runner]() {
            std::vector<ContextMenuProviderIconInfo> result;
            try {
                result = runner
                    ? runner(state->stopSource.get_token())
                    : ContextMenuProviderIconService().Load(state->stopSource.get_token());
            } catch (...) {
                result.clear();
            }
            if (state->abandoned.load() || state->stopSource.stop_requested()) {
                return;
            }
            {
                std::lock_guard lock(state->mutex);
                state->result = std::move(result);
            }
            PostMessageW(
                target,
                WM_CONTEXT_MENU_ICON_LOAD_DONE,
                static_cast<WPARAM>(state->generation),
                0);
        }).detach();
        return true;
    }

    void CompleteContextMenuIconLoad(std::uintptr_t generation) {
        const auto state = contextMenuIconAsyncState_;
        if (!state || generation != contextMenuIconLoadGeneration_ || generation != state->generation) {
            return;
        }
        std::optional<std::vector<ContextMenuProviderIconInfo>> result;
        {
            std::lock_guard lock(state->mutex);
            result = std::move(state->result);
        }
        contextMenuIconAsyncState_.reset();
        SetContextMenuIconLoadBusy(false);
        if (!result || result->size() != TrackedContextMenuProviders().size()) {
            contextMenuProviderLoadCompleted_ = true;
            contextMenuProviderLoadFailed_ = true;
            RebuildContextMenuImageList();
            AddContextMenuTableRows();
            return;
        }
        contextMenuProviderIcons_ = std::move(*result);
        contextMenuProviderLoadCompleted_ = true;
        contextMenuProviderLoadFailed_ = false;
        RebuildContextMenuImageList();
        AddContextMenuTableRows();
    }

    void AbandonContextMenuIconLoad() {
        const auto state = contextMenuIconAsyncState_;
        if (!state) {
            return;
        }
        state->abandoned = true;
        state->stopSource.request_stop();
        contextMenuIconAsyncState_.reset();
    }

    void ReadContextMenuTableDraft(AppConfig& value) const {
        const auto providers = TrackedContextMenuProviders();
        for (std::size_t rowIndex = 0; rowIndex < contextMenuTableOrder_.size(); ++rowIndex) {
            const auto& provider = providers[contextMenuTableOrder_[rowIndex]];
            value.*(provider.configMember) = ThemedUi::IsTableChecked(
                contextMenuTable_, static_cast<int>(rowIndex));
        }
    }

    bool HandleContextMenuTableEvent(LPARAM lParam) {
        ThemedTableEvent event{};
        if (!ThemedUi::DecodeTableEvent(contextMenuTable_, lParam, event)) {
            return false;
        }
        if (event.kind != ThemedTableEventKind::CheckChanged) {
            return true;
        }
        if (event.row < 0 || static_cast<std::size_t>(event.row) >= contextMenuTableOrder_.size()) {
            return true;
        }
        const auto& provider = TrackedContextMenuProviders()[contextMenuTableOrder_[static_cast<std::size_t>(event.row)]];
        draft_.*(provider.configMember) = event.checked;
        // 确保整行被选中显示
        ThemedUi::SetTableSelectedIndex(contextMenuTable_, event.row);
        return true;
    }

    bool HandleHotKeyTableEvent(LPARAM lParam) {
        ThemedTableEvent event{};
        if (!ThemedUi::DecodeTableEvent(hotKeyTable_, lParam, event)) {
            return false;
        }
        if (event.kind != ThemedTableEventKind::ActionInvoked) {
            return true;
        }
        if (event.actionId == ID_MAIN_HOTKEY_CAPTURE) {
            TrySetMainHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.mainHotKey));
        } else if (event.actionId == ID_PROCESS_LOCATOR_HOTKEY_CAPTURE) {
            TrySetProcessLocatorHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.processLocatorHotKey));
        } else if (event.actionId == ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE) {
            TrySetCopySelectedPathsHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.copySelectedPathsHotKey));
        }
        return true;
    }

    AppConfig ReadCurrentTabDraft() {
        AppConfig value = config_;
        switch (currentTab_) {
        case TabDisplay: {
            value.showTitle = ThemedUi::IsChecked(showTitle_);
            value.showGroup = ThemedUi::IsChecked(showGroup_);
            value.showTag = ThemedUi::IsChecked(showTag_);
            value.showToolboxButton = ThemedUi::IsChecked(showToolboxButton_);
            value.showSkinButton = ThemedUi::IsChecked(showSkinButton_);
            value.linkNameSingleLine = ThemedUi::IsChecked(linkNameSingleLine_);
            value.showTooltip = ThemedUi::IsChecked(showTooltip_);
            value.groupRight = ThemedUi::IsChecked(groupRight_);
            value.tagRight = ThemedUi::IsChecked(tagRight_);
            value.tagAlign = tagAlignIndex_ == 0 ? L"left" : (tagAlignIndex_ == 2 ? L"right" : L"center");
            auto alpha = ParseInt(GetText(alphaEdit_));
            value.alpha = alpha ? std::max(64, std::min(255, *alpha)) : 255;
            value.groupWidth = ClampNumber(groupWidthEdit_, 40, 240, value.groupWidth);
            value.tagWidth = ClampNumber(tagWidthEdit_, 40, 240, value.tagWidth);
            break;
        }
        case TabBehavior:
            value.autoDock = ThemedUi::IsChecked(autoDock_);
            value.hideWhenInactive = ThemedUi::IsChecked(hideInactive_);
            value.hideAfterLink = ThemedUi::IsChecked(hideAfterLink_);
            value.hideOnStart = ThemedUi::IsChecked(hideOnStart_);
            value.hideNotifyIcon = false;
            value.deleteConfirm = ThemedUi::IsChecked(deleteConfirm_);
            value.saveRunCount = ThemedUi::IsChecked(saveRunCount_);
            value.autoRun = ThemedUi::IsChecked(autoRun_);
            value.loggingEnabled = ThemedUi::IsChecked(loggingEnabled_);
            value.dockDelay = ClampNumber(dockDelayEdit_, 0, 5000, value.dockDelay);
            break;
        case TabContextMenu: {
            ReadContextMenuTableDraft(value);
            break;
        }
        case TabInteraction:
            value.doubleClickToRun = ThemedUi::IsChecked(doubleClick_);
            value.mouseEnterActiveGroup = ThemedUi::IsChecked(enterActiveGroup_);
            value.mouseEnterActiveTag = ThemedUi::IsChecked(enterActiveTag_);
            value.activeGroupDelay = ClampNumber(groupDelayEdit_, 0, 5000, value.activeGroupDelay);
            value.activeTagDelay = ClampNumber(tagDelayEdit_, 0, 5000, value.activeTagDelay);
            break;
        case TabHotKeys:
            value.globalHotKeysEnabled = ThemedUi::IsChecked(globalHotKeysEnabled_);
            value.mainHotKey = draft_.mainHotKey;
            value.processLocatorHotKey = draft_.processLocatorHotKey;
            break;
        case TabLinks:
            value.openDirCommand = GetText(openDirEdit_);
            value.helpUrl = GetText(helpUrlEdit_);
            value.updateUrl = GetText(updateUrlEdit_);
            value.faqUrl = GetText(faqUrlEdit_);
            value.rewardUrl = GetText(rewardUrlEdit_);
            break;
        case TabWebDav:
            value = ReadWebDavDraftFromControls();
            break;
        case TabHttp:
            value = ReadHttpDraftFromControls();
            break;
        case TabBackup:
        default:
            break;
        }
        return value;
    }

    bool TrySetMainHotKey(int key) {
        if (key == 0) {
            draft_.mainHotKey = 0;
            UpdateHotKeyLabels();
            return true;
        }

        const HotKeyAvailability availability = CheckMainHotKeyAvailability(hwnd_, key, CurrentRegisteredMainHotKey());
        if (!availability.available) {
            draft_.mainHotKey = key;
            UpdateHotKeyLabels();
            ShowThemedMessageBox(hwnd_, instance_, theme_, MainHotKeyConflictMessage(key, availability), L"热键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }

        draft_.mainHotKey = key;
        UpdateHotKeyLabels();
        return true;
    }

    int CurrentRegisteredMainHotKey() const {
        return mainHotKeyRegistered_ ? config_.mainHotKey : 0;
    }

    int CurrentRegisteredProcessLocatorHotKey() const {
        return processLocatorHotKeyRegistered_ ? config_.processLocatorHotKey : 0;
    }

    int CurrentRegisteredCopySelectedPathsHotKey() const {
        return copySelectedPathsHotKeyRegistered_ ? config_.copySelectedPathsHotKey : 0;
    }

    bool TrySetProcessLocatorHotKey(int key) {
        if (key == kMainHotKeyDoubleAlt) {
            key = 0;
        }
        const HotKeyAvailability availability = CheckCtrlAltHotKeyAvailability(hwnd_, key, CurrentRegisteredProcessLocatorHotKey());
        if (!availability.available) {
            draft_.processLocatorHotKey = key;
            UpdateHotKeyLabels();
            ShowThemedMessageBox(hwnd_, instance_, theme_, ProcessLocatorHotKeyStatusText(key, availability), L"热键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }
        draft_.processLocatorHotKey = key;
        UpdateHotKeyLabels();
        return true;
    }

    bool TrySetCopySelectedPathsHotKey(int key) {
        if (key == kMainHotKeyDoubleAlt) {
            key = 0;
        }
        const HotKeyAvailability availability = CheckCtrlAltHotKeyAvailability(
            hwnd_, key, CurrentRegisteredCopySelectedPathsHotKey());
        if (!availability.available) {
            draft_.copySelectedPathsHotKey = key;
            UpdateHotKeyLabels();
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, CopySelectedPathsHotKeyStatusText(key, availability),
                L"热键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }
        draft_.copySelectedPathsHotKey = key;
        UpdateHotKeyLabels();
        return true;
    }

    bool ValidateHotKeysBeforeSave() {
        if (!draft_.globalHotKeysEnabled) {
            UpdateHotKeyLabels();
            return true;
        }
        if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
            draft_.mainHotKey != 0 &&
            draft_.mainHotKey == draft_.processLocatorHotKey) {
            UpdateHotKeyLabels();
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"主窗口显隐和进程定位器不能使用同一个快捷键。", L"热键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
            draft_.mainHotKey != 0 &&
            draft_.mainHotKey == draft_.copySelectedPathsHotKey) {
            UpdateHotKeyLabels();
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"主窗口显隐和复制选中项绝对路径不能使用同一个快捷键。", L"热键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (draft_.processLocatorHotKey != 0 &&
            draft_.processLocatorHotKey == draft_.copySelectedPathsHotKey) {
            UpdateHotKeyLabels();
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"进程定位器和复制选中项绝对路径不能使用同一个快捷键。", L"热键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }
        const HotKeyAvailability availability = CheckMainHotKeyAvailability(hwnd_, draft_.mainHotKey, CurrentRegisteredMainHotKey());
        UpdateHotKeyLabels();
        if (!availability.available) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, MainHotKeyConflictMessage(draft_.mainHotKey, availability), L"热键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }

        const HotKeyAvailability locatorAvailability = CheckCtrlAltHotKeyAvailability(
            hwnd_, draft_.processLocatorHotKey, CurrentRegisteredProcessLocatorHotKey());
        UpdateHotKeyLabels();
        if (!locatorAvailability.available) {
            ShowThemedMessageBox(
                hwnd_,
                instance_,
                theme_,
                ProcessLocatorHotKeyStatusText(draft_.processLocatorHotKey, locatorAvailability),
                L"热键冲突",
                MB_OK | MB_ICONWARNING);
            return false;
        }
        const HotKeyAvailability copyAvailability = CheckCtrlAltHotKeyAvailability(
            hwnd_, draft_.copySelectedPathsHotKey, CurrentRegisteredCopySelectedPathsHotKey());
        UpdateHotKeyLabels();
        if (!copyAvailability.available) {
            ShowThemedMessageBox(
                hwnd_,
                instance_,
                theme_,
                CopySelectedPathsHotKeyStatusText(draft_.copySelectedPathsHotKey, copyAvailability),
                L"热键冲突",
                MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    AppConfig ReadWebDavDraftFromControls() {
        AppConfig value = config_;
        value.webDavEnabled = ThemedUi::IsChecked(webDavEnabled_);
        value.webDavUrl = GetText(webDavUrlEdit_);
        value.webDavRemotePath = GetText(webDavRemotePathEdit_);
        value.webDavUserName = GetText(webDavUserNameEdit_);
        value.webDavKeepCount = ClampNumber(webDavKeepCountEdit_, 1, 100, value.webDavKeepCount);
        value.webDavLastSyncAt = draft_.webDavLastSyncAt;
        if (Trim(value.webDavRemotePath).empty()) {
            value.webDavRemotePath = L"/Quattro/backups/";
        }
        return value;
    }

    void UpdateWebDavLastSyncLabel() {
        if (!webDavLastSyncLabel_) {
            return;
        }
        const std::wstring text = FormatWebDavLastSyncText(draft_.webDavLastSyncAt);
        SetWindowTextW(webDavLastSyncLabel_, text.c_str());
        ShowWindow(webDavLastSyncLabel_, text.empty() ? SW_HIDE : SW_SHOW);
    }

    void MarkWebDavSyncedNow(bool importedData) {
        AppConfig next = ReadWebDavDraftFromControls();
        next.webDavLastSyncAt = CurrentTodoTimestamp();
        draft_ = next;
        config_ = next;
        if (importedData) {
            importedData_ = true;
        }
        UpdateWebDavLastSyncLabel();
        if (applyCallback_) {
            mainHotKeyRegistered_ = applyCallback_(config_, importedData_);
            processLocatorHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.processLocatorHotKey != 0;
            copySelectedPathsHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.copySelectedPathsHotKey != 0;
            importedData_ = false;
        }
    }

    bool SaveWebDavPasswordIfNeeded(const AppConfig& value) {
        const std::wstring password = GetText(webDavPasswordEdit_);
        if (password.empty()) {
            return true;
        }
        std::wstring error;
        if (!WebDavCredentialService::SavePassword(value, password, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    void ClearWebDavPassword() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig value = ReadWebDavDraftFromControls();
        std::wstring error;
        if (WebDavCredentialService::DeletePassword(value, error)) {
            SetWindowTextW(webDavPasswordEdit_, L"");
            ShowToast(L"WebDAV 密码已清除。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
        }
    }

    void ResetContextMenu() {
        if (contextMenuRefreshBusy_ || contextMenuIconLoadBusy_) {
            ShowToast(L"Windows 菜单正在刷新，请稍候。", ThemedToastRole::Info);
            return;
        }
        if (!resetContextMenuCallback_) {
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"当前无法访问右键菜单缓存。", L"重置右键菜单", MB_OK | MB_ICONWARNING);
            return;
        }
        const int answer = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
            L"重置后将关闭所有右键菜单跟踪开关，并清除全部启动项缓存的菜单列表、启用状态和菜单图标。是否继续？",
            L"重置右键菜单",
            MB_YESNO | MB_ICONWARNING);
        if (answer != IDYES) {
            return;
        }
        if (resetContextMenuCallback_()) {
            for (const auto& provider : TrackedContextMenuProviders()) {
                config_.*(provider.configMember) = false;
                draft_.*(provider.configMember) = false;
            }
            AddContextMenuTableRows();
            ShowToast(L"右键菜单已重置，跟踪开关与缓存均已恢复默认。", ThemedToastRole::Success, 5000);
        } else {
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"右键菜单重置失败，请确认缓存目录可写。", L"重置右键菜单", MB_OK | MB_ICONWARNING);
        }
    }

    ShellContextMenuTrackingOptions ContextMenuTrackingDraft() const {
        ShellContextMenuTrackingOptions tracking;
        for (const auto& provider : TrackedContextMenuProviders()) {
            tracking.*(provider.trackingMember) = draft_.*(provider.configMember);
        }
        return tracking;
    }

    void SetContextMenuRefreshBusy(bool busy) {
        contextMenuRefreshBusy_ = busy;
        UpdateContextMenuRefreshButtonState();
    }

    std::wstring ContextMenuRefreshResultText(const ShellContextMenuRefreshResult& result) const {
        std::wstring message =
            L"扫描启动项: " + std::to_wstring(result.totalLinks) +
            L"\n成功: " + std::to_wstring(result.succeededLinks) +
            L"\n跳过: " + std::to_wstring(result.skippedLinks) +
            L"\n失败: " + std::to_wstring(result.failures.size()) +
            L"\n更新菜单项: " + std::to_wstring(result.menuItemCount);
        const std::size_t detailCount = std::min<std::size_t>(result.failures.size(), 5);
        for (std::size_t index = 0; index < detailCount; ++index) {
            const auto& failure = result.failures[index];
            message += L"\n\n";
            message += failure.linkName.empty() ? L"刷新服务" : failure.linkName;
            message += L": " + failure.message;
        }
        if (result.failures.size() > detailCount) {
            message += L"\n\n其余 " + std::to_wstring(result.failures.size() - detailCount) + L" 项请查看日志。";
        }
        return message;
    }

    void CompleteContextMenuRefresh() {
        std::optional<ShellContextMenuRefreshResult> result;
        {
            std::lock_guard lock(contextMenuRefreshMutex_);
            result = std::move(contextMenuRefreshResult_);
            contextMenuRefreshResult_.reset();
        }
        if (contextMenuRefreshThread_.joinable()) {
            contextMenuRefreshThread_.join();
        }
        SetContextMenuRefreshBusy(false);
        if (!result) {
            StartContextMenuIconLoad(true);
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"刷新线程未返回结果。", L"从Windows菜单刷新", MB_OK | MB_ICONWARNING);
            return;
        }
        if (result->cancelled) {
            ShowToast(L"已取消 Windows 菜单刷新。", ThemedToastRole::Info);
            return;
        }
        if (result->succeededLinks > 0 && contextMenuRefreshApplyCallback_) {
            contextMenuRefreshApplyCallback_(*result);
        }
        StartContextMenuIconLoad(true);
        if (result->failures.empty()) {
            const std::wstring message =
                L"已刷新 " + std::to_wstring(result->succeededLinks) +
                L" 个启动项，更新 " + std::to_wstring(result->menuItemCount) + L" 个菜单项。";
            ShowToast(message, ThemedToastRole::Success, 5000);
            return;
        }
        ShowThemedMessageBox(
            hwnd_, instance_, theme_, ContextMenuRefreshResultText(*result),
            L"从Windows菜单刷新", MB_OK | MB_ICONWARNING);
    }

    void RefreshContextMenuFromNative() {
        if (contextMenuRefreshBusy_ || contextMenuIconLoadBusy_) {
            ShowToast(L"Windows 菜单正在刷新，请稍候。", ThemedToastRole::Info);
            return;
        }
        const ShellContextMenuTrackingOptions tracking = ContextMenuTrackingDraft();
        if (!tracking.Any()) {
            StartContextMenuIconLoad(true);
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"没有启用需要跟踪的工具。", L"从Windows菜单刷新", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (contextMenuLinks_.empty()) {
            StartContextMenuIconLoad(true);
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"当前没有可刷新的启动项。", L"从Windows菜单刷新", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (contextMenuRefreshThread_.joinable()) {
            contextMenuRefreshThread_.join();
        }

        ShellContextMenuRefreshRequest request{contextMenuLinks_, tracking};
        const HWND target = hwnd_;
        const SettingsContextMenuRefreshRunner runner = contextMenuRefreshRunner_;
        SetContextMenuRefreshBusy(true);
        ShowToast(L"正在扫描 Windows 原生菜单...", ThemedToastRole::Info, 5000);
        contextMenuRefreshThread_ = std::jthread(
            [this, target, request = std::move(request), runner](std::stop_token stopToken) mutable {
                ShellContextMenuRefreshResult result;
                try {
                    result = runner
                        ? runner(request, stopToken)
                        : ShellContextMenuRefreshService().Refresh(request, stopToken);
                } catch (...) {
                    result.tracking = request.tracking;
                    result.totalLinks = static_cast<int>(request.links.size());
                    result.failures.push_back(ShellContextMenuRefreshFailure{
                        0, L"", L"刷新过程中发生未处理异常。"});
                }
                {
                    std::lock_guard lock(contextMenuRefreshMutex_);
                    contextMenuRefreshResult_ = std::move(result);
                }
                PostMessageW(target, WM_CONTEXT_MENU_REFRESH_DONE, 0, 0);
            });
    }

    bool PrepareWebDavOperation(AppConfig& value) {
        value = ReadWebDavDraftFromControls();
        if (!SaveWebDavPasswordIfNeeded(value)) {
            return false;
        }
        return true;
    }

    void SetWebDavBusy(bool busy, SettingsWebDavOperation operation = SettingsWebDavOperation::Test) {
        webDavBusy_ = busy;
        const ThemedUi ui = MakeUi();
        if (webDavUploadButton_) {
            ui.SetEnabled(webDavUploadButton_, !busy);
            SetWindowTextW(webDavUploadButton_, busy && operation == SettingsWebDavOperation::Upload ? L"上传中..." : L"上传到云端");
        }
        if (webDavDownloadButton_) {
            ui.SetEnabled(webDavDownloadButton_, !busy);
            const bool downloadBusy = operation == SettingsWebDavOperation::List || operation == SettingsWebDavOperation::Download;
            SetWindowTextW(webDavDownloadButton_, busy && downloadBusy ? L"处理中..." : L"从云端下载");
        }
        if (webDavTestButton_) {
            ui.SetEnabled(webDavTestButton_, !busy);
            SetWindowTextW(webDavTestButton_, busy && operation == SettingsWebDavOperation::Test ? L"测试中..." : L"测试连接");
        }
        if (webDavClearPasswordButton_) {
            ui.SetEnabled(webDavClearPasswordButton_, !busy);
        }
        if (okButton_) {
            ui.SetEnabled(okButton_, !busy);
        }
        if (cancelButton_) {
            ui.SetEnabled(cancelButton_, !busy);
        }
        if (applyButton_) {
            ui.SetEnabled(applyButton_, !busy);
        }
    }

    bool EnsureWebDavIdle() const {
        if (webDavBusy_) {
            MessageBeep(MB_ICONINFORMATION);
            return false;
        }
        return true;
    }

    void UploadWebDavBackup() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig config;
        if (!PrepareWebDavOperation(config)) {
            return;
        }
        SetWebDavBusy(true, SettingsWebDavOperation::Upload);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        std::thread([target, appDirectory, config]() {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::Upload;
            WebDavBackupService service(appDirectory, config);
            result->report = service.UploadBackup();
            result->ok = result->report.ok;
            result->message = result->report.message;
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void DownloadWebDavBackup() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig config;
        if (!PrepareWebDavOperation(config)) {
            return;
        }
        SetWebDavBusy(true, SettingsWebDavOperation::List);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        std::thread([target, appDirectory, config]() {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::List;
            result->config = config;
            WebDavBackupService service(appDirectory, config);
            std::wstring error;
            result->ok = service.ListBackups(result->backups, error);
            result->message = result->ok ? std::wstring{} : error;
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void DownloadSelectedWebDavBackup(const AppConfig& config, const std::wstring& fileName) {
        SetWebDavBusy(true, SettingsWebDavOperation::Download);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        std::thread([target, appDirectory, config, fileName]() {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::Download;
            WebDavBackupService service(appDirectory, config);
            result->report = service.DownloadAndImportMerge(fileName);
            result->ok = result->report.ok;
            result->message = result->report.importReport.message.empty()
                ? result->report.message
                : FormatConfigPackageReportText(result->report.importReport);
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void ContinueWebDavDownloadSelection(const SettingsWebDavResult& result) {
        if (!result.ok) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, result.message, L"从云端下载", MB_OK | MB_ICONWARNING);
            return;
        }
        if (result.backups.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"远端目录中没有可用的 .q4cfg 备份。", L"从云端下载", MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring fileName = result.backups.front().name;
        WebDavBackupSelectionDialog selectionDialog(hwnd_, instance_, theme_, result.backups, fileName);
        if (!selectionDialog.Run()) {
            return;
        }
        auto selectedBackup = std::find_if(result.backups.begin(), result.backups.end(), [&](const WebDavRemoteFile& backup) {
            return backup.name == fileName;
        });
        if (selectedBackup == result.backups.end()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"未找到所选 WebDAV 备份，请重新选择。", L"从云端下载", MB_OK | MB_ICONWARNING);
            return;
        }

        const int confirm = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
            FormatBackupConfirmationText(*selectedBackup),
            L"从云端下载",
            MB_OKCANCEL | MB_ICONINFORMATION);
        if (confirm != IDOK) {
            return;
        }

        DownloadSelectedWebDavBackup(result.config, fileName);
    }

    void TestWebDavConnection() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig value = ReadWebDavDraftFromControls();
        value.webDavEnabled = true;
        std::wstring password = GetText(webDavPasswordEdit_);
        SetWebDavBusy(true, SettingsWebDavOperation::Test);
        const HWND target = hwnd_;
        std::thread([target, value, password]() mutable {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::Test;
            std::wstring error;
            if (password.empty() && !WebDavCredentialService::LoadPassword(value, password, error)) {
                result->message = error;
            } else {
                WebDavClient client(value, password);
                result->ok = client.TestConnection();
                result->message = result->ok ? L"WebDAV 连接成功。" : client.lastError();
            }
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void HandleWebDavResult(std::unique_ptr<SettingsWebDavResult> result) {
        if (!result) {
            return;
        }
        SetWebDavBusy(false);
        switch (result->operation) {
        case SettingsWebDavOperation::Test:
            if (result->ok) {
                ShowToast(result->message, ThemedToastRole::Success);
            } else {
                ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
            }
            return;
        case SettingsWebDavOperation::Upload:
            if (result->ok) {
                MarkWebDavSyncedNow(false);
                ShowToast(result->message.empty() ? L"已上传到云端。" : result->message, ThemedToastRole::Success);
            } else {
                ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"上传到云端", MB_OK | MB_ICONWARNING);
            }
            return;
        case SettingsWebDavOperation::List:
            ContinueWebDavDownloadSelection(*result);
            return;
        case SettingsWebDavOperation::Download:
            if (result->ok) {
                MarkWebDavSyncedNow(true);
                ShowToast(result->message.empty() ? L"已从云端下载并合并。" : result->message, ThemedToastRole::Success);
            } else {
                ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"从云端下载", MB_OK | MB_ICONWARNING);
            }
            return;
        }
    }

    void ExportConfigPackage() {
        std::wstring targetPath;
        if (!SelectSavePath(hwnd_,
                (appDirectory_ / ConfigPackageFileName()).wstring(),
                L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0",
                L"q4cfg",
                targetPath)) {
            return;
        }
        ConfigPackageOptions options;
        options.includeConfig = true;
        options.includeData = true;
        options.includeUrlIcons = true;
        ConfigPackageService service(appDirectory_);
        const ConfigPackageReport report = service.ExportPackage(targetPath, options);
        if (report.ok) {
            ShowToast(L"配置包已导出。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, FormatConfigPackageReportText(report), L"导出配置包", MB_OK | MB_ICONWARNING);
        }
    }

    void ImportConfigPackage() {
        std::wstring packagePath;
        if (!SelectOpenPath(hwnd_,
                L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0",
                L"q4cfg",
                packagePath)) {
            return;
        }
        const int confirm = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
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
        const ConfigPackageReport report = service.ImportPackageMerge(packagePath, options);
        if (report.ok) {
            importedData_ = true;
            ShowToast(L"配置包已合并导入。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, FormatConfigPackageReportText(report), L"合并导入配置包", MB_OK | MB_ICONWARNING);
        }
    }

    void ExportTodosJson() {
        StorageService storage(appDirectory_);
        const AppModel model = storage.Load();
        TodoExportOptions options;
        options.includeCompleted = !todoIncludeCompleted_ || ThemedUi::IsChecked(todoIncludeCompleted_);
        options.includeDisabled = !todoIncludeDisabled_ || ThemedUi::IsChecked(todoIncludeDisabled_);
        options.onlyFuture = todoOnlyFuture_ && ThemedUi::IsChecked(todoOnlyFuture_);
        std::wstring targetPath;
        if (!SelectSavePath(hwnd_,
                (appDirectory_ / TodoJsonFileName()).wstring(),
                L"JSON 文件 (*.json)\0*.json\0所有文件\0*.*\0",
                L"json",
                targetPath)) {
            return;
        }
        if (!SaveUtf8File(targetPath, BuildTodoExportJson(model, options))) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"写入待办 JSON 文件失败。", L"导出待办 JSON", MB_OK | MB_ICONWARNING);
            return;
        }
        ShowToast(L"待办 JSON 导出完成。", ThemedToastRole::Success);
    }

    void ImportTodosJson() {
        std::wstring jsonPath;
        if (!SelectOpenPath(hwnd_,
                L"JSON 文件 (*.json)\0*.json\0所有文件\0*.*\0",
                L"json",
                jsonPath)) {
            return;
        }
        const int confirm = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
            L"将把 JSON 中的待办事项导入到当前数据；缺失的分组或待办标签会自动创建。",
            L"导入待办 JSON",
            MB_OKCANCEL | MB_ICONINFORMATION);
        if (confirm != IDOK) {
            return;
        }
        const TodoJsonImportReport report = ImportTodoJsonFile(appDirectory_, jsonPath);
        if (report.ok) {
            importedData_ = true;
            ShowToast(report.message.empty() ? L"导入完成。" : report.message, ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, report.message.empty() ? L"导入失败。" : report.message,
                L"导入待办 JSON", MB_OK | MB_ICONWARNING);
        }
    }

    AppConfig ReadHttpDraftFromControls() {
        AppConfig value = config_;
        value.httpServerEnabled = httpServer_ && httpServer_->IsRunning();
        value.httpServerAutoStart = httpServerAutoStart_ && ThemedUi::IsChecked(httpServerAutoStart_);
        value.httpServerLanAccess = true;
        value.httpServerPort = ParseHttpPortText(GetText(httpServerAddressEdit_), value.httpServerPort);
        value.httpServerRootPath = GetText(httpServerRootEdit_);
        if (Trim(value.httpServerRootPath).empty()) {
            value.httpServerRootPath = LocalHttpServerService::DefaultRootPath(httpRootBaseDirectory_).wstring();
        }
        return value;
    }

    std::wstring CurrentHttpAddress(bool trailingSlash) {
        const AppConfig value = ReadHttpDraftFromControls();
        return HttpAddressText(value.httpServerLanAccess, value.httpServerPort, trailingSlash);
    }

    void UpdateHttpAddressField(bool trailingSlash = false) {
        if (!httpServerAddressEdit_) {
            return;
        }
        SetWindowTextW(httpServerAddressEdit_, CurrentHttpAddress(trailingSlash).c_str());
    }

    void UpdateHttpButtons() {
        const bool running = httpServer_ && httpServer_->IsRunning();
        const ThemedUi ui = MakeUi();
        if (httpStartButton_) {
            ui.SetEnabled(httpStartButton_, !running);
        }
        if (httpStopButton_) {
            ui.SetEnabled(httpStopButton_, running);
        }
        if (httpServerAddressEdit_) {
            windowUi_->SetEditEnabled(httpServerAddressEdit_, !running);
        }
        if (httpServerRootEdit_) {
            windowUi_->SetEditEnabled(httpServerRootEdit_, !running);
        }
        if (httpBrowseRootButton_) {
            ui.SetEnabled(httpBrowseRootButton_, !running);
        }
    }

    void UpdateHttpStatusLabel() {
        if (!httpServerStatusTag_ || !httpServerStatusDetail_) {
            return;
        }
        std::wstring tag;
        std::wstring detail;
        const bool running = httpServer_ && httpServer_->IsRunning();
        if (running) {
            const auto& options = httpServer_->options();
            tag = L"运行中";
            detail = HttpAddressText(options.lanAccess, options.port, true);
        } else if (httpServer_ && !Trim(httpServer_->lastError()).empty()) {
            tag = L"启动异常";
            detail = httpServer_->lastError();
        } else if (!httpServer_) {
            tag = L"异常";
            detail = L"HTTP 服务对象不可用。";
        } else {
            tag = L"未启动";
            detail = L"服务未启动。";
        }
        SetWindowTextW(httpServerStatusTag_, tag.c_str());
        MakeUi().SetStatusBadgeRole(
            httpServerStatusTag_, running ? ThemedStatusRole::Success : ThemedStatusRole::Danger);
        SetWindowTextW(httpServerStatusDetail_, detail.c_str());
        UpdateHttpButtons();
    }

    void BrowseHttpRoot() {
        if (httpServer_ && httpServer_->IsRunning()) {
            return;
        }
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            return;
        }
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    SetWindowTextW(httpServerRootEdit_, path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    void OpenHttpRootDirectory() {
        const AppConfig value = ReadHttpDraftFromControls();
        const auto options = LocalHttpServerService::OptionsFromConfig(value, httpRootBaseDirectory_);
        std::error_code ec;
        std::filesystem::create_directories(options.rootPath, ec);
        if (ec) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"创建 Web Root 失败。", L"HTTP 服务", MB_OK | MB_ICONWARNING);
            return;
        }
        ShellExecuteW(hwnd_, L"open", options.rootPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenHttpConfigDirectory() {
        const AppConfig value = ReadHttpDraftFromControls();
        const auto options = LocalHttpServerService::OptionsFromConfig(value, httpRootBaseDirectory_);
        std::wstring error;
        if (!LocalHttpServerService::EnsureDetailConfig(options.rootPath, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"HTTP 服务", MB_OK | MB_ICONWARNING);
            return;
        }
        const std::filesystem::path configDirectory = LocalHttpServerService::DetailConfigDirectory();
        ShellExecuteW(hwnd_, L"open", configDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenHttpHome() {
        const std::wstring url = httpServer_ && httpServer_->IsRunning()
            ? HttpAddressText(httpServer_->options().lanAccess, httpServer_->options().port, true)
            : CurrentHttpAddress(true);
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

    void CopyHttpUrl() {
        const std::wstring url = httpServer_ && httpServer_->IsRunning()
            ? HttpAddressText(httpServer_->options().lanAccess, httpServer_->options().port, true)
            : CurrentHttpAddress(true);
        if (!OpenClipboard(hwnd_)) {
            ShowToast(L"复制失败，剪贴板被其他程序占用。", ThemedToastRole::Danger);
            return;
        }
        EmptyClipboard();
        const SIZE_T bytes = (url.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool copied = false;
        if (memory) {
            void* target = GlobalLock(memory);
            if (target) {
                memcpy(target, url.c_str(), bytes);
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
            ShowToast(L"访问地址已复制到剪贴板。", ThemedToastRole::Success);
        } else {
            ShowToast(L"复制失败。", ThemedToastRole::Danger);
        }
    }

    void StartHttpServerFromDialog(bool restart) {
        if (!httpServer_) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"HTTP 服务对象不可用。", L"HTTP 服务", MB_OK | MB_ICONWARNING);
            return;
        }
        AppConfig value = ReadHttpDraftFromControls();
        value.httpServerEnabled = true;
        std::wstring error;
        const auto options = LocalHttpServerService::OptionsFromConfig(value, httpRootBaseDirectory_);
        const bool ok = restart ? httpServer_->Restart(options, error) : httpServer_->Start(options, error);
        UpdateHttpStatusLabel();
        if (ok) {
            ShowToast(L"HTTP 服务已启动。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"HTTP 服务", MB_OK | MB_ICONWARNING);
        }
    }

    void StopHttpServerFromDialog() {
        if (!httpServer_) {
            return;
        }
        httpServer_->Stop();
        UpdateHttpStatusLabel();
        ShowToast(L"HTTP 服务已停止。", ThemedToastRole::Info);
    }

    bool CommitSettings(bool closeAfterCommit) {
        if (webDavBusy_) {
            ShowToast(L"WebDAV 操作正在进行，请稍候完成。", ThemedToastRole::Warning);
            return false;
        }
        AppConfig next = ReadCurrentTabDraft();
        if (currentTab_ == TabHotKeys) {
            draft_.globalHotKeysEnabled = next.globalHotKeysEnabled;
        }
        if (currentTab_ == TabHotKeys && !ValidateHotKeysBeforeSave()) {
            return false;
        }
        if (currentTab_ == TabWebDav && !SaveWebDavPasswordIfNeeded(next)) {
            return false;
        }

        config_ = next;
        if (!closeAfterCommit && applyCallback_) {
            mainHotKeyRegistered_ = applyCallback_(config_, importedData_);
            processLocatorHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.processLocatorHotKey != 0;
            copySelectedPathsHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.copySelectedPathsHotKey != 0;
            importedData_ = false;
            UpdateHotKeyLabels();
            UpdateHttpStatusLabel();
            ShowToast(L"设置已应用。", ThemedToastRole::Success);
        }
        accepted_ = true;
        return true;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact,
                client.right - client.left, client.bottom - client.top);
            windowUi_->SetDpiChangedCallback([this](UINT) {
                if (!contextMenuTable_) {
                    return;
                }
                RebuildContextMenuImageList();
                AddContextMenuTableRows();
            });
            CreateTabs();
            const ThemedUi settingsUi = MakeUi();
            const DialogLayoutMetrics& behaviorLayout = settingsUi.layout();
            const ThemedFormLayout behaviorForm(settingsUi);
            const int settingsClientWidth = settingsUi.clientWidth();
            const int pageLeft = behaviorLayout.contentInsetX;
            const int pageWidth = settingsClientWidth - behaviorLayout.contentInsetX * 2;
            const int pageTop = tabStripRect_.bottom + behaviorLayout.sectionGap;

            const int behaviorFrameLeft = behaviorLayout.contentInsetX;
            const int behaviorFrameRight = settingsClientWidth - behaviorLayout.contentInsetX;
            const ThemedContentInsets groupInsets = settingsUi.groupBoxInsets();
            const int behaviorFrameGap = behaviorLayout.sectionGap;
            const int behaviorFrameWidth = behaviorFrameRight - behaviorFrameLeft;
            const int behaviorCheckHeight = settingsUi.checkBoxHeight();
            const int behaviorContentLeft = behaviorFrameLeft + groupInsets.left;
            const int behaviorContentWidth = behaviorFrameRight - behaviorFrameLeft - groupInsets.left - groupInsets.right;
            const int behaviorColumnGap = behaviorLayout.controlGapX * 2;
            const int behaviorColumnWidth = (behaviorContentWidth - behaviorColumnGap) / 2;
            const int behaviorLeft = behaviorContentLeft;
            const int behaviorRight = behaviorContentLeft + behaviorColumnWidth + behaviorColumnGap;
            const int behaviorCheckWidth = behaviorColumnWidth;

            const ThemedSectionGeometry displayElementsSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND displayElementsGroup = AddSectionFrame(TabDisplay, L"界面元素", displayElementsSection.frame);
            const int displayElementsFirstY = behaviorForm.sectionItemY(displayElementsSection, 0, behaviorCheckHeight);
            const int displayElementsSecondY = behaviorForm.sectionItemY(displayElementsSection, 1, behaviorCheckHeight);
            const int displayElementsThirdY = behaviorForm.sectionItemY(displayElementsSection, 2, behaviorCheckHeight);
            showTitle_ = CheckBox(TabDisplay, 101, L"显示标题栏", behaviorLeft, displayElementsFirstY, draft_.showTitle, behaviorCheckWidth);
            showGroup_ = CheckBox(TabDisplay, 102, L"显示分组栏", behaviorRight, displayElementsFirstY, draft_.showGroup, behaviorCheckWidth);
            showTag_ = CheckBox(TabDisplay, 103, L"显示标签栏", behaviorLeft, displayElementsSecondY, draft_.showTag, behaviorCheckWidth);
            showToolboxButton_ = CheckBox(TabDisplay, 115, L"显示工具箱按钮", behaviorRight, displayElementsSecondY, draft_.showToolboxButton, behaviorCheckWidth);
            showSkinButton_ = CheckBox(TabDisplay, 121, L"显示主题按钮", behaviorLeft, displayElementsThirdY, draft_.showSkinButton, behaviorCheckWidth);
            showTooltip_ = CheckBox(TabDisplay, 119, L"显示提示", behaviorRight, displayElementsThirdY, draft_.showTooltip, behaviorCheckWidth);
            ThemedUi::BindGroupChildren(displayElementsGroup, {showTitle_, showGroup_, showTag_, showToolboxButton_, showSkinButton_, showTooltip_});

            const ThemedSectionGeometry displayLayoutSection = behaviorForm.section(
                behaviorFrameLeft, displayElementsSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit})});
            HWND displayLayoutGroup = AddSectionFrame(TabDisplay, L"布局与外观", displayLayoutSection.frame);
            const int displayLayoutFirstY = behaviorForm.sectionItemY(displayLayoutSection, 0, behaviorCheckHeight);
            const int displayLayoutSecondY = behaviorForm.sectionItemY(displayLayoutSection, 1, behaviorCheckHeight);
            linkNameSingleLine_ = CheckBox(TabDisplay, 118, L"启动项名称单行", behaviorLeft, displayLayoutFirstY, draft_.linkNameSingleLine, behaviorCheckWidth);
            groupRight_ = CheckBox(TabDisplay, 120, L"分组栏在右侧", behaviorRight, displayLayoutFirstY, draft_.groupRight, behaviorCheckWidth);
            tagRight_ = CheckBox(TabDisplay, 122, L"标签栏在右侧", behaviorLeft, displayLayoutSecondY, draft_.tagRight, behaviorCheckWidth);

            const int displayLabelWidth = behaviorForm.labelWidthForTexts({L"透明度", L"标签文字", L"分组宽度", L"标签宽度"});
            const int displayFieldWidth = behaviorColumnWidth - displayLabelWidth - behaviorLayout.labelGap;
            const int displayThirdLabelY = behaviorForm.sectionItemY(displayLayoutSection, 2, settingsUi.labelHeight());
            const int displayThirdFieldY = behaviorForm.sectionItemY(displayLayoutSection, 2, settingsUi.editHeight());
            HWND alphaLabel = Label(TabDisplay, L"透明度", behaviorLeft, displayThirdLabelY, displayLabelWidth);
            alphaEdit_ = NumberEdit(TabDisplay, 201, behaviorLeft + displayLabelWidth + behaviorLayout.labelGap, displayThirdFieldY, displayFieldWidth, draft_.alpha);
            HWND tagAlignLabel = Label(TabDisplay, L"标签文字", behaviorRight, displayThirdLabelY, displayLabelWidth);
            const int alignButtonWidth = settingsUi.tabButtonWidth(L"左");
            const int alignX = behaviorRight + displayLabelWidth + behaviorLayout.labelGap;
            tagAlignLeft_ = settingsUi.TabButton(ID_TAG_ALIGN_LEFT, L"左", alignX, ContentY(displayThirdFieldY), alignButtonWidth, false);
            tagAlignCenter_ = settingsUi.TabButton(ID_TAG_ALIGN_CENTER, L"中", alignX + alignButtonWidth, ContentY(displayThirdFieldY), alignButtonWidth, true);
            tagAlignRight_ = settingsUi.TabButton(ID_TAG_ALIGN_RIGHT, L"右", alignX + alignButtonWidth * 2, ContentY(displayThirdFieldY), alignButtonWidth, false);
            AddTabChild(tagAlignLeft_, TabDisplay);
            AddTabChild(tagAlignCenter_, TabDisplay);
            AddTabChild(tagAlignRight_, TabDisplay);
            SelectTagAlign();
            const int displayFourthLabelY = behaviorForm.sectionItemY(displayLayoutSection, 3, settingsUi.labelHeight());
            const int displayFourthFieldY = behaviorForm.sectionItemY(displayLayoutSection, 3, settingsUi.editHeight());
            HWND groupWidthLabel = Label(TabDisplay, L"分组宽度", behaviorLeft, displayFourthLabelY, displayLabelWidth);
            groupWidthEdit_ = NumberEdit(TabDisplay, ID_GROUP_WIDTH, behaviorLeft + displayLabelWidth + behaviorLayout.labelGap, displayFourthFieldY, displayFieldWidth, draft_.groupWidth);
            HWND tagWidthLabel = Label(TabDisplay, L"标签宽度", behaviorRight, displayFourthLabelY, displayLabelWidth);
            tagWidthEdit_ = NumberEdit(TabDisplay, ID_TAG_WIDTH, behaviorRight + displayLabelWidth + behaviorLayout.labelGap, displayFourthFieldY, displayFieldWidth, draft_.tagWidth);
            ThemedUi::BindGroupChildren(displayLayoutGroup, {
                linkNameSingleLine_, groupRight_, tagRight_, alphaLabel, alphaEdit_, tagAlignLabel,
                tagAlignLeft_, tagAlignCenter_, tagAlignRight_, groupWidthLabel, groupWidthEdit_, tagWidthLabel, tagWidthEdit_});

            const int behaviorDelayLabelWidth = behaviorForm.labelWidthForText(L"停靠延迟");
            const int behaviorUnitWidth = behaviorForm.labelWidthForText(L"ms");
            const int behaviorFieldWidth = behaviorColumnWidth - behaviorDelayLabelWidth - behaviorLayout.labelGap
                - behaviorLayout.controlGapX - behaviorUnitWidth;
            const int behaviorWindowFrameTop = tabStripRect_.bottom + behaviorLayout.sectionGap;
            const ThemedSectionGeometry behaviorWindowSection = behaviorForm.section(
                behaviorFrameLeft, behaviorWindowFrameTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox, ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND behaviorWindowGroup = AddSectionFrame(TabBehavior, L"窗口行为", behaviorWindowSection.frame);
            const int behaviorWindowCheckY = behaviorForm.sectionItemY(behaviorWindowSection, 0, behaviorCheckHeight);
            const int behaviorWindowLabelY = behaviorForm.sectionItemY(behaviorWindowSection, 0, settingsUi.labelHeight());
            const int behaviorWindowEditY = behaviorForm.sectionItemY(behaviorWindowSection, 0, settingsUi.editHeight());
            autoDock_ = CheckBox(TabBehavior, 105, L"贴边自动隐藏", behaviorLeft, behaviorWindowCheckY, draft_.autoDock, behaviorCheckWidth);
            HWND dockDelayLabel = Label(TabBehavior, L"停靠延迟", behaviorRight, behaviorWindowLabelY, behaviorDelayLabelWidth);
            dockDelayEdit_ = NumberEdit(
                TabBehavior,
                ID_DOCK_DELAY,
                behaviorRight + behaviorDelayLabelWidth + behaviorLayout.labelGap,
                behaviorWindowEditY,
                behaviorFieldWidth,
                draft_.dockDelay);
            HWND dockDelayUnit = Label(
                TabBehavior,
                L"ms",
                behaviorRight + behaviorDelayLabelWidth + behaviorLayout.labelGap + behaviorFieldWidth + behaviorLayout.controlGapX,
                behaviorWindowLabelY,
                behaviorUnitWidth);
            hideInactive_ = CheckBox(
                TabBehavior, 106, L"失焦隐藏", behaviorLeft,
                behaviorForm.sectionItemY(behaviorWindowSection, 1, behaviorCheckHeight),
                draft_.hideWhenInactive, behaviorCheckWidth);
            ThemedUi::BindGroupChildren(behaviorWindowGroup, {autoDock_, dockDelayLabel, dockDelayEdit_, dockDelayUnit, hideInactive_});

            const int behaviorRunFrameTop = behaviorWindowSection.frame.bottom + behaviorFrameGap;
            const ThemedSectionGeometry behaviorRunSection = behaviorForm.section(
                behaviorFrameLeft, behaviorRunFrameTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND behaviorRunGroup = AddSectionFrame(TabBehavior, L"运行与数据", behaviorRunSection.frame);
            const int behaviorRunFirstY = behaviorForm.sectionItemY(behaviorRunSection, 0, behaviorCheckHeight);
            const int behaviorRunColumnWidth = behaviorContentWidth / 3;
            hideAfterLink_ = CheckBox(TabBehavior, 107, L"启动项运行后隐藏", behaviorLeft, behaviorRunFirstY, draft_.hideAfterLink, behaviorRunColumnWidth);
            saveRunCount_ = CheckBox(TabBehavior, 112, L"记录运行次数", behaviorLeft + behaviorRunColumnWidth, behaviorRunFirstY, draft_.saveRunCount, behaviorRunColumnWidth);
            deleteConfirm_ = CheckBox(TabBehavior, 111, L"删除前确认", behaviorLeft + behaviorRunColumnWidth * 2, behaviorRunFirstY, draft_.deleteConfirm, behaviorContentWidth - behaviorRunColumnWidth * 2);
            ThemedUi::BindGroupChildren(behaviorRunGroup, {hideAfterLink_, saveRunCount_, deleteConfirm_});

            const int behaviorSystemFrameTop = behaviorRunSection.frame.bottom + behaviorFrameGap;
            const ThemedSectionGeometry behaviorSystemSection = behaviorForm.section(
                behaviorFrameLeft, behaviorSystemFrameTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND behaviorSystemGroup = AddSectionFrame(TabBehavior, L"系统集成", behaviorSystemSection.frame);
            const int behaviorSystemFirstY = behaviorForm.sectionItemY(behaviorSystemSection, 0, behaviorCheckHeight);
            hideOnStart_ = CheckBox(TabBehavior, 116, L"启动后隐藏", behaviorLeft, behaviorSystemFirstY, draft_.hideOnStart, behaviorRunColumnWidth);
            autoRun_ = CheckBox(TabBehavior, 117, L"开机启动", behaviorLeft + behaviorRunColumnWidth, behaviorSystemFirstY, draft_.autoRun, behaviorRunColumnWidth);
            loggingEnabled_ = CheckBox(TabBehavior, ID_LOGGING_ENABLED, L"启用日志", behaviorLeft + behaviorRunColumnWidth * 2, behaviorSystemFirstY, draft_.loggingEnabled, behaviorContentWidth - behaviorRunColumnWidth * 2);
            ThemedUi::BindGroupChildren(behaviorSystemGroup, {hideOnStart_, autoRun_, loggingEnabled_});

            // 表格固定显示 7 行高度，更多 provider 由 ListView 垂直滚动承载，
            // 对话框高度不随 provider 数量增长。
            constexpr int kContextMenuVisibleRows = 7;
            const int contextMenuTableHeight = settingsUi.tableHeightForRows(kContextMenuVisibleRows, false);
            const ThemedSectionGeometry contextMenuTrackingSection = behaviorForm.contentSection(
                behaviorFrameLeft, pageTop, behaviorFrameWidth, contextMenuTableHeight);
            HWND contextMenuTrackingGroup = AddSectionFrame(TabContextMenu, L"自动跟踪", contextMenuTrackingSection.frame);
            RECT contextMenuTableFrame{
                contextMenuTrackingSection.content.left,
                ContentY(contextMenuTrackingSection.content.top),
                contextMenuTrackingSection.content.right,
                ContentY(contextMenuTrackingSection.content.bottom),
            };
            ThemedTableOptions contextMenuTableOptions{};
            contextMenuTableOptions.checkable = true;
            contextMenuTableOptions.showHeader = false;
            contextMenuTableOptions.reserveScrollBarGutter = true;
            contextMenuTableOptions.fullRowSelect = true;
            contextMenuTable_ = MakeUi().Table(
                ID_CONTEXT_MENU_TABLE,
                contextMenuTableFrame,
                {
                    ThemedTableColumn{
                        L"tool",
                        L"工具",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Remaining},
                    ThemedTableColumn{
                        L"status",
                        L"状态",
                        ThemedTableColumnAlign::End,
                        ThemedTableColumnWidth::Fixed,
                        settingsUi.tableColumnWidth({L"已安装(注册表)", L"已安装(菜单)", L"未安装"})},
                },
                contextMenuTableOptions);
            AddTabChild(contextMenuTable_, TabContextMenu);
            RebuildContextMenuImageList();
            AddContextMenuTableRows();
            ThemedUi::BindGroupChildren(contextMenuTrackingGroup, {contextMenuTable_});

            const int resetContextMenuWidth = settingsUi.buttonWidth(
                L"重置右键菜单",
                ThemedButtonRole::Normal,
                ThemedButtonSize::Compact,
                ThemedButtonWidthMode::Text);
            const int refreshContextMenuWidth = settingsUi.buttonWidth(
                L"从Windows菜单刷新",
                ThemedButtonRole::Normal,
                ThemedButtonSize::Compact,
                ThemedButtonWidthMode::Text);
            const ThemedSectionGeometry contextMenuMaintenanceSection = behaviorForm.section(
                behaviorFrameLeft, contextMenuTrackingSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CompactButton, ThemedSectionItemKind::CompactButton})});
            HWND contextMenuMaintenanceGroup = AddSectionFrame(TabContextMenu, L"缓存维护", contextMenuMaintenanceSection.frame);
            const int contextMenuMaintenanceY = behaviorForm.sectionItemY(contextMenuMaintenanceSection, 0, settingsUi.compactButtonHeight());
            resetContextMenuButton_ = Button(
                TabContextMenu,
                ID_RESET_CONTEXT_MENU,
                L"重置右键菜单",
                behaviorLeft,
                contextMenuMaintenanceY,
                resetContextMenuWidth);
            refreshContextMenuButton_ = Button(
                TabContextMenu,
                ID_REFRESH_CONTEXT_MENU_FROM_NATIVE,
                L"从Windows菜单刷新",
                behaviorLeft + resetContextMenuWidth + behaviorLayout.controlGapX,
                contextMenuMaintenanceY,
                refreshContextMenuWidth);
            ThemedTooltipOptions resetContextMenuTooltipOptions{};
            resetContextMenuTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
            settingsUi.SetTooltip(
                resetContextMenuButton_,
                L"恢复跟踪开关默认值，并清除全部菜单列表、状态与图标缓存。",
                resetContextMenuTooltipOptions);
            ThemedTooltipOptions refreshContextMenuTooltipOptions{};
            refreshContextMenuTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
            settingsUi.SetTooltip(
                refreshContextMenuButton_,
                L"扫描Windows原生菜单，增量更新所有启用的工具菜单和图标。",
                refreshContextMenuTooltipOptions);
            ThemedUi::BindGroupChildren(contextMenuMaintenanceGroup, {resetContextMenuButton_, refreshContextMenuButton_});

            const ThemedSectionGeometry interactionLaunchSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND interactionLaunchGroup = AddSectionFrame(TabInteraction, L"启动操作", interactionLaunchSection.frame);
            doubleClick_ = CheckBox(
                TabInteraction, 109, L"双击运行", behaviorLeft,
                behaviorForm.sectionItemY(interactionLaunchSection, 0, behaviorCheckHeight),
                draft_.doubleClickToRun, behaviorContentWidth);
            ThemedUi::BindGroupChildren(interactionLaunchGroup, {doubleClick_});

            const ThemedSectionGeometry interactionHoverSection = behaviorForm.section(
                behaviorFrameLeft, interactionLaunchSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit})});
            HWND interactionHoverGroup = AddSectionFrame(TabInteraction, L"悬停激活", interactionHoverSection.frame);
            const int interactionHoverCheckY = behaviorForm.sectionItemY(interactionHoverSection, 0, behaviorCheckHeight);
            enterActiveGroup_ = CheckBox(TabInteraction, 124, L"鼠标进入激活分组", behaviorLeft, interactionHoverCheckY, draft_.mouseEnterActiveGroup, behaviorCheckWidth);
            enterActiveTag_ = CheckBox(TabInteraction, 125, L"鼠标进入激活标签", behaviorRight, interactionHoverCheckY, draft_.mouseEnterActiveTag, behaviorCheckWidth);
            const int delayLabelWidth = behaviorForm.labelWidthForTexts({L"分组激活延迟", L"标签激活延迟"});
            const int unitWidth = behaviorForm.labelWidthForText(L"ms");
            const int delayFieldWidth = behaviorColumnWidth - delayLabelWidth - behaviorLayout.labelGap - behaviorLayout.controlGapX - unitWidth;
            const int interactionDelayLabelY = behaviorForm.sectionItemY(interactionHoverSection, 1, settingsUi.labelHeight());
            const int interactionDelayEditY = behaviorForm.sectionItemY(interactionHoverSection, 1, settingsUi.editHeight());
            HWND groupDelayLabel = Label(TabInteraction, L"分组激活延迟", behaviorLeft, interactionDelayLabelY, delayLabelWidth);
            groupDelayEdit_ = NumberEdit(TabInteraction, ID_GROUP_DELAY, behaviorLeft + delayLabelWidth + behaviorLayout.labelGap, interactionDelayEditY, delayFieldWidth, draft_.activeGroupDelay);
            HWND groupDelayUnit = Label(TabInteraction, L"ms", behaviorLeft + behaviorColumnWidth - unitWidth, interactionDelayLabelY, unitWidth);
            HWND tagDelayLabel = Label(TabInteraction, L"标签激活延迟", behaviorRight, interactionDelayLabelY, delayLabelWidth);
            tagDelayEdit_ = NumberEdit(TabInteraction, ID_TAG_DELAY, behaviorRight + delayLabelWidth + behaviorLayout.labelGap, interactionDelayEditY, delayFieldWidth, draft_.activeTagDelay);
            HWND tagDelayUnit = Label(TabInteraction, L"ms", behaviorRight + behaviorColumnWidth - unitWidth, interactionDelayLabelY, unitWidth);
            ThemedUi::BindGroupChildren(interactionHoverGroup, {
                enterActiveGroup_, enterActiveTag_, groupDelayLabel, groupDelayEdit_, groupDelayUnit,
                tagDelayLabel, tagDelayEdit_, tagDelayUnit});

            const int hotKeyPageBottom = settingsUi.footerButtonY(settingsUi.footerButtonHeight()) - behaviorLayout.footerGap;
            const RECT hotKeyGroupFrame{
                behaviorFrameLeft,
                pageTop,
                behaviorFrameLeft + behaviorFrameWidth,
                hotKeyPageBottom,
            };
            const RECT hotKeyContent{
                hotKeyGroupFrame.left + groupInsets.left,
                hotKeyGroupFrame.top + groupInsets.top,
                hotKeyGroupFrame.right - groupInsets.right,
                hotKeyGroupFrame.bottom - groupInsets.bottom,
            };
            HWND hotKeyGroup = AddSectionFrame(TabHotKeys, L"全局快捷键", hotKeyGroupFrame);
            const int hotKeyToggleY = hotKeyContent.top;
            globalHotKeysEnabled_ = Toggle(
                TabHotKeys, ID_GLOBAL_HOTKEYS_ENABLED, L"启用全局快捷键", behaviorLeft,
                hotKeyToggleY, draft_.globalHotKeysEnabled, behaviorContentWidth);
            const int hotKeyStatusY = hotKeyContent.bottom - settingsUi.labelHeight();
            const int hotKeyTableTop = hotKeyToggleY + settingsUi.toggleHeight() + behaviorLayout.rowGap;
            const int hotKeyTableBottom = hotKeyStatusY - behaviorLayout.rowGap;
            RECT hotKeyTableFrame{
                behaviorLeft,
                ContentY(hotKeyTableTop),
                behaviorLeft + behaviorContentWidth,
                ContentY(hotKeyTableBottom),
            };
            hotKeyTable_ = MakeUi().Table(
                ID_HOTKEY_TABLE,
                hotKeyTableFrame,
                {
                    ThemedTableColumn{
                        L"function",
                        L"功能",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Remaining},
                    ThemedTableColumn{
                        L"hotkey",
                        L"快捷键",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Fixed,
                        settingsUi.tableColumnWidth({L"快捷键", L"Ctrl+Alt+Page Down", L"双击 Alt", L"未设置"})},
                    ThemedTableColumn{
                        L"action",
                        L"操作",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Fixed,
                        settingsUi.buttonWidth(
                            L"录入",
                            ThemedButtonRole::Normal,
                            ThemedButtonSize::Compact,
                            ThemedButtonWidthMode::Text) + behaviorLayout.controlGapX},
                });
            AddTabChild(hotKeyTable_, TabHotKeys);
            mainHotKeyStatus_ = Label(
                TabHotKeys, L"", behaviorLeft,
                hotKeyStatusY, behaviorContentWidth);
            ThemedUi::BindGroupChildren(hotKeyGroup, {
                globalHotKeysEnabled_, hotKeyTable_, mainHotKeyStatus_});
            UpdateHotKeyLabels();

            const ThemedSectionGeometry directoryCommandSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit})});
            HWND directoryCommandGroup = AddSectionFrame(TabLinks, L"目录命令", directoryCommandSection.frame);
            HWND openDirLabel = Label(
                TabLinks, L"打开目录命令", behaviorLeft,
                behaviorForm.sectionItemY(directoryCommandSection, 0, settingsUi.labelHeight()), behaviorContentWidth);
            openDirEdit_ = FramedEdit(
                TabLinks, 202, behaviorLeft,
                behaviorForm.sectionItemY(directoryCommandSection, 1, settingsUi.editHeight()), behaviorContentWidth, draft_.openDirCommand);
            ThemedUi::BindGroupChildren(directoryCommandGroup, {openDirLabel, openDirEdit_});

            const ThemedSectionGeometry publicLinksSection = behaviorForm.section(
                behaviorFrameLeft, directoryCommandSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit})});
            HWND publicLinksGroup = AddSectionFrame(TabLinks, L"公共链接", publicLinksSection.frame);
            HWND helpUrlLabel = Label(TabLinks, L"帮助链接", behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 0, settingsUi.labelHeight()), behaviorContentWidth);
            helpUrlEdit_ = FramedEdit(TabLinks, 203, behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 1, settingsUi.editHeight()), behaviorContentWidth, draft_.helpUrl);
            HWND updateUrlLabel = Label(TabLinks, L"更新链接", behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 2, settingsUi.labelHeight()), behaviorContentWidth);
            updateUrlEdit_ = FramedEdit(TabLinks, 204, behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 3, settingsUi.editHeight()), behaviorContentWidth, draft_.updateUrl);
            HWND faqUrlLabel = Label(TabLinks, L"FAQ 链接", behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 4, settingsUi.labelHeight()), behaviorCheckWidth);
            HWND rewardUrlLabel = Label(TabLinks, L"赞助链接", behaviorRight, behaviorForm.sectionItemY(publicLinksSection, 4, settingsUi.labelHeight()), behaviorCheckWidth);
            faqUrlEdit_ = FramedEdit(TabLinks, 205, behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 5, settingsUi.editHeight()), behaviorCheckWidth, draft_.faqUrl);
            rewardUrlEdit_ = FramedEdit(TabLinks, 206, behaviorRight, behaviorForm.sectionItemY(publicLinksSection, 5, settingsUi.editHeight()), behaviorCheckWidth, draft_.rewardUrl);
            ThemedUi::BindGroupChildren(publicLinksGroup, {
                helpUrlLabel, helpUrlEdit_, updateUrlLabel, updateUrlEdit_, faqUrlLabel, rewardUrlLabel, faqUrlEdit_, rewardUrlEdit_});

            const int uploadWidth = settingsUi.buttonWidth(L"上传到云端", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int downloadWidth = settingsUi.buttonWidth(L"从云端下载", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int testWidth = settingsUi.buttonWidth(L"测试连接", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int clearWidth = settingsUi.buttonWidth(L"清除密码", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const ThemedSectionGeometry webDavSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CompactButton})});
            HWND webDavGroup = AddSectionFrame(TabWebDav, L"WebDAV 备份", webDavSection.frame);
            webDavEnabled_ = CheckBox(TabWebDav, 208, L"启用 WebDAV 备份", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 0, behaviorCheckHeight), draft_.webDavEnabled, behaviorCheckWidth);
            webDavLastSyncLabel_ = Label(TabWebDav, L"", behaviorRight, behaviorForm.sectionItemY(webDavSection, 0, settingsUi.labelHeight()), behaviorCheckWidth);
            UpdateWebDavLastSyncLabel();
            const int webDavServerLabelWidth = behaviorForm.labelWidthForText(L"服务器地址");
            const int webDavServerFieldX = behaviorLeft + webDavServerLabelWidth + behaviorLayout.labelGap;
            HWND webDavUrlLabel = Label(TabWebDav, L"服务器地址", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 1, settingsUi.labelHeight()), webDavServerLabelWidth);
            webDavUrlEdit_ = FramedEdit(TabWebDav, 209, webDavServerFieldX, behaviorForm.sectionItemY(webDavSection, 1, settingsUi.editHeight()), behaviorContentWidth - webDavServerLabelWidth - behaviorLayout.labelGap, draft_.webDavUrl);
            HWND webDavUserLabel = Label(TabWebDav, L"用户名", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 2, settingsUi.labelHeight()), behaviorCheckWidth);
            HWND webDavPasswordLabel = Label(TabWebDav, L"密码/应用密码", behaviorRight, behaviorForm.sectionItemY(webDavSection, 2, settingsUi.labelHeight()), behaviorCheckWidth);
            webDavUserNameEdit_ = FramedEdit(TabWebDav, 212, behaviorLeft, behaviorForm.sectionItemY(webDavSection, 3, settingsUi.editHeight()), behaviorCheckWidth, draft_.webDavUserName);
            ThemedEditOptions passwordOptions{};
            passwordOptions.content = ThemedEditContent::Password;
            webDavPasswordEdit_ = FramedEdit(TabWebDav, 213, behaviorRight, behaviorForm.sectionItemY(webDavSection, 3, settingsUi.editHeight()), behaviorCheckWidth, L"", passwordOptions);
            HWND webDavRemoteLabel = Label(TabWebDav, L"远端目录", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 4, settingsUi.labelHeight()), behaviorCheckWidth);
            HWND webDavKeepLabel = Label(TabWebDav, L"保留数量", behaviorRight, behaviorForm.sectionItemY(webDavSection, 4, settingsUi.labelHeight()), behaviorCheckWidth);
            webDavRemotePathEdit_ = FramedEdit(TabWebDav, 210, behaviorLeft, behaviorForm.sectionItemY(webDavSection, 5, settingsUi.editHeight()), behaviorCheckWidth, draft_.webDavRemotePath);
            webDavKeepCountEdit_ = NumberEdit(TabWebDav, 211, behaviorRight, behaviorForm.sectionItemY(webDavSection, 5, settingsUi.editHeight()), behaviorCheckWidth, draft_.webDavKeepCount);
            const int webDavButtonsWidth =
                testWidth + behaviorLayout.controlGapX + clearWidth + behaviorLayout.controlGapX
                + uploadWidth + behaviorLayout.controlGapX + downloadWidth;
            const int webDavButtonsX = settingsUi.centeredGroupX(webDavButtonsWidth);
            const int webDavButtonsY = behaviorForm.sectionItemY(webDavSection, 6, settingsUi.compactButtonHeight());
            webDavTestButton_ = Button(TabWebDav, ID_WEBDAV_TEST, L"测试连接", webDavButtonsX, webDavButtonsY, testWidth);
            webDavClearPasswordButton_ = Button(TabWebDav, ID_WEBDAV_CLEAR_PASSWORD, L"清除密码", webDavButtonsX + testWidth + behaviorLayout.controlGapX, webDavButtonsY, clearWidth);
            webDavUploadButton_ = Button(TabWebDav, ID_WEBDAV_UPLOAD, L"上传到云端", webDavButtonsX + testWidth + behaviorLayout.controlGapX + clearWidth + behaviorLayout.controlGapX, webDavButtonsY, uploadWidth);
            webDavDownloadButton_ = Button(TabWebDav, ID_WEBDAV_DOWNLOAD, L"从云端下载", webDavButtonsX + testWidth + behaviorLayout.controlGapX + clearWidth + behaviorLayout.controlGapX + uploadWidth + behaviorLayout.controlGapX, webDavButtonsY, downloadWidth);
            ThemedUi::BindGroupChildren(webDavGroup, {
                webDavEnabled_, webDavLastSyncLabel_, webDavUrlLabel, webDavUrlEdit_, webDavUserLabel,
                webDavPasswordLabel, webDavUserNameEdit_, webDavPasswordEdit_, webDavTestButton_,
                webDavClearPasswordButton_, webDavRemoteLabel, webDavKeepLabel,
                webDavRemotePathEdit_, webDavKeepCountEdit_, webDavUploadButton_, webDavDownloadButton_});

            const DialogLayoutMetrics& httpLayout = settingsUi.layout();
            const int httpPanelPaddingX = groupInsets.left;
            const int httpFrameLeft = pageLeft;
            const int httpFrameRight = pageLeft + pageWidth;
            const int httpContentLeft = httpFrameLeft + httpPanelPaddingX;
            const int httpContentRight = httpFrameRight - httpPanelPaddingX;
            const int httpLabelWidth = behaviorForm.labelWidthForTexts({L"站点网址", L"绑定磁盘路径"});
            const int httpFieldX = httpContentLeft + httpLabelWidth + httpLayout.labelGap;
            const int httpFrameGap = httpLayout.sectionGap;
            const int httpFrameWidth = httpFrameRight - httpFrameLeft;
            const int httpCheckHeight = settingsUi.checkBoxHeight();
            const int httpEditHeight = settingsUi.editHeight();
            const int httpLabelHeight = settingsUi.labelHeight();
            const int httpButtonHeight = settingsUi.compactButtonHeight();

            const int httpConfigFrameTop = tabStripRect_.bottom + httpLayout.sectionGap;
            const ThemedSectionGeometry httpServiceSection = behaviorForm.section(
                httpFrameLeft, httpConfigFrameTop, httpFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit, ThemedSectionItemKind::CompactButton})});
            HWND httpServiceGroup = AddSectionFrame(TabHttp, L"服务配置", httpServiceSection.frame);
            const int httpBrowseWidth = settingsUi.buttonWidth(L"选择", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int httpOpenRootWidth = settingsUi.buttonWidth(L"打开目录", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int httpFieldWidth = httpContentRight - httpFieldX - httpLayout.controlGapX * 2 - httpBrowseWidth - httpOpenRootWidth;
            const int httpAutoStartY = behaviorForm.sectionItemY(httpServiceSection, 0, httpCheckHeight);
            httpServerAutoStart_ = CheckBox(TabHttp, 215, L"随应用启动", httpContentLeft, httpAutoStartY, draft_.httpServerAutoStart, httpContentRight - httpContentLeft);
            const int httpAddressLabelY = behaviorForm.sectionItemY(httpServiceSection, 1, httpLabelHeight);
            const int httpAddressEditY = behaviorForm.sectionItemY(httpServiceSection, 1, httpEditHeight);
            const int httpRootLabelY = behaviorForm.sectionItemY(httpServiceSection, 2, httpLabelHeight);
            const int httpRootEditY = behaviorForm.sectionItemY(httpServiceSection, 2, httpEditHeight);
            const int httpRootButtonY = behaviorForm.sectionItemY(httpServiceSection, 2, httpButtonHeight);
            HWND httpAddressLabel = Label(TabHttp, L"站点网址", httpContentLeft, httpAddressLabelY, httpLabelWidth);
            httpServerAddressEdit_ = FramedEdit(
                TabHttp,
                ID_HTTP_ADDRESS,
                httpFieldX,
                httpAddressEditY,
                httpFieldWidth,
                HttpAddressText(true, draft_.httpServerPort, false));
            HWND httpRootLabel = Label(TabHttp, L"绑定磁盘路径", httpContentLeft, httpRootLabelY, httpLabelWidth);
            httpServerRootEdit_ = FramedEdit(
                TabHttp,
                218,
                httpFieldX,
                httpRootEditY,
                httpFieldWidth,
                Trim(draft_.httpServerRootPath).empty() ? LocalHttpServerService::DefaultRootPath(httpRootBaseDirectory_).wstring() : draft_.httpServerRootPath);
            const int httpBrowseX = httpFieldX + httpFieldWidth + httpLayout.controlGapX;
            httpBrowseRootButton_ = Button(TabHttp, ID_HTTP_BROWSE_ROOT, L"选择", httpBrowseX, httpRootButtonY, httpBrowseWidth);
            HWND httpOpenRootButton = Button(TabHttp, ID_HTTP_OPEN_ROOT, L"打开目录", httpBrowseX + httpBrowseWidth + httpLayout.controlGapX, httpRootButtonY, httpOpenRootWidth);
            ThemedUi::BindGroupChildren(httpServiceGroup, {
                httpServerAutoStart_, httpAddressLabel, httpServerAddressEdit_, httpRootLabel,
                httpServerRootEdit_, httpBrowseRootButton_, httpOpenRootButton});

            const int httpControlFrameTop = httpServiceSection.frame.bottom + httpFrameGap;
            const ThemedSectionGeometry httpControlSection = behaviorForm.section(
                httpFrameLeft, httpControlFrameTop, httpFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::StatusBadge, ThemedSectionItemKind::Text}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CompactButton})});
            HWND httpControlGroup = AddSectionFrame(TabHttp, L"运行控制", httpControlSection.frame);
            const int httpStatusY = behaviorForm.sectionItemY(httpControlSection, 0, httpLabelHeight);
            const int httpButtonY = behaviorForm.sectionItemY(httpControlSection, 1, httpButtonHeight);
            const int httpStatusLabelWidth = behaviorForm.labelWidthForText(L"状态");
            HWND httpStatusLabel = Label(TabHttp, L"状态", httpContentLeft, httpStatusY, httpStatusLabelWidth);
            httpServerStatusTag_ = StatusBadge(
                TabHttp, L"", httpContentLeft + httpStatusLabelWidth + httpLayout.controlGapX, httpStatusY, settingsUi.textWidth(L"未运行") + httpLayout.controlGapX * 2, ThemedStatusRole::Danger);
            const int httpStatusDetailX = httpContentLeft + httpStatusLabelWidth + httpLayout.controlGapX * 2 + settingsUi.textWidth(L"未运行") + httpLayout.controlGapX * 2;
            httpServerStatusDetail_ = Label(TabHttp, L"", httpStatusDetailX, httpStatusY, httpContentRight - httpStatusDetailX);
            const int httpStartWidth = settingsUi.buttonWidth(L"启动", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int httpStopWidth = settingsUi.buttonWidth(L"停止", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int httpRestartWidth = settingsUi.buttonWidth(L"重启", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int httpHomeWidth = settingsUi.buttonWidth(L"打开网站", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int httpCopyWidth = settingsUi.buttonWidth(L"复制地址", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            int httpButtonX = httpContentLeft;
            httpStartButton_ = Button(TabHttp, ID_HTTP_START, L"启动", httpButtonX, httpButtonY, httpStartWidth);
            httpButtonX += httpStartWidth + httpLayout.controlGapX;
            httpStopButton_ = Button(TabHttp, ID_HTTP_STOP, L"停止", httpButtonX, httpButtonY, httpStopWidth);
            httpButtonX += httpStopWidth + httpLayout.controlGapX;
            httpRestartButton_ = Button(TabHttp, ID_HTTP_RESTART, L"重启", httpButtonX, httpButtonY, httpRestartWidth);
            httpButtonX += httpRestartWidth + httpLayout.controlGapX;
            HWND httpHomeButton = Button(TabHttp, ID_HTTP_OPEN_HOME, L"打开网站", httpButtonX, httpButtonY, httpHomeWidth);
            httpButtonX += httpHomeWidth + httpLayout.controlGapX;
            HWND httpCopyButton = Button(TabHttp, ID_HTTP_COPY_URL, L"复制地址", httpButtonX, httpButtonY, httpCopyWidth);
            ThemedUi::BindGroupChildren(httpControlGroup, {
                httpStatusLabel, httpServerStatusTag_, httpServerStatusDetail_, httpStartButton_, httpStopButton_,
                httpRestartButton_, httpHomeButton, httpCopyButton});

            const ThemedSectionGeometry httpAdvancedSection = behaviorForm.section(
                httpFrameLeft, httpControlSection.frame.bottom + httpFrameGap, httpFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CompactButton, ThemedSectionItemKind::Text})});
            HWND httpAdvancedGroup = AddSectionFrame(TabHttp, L"高级配置", httpAdvancedSection.frame);
            const int httpConfigY = behaviorForm.sectionItemY(httpAdvancedSection, 0, httpButtonHeight);
            const int httpConfigWidth = settingsUi.buttonWidth(L"配置目录", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            HWND httpConfigButton = Button(TabHttp, ID_HTTP_OPEN_CONFIG_DIR, L"配置目录", httpContentLeft, httpConfigY, httpConfigWidth);
            HWND httpConfigNote = Label(TabHttp, L"权限、账号、MIME 与下载策略在配置目录修改，重启后生效。", httpContentLeft + httpConfigWidth + httpLayout.controlGapX, httpConfigY, httpContentRight - httpContentLeft - httpConfigWidth - httpLayout.controlGapX);
            ThemedUi::BindGroupChildren(httpAdvancedGroup, {httpConfigButton, httpConfigNote});
            UpdateHttpStatusLabel();

            const int configExportWidth = settingsUi.buttonWidth(L"导出配置包", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int configImportWidth = settingsUi.buttonWidth(L"导入配置包", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int configButtonsX = behaviorLeft;
            const ThemedSectionGeometry configBackupSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CompactButton})});
            HWND configBackupGroup = AddSectionFrame(TabBackup, L"配置包", configBackupSection.frame);
            const int configButtonsY = behaviorForm.sectionItemY(configBackupSection, 0, settingsUi.compactButtonHeight());
            HWND configExportButton = Button(TabBackup, ID_CONFIG_EXPORT, L"导出配置包", configButtonsX, configButtonsY, configExportWidth);
            HWND configImportButton = Button(TabBackup, ID_CONFIG_IMPORT, L"导入配置包", configButtonsX + configExportWidth + behaviorLayout.controlGapX, configButtonsY, configImportWidth);
            ThemedUi::BindGroupChildren(configBackupGroup, {configExportButton, configImportButton});

            const int todoExportWidth = settingsUi.buttonWidth(L"导出", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int todoImportWidth = settingsUi.buttonWidth(L"导入", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
            const int todoButtonsX = behaviorLeft;
            const ThemedSectionGeometry todoBackupSection = behaviorForm.section(
                behaviorFrameLeft, configBackupSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CompactButton}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label})});
            HWND todoBackupGroup = AddSectionFrame(TabBackup, L"待办事项", todoBackupSection.frame);
            const int todoButtonsY = behaviorForm.sectionItemY(todoBackupSection, 0, settingsUi.compactButtonHeight());
            HWND todoExportButton = Button(TabBackup, ID_TODO_EXPORT, L"导出", todoButtonsX, todoButtonsY, todoExportWidth);
            HWND todoImportButton = Button(TabBackup, ID_TODO_IMPORT, L"导入", todoButtonsX + todoExportWidth + behaviorLayout.controlGapX, todoButtonsY, todoImportWidth);
            const int backupCheckWidth = behaviorContentWidth / 3;
            const int backupCheckY = behaviorForm.sectionItemY(todoBackupSection, 1, behaviorCheckHeight);
            todoIncludeCompleted_ = CheckBox(TabBackup, ID_TODO_INCLUDE_COMPLETED, L"含已完成", behaviorLeft, backupCheckY, true, backupCheckWidth);
            todoIncludeDisabled_ = CheckBox(TabBackup, ID_TODO_INCLUDE_DISABLED, L"含已禁用", behaviorLeft + backupCheckWidth, backupCheckY, true, backupCheckWidth);
            todoOnlyFuture_ = CheckBox(TabBackup, ID_TODO_ONLY_FUTURE, L"仅未来", behaviorLeft + backupCheckWidth * 2, backupCheckY, false, behaviorContentWidth - backupCheckWidth * 2);
            HWND todoBackupNote = Label(
                TabBackup,
                L"待办事项备份可用于 Quattro 恢复，也可通过 Apple 快捷指令导入提醒事项。",
                behaviorLeft,
                behaviorForm.sectionItemY(todoBackupSection, 2, settingsUi.labelHeight()),
                behaviorContentWidth);
            ThemedUi::BindGroupChildren(todoBackupGroup, {
                todoExportButton, todoImportButton, todoIncludeCompleted_, todoIncludeDisabled_, todoOnlyFuture_, todoBackupNote});

            const ThemedUi footerUi = MakeUi();
            okButton_ = footerUi.FooterButton(IDOK, L"确定", 0, 3, true, true);
            applyButton_ = footerUi.FooterButton(ID_SETTINGS_APPLY, L"应用", 1, 3);
            cancelButton_ = footerUi.FooterButton(IDCANCEL, L"取消", 2, 3);
            BindTabPages();
            ShowTab(TabDisplay);
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
        case WM_SETTINGS_WEBDAV_DONE:
            HandleWebDavResult(std::unique_ptr<SettingsWebDavResult>(reinterpret_cast<SettingsWebDavResult*>(lParam)));
            return 0;
        case WM_CONTEXT_MENU_ICON_LOAD_REQUEST:
            if (currentTab_ == TabContextMenu && contextMenuIconAutoRequested_) {
                StartContextMenuIconLoad(false);
            }
            return 0;
        case WM_CONTEXT_MENU_ICON_LOAD_DONE:
            CompleteContextMenuIconLoad(static_cast<std::uintptr_t>(wParam));
            return 0;
        case WM_CONTEXT_MENU_REFRESH_DONE:
            CompleteContextMenuRefresh();
            return 0;
        case WM_NOTIFY:
            if (HandleHotKeyTableEvent(lParam)) {
                return 0;
            }
            if (HandleContextMenuTableEvent(lParam)) {
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_SETTINGS_TAB_CONTROL && HIWORD(wParam) == CBN_SELCHANGE) {
                ShowTab(ThemedUi::ActiveTab(settingsTabs_));
                return 0;
            }
        if (LOWORD(wParam) >= ID_TAG_ALIGN_LEFT && LOWORD(wParam) <= ID_TAG_ALIGN_RIGHT) {
            tagAlignIndex_ = static_cast<int>(LOWORD(wParam) - ID_TAG_ALIGN_LEFT);
            UpdateTagAlignButtons();
            return 0;
        }
        if (LOWORD(wParam) == ID_HTTP_ADDRESS && (HIWORD(wParam) == EN_KILLFOCUS || HIWORD(wParam) == EN_CHANGE)) {
            UpdateHttpStatusLabel();
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateHttpAddressField(false);
            }
        }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CAPTURE) {
                TrySetMainHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.mainHotKey));
                return 0;
            }
            if (LOWORD(wParam) == ID_PROCESS_LOCATOR_HOTKEY_CAPTURE) {
                TrySetProcessLocatorHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.processLocatorHotKey));
                return 0;
            }
            if (LOWORD(wParam) == ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE) {
                TrySetCopySelectedPathsHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.copySelectedPathsHotKey));
                return 0;
            }
            if (LOWORD(wParam) == ID_GLOBAL_HOTKEYS_ENABLED) {
                draft_.globalHotKeysEnabled = ThemedUi::IsChecked(globalHotKeysEnabled_);
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CLEAR) {
                draft_.mainHotKey = 0;
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_TEST) {
                TestWebDavConnection();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_CLEAR_PASSWORD) {
                ClearWebDavPassword();
                return 0;
            }
            if (LOWORD(wParam) == ID_RESET_CONTEXT_MENU) {
                ResetContextMenu();
                return 0;
            }
            if (LOWORD(wParam) == ID_REFRESH_CONTEXT_MENU_FROM_NATIVE) {
                RefreshContextMenuFromNative();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_UPLOAD) {
                UploadWebDavBackup();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_DOWNLOAD) {
                DownloadWebDavBackup();
                return 0;
            }
            if (LOWORD(wParam) == ID_CONFIG_EXPORT) {
                ExportConfigPackage();
                return 0;
            }
            if (LOWORD(wParam) == ID_CONFIG_IMPORT) {
                ImportConfigPackage();
                return 0;
            }
            if (LOWORD(wParam) == ID_TODO_EXPORT) {
                ExportTodosJson();
                return 0;
            }
            if (LOWORD(wParam) == ID_TODO_IMPORT) {
                ImportTodosJson();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_BROWSE_ROOT) {
                BrowseHttpRoot();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_OPEN_ROOT) {
                OpenHttpRootDirectory();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_OPEN_CONFIG_DIR) {
                OpenHttpConfigDirectory();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_OPEN_HOME) {
                OpenHttpHome();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_COPY_URL) {
                CopyHttpUrl();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_START) {
                StartHttpServerFromDialog(false);
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_STOP) {
                StopHttpServerFromDialog();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_RESTART) {
                StartHttpServerFromDialog(true);
                return 0;
            }
            if (LOWORD(wParam) == ID_SETTINGS_APPLY) {
                if (contextMenuRefreshBusy_) {
                    ShowToast(L"Windows 菜单正在刷新，请稍候。", ThemedToastRole::Info);
                    return 0;
                }
                CommitSettings(false);
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                if (contextMenuRefreshBusy_) {
                    ShowToast(L"Windows 菜单正在刷新，请等待完成后关闭设置。", ThemedToastRole::Info);
                    return 0;
                }
                if (!CommitSettings(true)) {
                    return 0;
                }
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                if (contextMenuRefreshBusy_) {
                    ShowToast(L"Windows 菜单正在刷新，请等待完成后关闭设置。", ThemedToastRole::Info);
                    return 0;
                }
                if (webDavBusy_) {
                    ShowToast(L"WebDAV 操作正在进行，请稍候完成。", ThemedToastRole::Warning);
                    return 0;
                }
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            if (contextMenuRefreshBusy_) {
                ShowToast(L"Windows 菜单正在刷新，请等待完成后关闭设置。", ThemedToastRole::Info);
                return 0;
            }
            if (webDavBusy_) {
                ShowToast(L"WebDAV 操作正在进行，请稍候完成。", ThemedToastRole::Warning);
                return 0;
            }
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void UpdateHotKeyLabels() {
        AddHotKeyTableRows();
        if (mainHotKeyStatus_) {
            if (!draft_.globalHotKeysEnabled) {
                SetWindowTextW(mainHotKeyStatus_, L"全局快捷键已关闭。");
                return;
            }
            if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
                draft_.mainHotKey != 0 &&
                draft_.mainHotKey == draft_.processLocatorHotKey) {
                SetWindowTextW(mainHotKeyStatus_, L"主窗口显隐和进程定位器不能使用同一个快捷键。");
                return;
            }
            if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
                draft_.mainHotKey != 0 &&
                draft_.mainHotKey == draft_.copySelectedPathsHotKey) {
                SetWindowTextW(mainHotKeyStatus_, L"主窗口显隐和复制选中项绝对路径不能使用同一个快捷键。");
                return;
            }
            if (draft_.processLocatorHotKey != 0 &&
                draft_.processLocatorHotKey == draft_.copySelectedPathsHotKey) {
                SetWindowTextW(mainHotKeyStatus_, L"进程定位器和复制选中项绝对路径不能使用同一个快捷键。");
                return;
            }
            const HotKeyAvailability mainAvailability = CheckMainHotKeyAvailability(hwnd_, draft_.mainHotKey, CurrentRegisteredMainHotKey());
            if (!mainAvailability.available) {
                SetWindowTextW(mainHotKeyStatus_, MainHotKeyStatusText(draft_.mainHotKey, mainAvailability).c_str());
                return;
            }
            const HotKeyAvailability locatorAvailability = CheckCtrlAltHotKeyAvailability(
                hwnd_, draft_.processLocatorHotKey, CurrentRegisteredProcessLocatorHotKey());
            if (!locatorAvailability.available) {
                SetWindowTextW(mainHotKeyStatus_, ProcessLocatorHotKeyStatusText(draft_.processLocatorHotKey, locatorAvailability).c_str());
                return;
            }
            const HotKeyAvailability copyAvailability = CheckCtrlAltHotKeyAvailability(
                hwnd_, draft_.copySelectedPathsHotKey, CurrentRegisteredCopySelectedPathsHotKey());
            SetWindowTextW(mainHotKeyStatus_, CopySelectedPathsHotKeyStatusText(draft_.copySelectedPathsHotKey, copyAvailability).c_str());
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    AppConfig& config_;
    AppConfig draft_;
    const Theme& theme_;
    std::filesystem::path appDirectory_;
    std::filesystem::path httpRootBaseDirectory_;
    LocalHttpServerService* httpServer_ = nullptr;
    bool mainHotKeyRegistered_ = false;
    bool processLocatorHotKeyRegistered_ = false;
    bool copySelectedPathsHotKeyRegistered_ = false;
    int currentTab_ = -1;
    RECT tabStripRect_{};
    int tabContentOffsetY_ = 0;
    HWND settingsTabs_ = nullptr;
    std::vector<TabChild> tabChildren_;
    int nextGeneratedControlId_ = 6000;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND showTitle_ = nullptr;
    HWND showGroup_ = nullptr;
    HWND showTag_ = nullptr;
    HWND autoDock_ = nullptr;
    HWND hideInactive_ = nullptr;
    HWND hideAfterLink_ = nullptr;
    HWND hideOnStart_ = nullptr;
    HWND doubleClick_ = nullptr;
    HWND deleteConfirm_ = nullptr;
    HWND saveRunCount_ = nullptr;
    HWND showToolboxButton_ = nullptr;
    HWND showSkinButton_ = nullptr;
    HWND autoRun_ = nullptr;
    HWND loggingEnabled_ = nullptr;
    HWND contextMenuTable_ = nullptr;
    HWND resetContextMenuButton_ = nullptr;
    HWND refreshContextMenuButton_ = nullptr;
    HIMAGELIST contextMenuImages_ = nullptr;
    // 表格行序 → 绑定表下标（已安装在前、未安装沉底）。
    std::vector<std::size_t> contextMenuTableOrder_;
    std::vector<ContextMenuProviderIconInfo> contextMenuProviderIcons_;
    std::vector<int> contextMenuProviderImageIndexes_;
    HWND linkNameSingleLine_ = nullptr;
    HWND showTooltip_ = nullptr;
    HWND groupRight_ = nullptr;
    HWND tagRight_ = nullptr;
    HWND enterActiveGroup_ = nullptr;
    HWND enterActiveTag_ = nullptr;
    HWND alphaEdit_ = nullptr;
    HWND groupWidthEdit_ = nullptr;
    HWND tagWidthEdit_ = nullptr;
    HWND dockDelayEdit_ = nullptr;
    HWND groupDelayEdit_ = nullptr;
    HWND tagDelayEdit_ = nullptr;
    int tagAlignIndex_ = 1;
    HWND tagAlignLeft_ = nullptr;
    HWND tagAlignCenter_ = nullptr;
    HWND tagAlignRight_ = nullptr;
    HWND globalHotKeysEnabled_ = nullptr;
    HWND hotKeyTable_ = nullptr;
    HWND mainHotKeyStatus_ = nullptr;
    HWND openDirEdit_ = nullptr;
    HWND helpUrlEdit_ = nullptr;
    HWND updateUrlEdit_ = nullptr;
    HWND faqUrlEdit_ = nullptr;
    HWND rewardUrlEdit_ = nullptr;
    HWND webDavEnabled_ = nullptr;
    HWND webDavUrlEdit_ = nullptr;
    HWND webDavRemotePathEdit_ = nullptr;
    HWND webDavKeepCountEdit_ = nullptr;
    HWND webDavUserNameEdit_ = nullptr;
    HWND webDavPasswordEdit_ = nullptr;
    HWND webDavUploadButton_ = nullptr;
    HWND webDavDownloadButton_ = nullptr;
    HWND webDavTestButton_ = nullptr;
    HWND webDavClearPasswordButton_ = nullptr;
    HWND webDavLastSyncLabel_ = nullptr;
    HWND httpServerAutoStart_ = nullptr;
    HWND httpServerAddressEdit_ = nullptr;
    HWND httpServerRootEdit_ = nullptr;
    HWND httpServerStatusTag_ = nullptr;
    HWND httpServerStatusDetail_ = nullptr;
    HWND httpBrowseRootButton_ = nullptr;
    HWND httpStartButton_ = nullptr;
    HWND httpStopButton_ = nullptr;
    HWND httpRestartButton_ = nullptr;
    HWND okButton_ = nullptr;
    HWND applyButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND todoIncludeCompleted_ = nullptr;
    HWND todoIncludeDisabled_ = nullptr;
    HWND todoOnlyFuture_ = nullptr;
    bool importedData_ = false;
    bool webDavBusy_ = false;
    bool contextMenuRefreshBusy_ = false;
    bool contextMenuIconLoadBusy_ = false;
    bool contextMenuIconAutoRequested_ = false;
    bool contextMenuProviderLoadCompleted_ = false;
    bool contextMenuProviderLoadFailed_ = false;
    bool accepted_ = false;
    bool done_ = false;
    SettingsApplyCallback applyCallback_;
    SettingsResetContextMenuCallback resetContextMenuCallback_;
    std::vector<Link> contextMenuLinks_;
    SettingsContextMenuRefreshRunner contextMenuRefreshRunner_;
    SettingsContextMenuRefreshApplyCallback contextMenuRefreshApplyCallback_;
    SettingsContextMenuProviderIconRunner contextMenuProviderIconRunner_;
    std::jthread contextMenuRefreshThread_;
    std::mutex contextMenuRefreshMutex_;
    std::optional<ShellContextMenuRefreshResult> contextMenuRefreshResult_;
    std::shared_ptr<SettingsContextMenuIconAsyncState> contextMenuIconAsyncState_;
    std::uintptr_t contextMenuIconLoadGeneration_ = 0;
};
}

bool ShowTextInputDialog(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& title, const std::wstring& label, std::wstring& value) {
    TextDialog dialog(owner, instance, theme, title, label, value);
    return dialog.Run();
}

int ShowThemedMessageBox(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& message, const std::wstring& title, UINT flags) {
    ThemedMessageDialog dialog(owner, instance, theme, message, title, flags);
    return dialog.Run();
}

bool ShowHotKeyConflictDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::wstring& message,
    bool& ignoreFutureWarnings) {
    HotKeyConflictDialog dialog(owner, instance, theme, message, ignoreFutureWarnings);
    return dialog.Run();
}

bool ShowWebDavBackupSelectionDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::vector<WebDavRemoteFile>& backups,
    std::wstring& selectedName) {
    WebDavBackupSelectionDialog dialog(owner, instance, theme, backups, selectedName);
    return dialog.Run();
}

bool ShowSettingsDialog(
    HWND owner,
    HINSTANCE instance,
    AppConfig& config,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    const std::filesystem::path& httpRootBaseDirectory,
    bool* importedData,
    LocalHttpServerService* httpServer,
    bool mainHotKeyRegistered,
    bool processLocatorHotKeyRegistered,
    bool copySelectedPathsHotKeyRegistered,
    SettingsApplyCallback applyCallback,
    SettingsResetContextMenuCallback resetContextMenuCallback,
    const std::vector<Link>& contextMenuLinks,
    SettingsContextMenuRefreshRunner contextMenuRefreshRunner,
    SettingsContextMenuRefreshApplyCallback contextMenuRefreshApplyCallback,
    SettingsContextMenuProviderIconRunner contextMenuProviderIconRunner) {
    SettingsDialog dialog(
        owner,
        instance,
        config,
        theme,
        appDirectory,
        httpRootBaseDirectory,
        httpServer,
        mainHotKeyRegistered,
        processLocatorHotKeyRegistered,
        copySelectedPathsHotKeyRegistered,
        std::move(applyCallback),
        std::move(resetContextMenuCallback),
        contextMenuLinks,
        std::move(contextMenuRefreshRunner),
        std::move(contextMenuRefreshApplyCallback),
        std::move(contextMenuProviderIconRunner));
    const bool accepted = dialog.Run();
    if (importedData) {
        *importedData = dialog.webDavDataImported();
    }
    return accepted;
}
