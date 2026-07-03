#include "SimpleDialogs.h"

#include "../resources/resource.h"

#include "AppLog.h"
#include "ConfigPackageService.h"
#include "DialogLayout.h"
#include "HotKeyEditor.h"
#include "JsonValue.h"
#include "Storage.h"
#include "ThemedControls.h"
#include "Utilities.h"
#include "WebDavBackupService.h"
#include "WebDavCredentialService.h"

#include <commdlg.h>
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace {
constexpr int ID_SETTINGS_TAB_BASE = 280;
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
constexpr int ID_MESSAGE_TEXT = 501;

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
        report.pluginSettingsAdded > 0 || report.urlIconsAdded > 0) {
        text += L"\n\n新增分组: " + std::to_wstring(report.groupsAdded);
        text += L"\n复用分组: " + std::to_wstring(report.groupsMerged);
        text += L"\n新增标签: " + std::to_wstring(report.tagsAdded);
        text += L"\n复用标签: " + std::to_wstring(report.tagsMerged);
        text += L"\n新增启动项: " + std::to_wstring(report.linksAdded);
        text += L"\n跳过重复启动项: " + std::to_wstring(report.linksSkippedDuplicate);
        text += L"\n新增便签: " + std::to_wstring(report.notesAdded);
        text += L"\n合并便签: " + std::to_wstring(report.notesMerged);
        text += L"\n新增待办: " + std::to_wstring(report.todosAdded);
        text += L"\n新增工具设置: " + std::to_wstring(report.pluginSettingsAdded);
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

std::wstring BuildTodoExportJson(const AppModel& model) {
    std::wstringstream out;
    out << L"{\n";
    out << L"  \"app\": \"Quattro\",\n";
    out << L"  \"formatVersion\": 1,\n";
    out << L"  \"todos\": [\n";
    bool first = true;
    for (const auto& todo : model.todos) {
        const Group* tag = FindGroupById(model.groups, todo.tagId);
        const Group* parent = tag ? FindGroupById(model.groups, tag->parentGroup) : nullptr;
        if (!first) {
            out << L",\n";
        }
        first = false;
        out << L"    {\n";
        out << L"      \"groupName\": \"" << JsonEscape(parent ? parent->name : L"默认分组") << L"\",\n";
        out << L"      \"tagName\": \"" << JsonEscape(tag ? tag->name : L"待办事项") << L"\",\n";
        out << L"      \"title\": \"" << JsonEscape(todo.title) << L"\",\n";
        out << L"      \"content\": \"" << JsonEscape(todo.content) << L"\",\n";
        out << L"      \"enabled\": " << BoolJson(todo.enabled) << L",\n";
        out << L"      \"scheduleKind\": " << static_cast<int>(todo.scheduleKind) << L",\n";
        out << L"      \"repeatMode\": " << static_cast<int>(todo.repeatMode) << L",\n";
        out << L"      \"repeatInterval\": " << todo.repeatInterval << L",\n";
        out << L"      \"repeatLimit\": " << todo.repeatLimit << L",\n";
        out << L"      \"repeatFinished\": " << todo.repeatFinished << L",\n";
        out << L"      \"cronExpression\": \"" << JsonEscape(todo.cronExpression) << L"\",\n";
        out << L"      \"anchorAt\": \"" << JsonEscape(todo.anchorAt) << L"\",\n";
        out << L"      \"nextDueAt\": \"" << JsonEscape(todo.nextDueAt) << L"\",\n";
        out << L"      \"completedAt\": \"" << JsonEscape(todo.completedAt) << L"\",\n";
        out << L"      \"createdAt\": \"" << JsonEscape(todo.createdAt) << L"\",\n";
        out << L"      \"updatedAt\": \"" << JsonEscape(todo.updatedAt) << L"\"\n";
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
        const std::wstring groupName = Trim(entry.get(L"groupName") ? entry.get(L"groupName")->stringOr(L"默认分组") : L"默认分组");
        const std::wstring tagName = Trim(entry.get(L"tagName") ? entry.get(L"tagName")->stringOr(L"待办事项") : L"待办事项");
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
        item.title = entry.get(L"title") ? entry.get(L"title")->stringOr() : L"";
        item.content = entry.get(L"content") ? entry.get(L"content")->stringOr() : L"";
        item.enabled = entry.get(L"enabled") ? entry.get(L"enabled")->boolOr(true) : true;
        item.scheduleKind = static_cast<TodoScheduleKind>(entry.get(L"scheduleKind") ? entry.get(L"scheduleKind")->intOr(0) : 0);
        item.repeatMode = static_cast<TodoRepeatMode>(entry.get(L"repeatMode") ? entry.get(L"repeatMode")->intOr(0) : 0);
        item.repeatInterval = std::max(1, entry.get(L"repeatInterval") ? entry.get(L"repeatInterval")->intOr(1) : 1);
        item.repeatLimit = std::max(0, entry.get(L"repeatLimit") ? entry.get(L"repeatLimit")->intOr(0) : 0);
        item.repeatFinished = std::max(0, entry.get(L"repeatFinished") ? entry.get(L"repeatFinished")->intOr(0) : 0);
        item.cronExpression = entry.get(L"cronExpression") ? entry.get(L"cronExpression")->stringOr() : L"";
        item.anchorAt = entry.get(L"anchorAt") ? entry.get(L"anchorAt")->stringOr() : L"";
        item.nextDueAt = entry.get(L"nextDueAt") ? entry.get(L"nextDueAt")->stringOr() : L"";
        item.completedAt = entry.get(L"completedAt") ? entry.get(L"completedAt")->stringOr() : L"";
        item.createdAt = entry.get(L"createdAt") ? entry.get(L"createdAt")->stringOr() : L"";
        item.updatedAt = entry.get(L"updatedAt") ? entry.get(L"updatedAt")->stringOr() : L"";
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
        L"\n\n将把该备份中的分组、标签、启动项、便签、待办和工具设置合并到当前数据。"
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

        RECT ownerRect{};
        if (owner_) {
            GetWindowRect(owner_, &ownerRect);
        } else {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
        }
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
        const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
        RECT windowRect{0, 0, width_, clientHeight};
        AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
        const int windowWidth = windowRect.right - windowRect.left;
        const int windowHeight = windowRect.bottom - windowRect.top;

        const int ownerWidth = ownerRect.right - ownerRect.left;
        const int ownerHeight = ownerRect.bottom - ownerRect.top;
        const int x = ownerRect.left + std::max(0, (ownerWidth - windowWidth) / 2);
        const int y = ownerRect.top + std::max(0, (ownerHeight - windowHeight) / 2);

        hwnd_ = CreateWindowExW(
            exStyle,
            wc.lpszClassName,
            title_.empty() ? L"提示" : title_.c_str(),
            style,
            x,
            y,
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
            if (!IsDialogMessageW(hwnd_, &message)) {
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
            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            RECT client{};
            GetClientRect(hwnd_, &client);
            const int clientWidth = client.right - client.left;
            const int clientHeight = client.bottom - client.top;
            CreateMessageTextControl(layout, clientWidth);
            const int y = layout.FooterButtonY(clientHeight, buttonHeight);
            if (YesNo()) {
                ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDYES, L"是", layout.FooterButtonX(clientWidth, 0, 2), y, layout.footerButtonWidth, buttonHeight, font_, true);
                ThemedControls::CreateButton(instance_, hwnd_, IDNO, L"否", layout.FooterButtonX(clientWidth, 1, 2), y, layout.footerButtonWidth, buttonHeight, font_);
            } else if (OkCancel()) {
                ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"确定", layout.FooterButtonX(clientWidth, 0, 2), y, layout.footerButtonWidth, buttonHeight, font_, true);
                ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", layout.FooterButtonX(clientWidth, 1, 2), y, layout.footerButtonWidth, buttonHeight, font_);
            } else {
                ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"确定", layout.FooterButtonX(clientWidth, 0, 1), y, layout.footerButtonWidth, buttonHeight, font_, true);
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
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
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
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TextDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = className.c_str();
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(L"文本输入窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        SetLastError(ERROR_SUCCESS);
        hwnd_ = nullptr;
        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        constexpr int kClientWidth = 390;
        constexpr int kClientHeight = 162;
        const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
        const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
        RECT windowRect{0, 0, kClientWidth, kClientHeight};
        AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
        hwnd_ = CreateWindowExW(
            exStyle,
            className.c_str(),
            title_.c_str(),
            style,
            ownerRect.left + 80,
            ownerRect.top + 100,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            const DWORD error = GetLastError();
            WriteAppLog(L"文本输入窗口创建失败: " + FormatLastError(error));
            return false;
        }
        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
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
            const DialogLayoutMetrics layout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Mini);
            RECT client{};
            GetClientRect(hwnd_, &client);
            const int clientWidth = client.right - client.left;
            const int clientHeight = client.bottom - client.top;
            const int contentWidth = clientWidth - layout.contentInsetX * 2;
            const int labelY = layout.contentInsetY;
            ThemedControls::CreateLabelText(instance_, hwnd_, label_.c_str(), layout.contentInsetX, labelY, contentWidth, theme_, font_);
            const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
            const int editY = labelY + ThemedControls::LabelHeight(theme_) + layout.rowGap;
            editFrame_ = RECT{layout.contentInsetX, editY, layout.contentInsetX + contentWidth, editY + fieldHeight};
            edit_ = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, 100, theme_, editFrame_, value_, editFont_ ? editFont_ : font_);
            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            const int footerY = layout.FooterButtonY(clientHeight, buttonHeight);
            ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"确定", layout.FooterButtonX(clientWidth, 0, 2), footerY, layout.footerButtonWidth, buttonHeight, font_, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", layout.FooterButtonX(clientWidth, 1, 2), footerY, layout.footerButtonWidth, buttonHeight, font_);
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            ThemedControls::DrawFieldFrame(theme_, dc, editFrame_, edit_);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC: {
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
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateRect(hwnd_, &editFrame_, TRUE);
            }
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
        case WM_DESTROY:
            done_ = true;
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (editFont_) {
                DeleteObject(editFont_);
                editFont_ = nullptr;
            }
            if (ownsFont_ && font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
                fieldBrush_ = nullptr;
            }
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
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HFONT editFont_ = nullptr;
    bool ownsFont_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
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

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            L"选择 WebDAV 备份",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 60,
            ownerRect.top + 80,
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
            if (!IsDialogMessageW(hwnd_, &message)) {
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
            ThemedControls::CreateLabelText(instance_, hwnd_, L"云端备份记录", 24, 20, 180, theme_, font_);
            list_ = ThemedControls::CreateListBox(
                instance_,
                hwnd_,
                ID_WEBDAV_BACKUP_LIST,
                24,
                48,
                500,
                238,
                font_,
                theme_);
            for (const auto& backup : backups_) {
                SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(FormatBackupListItem(backup).c_str()));
            }
            if (!backups_.empty()) {
                SendMessageW(list_, LB_SETCURSEL, 0, 0);
            }

            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"下载", 360, 310, 76, buttonHeight, font_, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 452, 310, 76, buttonHeight, font_);
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
    SettingsDialog(HWND owner, HINSTANCE instance, AppConfig& config, const Theme& theme, std::filesystem::path appDirectory)
        : owner_(owner), instance_(instance), config_(config), draft_(config), theme_(theme), appDirectory_(std::move(appDirectory)) {}

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

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"设置",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 60,
            ownerRect.top + 70,
            560,
            520,
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
                ShowTab((currentTab_ + (reverse ? 6 : 1)) % 7);
                continue;
            }
            if (!IsDialogMessageW(hwnd_, &message)) {
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
        TabBackup = 6,
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

    HWND Label(int tab, const wchar_t* text, int x, int y, int width = 110) {
        HWND hwnd = ThemedControls::CreateLabelText(instance_, hwnd_, text, x, y, width, theme_, font_);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND CheckBox(int tab, int id, const wchar_t* text, int x, int y, bool checked, int width = 210) {
        HWND hwnd = ThemedControls::CreateCheckBox(instance_, hwnd_, id, text, x, y, width, ThemedControls::CheckBoxHeight(theme_), font_, checked);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND Button(int tab, int id, const wchar_t* text, int x, int y, int width) {
        HWND hwnd = ThemedControls::CreateButton(instance_, hwnd_, id, text, x, y, width, ThemedControls::CompactButtonHeight(theme_), font_);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND FramedEdit(int tab, int id, int x, int y, int width, const std::wstring& text, DWORD extraStyle = ES_AUTOHSCROLL) {
        const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, id, theme_, frame, text, editFont_ ? editFont_ : font_, extraStyle);
        AddTabChild(hwnd, tab);
        fieldFrames_.push_back(FieldFrame{frame, hwnd, tab, false});
        return hwnd;
    }

    HWND FramedStatic(int tab, int x, int y, int width, const std::wstring& text) {
        const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ThemedControls::CreateFramedStatic(instance_, hwnd_, theme_, frame, text, font_);
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
                SendMessageW(buttons[i], BM_SETCHECK, i == tagAlignIndex_ ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(buttons[i], nullptr, TRUE);
            }
        }
    }

    void CreateTabs() {
        const wchar_t* titles[] = {L"显示", L"行为", L"交互", L"热键", L"链接", L"WebDAV", L"备份"};
        const int startX = 30;
        const int startY = 18;
        const int itemWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupItemWidth", 58.0f));
        const std::array<int, 7> itemWidths{
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 76),
            std::max(itemWidth, 76),
        };
        const int itemGap = static_cast<int>(theme_.metric(L"tabButton", L"groupGap", 0.0f));
        const int separatorWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupBorderWidth", 1.0f));
        const int itemSpacing = std::max(itemGap, separatorWidth > 0 ? separatorWidth : 0);
        const int itemHeight = ThemedControls::TabButtonHeight(theme_);
        const int stripPadding = static_cast<int>(theme_.metric(L"tabButton", L"groupPadding", 3.0f));
        int stripWidth = 0;
        for (int width : itemWidths) {
            stripWidth += width;
        }
        stripWidth += 6 * itemSpacing;
        tabStripRect_ = RECT{
            startX - stripPadding,
            startY - stripPadding,
            startX + stripWidth + stripPadding,
            startY + itemHeight + stripPadding};
        int x = startX;
        tabSeparatorXs_.clear();
        for (int i = 0; i < 7; ++i) {
            HWND button = ThemedControls::CreateTabButton(
                instance_,
                hwnd_,
                ID_SETTINGS_TAB_BASE + i,
                titles[i],
                x,
                startY,
                itemWidths[static_cast<std::size_t>(i)],
                itemHeight,
                font_,
                i == TabDisplay);
            tabButtons_.push_back(button);
            x += itemWidths[static_cast<std::size_t>(i)];
            if (i < 6) {
                tabSeparatorXs_.push_back(x);
                x += itemSpacing;
            }
        }
    }

    void PaintTabs(HDC dc) {
        if (tabStripRect_.right <= tabStripRect_.left || tabStripRect_.bottom <= tabStripRect_.top) {
            return;
        }
        ThemedControls::DrawTabGroupFrame(theme_, dc, tabStripRect_);
        const int separatorWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupBorderWidth", 1.0f));
        if (separatorWidth <= 0) {
            return;
        }
        HBRUSH separator = CreateSolidBrush(ToColorRef(theme_.color(L"tabButton", L"normal", L"groupBorder")));
        for (int x : tabSeparatorXs_) {
            RECT line{
                x,
                tabStripRect_.top,
                x + separatorWidth,
                tabStripRect_.bottom};
            FillRect(dc, &line, separator);
        }
        DeleteObject(separator);
    }

    void ShowTab(int tab) {
        currentTab_ = tab;
        for (int i = 0; i < static_cast<int>(tabButtons_.size()); ++i) {
            SendMessageW(tabButtons_[i], BM_SETCHECK, i == currentTab_ ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(tabButtons_[i], nullptr, TRUE);
        }
        for (const auto& child : tabChildren_) {
            const bool visible = child.tab == currentTab_;
            ShowWindow(child.hwnd, visible ? SW_SHOW : SW_HIDE);
            EnableWindow(child.hwnd, visible ? TRUE : FALSE);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    bool IsFieldChild(HWND hwnd) const {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == hwnd) {
                return true;
            }
        }
        return false;
    }

    void InvalidateField(HWND hwnd) {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == hwnd) {
                InvalidateRect(hwnd_, &frame.rect, TRUE);
                return;
            }
        }
    }

    void PaintFields(HDC dc) {
        for (const auto& frame : fieldFrames_) {
            if (frame.tab != currentTab_) {
                continue;
            }
            ThemedControls::DrawFieldFrame(theme_, dc, frame.rect, frame.child, frame.readOnly);
        }
    }

    void ReadDraft() {
        draft_.showTitle = SendMessageW(showTitle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showGroup = SendMessageW(showGroup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showTag = SendMessageW(showTag_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.topMost = SendMessageW(topMost_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.autoDock = SendMessageW(autoDock_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideWhenInactive = SendMessageW(hideInactive_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideAfterLink = SendMessageW(hideAfterLink_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideOnStart = SendMessageW(hideOnStart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.doubleClickToRun = SendMessageW(doubleClick_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.hideNotifyIcon = SendMessageW(hideNotify_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.deleteConfirm = SendMessageW(deleteConfirm_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.saveRunCount = SendMessageW(saveRunCount_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showDate = SendMessageW(showDate_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showMenuButton = SendMessageW(showMenuButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.showSkinButton = SendMessageW(showSkinButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.autoRun = SendMessageW(autoRun_, BM_GETCHECK, 0, 0) == BST_CHECKED;
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

    void TestWebDavConnection() {
        AppConfig value = ReadWebDavDraftFromControls();
        value.webDavEnabled = true;
        std::wstring password = GetText(webDavPasswordEdit_);
        std::wstring error;
        if (password.empty() && !WebDavCredentialService::LoadPassword(value, password, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
            return;
        }
        WebDavClient client(value, password);
        if (client.TestConnection()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"WebDAV 连接成功。", L"WebDAV 备份", MB_OK | MB_ICONINFORMATION);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, client.lastError(), L"WebDAV 备份", MB_OK | MB_ICONWARNING);
        }
    }

    void ClearWebDavPassword() {
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

    void UploadWebDavBackup() {
        if (!PrepareWebDavOperation()) {
            return;
        }
        WebDavBackupService service(appDirectory_, draft_);
        const WebDavBackupReport report = service.UploadBackup();
        ShowThemedMessageBox(hwnd_, instance_, theme_, report.message, L"上传到云端", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    void DownloadWebDavBackup() {
        if (!PrepareWebDavOperation()) {
            return;
        }
        WebDavBackupService service(appDirectory_, draft_);
        std::vector<WebDavRemoteFile> backups;
        std::wstring error;
        if (!service.ListBackups(backups, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"从云端下载", MB_OK | MB_ICONWARNING);
            return;
        }
        if (backups.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"远端目录中没有可用的 .q4cfg 备份。", L"从云端下载", MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring fileName = backups.front().name;
        if (!ShowWebDavBackupSelectionDialog(hwnd_, instance_, theme_, backups, fileName)) {
            return;
        }
        auto selectedBackup = std::find_if(backups.begin(), backups.end(), [&](const WebDavRemoteFile& backup) {
            return backup.name == fileName;
        });
        if (selectedBackup == backups.end()) {
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

        const WebDavBackupReport report = service.DownloadAndImportMerge(fileName);
        if (report.ok) {
            importedData_ = true;
        }
        const std::wstring text = report.importReport.message.empty()
            ? report.message
            : FormatConfigPackageReportText(report.importReport);
        ShowThemedMessageBox(hwnd_, instance_, theme_, text, L"从云端下载", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
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
        options.includePluginSettings = true;
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
            L"将把配置包中的分组、标签、启动项、便签、待办和工具设置合并到当前数据。\n\n当前数据不会被覆盖，导入前会自动备份。",
            L"合并导入配置包",
            MB_OKCANCEL | MB_ICONINFORMATION);
        if (confirm != IDOK) {
            return;
        }
        ConfigPackageOptions options;
        options.includeConfig = false;
        options.includeData = true;
        options.includeUrlIcons = true;
        options.includePluginSettings = true;
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
        std::wstring targetPath;
        if (!SelectSavePath(hwnd_,
                (appDirectory_ / TodoJsonFileName()).wstring(),
                L"JSON 文件 (*.json)\0*.json\0所有文件\0*.*\0",
                L"json",
                targetPath)) {
            return;
        }
        if (!SaveUtf8File(targetPath, BuildTodoExportJson(model))) {
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
            CreateTabs();

            showTitle_ = CheckBox(TabDisplay, 101, L"显示标题栏", 34, 64, draft_.showTitle);
            showGroup_ = CheckBox(TabDisplay, 102, L"显示分组栏", 282, 64, draft_.showGroup);
            showTag_ = CheckBox(TabDisplay, 103, L"显示标签栏", 34, 94, draft_.showTag);
            showDate_ = CheckBox(TabDisplay, 113, L"显示日期", 282, 94, draft_.showDate);
            showMenuButton_ = CheckBox(TabDisplay, 115, L"显示菜单按钮", 282, 124, draft_.showMenuButton);
            showSkinButton_ = CheckBox(TabDisplay, 121, L"显示主题按钮", 34, 124, draft_.showSkinButton);
            linkNameSingleLine_ = CheckBox(TabDisplay, 118, L"启动项名称单行", 282, 154, draft_.linkNameSingleLine);
            showTooltip_ = CheckBox(TabDisplay, 119, L"显示提示", 34, 154, draft_.showTooltip);
            groupRight_ = CheckBox(TabDisplay, 120, L"分组栏在右侧", 282, 184, draft_.groupRight);
            tagRight_ = CheckBox(TabDisplay, 122, L"标签栏在右侧", 34, 184, draft_.tagRight);

            Label(TabDisplay, L"透明度", 34, 260, 76);
            alphaEdit_ = NumberEdit(TabDisplay, 201, 118, 254, 78, draft_.alpha);
            Label(TabDisplay, L"标签文字", 282, 260, 72);
            const int tabButtonHeight = ThemedControls::TabButtonHeight(theme_);
            tagAlignLeft_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAG_ALIGN_LEFT, L"左", 364, 255, 36, tabButtonHeight, font_, false);
            tagAlignCenter_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAG_ALIGN_CENTER, L"中", 404, 255, 36, tabButtonHeight, font_, true);
            tagAlignRight_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAG_ALIGN_RIGHT, L"右", 444, 255, 36, tabButtonHeight, font_, false);
            AddTabChild(tagAlignLeft_, TabDisplay);
            AddTabChild(tagAlignCenter_, TabDisplay);
            AddTabChild(tagAlignRight_, TabDisplay);
            SelectTagAlign();

            Label(TabDisplay, L"分组宽度", 34, 314, 76);
            groupWidthEdit_ = NumberEdit(TabDisplay, ID_GROUP_WIDTH, 118, 308, 78, draft_.groupWidth);
            Label(TabDisplay, L"标签宽度", 282, 314, 72);
            tagWidthEdit_ = NumberEdit(TabDisplay, ID_TAG_WIDTH, 364, 308, 78, draft_.tagWidth);

            topMost_ = CheckBox(TabBehavior, 104, L"窗口置顶", 34, 64, draft_.topMost);
            autoDock_ = CheckBox(TabBehavior, 105, L"自动停靠", 282, 64, draft_.autoDock);
            hideInactive_ = CheckBox(TabBehavior, 106, L"失焦隐藏", 34, 94, draft_.hideWhenInactive);
            hideAfterLink_ = CheckBox(TabBehavior, 107, L"运行后隐藏", 282, 94, draft_.hideAfterLink);
            hideOnStart_ = CheckBox(TabBehavior, 116, L"启动后隐藏", 34, 124, draft_.hideOnStart);
            autoRun_ = CheckBox(TabBehavior, 117, L"开机自启动", 282, 124, draft_.autoRun);
            hideNotify_ = CheckBox(TabBehavior, 110, L"隐藏托盘图标", 34, 154, draft_.hideNotifyIcon);
            deleteConfirm_ = CheckBox(TabBehavior, 111, L"删除确认", 282, 154, draft_.deleteConfirm);
            saveRunCount_ = CheckBox(TabBehavior, 112, L"保存运行次数", 34, 184, draft_.saveRunCount);
            Label(TabBehavior, L"停靠延迟", 34, 238, 76);
            dockDelayEdit_ = NumberEdit(TabBehavior, ID_DOCK_DELAY, 118, 232, 88, draft_.dockDelay);
            Label(TabBehavior, L"ms", 214, 238, 32);

            doubleClick_ = CheckBox(TabInteraction, 109, L"双击运行", 34, 64, draft_.doubleClickToRun);
            enterActiveGroup_ = CheckBox(TabInteraction, 124, L"鼠标进入激活分组", 34, 94, draft_.mouseEnterActiveGroup);
            enterActiveTag_ = CheckBox(TabInteraction, 125, L"鼠标进入激活标签", 282, 94, draft_.mouseEnterActiveTag);
            Label(TabInteraction, L"分组激活延迟", 34, 154, 100);
            groupDelayEdit_ = NumberEdit(TabInteraction, ID_GROUP_DELAY, 144, 148, 88, draft_.activeGroupDelay);
            Label(TabInteraction, L"ms", 240, 154, 32);
            Label(TabInteraction, L"标签激活延迟", 282, 154, 100);
            tagDelayEdit_ = NumberEdit(TabInteraction, ID_TAG_DELAY, 392, 148, 88, draft_.activeTagDelay);
            Label(TabInteraction, L"ms", 488, 154, 32);

            Label(TabHotKeys, L"主窗口热键", 34, 74, 84);
            mainHotKeyText_ = FramedStatic(TabHotKeys, 128, 66, 210, FormatHotKeyText(draft_.mainHotKey));
            Button(TabHotKeys, ID_MAIN_HOTKEY_CAPTURE, L"录入", 354, 68, 56);
            Button(TabHotKeys, ID_MAIN_HOTKEY_CLEAR, L"清除", 424, 68, 56);

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
            Button(TabWebDav, ID_WEBDAV_UPLOAD, L"上传到云端", 34, 340, 104);
            Button(TabWebDav, ID_WEBDAV_DOWNLOAD, L"从云端下载", 150, 340, 104);
            Button(TabWebDav, ID_WEBDAV_TEST, L"测试连接", 286, 340, 92);
            Button(TabWebDav, ID_WEBDAV_CLEAR_PASSWORD, L"清除密码", 390, 340, 90);

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
            Label(TabBackup, L"导入配置包会合并现有数据；导入待办事项备份会自动补齐缺失分组和待办标签。", backupNoteX, 308, backupNoteWidth);

            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"确定", 350, 428, 76, buttonHeight, font_, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 442, 428, 76, buttonHeight, font_);
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
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            HWND child = reinterpret_cast<HWND>(lParam);
            const bool fieldChild = IsFieldChild(child);
            SetTextColor(dc, ToColorRef(fieldChild ? theme_.color(L"field", L"readonly", L"text") : theme_.color(L"label", L"normal", L"text")));
            if (fieldChild && readOnlyFieldBrush_) {
                return reinterpret_cast<LRESULT>(readOnlyFieldBrush_);
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateField(reinterpret_cast<HWND>(lParam));
            }
            if (LOWORD(wParam) >= ID_SETTINGS_TAB_BASE && LOWORD(wParam) < ID_SETTINGS_TAB_BASE + 7) {
                ShowTab(static_cast<int>(LOWORD(wParam) - ID_SETTINGS_TAB_BASE));
                return 0;
            }
            if (LOWORD(wParam) >= ID_TAG_ALIGN_LEFT && LOWORD(wParam) <= ID_TAG_ALIGN_RIGHT) {
                tagAlignIndex_ = static_cast<int>(LOWORD(wParam) - ID_TAG_ALIGN_LEFT);
                UpdateTagAlignButtons();
                return 0;
            }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CAPTURE) {
                draft_.mainHotKey = ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.mainHotKey);
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
            if (LOWORD(wParam) == IDOK) {
                ReadDraft();
                if (!SaveWebDavPasswordIfNeeded()) {
                    return 0;
                }
                config_ = draft_;
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
            SetWindowTextW(mainHotKeyText_, FormatHotKeyText(draft_.mainHotKey).c_str());
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
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HBRUSH readOnlyFieldBrush_ = nullptr;
    bool ownsFont_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    int currentTab_ = TabDisplay;
    RECT tabStripRect_{};
    std::vector<HWND> tabButtons_;
    std::vector<int> tabSeparatorXs_;
    std::vector<TabChild> tabChildren_;
    std::vector<FieldFrame> fieldFrames_;
    HWND showTitle_ = nullptr;
    HWND showGroup_ = nullptr;
    HWND showTag_ = nullptr;
    HWND topMost_ = nullptr;
    HWND autoDock_ = nullptr;
    HWND hideInactive_ = nullptr;
    HWND hideAfterLink_ = nullptr;
    HWND hideOnStart_ = nullptr;
    HWND doubleClick_ = nullptr;
    HWND hideNotify_ = nullptr;
    HWND deleteConfirm_ = nullptr;
    HWND saveRunCount_ = nullptr;
    HWND showDate_ = nullptr;
    HWND showMenuButton_ = nullptr;
    HWND showSkinButton_ = nullptr;
    HWND autoRun_ = nullptr;
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
    bool importedData_ = false;
    bool accepted_ = false;
    bool done_ = false;
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
    bool* importedData) {
    SettingsDialog dialog(owner, instance, config, theme, appDirectory);
    const bool accepted = dialog.Run();
    if (importedData) {
        *importedData = dialog.webDavDataImported();
    }
    return accepted;
}
