#include "SimpleDialogs.h"

#include "../../resources/resource.h"

#include "AppLog.h"
#include "ConfigPackageService.h"
#include "DialogLayout.h"
#include "HotKeyEditor.h"
#include "JsonValue.h"
#include "LocalHttpServerService.h"
#include "MainHotKey.h"
#include "Storage.h"
#include "ThemedControls.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "TodoSchedule.h"
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
#include <array>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <memory>
#include <map>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr int ID_SETTINGS_TAB_BASE = 280;
constexpr int ID_SETTINGS_TAB_CONTROL = 279;
constexpr int ID_MAIN_HOTKEY_CAPTURE = 301;
constexpr int ID_MAIN_HOTKEY_CLEAR = 302;
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
constexpr int ID_MESSAGE_TEXT = 501;
constexpr int ID_MAIN_HOTKEY_PROBE = 0x5148;
constexpr UINT WM_SETTINGS_WEBDAV_DONE = WM_APP + 0x81;

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
        SettingsApplyCallback applyCallback)
        : owner_(owner),
          instance_(instance),
          config_(config),
          draft_(config),
          theme_(theme),
          appDirectory_(std::move(appDirectory)),
          httpRootBaseDirectory_(std::move(httpRootBaseDirectory)),
          httpServer_(httpServer),
          mainHotKeyRegistered_(mainHotKeyRegistered),
          applyCallback_(std::move(applyCallback)) {}

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QuattroSettingsDialog";
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(L"设置窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        const int settingsWidth = 560;
        const int settingsHeight = 480;
        const POINT position = OffsetWindowFromOwnerOnMonitor(owner_, settingsWidth, settingsHeight, 60, 70);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"设置",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
            position.x,
            position.y,
            settingsWidth,
            settingsHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(L"设置窗口创建失败: " + FormatLastError(GetLastError()));
            return false;
        }
        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN &&
                message.wParam == VK_TAB &&
                (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                ShowTab((currentTab_ + (reverse ? TabCount - 1 : 1)) % TabCount);
                continue;
            }
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return accepted_;
    }

    bool webDavDataImported() const {
        return importedData_;
    }

private:
    enum TabIndex {
        TabDisplay = 0,
        TabBehavior = 1,
        TabInteraction = 2,
        TabHotKeys = 3,
        TabLinks = 4,
        TabWebDav = 5,
        TabHttp = 6,
        TabBackup = 7,
        TabCount = 8,
    };

    struct TabChild {
        HWND hwnd = nullptr;
        int tab = 0;
    };

    struct FieldFrame {
        RECT rect{};
        HWND child = nullptr;
        int tab = 0;
        bool readOnly = false;
    };

    struct SectionFrame {
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

    void UsePanelBackground(HWND hwnd) {
        if (!hwnd) {
            return;
        }
        panelBackgroundChildren_.push_back(hwnd);
        wchar_t className[32]{};
        if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) && std::wcscmp(className, L"Button") == 0) {
            ThemedControls::SetControlBackgroundComponent(hwnd, L"panel");
        }
    }

    int ContentY(int y) const {
        return y + tabContentOffsetY_;
    }

    HWND Label(int tab, const wchar_t* text, int x, int y, int width = 110) {
        HWND hwnd = MakeUi().Label(text, x, ContentY(y), width);
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

    HWND MultiLineCheckBox(int tab, int id, const wchar_t* text, int x, int y, bool checked, int width, int height) {
        (void)height;
        ThemedCheckBoxOptions options{};
        options.checked = checked;
        options.size = ThemedCheckBoxSize::TwoLines;
        HWND hwnd = MakeUi().CheckBox(id, text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    ThemedUi MakeUi() const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        return ThemedUi(
            instance_, hwnd_, theme_, font_, DialogLayoutKind::Standard,
            client.right - client.left, client.bottom - client.top,
            const_cast<ThemedEditFrameCollection*>(&editFrameCollection_));
    }

    HWND Button(int tab, int id, const wchar_t* text, int x, int y, int width) {
        HWND hwnd = MakeUi().Button(id, text, x, ContentY(y), ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Fixed, width);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND FramedEdit(int tab, int id, int x, int y, int width, const std::wstring& text, DWORD extraStyle = ES_AUTOHSCROLL) {
        const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
        y = ContentY(y);
        const RECT frame{x, y, x + width, y + fieldHeight};
        ThemedEditOptions options{};
        options.readOnly = (extraStyle & ES_READONLY) != 0;
        options.content = (extraStyle & ES_PASSWORD) != 0
            ? ThemedEditContent::Password
            : ((extraStyle & ES_NUMBER) != 0 ? ThemedEditContent::Integer : ThemedEditContent::Text);
        HWND hwnd = MakeUi().Edit(id, frame, text, options);
        AddTabChild(hwnd, tab);
        fieldFrames_.push_back(FieldFrame{frame, hwnd, tab, false});
        return hwnd;
    }

    HWND FramedStatic(int tab, int x, int y, int width, const std::wstring& text) {
        const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
        y = ContentY(y);
        const RECT frame{x, y, x + width, y + fieldHeight};
        ThemedEditOptions options{};
        options.readOnly = true;
        HWND hwnd = MakeUi().Edit(nextGeneratedControlId_++, frame, text, options);
        AddTabChild(hwnd, tab);
        fieldFrames_.push_back(FieldFrame{frame, hwnd, tab, true});
        return hwnd;
    }

    HWND NumberEdit(int tab, int id, int x, int y, int width, int value) {
        return FramedEdit(tab, id, x, y, width, std::to_wstring(value), ES_NUMBER);
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

    int TextWidth(const wchar_t* text) const {
        HDC dc = hwnd_ ? GetDC(hwnd_) : nullptr;
        if (!dc) {
            return static_cast<int>(std::wcslen(text)) * 14;
        }
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT)));
        SIZE size{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(std::wcslen(text)), &size);
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
        ReleaseDC(hwnd_, dc);
        return size.cx;
    }

    int SettingsTabWidth(const wchar_t* title) const {
        const int minWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupItemWidth", 58.0f));
        const int paddingX = static_cast<int>(theme_.metric(L"tabButton", L"paddingX", 12.0f));
        return std::max(minWidth, TextWidth(title) + paddingX * 2 + 4);
    }

    void CreateTabs() {
        const wchar_t* titles[] = {L"显示", L"行为", L"交互", L"热键", L"链接", L"WebDAV", L"HTTP", L"备份"};
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = std::max(1, static_cast<int>(client.right - client.left));
        const DialogLayoutMetrics layout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Compact);
        const int startY = 15;
        std::vector<ThemedTabItem> items;
        items.reserve(TabCount);
        for (int i = 0; i < TabCount; ++i) {
            items.push_back(ThemedTabItem{ID_SETTINGS_TAB_BASE + i, titles[i], true});
        }
        const int itemHeight = ThemedControls::TabButtonHeight(theme_);
        tabStripRect_ = RECT{
            layout.contentInsetX,
            startY,
            clientWidth - layout.contentInsetX,
            startY + itemHeight + 8};
        ThemedTabControlOptions options{};
        options.activeIndex = TabDisplay;
        options.equalWidth = false;
        settingsTabs_ = MakeUi().TabControl(ID_SETTINGS_TAB_CONTROL, tabStripRect_, items, options);
        tabContentOffsetY_ = 0;
    }

    void PaintTabs(HDC) {}

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
        InvalidateRect(hwnd_, &tabStripRect_, FALSE);
    }

    bool IsFieldChild(HWND hwnd) const {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == hwnd) {
                return true;
            }
        }
        return false;
    }

    bool IsPanelBackgroundChild(HWND hwnd) const {
        return std::find(panelBackgroundChildren_.begin(), panelBackgroundChildren_.end(), hwnd) != panelBackgroundChildren_.end();
    }

    void InvalidateField(HWND hwnd) {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == hwnd) {
                InvalidateRect(hwnd_, &frame.rect, TRUE);
                return;
            }
        }
    }

    void AddSectionFrame(int tab, RECT rect) {
        rect.top = ContentY(rect.top);
        rect.bottom = ContentY(rect.bottom);
        HWND group = MakeUi().GroupBox(nextGeneratedControlId_++, L"", rect);
        AddTabChild(group, tab);
        sectionFrames_.push_back(SectionFrame{group, tab});
    }

    void PaintSectionFrames(HDC) {}

    void PaintFields(HDC dc) {
        editFrameCollection_.Draw(theme_, dc);
    }

    void ReadDraft() {
        draft_.showTitle = SendMessageW(showTitle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showGroup = SendMessageW(showGroup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showTag = SendMessageW(showTag_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.autoDock = SendMessageW(autoDock_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideWhenInactive = SendMessageW(hideInactive_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideAfterLink = SendMessageW(hideAfterLink_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideOnStart = SendMessageW(hideOnStart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.doubleClickToRun = SendMessageW(doubleClick_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideNotifyIcon = false;
        draft_.deleteConfirm = SendMessageW(deleteConfirm_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.saveRunCount = SendMessageW(saveRunCount_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showToolboxButton = SendMessageW(showToolboxButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showSkinButton = SendMessageW(showSkinButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.autoRun = SendMessageW(autoRun_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.loggingEnabled = SendMessageW(loggingEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.linkNameSingleLine = SendMessageW(linkNameSingleLine_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showTooltip = SendMessageW(showTooltip_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.groupRight = SendMessageW(groupRight_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.tagRight = SendMessageW(tagRight_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.mouseEnterActiveGroup = SendMessageW(enterActiveGroup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.mouseEnterActiveTag = SendMessageW(enterActiveTag_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.tagAlign = tagAlignIndex_ == 0 ? L"left" : (tagAlignIndex_ == 2 ? L"right" : L"center");
        auto alpha = ParseInt(GetText(alphaEdit_));
        draft_.alpha = alpha ? std::max(64, std::min(255, *alpha)) : 255;
        draft_.groupWidth = ClampNumber(groupWidthEdit_, 40, 240, draft_.groupWidth);
        draft_.tagWidth = ClampNumber(tagWidthEdit_, 40, 240, draft_.tagWidth);
        draft_.dockDelay = ClampNumber(dockDelayEdit_, 0, 5000, draft_.dockDelay);
        draft_.activeGroupDelay = ClampNumber(groupDelayEdit_, 0, 5000, draft_.activeGroupDelay);
        draft_.activeTagDelay = ClampNumber(tagDelayEdit_, 0, 5000, draft_.activeTagDelay);
        draft_.openDirCommand = GetText(openDirEdit_);
        draft_.helpUrl = GetText(helpUrlEdit_);
        draft_.updateUrl = GetText(updateUrlEdit_);
        draft_.faqUrl = GetText(faqUrlEdit_);
        draft_.rewardUrl = GetText(rewardUrlEdit_);
        draft_.webDavEnabled = SendMessageW(webDavEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.webDavUrl = GetText(webDavUrlEdit_);
        draft_.webDavRemotePath = GetText(webDavRemotePathEdit_);
        draft_.webDavUserName = GetText(webDavUserNameEdit_);
        draft_.webDavKeepCount = ClampNumber(webDavKeepCountEdit_, 1, 100, draft_.webDavKeepCount);
        if (Trim(draft_.webDavRemotePath).empty()) {
            draft_.webDavRemotePath = L"/Quattro/backups/";
        }
        draft_.httpServerEnabled = httpServer_ && httpServer_->IsRunning();
        draft_.httpServerAutoStart = httpServerAutoStart_ && SendMessageW(httpServerAutoStart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.httpServerLanAccess = true;
        draft_.httpServerPort = ParseHttpPortText(GetText(httpServerAddressEdit_), draft_.httpServerPort);
        draft_.httpServerRootPath = GetText(httpServerRootEdit_);
        if (Trim(draft_.httpServerRootPath).empty()) {
            draft_.httpServerRootPath = LocalHttpServerService::DefaultRootPath(httpRootBaseDirectory_).wstring();
        }
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

    bool ValidateMainHotKeyBeforeSave() {
        const HotKeyAvailability availability = CheckMainHotKeyAvailability(hwnd_, draft_.mainHotKey, CurrentRegisteredMainHotKey());
        UpdateHotKeyLabels();
        if (availability.available) {
            return true;
        }

        ShowThemedMessageBox(hwnd_, instance_, theme_, MainHotKeyConflictMessage(draft_.mainHotKey, availability), L"热键冲突", MB_OK | MB_ICONWARNING);
        return false;
    }

    AppConfig ReadWebDavDraftFromControls() {
        AppConfig value = draft_;
        value.webDavEnabled = SendMessageW(webDavEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        value.webDavUrl = GetText(webDavUrlEdit_);
        value.webDavRemotePath = GetText(webDavRemotePathEdit_);
        value.webDavUserName = GetText(webDavUserNameEdit_);
        value.webDavKeepCount = ClampNumber(webDavKeepCountEdit_, 1, 100, value.webDavKeepCount);
        if (Trim(value.webDavRemotePath).empty()) {
            value.webDavRemotePath = L"/Quattro/backups/";
        }
        return value;
    }

    bool SaveWebDavPasswordIfNeeded() {
        const std::wstring password = GetText(webDavPasswordEdit_);
        if (password.empty()) {
            return true;
        }
        std::wstring error;
        if (!WebDavCredentialService::SavePassword(draft_, password, error)) {
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
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"WebDAV 密码已清除。", L"WebDAV 备份", MB_OK | MB_ICONINFORMATION);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
        }
    }

    bool PrepareWebDavOperation() {
        ReadDraft();
        if (!SaveWebDavPasswordIfNeeded()) {
            return false;
        }
        return true;
    }

    void SetWebDavBusy(bool busy, SettingsWebDavOperation operation = SettingsWebDavOperation::Test) {
        webDavBusy_ = busy;
        if (webDavUploadButton_) {
            EnableWindow(webDavUploadButton_, !busy);
            SetWindowTextW(webDavUploadButton_, busy && operation == SettingsWebDavOperation::Upload ? L"上传中..." : L"上传到云端");
        }
        if (webDavDownloadButton_) {
            EnableWindow(webDavDownloadButton_, !busy);
            const bool downloadBusy = operation == SettingsWebDavOperation::List || operation == SettingsWebDavOperation::Download;
            SetWindowTextW(webDavDownloadButton_, busy && downloadBusy ? L"处理中..." : L"从云端下载");
        }
        if (webDavTestButton_) {
            EnableWindow(webDavTestButton_, !busy);
            SetWindowTextW(webDavTestButton_, busy && operation == SettingsWebDavOperation::Test ? L"测试中..." : L"测试连接");
        }
        if (webDavClearPasswordButton_) {
            EnableWindow(webDavClearPasswordButton_, !busy);
        }
        if (okButton_) {
            EnableWindow(okButton_, !busy);
        }
        if (cancelButton_) {
            EnableWindow(cancelButton_, !busy);
        }
        if (applyButton_) {
            EnableWindow(applyButton_, !busy);
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
        if (!PrepareWebDavOperation()) {
            return;
        }
        SetWebDavBusy(true, SettingsWebDavOperation::Upload);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        const AppConfig config = draft_;
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
        if (!PrepareWebDavOperation()) {
            return;
        }
        SetWebDavBusy(true, SettingsWebDavOperation::List);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        const AppConfig config = draft_;
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
            ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"WebDAV 备份", MB_OK | (result->ok ? MB_ICONINFORMATION : MB_ICONWARNING));
            return;
        case SettingsWebDavOperation::Upload:
            ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"上传到云端", MB_OK | (result->ok ? MB_ICONINFORMATION : MB_ICONWARNING));
            return;
        case SettingsWebDavOperation::List:
            ContinueWebDavDownloadSelection(*result);
            return;
        case SettingsWebDavOperation::Download:
            if (result->ok) {
                importedData_ = true;
            }
            ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"从云端下载", MB_OK | (result->ok ? MB_ICONINFORMATION : MB_ICONWARNING));
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
        ShowThemedMessageBox(hwnd_, instance_, theme_, FormatConfigPackageReportText(report), L"导出配置包", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
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
        }
        ShowThemedMessageBox(hwnd_, instance_, theme_, FormatConfigPackageReportText(report), L"合并导入配置包", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    void ExportTodosJson() {
        StorageService storage(appDirectory_);
        const AppModel model = storage.Load();
        TodoExportOptions options;
        options.includeCompleted = !todoIncludeCompleted_ || SendMessageW(todoIncludeCompleted_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        options.includeDisabled = !todoIncludeDisabled_ || SendMessageW(todoIncludeDisabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        options.onlyFuture = todoOnlyFuture_ && SendMessageW(todoOnlyFuture_, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
        ShowThemedMessageBox(hwnd_, instance_, theme_, L"待办 JSON 导出完成。", L"导出待办 JSON", MB_OK | MB_ICONINFORMATION);
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
        }
        ShowThemedMessageBox(hwnd_, instance_, theme_, report.message.empty() ? (report.ok ? L"导入完成。" : L"导入失败。") : report.message,
            L"导入待办 JSON", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    AppConfig ReadHttpDraftFromControls() {
        AppConfig value = draft_;
        value.httpServerEnabled = httpServer_ && httpServer_->IsRunning();
        value.httpServerAutoStart = httpServerAutoStart_ && SendMessageW(httpServerAutoStart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
        if (httpStartButton_) {
            EnableWindow(httpStartButton_, running ? FALSE : TRUE);
            InvalidateRect(httpStartButton_, nullptr, TRUE);
        }
        if (httpStopButton_) {
            EnableWindow(httpStopButton_, running ? TRUE : FALSE);
            InvalidateRect(httpStopButton_, nullptr, TRUE);
        }
        if (httpRestartButton_) {
            InvalidateRect(httpRestartButton_, nullptr, TRUE);
        }
        if (httpServerAddressEdit_) {
            EnableWindow(httpServerAddressEdit_, running ? FALSE : TRUE);
            InvalidateField(httpServerAddressEdit_);
        }
        if (httpServerRootEdit_) {
            EnableWindow(httpServerRootEdit_, running ? FALSE : TRUE);
            InvalidateField(httpServerRootEdit_);
        }
        if (httpBrowseRootButton_) {
            EnableWindow(httpBrowseRootButton_, running ? FALSE : TRUE);
            InvalidateRect(httpBrowseRootButton_, nullptr, TRUE);
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
        InvalidateRect(httpServerStatusTag_, nullptr, TRUE);
        InvalidateRect(httpServerStatusDetail_, nullptr, TRUE);
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

    void CopyHttpUrl() {
        const std::wstring url = httpServer_ && httpServer_->IsRunning()
            ? HttpAddressText(httpServer_->options().lanAccess, httpServer_->options().port, true)
            : CurrentHttpAddress(true);
        if (!OpenClipboard(hwnd_)) {
            return;
        }
        EmptyClipboard();
        const SIZE_T bytes = (url.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* target = GlobalLock(memory);
            if (target) {
                memcpy(target, url.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
                memory = nullptr;
            }
        }
        if (memory) {
            GlobalFree(memory);
        }
        CloseClipboard();
    }

    void StartHttpServerFromDialog(bool restart) {
        if (!httpServer_) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"HTTP 服务对象不可用。", L"HTTP 服务", MB_OK | MB_ICONWARNING);
            return;
        }
        AppConfig value = ReadHttpDraftFromControls();
        value.httpServerEnabled = true;
        draft_ = value;
        std::wstring error;
        const auto options = LocalHttpServerService::OptionsFromConfig(value, httpRootBaseDirectory_);
        const bool ok = restart ? httpServer_->Restart(options, error) : httpServer_->Start(options, error);
        UpdateHttpStatusLabel();
        ShowThemedMessageBox(hwnd_, instance_, theme_, ok ? L"HTTP 服务已启动。" : error, L"HTTP 服务", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    void StopHttpServerFromDialog() {
        if (!httpServer_) {
            return;
        }
        httpServer_->Stop();
        draft_ = ReadHttpDraftFromControls();
        draft_.httpServerEnabled = false;
        UpdateHttpStatusLabel();
        ShowThemedMessageBox(hwnd_, instance_, theme_, L"HTTP 服务已停止。", L"HTTP 服务", MB_OK | MB_ICONINFORMATION);
    }

    bool CommitSettings(bool closeAfterCommit) {
        if (webDavBusy_) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"WebDAV 操作正在进行，请稍候完成。", L"WebDAV 备份", MB_OK | MB_ICONINFORMATION);
            return false;
        }
        ReadDraft();
        if (!ValidateMainHotKeyBeforeSave()) {
            return false;
        }
        if (!SaveWebDavPasswordIfNeeded()) {
            return false;
        }

        config_ = draft_;
        if (!closeAfterCommit && applyCallback_) {
            mainHotKeyRegistered_ = applyCallback_(config_, importedData_);
            importedData_ = false;
            draft_ = config_;
            UpdateHotKeyLabels();
            UpdateHttpStatusLabel();
        }
        accepted_ = true;
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
            editFont_ = ThemedControls::CreateEditFont(theme_);
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            fieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            readOnlyFieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"field", L"readonly", L"bg")));
            panelBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"panel", L"normal", L"bg")));
            CreateTabs();

            showTitle_ = CheckBox(TabDisplay, 101, L"显示标题栏", 34, 64, draft_.showTitle);
            showGroup_ = CheckBox(TabDisplay, 102, L"显示分组栏", 282, 64, draft_.showGroup);
            showTag_ = CheckBox(TabDisplay, 103, L"显示标签栏", 34, 94, draft_.showTag);
            showToolboxButton_ = CheckBox(TabDisplay, 115, L"显示工具箱按钮", 282, 124, draft_.showToolboxButton);
            showSkinButton_ = CheckBox(TabDisplay, 121, L"显示主题按钮", 34, 124, draft_.showSkinButton);
            linkNameSingleLine_ = CheckBox(TabDisplay, 118, L"启动项名称单行", 282, 154, draft_.linkNameSingleLine);
            showTooltip_ = CheckBox(TabDisplay, 119, L"显示提示", 34, 154, draft_.showTooltip);
            groupRight_ = CheckBox(TabDisplay, 120, L"分组栏在右侧", 282, 184, draft_.groupRight);
            tagRight_ = CheckBox(TabDisplay, 122, L"标签栏在右侧", 34, 184, draft_.tagRight);

            Label(TabDisplay, L"透明度", 34, 260, 76);
            alphaEdit_ = NumberEdit(TabDisplay, 201, 118, 254, 78, draft_.alpha);
            Label(TabDisplay, L"标签文字", 282, 260, 72);
            const ThemedUi tagAlignUi = MakeUi();
            tagAlignLeft_ = tagAlignUi.TabButton(ID_TAG_ALIGN_LEFT, L"左", 364, ContentY(255), 36, false);
            tagAlignCenter_ = tagAlignUi.TabButton(ID_TAG_ALIGN_CENTER, L"中", 404, ContentY(255), 36, true);
            tagAlignRight_ = tagAlignUi.TabButton(ID_TAG_ALIGN_RIGHT, L"右", 444, ContentY(255), 36, false);
            AddTabChild(tagAlignLeft_, TabDisplay);
            AddTabChild(tagAlignCenter_, TabDisplay);
            AddTabChild(tagAlignRight_, TabDisplay);
            SelectTagAlign();

            Label(TabDisplay, L"分组宽度", 34, 314, 76);
            groupWidthEdit_ = NumberEdit(TabDisplay, ID_GROUP_WIDTH, 118, 308, 78, draft_.groupWidth);
            Label(TabDisplay, L"标签宽度", 282, 314, 72);
            tagWidthEdit_ = NumberEdit(TabDisplay, ID_TAG_WIDTH, 364, 308, 78, draft_.tagWidth);

            const DialogLayoutMetrics behaviorLayout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Compact);
            RECT settingsClient{};
            GetClientRect(hwnd_, &settingsClient);
            const int settingsClientWidth = static_cast<int>(settingsClient.right - settingsClient.left);
            const int behaviorCheckWidth = 148;
            const int behaviorFieldWidth = 88;
            const int behaviorDelayLabelWidth = 76;
            const int behaviorUnitWidth = 32;
            const int behaviorColumnGap = std::max(behaviorLayout.controlGapX * 3, 30);
            const int behaviorColumnWidth = std::max(
                behaviorCheckWidth,
                behaviorDelayLabelWidth + behaviorLayout.labelGap + behaviorFieldWidth + behaviorLayout.controlGapX + behaviorUnitWidth);
            const int behaviorLeft = behaviorLayout.CenteredGroupX(settingsClientWidth, behaviorColumnWidth * 2 + behaviorColumnGap);
            const int behaviorRight = behaviorLeft + behaviorColumnWidth + behaviorColumnGap;
            const int behaviorRowStep = std::max(28, ThemedControls::CheckBoxHeight(theme_) + std::max(4, behaviorLayout.rowGap - 2));
            const int behaviorPanelPaddingX = static_cast<int>(theme_.metric(L"panel", L"paddingX", 10.0f));
            const int behaviorPanelPaddingY = static_cast<int>(theme_.metric(L"panel", L"paddingY", 8.0f));
            const int behaviorTitleGap = ThemedControls::LabelHeight(theme_) + std::max(3, behaviorLayout.rowGap - 3);
            const int behaviorFrameGap = std::max(10, behaviorLayout.sectionGap - 2);
            const int behaviorHeadingWidth = 128;
            const int behaviorFrameLeft = behaviorLeft - behaviorPanelPaddingX;
            const int behaviorFrameRight = behaviorRight + behaviorColumnWidth + behaviorPanelPaddingX;
            const int behaviorWindowFrameTop = 44 + behaviorLayout.sectionGap;
            const int behaviorWindowHeadingY = behaviorWindowFrameTop + behaviorPanelPaddingY;
            const int behaviorWindowRowY = behaviorWindowHeadingY + behaviorTitleGap;
            const int behaviorWindowFrameBottom = behaviorWindowRowY + behaviorRowStep * 2 + behaviorPanelPaddingY;
            AddSectionFrame(TabBehavior, RECT{behaviorFrameLeft, behaviorWindowFrameTop, behaviorFrameRight, behaviorWindowFrameBottom});
            UsePanelBackground(Label(TabBehavior, L"窗口行为", behaviorLeft, behaviorWindowHeadingY, behaviorHeadingWidth));
            autoDock_ = CheckBox(TabBehavior, 105, L"贴边自动隐藏", behaviorLeft, behaviorWindowRowY, draft_.autoDock, behaviorCheckWidth);
            UsePanelBackground(autoDock_);
            UsePanelBackground(Label(TabBehavior, L"停靠延迟", behaviorRight, behaviorWindowRowY + 6, behaviorDelayLabelWidth));
            dockDelayEdit_ = NumberEdit(
                TabBehavior,
                ID_DOCK_DELAY,
                behaviorRight + behaviorDelayLabelWidth + behaviorLayout.labelGap,
                behaviorWindowRowY,
                behaviorFieldWidth,
                draft_.dockDelay);
            UsePanelBackground(Label(
                TabBehavior,
                L"ms",
                behaviorRight + behaviorDelayLabelWidth + behaviorLayout.labelGap + behaviorFieldWidth + behaviorLayout.controlGapX,
                behaviorWindowRowY + 6,
                behaviorUnitWidth));
            hideInactive_ = CheckBox(TabBehavior, 106, L"失焦隐藏", behaviorLeft, behaviorWindowRowY + behaviorRowStep, draft_.hideWhenInactive, behaviorCheckWidth);
            UsePanelBackground(hideInactive_);

            const int behaviorRunFrameTop = behaviorWindowFrameBottom + behaviorFrameGap;
            const int behaviorRunHeadingY = behaviorRunFrameTop + behaviorPanelPaddingY;
            const int behaviorRunRowY = behaviorRunHeadingY + behaviorTitleGap;
            const int behaviorRunFrameBottom = behaviorRunRowY + behaviorRowStep * 2 + behaviorPanelPaddingY;
            AddSectionFrame(TabBehavior, RECT{behaviorFrameLeft, behaviorRunFrameTop, behaviorFrameRight, behaviorRunFrameBottom});
            UsePanelBackground(Label(TabBehavior, L"运行与数据", behaviorLeft, behaviorRunHeadingY, behaviorHeadingWidth));
            hideAfterLink_ = CheckBox(TabBehavior, 107, L"启动项运行后隐藏", behaviorLeft, behaviorRunRowY, draft_.hideAfterLink, behaviorCheckWidth);
            saveRunCount_ = CheckBox(TabBehavior, 112, L"记录运行次数", behaviorRight, behaviorRunRowY, draft_.saveRunCount, behaviorCheckWidth);
            deleteConfirm_ = CheckBox(TabBehavior, 111, L"删除前确认", behaviorLeft, behaviorRunRowY + behaviorRowStep, draft_.deleteConfirm, behaviorCheckWidth);
            UsePanelBackground(hideAfterLink_);
            UsePanelBackground(saveRunCount_);
            UsePanelBackground(deleteConfirm_);

            const int behaviorSystemFrameTop = behaviorRunFrameBottom + behaviorFrameGap;
            const int behaviorSystemHeadingY = behaviorSystemFrameTop + behaviorPanelPaddingY;
            const int behaviorSystemRowY = behaviorSystemHeadingY + behaviorTitleGap;
            const int behaviorSystemFrameBottom = behaviorSystemRowY + behaviorRowStep * 2 + behaviorPanelPaddingY;
            AddSectionFrame(TabBehavior, RECT{behaviorFrameLeft, behaviorSystemFrameTop, behaviorFrameRight, behaviorSystemFrameBottom});
            UsePanelBackground(Label(TabBehavior, L"系统集成", behaviorLeft, behaviorSystemHeadingY, behaviorHeadingWidth));
            hideOnStart_ = CheckBox(TabBehavior, 116, L"启动后隐藏", behaviorLeft, behaviorSystemRowY, draft_.hideOnStart, behaviorCheckWidth);
            autoRun_ = CheckBox(TabBehavior, 117, L"开机启动", behaviorRight, behaviorSystemRowY, draft_.autoRun, behaviorCheckWidth);
            loggingEnabled_ = CheckBox(TabBehavior, ID_LOGGING_ENABLED, L"启用日志", behaviorLeft, behaviorSystemRowY + behaviorRowStep, draft_.loggingEnabled, behaviorCheckWidth);
            UsePanelBackground(hideOnStart_);
            UsePanelBackground(autoRun_);
            UsePanelBackground(loggingEnabled_);

            doubleClick_ = CheckBox(TabInteraction, 109, L"双击运行", 34, 64, draft_.doubleClickToRun);
            enterActiveGroup_ = CheckBox(TabInteraction, 124, L"鼠标进入激活分组", 34, 94, draft_.mouseEnterActiveGroup);
            enterActiveTag_ = CheckBox(TabInteraction, 125, L"鼠标进入激活标签", 282, 94, draft_.mouseEnterActiveTag);
            Label(TabInteraction, L"分组激活延迟", 34, 154, 100);
            groupDelayEdit_ = NumberEdit(TabInteraction, ID_GROUP_DELAY, 144, 148, 88, draft_.activeGroupDelay);
            Label(TabInteraction, L"ms", 240, 154, 32);
            Label(TabInteraction, L"标签激活延迟", 282, 154, 100);
            tagDelayEdit_ = NumberEdit(TabInteraction, ID_TAG_DELAY, 392, 148, 88, draft_.activeTagDelay);
            Label(TabInteraction, L"ms", 488, 154, 32);

            const DialogLayoutMetrics hotKeyLayout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Compact);
            const int hotKeyLabelWidth = 84;
            const int hotKeyFieldWidth = 210;
            const int hotKeyButtonWidth = 56;
            const int hotKeyRowWidth = hotKeyLabelWidth + hotKeyLayout.labelGap + hotKeyFieldWidth +
                hotKeyLayout.controlGapX + hotKeyButtonWidth + hotKeyLayout.controlGapX + hotKeyButtonWidth;
            const int hotKeyX = hotKeyLayout.CenteredGroupX(560, hotKeyRowWidth);
            const int hotKeyFieldX = hotKeyX + hotKeyLabelWidth + hotKeyLayout.labelGap;
            const int hotKeyCaptureX = hotKeyFieldX + hotKeyFieldWidth + hotKeyLayout.controlGapX;
            const int hotKeyClearX = hotKeyCaptureX + hotKeyButtonWidth + hotKeyLayout.controlGapX;
            Label(TabHotKeys, L"主窗口热键", hotKeyX, 74, hotKeyLabelWidth);
            mainHotKeyText_ = FramedStatic(TabHotKeys, hotKeyFieldX, 66, hotKeyFieldWidth, FormatMainHotKeyText(draft_.mainHotKey));
            Button(TabHotKeys, ID_MAIN_HOTKEY_CAPTURE, L"录入", hotKeyCaptureX, 68, hotKeyButtonWidth);
            Button(TabHotKeys, ID_MAIN_HOTKEY_CLEAR, L"清除", hotKeyClearX, 68, hotKeyButtonWidth);
            mainHotKeyStatus_ = Label(TabHotKeys, L"", hotKeyX, 112, hotKeyRowWidth);
            UpdateHotKeyLabels();

            Label(TabLinks, L"打开目录命令", 34, 68, 110);
            openDirEdit_ = FramedEdit(TabLinks, 202, 34, 92, 446, draft_.openDirCommand);
            Label(TabLinks, L"帮助链接", 34, 136, 110);
            helpUrlEdit_ = FramedEdit(TabLinks, 203, 34, 160, 446, draft_.helpUrl);
            Label(TabLinks, L"更新链接", 34, 204, 110);
            updateUrlEdit_ = FramedEdit(TabLinks, 204, 34, 228, 446, draft_.updateUrl);
            Label(TabLinks, L"FAQ 链接", 34, 272, 110);
            faqUrlEdit_ = FramedEdit(TabLinks, 205, 34, 296, 206, draft_.faqUrl);
            Label(TabLinks, L"赞助链接", 282, 272, 110);
            rewardUrlEdit_ = FramedEdit(TabLinks, 206, 282, 296, 198, draft_.rewardUrl);

            webDavEnabled_ = CheckBox(TabWebDav, 208, L"启用 WebDAV 备份", 34, 64, draft_.webDavEnabled, 220);
            Label(TabWebDav, L"服务器地址", 34, 112, 110);
            webDavUrlEdit_ = FramedEdit(TabWebDav, 209, 34, 136, 446, draft_.webDavUrl);
            Label(TabWebDav, L"远端目录", 34, 184, 110);
            webDavRemotePathEdit_ = FramedEdit(TabWebDav, 210, 34, 208, 206, draft_.webDavRemotePath);
            Label(TabWebDav, L"保留数量", 282, 184, 110);
            webDavKeepCountEdit_ = NumberEdit(TabWebDav, 211, 282, 208, 90, draft_.webDavKeepCount);
            Label(TabWebDav, L"用户名", 34, 256, 110);
            webDavUserNameEdit_ = FramedEdit(TabWebDav, 212, 34, 280, 206, draft_.webDavUserName);
            Label(TabWebDav, L"密码/应用密码", 282, 256, 130);
            webDavPasswordEdit_ = FramedEdit(TabWebDav, 213, 282, 280, 198, L"", ES_AUTOHSCROLL | ES_PASSWORD);
            webDavUploadButton_ = Button(TabWebDav, ID_WEBDAV_UPLOAD, L"上传到云端", 34, 340, 104);
            webDavDownloadButton_ = Button(TabWebDav, ID_WEBDAV_DOWNLOAD, L"从云端下载", 150, 340, 104);
            webDavTestButton_ = Button(TabWebDav, ID_WEBDAV_TEST, L"测试连接", 286, 340, 92);
            webDavClearPasswordButton_ = Button(TabWebDav, ID_WEBDAV_CLEAR_PASSWORD, L"清除密码", 390, 340, 90);

            const DialogLayoutMetrics httpLayout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Compact);
            const int httpPanelPaddingX = static_cast<int>(theme_.metric(L"panel", L"paddingX", 10.0f));
            const int httpPanelPaddingY = static_cast<int>(theme_.metric(L"panel", L"paddingY", 8.0f));
            const int httpFrameWidth = 482;
            const int httpFrameLeft = httpLayout.CenteredGroupX(settingsClientWidth, httpFrameWidth);
            const int httpFrameRight = httpFrameLeft + httpFrameWidth;
            const int httpContentLeft = httpFrameLeft + httpPanelPaddingX;
            const int httpContentRight = httpFrameRight - httpPanelPaddingX;
            const int httpHeadingWidth = 128;
            const int httpLabelWidth = 92;
            const int httpFieldX = httpContentLeft + httpLabelWidth + httpLayout.labelGap;
            const int httpRowStep = httpLayout.RowStep(ThemedControls::CheckBoxHeight(theme_));
            const int httpFieldRowStep = httpLayout.RowStep(ThemedControls::EditFrameHeight(theme_));
            const int httpLabelRowStep = httpLayout.RowStep(ThemedControls::LabelHeight(theme_));
            const int httpButtonRowStep = httpLayout.RowStep(ThemedControls::CompactButtonHeight(theme_));
            const int httpTitleGap = ThemedControls::LabelHeight(theme_) + std::max(3, httpLayout.rowGap - 3);
            const int httpFrameGap = httpLayout.sectionGap;

            const int httpOptionsFrameTop = 44 + httpLayout.sectionGap;
            const int httpOptionsHeadingY = httpOptionsFrameTop + httpPanelPaddingY;
            const int httpOptionsRowY = httpOptionsHeadingY + httpTitleGap;
            const int httpOptionsFrameBottom = httpOptionsRowY + httpRowStep + httpPanelPaddingY;
            AddSectionFrame(TabHttp, RECT{httpFrameLeft, httpOptionsFrameTop, httpFrameRight, httpOptionsFrameBottom});
            UsePanelBackground(Label(TabHttp, L"服务选项", httpContentLeft, httpOptionsHeadingY, httpHeadingWidth));
            httpServerAutoStart_ = CheckBox(TabHttp, 215, L"随应用启动", httpContentLeft, httpOptionsRowY, draft_.httpServerAutoStart, 220);
            UsePanelBackground(httpServerAutoStart_);

            const int httpBindingFrameTop = httpOptionsFrameBottom + httpFrameGap;
            const int httpBindingHeadingY = httpBindingFrameTop + httpPanelPaddingY;
            const int httpBindingRowY = httpBindingHeadingY + httpTitleGap;
            const int httpBindingFrameBottom = httpBindingRowY + httpFieldRowStep + ThemedControls::EditFrameHeight(theme_) + httpPanelPaddingY;
            AddSectionFrame(TabHttp, RECT{httpFrameLeft, httpBindingFrameTop, httpFrameRight, httpBindingFrameBottom});
            UsePanelBackground(Label(TabHttp, L"站点绑定", httpContentLeft, httpBindingHeadingY, httpHeadingWidth));
            UsePanelBackground(Label(TabHttp, L"站点网址", httpContentLeft, httpBindingRowY + 6, httpLabelWidth));
            httpServerAddressEdit_ = FramedEdit(
                TabHttp,
                ID_HTTP_ADDRESS,
                httpFieldX,
                httpBindingRowY,
                206,
                HttpAddressText(true, draft_.httpServerPort, false));
            UsePanelBackground(Label(TabHttp, L"绑定磁盘路径", httpContentLeft, httpBindingRowY + httpFieldRowStep + 6, httpLabelWidth));
            httpServerRootEdit_ = FramedEdit(
                TabHttp,
                218,
                httpFieldX,
                httpBindingRowY + httpFieldRowStep,
                206,
                Trim(draft_.httpServerRootPath).empty() ? LocalHttpServerService::DefaultRootPath(httpRootBaseDirectory_).wstring() : draft_.httpServerRootPath);
            const int httpBrowseX = httpFieldX + 206 + httpLayout.controlGapX;
            httpBrowseRootButton_ = Button(TabHttp, ID_HTTP_BROWSE_ROOT, L"选择", httpBrowseX, httpBindingRowY + httpFieldRowStep + 1, 54);
            Button(TabHttp, ID_HTTP_OPEN_ROOT, L"打开目录", httpBrowseX + 54 + httpLayout.controlGapX, httpBindingRowY + httpFieldRowStep + 1, 84);

            const int httpControlFrameTop = httpBindingFrameBottom + httpFrameGap;
            const int httpControlHeadingY = httpControlFrameTop + httpPanelPaddingY;
            const int httpStatusY = httpControlHeadingY + httpTitleGap;
            const int httpButtonY = httpStatusY + httpLabelRowStep;
            const int httpConfigY = httpButtonY + httpButtonRowStep;
            const int httpControlFrameBottom = httpConfigY + ThemedControls::CompactButtonHeight(theme_) + httpPanelPaddingY;
            AddSectionFrame(TabHttp, RECT{httpFrameLeft, httpControlFrameTop, httpFrameRight, httpControlFrameBottom});
            UsePanelBackground(Label(TabHttp, L"运行控制", httpContentLeft, httpControlHeadingY, httpHeadingWidth));
            UsePanelBackground(Label(TabHttp, L"状态", httpContentLeft, httpStatusY + 4, 34));
            httpServerStatusTag_ = StatusBadge(
                TabHttp, L"", httpContentLeft + 42, httpStatusY + 4, 64, ThemedStatusRole::Danger);
            httpServerStatusDetail_ = Label(TabHttp, L"", httpContentLeft + 112, httpStatusY + 4, httpContentRight - httpContentLeft - 112);
            UsePanelBackground(httpServerStatusTag_);
            ThemedControls::SetControlBackgroundComponent(httpServerStatusTag_, L"panel");
            UsePanelBackground(httpServerStatusDetail_);
            httpStartButton_ = Button(TabHttp, ID_HTTP_START, L"启动", httpContentLeft, httpButtonY, 72);
            httpStopButton_ = Button(TabHttp, ID_HTTP_STOP, L"停止", httpContentLeft + 82, httpButtonY, 72);
            httpRestartButton_ = Button(TabHttp, ID_HTTP_RESTART, L"重启", httpContentLeft + 164, httpButtonY, 72);
            Button(TabHttp, ID_HTTP_OPEN_HOME, L"打开网站", httpContentLeft + 248, httpButtonY, 84);
            Button(TabHttp, ID_HTTP_COPY_URL, L"复制地址", httpContentLeft + 342, httpButtonY, 84);
            Button(TabHttp, ID_HTTP_OPEN_CONFIG_DIR, L"配置目录", httpContentLeft, httpConfigY, 92);
            UsePanelBackground(Label(TabHttp, L"详细权限、账号密码、上传、MIME 与下载策略请在配置目录的 http-server.ini 中修改；重启 HTTP 服务后生效。", httpContentLeft + 108, httpConfigY + 4, httpContentRight - httpContentLeft - 108));
            UpdateHttpStatusLabel();

            const DialogLayoutMetrics backupLayout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Compact);
            const int backupButtonWidth = 118;
            const int backupRowWidth = backupButtonWidth * 2 + backupLayout.controlGapX;
            const int backupGroupX = backupLayout.CenteredGroupX(560, backupRowWidth);
            const int backupNoteWidth = 360;
            const int backupNoteX = backupLayout.CenteredGroupX(560, backupNoteWidth);
            Label(TabBackup, L"配置包", backupGroupX, 92, 120);
            Button(TabBackup, ID_CONFIG_EXPORT, L"导出配置包", backupGroupX, 124, backupButtonWidth);
            Button(TabBackup, ID_CONFIG_IMPORT, L"导入配置包", backupGroupX + backupButtonWidth + backupLayout.controlGapX, 124, backupButtonWidth);
            Label(TabBackup, L"待办事项单独备份（JSON 格式）", backupNoteX, 210, backupNoteWidth);
            Button(TabBackup, ID_TODO_EXPORT, L"导出", backupGroupX, 242, backupButtonWidth);
            Button(TabBackup, ID_TODO_IMPORT, L"导入", backupGroupX + backupButtonWidth + backupLayout.controlGapX, 242, backupButtonWidth);
            todoIncludeCompleted_ = CheckBox(TabBackup, ID_TODO_INCLUDE_COMPLETED, L"含已完成", backupNoteX, 280, true, 112);
            todoIncludeDisabled_ = CheckBox(TabBackup, ID_TODO_INCLUDE_DISABLED, L"含已禁用", backupNoteX + 120, 280, true, 112);
            todoOnlyFuture_ = CheckBox(TabBackup, ID_TODO_ONLY_FUTURE, L"仅未来", backupNoteX + 240, 280, false, 104);
            Label(TabBackup, L"待办事项备份可用于 Quattro 恢复，也可通过 Apple 快捷指令导入提醒事项。", backupNoteX, 326, backupNoteWidth);

            RECT client{};
            GetClientRect(hwnd_, &client);
            const ThemedUi footerUi(instance_, hwnd_, theme_, font_, DialogLayoutKind::Compact, client.right - client.left, client.bottom - client.top);
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
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            PaintTabs(dc);
            PaintSectionFrames(dc);
            PaintFields(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: {
            return 1;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            HWND child = reinterpret_cast<HWND>(lParam);
            const bool fieldChild = IsFieldChild(child);
            SetTextColor(dc, ToColorRef(fieldChild ? theme_.color(L"field", L"readonly", L"text") : theme_.color(L"label", L"normal", L"text")));
            if (fieldChild && readOnlyFieldBrush_) {
                return reinterpret_cast<LRESULT>(readOnlyFieldBrush_);
            }
            if (IsPanelBackgroundChild(child) && panelBrush_) {
                return reinterpret_cast<LRESULT>(panelBrush_);
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_SETTINGS_WEBDAV_DONE:
            HandleWebDavResult(std::unique_ptr<SettingsWebDavResult>(reinterpret_cast<SettingsWebDavResult*>(lParam)));
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateField(reinterpret_cast<HWND>(lParam));
            }
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
                CommitSettings(false);
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                if (!CommitSettings(true)) {
                    return 0;
                }
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                if (webDavBusy_) {
                    ShowThemedMessageBox(hwnd_, instance_, theme_, L"WebDAV 操作正在进行，请稍候完成。", L"WebDAV 备份", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            if (webDavBusy_) {
                ShowThemedMessageBox(hwnd_, instance_, theme_, L"WebDAV 操作正在进行，请稍候完成。", L"WebDAV 备份", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (editFont_) {
                DeleteObject(editFont_);
                editFont_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
                fieldBrush_ = nullptr;
            }
            if (readOnlyFieldBrush_) {
                DeleteObject(readOnlyFieldBrush_);
                readOnlyFieldBrush_ = nullptr;
            }
            if (panelBrush_) {
                DeleteObject(panelBrush_);
                panelBrush_ = nullptr;
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

    void UpdateHotKeyLabels() {
        if (mainHotKeyText_) {
            SetWindowTextW(mainHotKeyText_, FormatMainHotKeyText(draft_.mainHotKey).c_str());
        }
        if (mainHotKeyStatus_) {
            const HotKeyAvailability availability = CheckMainHotKeyAvailability(hwnd_, draft_.mainHotKey, CurrentRegisteredMainHotKey());
            SetWindowTextW(mainHotKeyStatus_, MainHotKeyStatusText(draft_.mainHotKey, availability).c_str());
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT editFont_ = nullptr;
    AppConfig& config_;
    AppConfig draft_;
    const Theme& theme_;
    std::filesystem::path appDirectory_;
    std::filesystem::path httpRootBaseDirectory_;
    LocalHttpServerService* httpServer_ = nullptr;
    bool mainHotKeyRegistered_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HBRUSH readOnlyFieldBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    bool ownsFont_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    int currentTab_ = -1;
    RECT tabStripRect_{};
    int tabContentOffsetY_ = 0;
    HWND settingsTabs_ = nullptr;
    std::vector<TabChild> tabChildren_;
    std::vector<SectionFrame> sectionFrames_;
    std::vector<FieldFrame> fieldFrames_;
    mutable ThemedEditFrameCollection editFrameCollection_;
    int nextGeneratedControlId_ = 6000;
    std::vector<HWND> panelBackgroundChildren_;
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
    HWND mainHotKeyText_ = nullptr;
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
    bool accepted_ = false;
    bool done_ = false;
    SettingsApplyCallback applyCallback_;
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
    SettingsApplyCallback applyCallback) {
    SettingsDialog dialog(owner, instance, config, theme, appDirectory, httpRootBaseDirectory, httpServer, mainHotKeyRegistered, std::move(applyCallback));
    const bool accepted = dialog.Run();
    if (importedData) {
        *importedData = dialog.webDavDataImported();
    }
    return accepted;
}
