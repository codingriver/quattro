#include "TodoJsonBackupService.h"

#include "JsonValue.h"
#include "Storage.h"
#include "TodoSchedule.h"
#include "Utilities.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {
constexpr int kTodoJsonFormatVersion = 2;
constexpr const wchar_t* kStableFallbackLocalTimestamp = L"1970-01-01 00:00:00";
constexpr const wchar_t* kStableFallbackMergeTimestamp = L"1970-01-01T00:00:00.000Z";

class SQLiteDatabase {
public:
    explicit SQLiteDatabase(const std::filesystem::path& path) {
        if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
            if (db_) {
                lastError_ = Error();
                sqlite3_close(db_);
                db_ = nullptr;
            }
        }
    }

    ~SQLiteDatabase() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    SQLiteDatabase(const SQLiteDatabase&) = delete;
    SQLiteDatabase& operator=(const SQLiteDatabase&) = delete;

    bool ok() const { return db_ != nullptr; }
    sqlite3* get() const { return db_; }

    std::wstring Error() const {
        if (!db_) {
            return lastError_.empty() ? L"无法打开数据库。" : lastError_;
        }
        const void* message = sqlite3_errmsg16(db_);
        return message ? static_cast<const wchar_t*>(message) : L"数据库错误。";
    }

private:
    sqlite3* db_ = nullptr;
    std::wstring lastError_;
};

class SQLiteStatement {
public:
    SQLiteStatement(sqlite3* db, const wchar_t* sql) {
        sqlite3_prepare16_v2(db, sql, -1, &stmt_, nullptr);
    }

    ~SQLiteStatement() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
    }

    SQLiteStatement(const SQLiteStatement&) = delete;
    SQLiteStatement& operator=(const SQLiteStatement&) = delete;

    bool ok() const { return stmt_ != nullptr; }
    int step() { return sqlite3_step(stmt_); }
    int columnInt(int index) const { return sqlite3_column_int(stmt_, index); }
    std::wstring columnText(int index) const {
        const void* text = sqlite3_column_text16(stmt_, index);
        return text ? static_cast<const wchar_t*>(text) : L"";
    }
    void bindInt(int index, int value) { sqlite3_bind_int(stmt_, index, value); }
    void bindText(int index, const std::wstring& value) {
        sqlite3_bind_text16(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) {
        return L"数据库执行失败。";
    }
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
    return Trim(wide);
}

bool Exec(sqlite3* db, const char* sql, std::wstring& error) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        error = Utf8ToWide(message);
        sqlite3_free(message);
        if (error.empty()) {
            const void* dbError = sqlite3_errmsg16(db);
            error = dbError ? static_cast<const wchar_t*>(dbError) : L"数据库执行失败。";
        }
        return false;
    }
    return true;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::wstring ContentHash(const std::vector<std::uint8_t>& data) {
    std::uint32_t hash = 2166136261u;
    for (std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return Hex8(hash);
}

bool SaveUtf8File(const std::filesystem::path& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return false;
    }
    std::string bytes(static_cast<std::size_t>(length - 1), '\0');
    if (!bytes.empty()) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, bytes.data(), length, nullptr, nullptr);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(file);
}

bool BackupDatabase(const std::filesystem::path& sourcePath, const std::filesystem::path& targetPath, std::wstring& error) {
    if (!FileExists(sourcePath)) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(targetPath.parent_path(), ec);
    SQLiteDatabase source(sourcePath);
    if (!source.ok()) {
        error = source.Error();
        return false;
    }
    SQLiteDatabase target(targetPath);
    if (!target.ok()) {
        error = target.Error();
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(target.get(), "main", source.get(), "main");
    if (!backup) {
        const void* message = sqlite3_errmsg16(target.get());
        error = message ? static_cast<const wchar_t*>(message) : L"数据库快照失败。";
        return false;
    }
    const int rc = sqlite3_backup_step(backup, -1);
    sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(target.get());
        error = message ? static_cast<const wchar_t*>(message) : L"数据库快照失败。";
        return false;
    }
    return true;
}

std::filesystem::path CreateSafetyBackup(const std::filesystem::path& appDirectory, TodoJsonImportReport& report) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path backupDirectory =
        appDirectory / L"backups" / (L"todo-json-import-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks));
    std::error_code ec;
    std::filesystem::create_directories(backupDirectory / L"db", ec);
    std::wstring error;
    if (!BackupDatabase(appDirectory / L"db" / L"link.db", backupDirectory / L"db" / L"link.db", error) && !error.empty()) {
        report.warnings.push_back(L"备份 link.db 失败: " + error);
    }
    return backupDirectory;
}

void RestoreSafetyBackup(const std::filesystem::path& appDirectory, const std::filesystem::path& backupDirectory, TodoJsonImportReport& report) {
    const std::filesystem::path backupDb = backupDirectory / L"db" / L"link.db";
    if (!FileExists(backupDb)) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(appDirectory / L"db", ec);
    std::filesystem::copy_file(backupDb, appDirectory / L"db" / L"link.db", std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        report.warnings.push_back(L"导入失败后恢复 link.db 失败。");
    }
}

std::wstring GenerateStableUuid() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) {
        return {};
    }
    wchar_t buffer[40]{};
    if (StringFromGUID2(value, buffer, static_cast<int>(std::size(buffer))) <= 0) {
        return {};
    }
    std::wstring result(buffer);
    if (result.size() >= 2 && result.front() == L'{' && result.back() == L'}') {
        result = result.substr(1, result.size() - 2);
    }
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

std::wstring FormatUtcTimestamp(const SYSTEMTIME& value) {
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        value.wYear,
        value.wMonth,
        value.wDay,
        value.wHour,
        value.wMinute,
        value.wSecond,
        value.wMilliseconds);
    return buffer;
}

std::wstring LocalTimestampToUtc(const std::wstring& value) {
    SYSTEMTIME local{};
    if (!TryParseTodoTimestamp(value, local)) {
        return {};
    }
    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
        return {};
    }
    return FormatUtcTimestamp(utc);
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
    swprintf_s(buffer,
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

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': escaped += L"\\\\"; break;
        case L'"': escaped += L"\\\""; break;
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

const JsonValue* ObjectField(const JsonValue& object, const std::wstring& key) {
    const JsonValue* value = object.get(key);
    return value && value->isObject() ? value : nullptr;
}

std::wstring JsonStringField(const JsonValue& object, const std::wstring& key, const std::wstring& fallback = L"") {
    const JsonValue* value = object.get(key);
    return value ? value->stringOr(fallback) : fallback;
}

int JsonIntField(const JsonValue& object, const std::wstring& key, int fallback = 0) {
    const JsonValue* value = object.get(key);
    return value ? value->intOr(fallback) : fallback;
}

bool JsonBoolField(const JsonValue& object, const std::wstring& key, bool fallback = false) {
    const JsonValue* value = object.get(key);
    return value ? value->boolOr(fallback) : fallback;
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

std::wstring NormalizedNameKey(const std::wstring& value) {
    return ToLower(Trim(value));
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

const Group* FindGroupByUid(const std::vector<Group>& groups, const std::wstring& uid) {
    if (uid.empty()) {
        return nullptr;
    }
    for (const auto& group : groups) {
        if (group.groupUid == uid) {
            return &group;
        }
    }
    return nullptr;
}

const Group* FindRootGroupByName(const std::vector<Group>& groups, const std::wstring& groupName) {
    const std::wstring normalized = NormalizedNameKey(groupName);
    for (const auto& group : groups) {
        if (group.parentGroup == 0 && NormalizedNameKey(group.name) == normalized) {
            return &group;
        }
    }
    return nullptr;
}

const Group* FindTodoTagByName(const std::vector<Group>& groups, int parentGroupId, const std::wstring& tagName) {
    const std::wstring normalized = NormalizedNameKey(tagName);
    for (const auto& group : groups) {
        if (group.parentGroup == parentGroupId && IsTodoItemsTag(group) && NormalizedNameKey(group.name) == normalized) {
            return &group;
        }
    }
    return nullptr;
}

bool HasSiblingGroupName(const std::vector<Group>& groups, int parentGroup, const std::wstring& name) {
    const std::wstring normalized = NormalizedNameKey(name);
    for (const auto& group : groups) {
        if (group.parentGroup == parentGroup && NormalizedNameKey(group.name) == normalized) {
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

int NextGroupPosition(sqlite3* db, int parentGroup) {
    SQLiteStatement query(db, L"SELECT COALESCE(MAX(POS),-1)+1 FROM Groups WHERE ParentGroup=?;");
    query.bindInt(1, parentGroup);
    return query.ok() && query.step() == SQLITE_ROW ? query.columnInt(0) : 0;
}

int NextTodoPosition(sqlite3* db, int tagId) {
    SQLiteStatement query(db, L"SELECT COALESCE(MAX(POS),-1)+1 FROM TodoItems WHERE TagId=?;");
    query.bindInt(1, tagId);
    return query.ok() && query.step() == SQLITE_ROW ? query.columnInt(0) : 0;
}

bool InsertGroupRaw(sqlite3* db, Group& group, std::wstring& error) {
    group.name = Trim(group.name);
    group.icon = Trim(group.icon);
    group.content = Trim(group.content);
    if (group.name.empty()) {
        error = L"分组名称不能为空。";
        return false;
    }
    if (group.groupUid.empty()) {
        group.groupUid = GenerateStableUuid();
    }
    if (group.groupUid.empty()) {
        error = L"创建分组同步标识失败。";
        return false;
    }
    group.pos = NextGroupPosition(db, group.parentGroup);
    SQLiteStatement statement(db,
        L"INSERT INTO Groups(NAME,TYPE,SORT,SORTDIRECTION,POS,ParentGroup,ICON,LAYOUT,ICONSIZE,FLAG,Content,GroupUid) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        error = L"准备创建分组 SQL 失败。";
        return false;
    }
    statement.bindText(1, group.name);
    statement.bindInt(2, group.type);
    statement.bindInt(3, group.sort);
    statement.bindInt(4, group.sortDirection);
    statement.bindInt(5, group.pos);
    statement.bindInt(6, group.parentGroup);
    statement.bindText(7, group.icon);
    statement.bindInt(8, group.layout);
    statement.bindInt(9, group.iconSize);
    statement.bindInt(10, group.flag);
    statement.bindText(11, group.content);
    statement.bindText(12, group.groupUid);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db);
        error = message ? static_cast<const wchar_t*>(message) : L"创建分组失败。";
        return false;
    }
    group.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    return true;
}

bool NormalizeTodoForImport(sqlite3* db, TodoItem& item, bool assignPosition, std::wstring& error) {
    item.title = Trim(item.title);
    item.content = Trim(item.content);
    item.anchorAt = NormalizeTodoTimestamp(item.anchorAt);
    item.nextDueAt = NormalizeTodoTimestamp(item.nextDueAt);
    item.completedAt = NormalizeTodoTimestamp(item.completedAt);
    item.lastNotifiedDueAt = NormalizeTodoTimestamp(item.lastNotifiedDueAt);
    item.lastNotifiedAt = NormalizeTodoTimestamp(item.lastNotifiedAt);
    item.lastViewedDueAt = NormalizeTodoTimestamp(item.lastViewedDueAt);
    item.lastViewedAt = NormalizeTodoTimestamp(item.lastViewedAt);
    item.ignoredDueAt = NormalizeTodoTimestamp(item.ignoredDueAt);
    item.snoozedUntil = NormalizeTodoTimestamp(item.snoozedUntil);
    item.createdAt = ImportableTodoTimestamp(item.createdAt);
    item.updatedAt = ImportableTodoTimestamp(item.updatedAt);
    if (item.title.empty() || item.tagId <= 0) {
        error = L"待办事项标题不能为空。";
        return false;
    }
    if (static_cast<int>(item.scheduleKind) < static_cast<int>(TodoScheduleKind::None) ||
        static_cast<int>(item.scheduleKind) > static_cast<int>(TodoScheduleKind::Cron)) {
        item.scheduleKind = TodoScheduleKind::None;
    }
    if (static_cast<int>(item.repeatMode) < static_cast<int>(TodoRepeatMode::FixedPoint) ||
        static_cast<int>(item.repeatMode) > static_cast<int>(TodoRepeatMode::Interval)) {
        item.repeatMode = TodoRepeatMode::FixedPoint;
    }
    item.repeatInterval = std::max(1, item.repeatInterval);
    item.repeatLimit = std::max(0, item.repeatLimit);
    item.repeatFinished = std::max(0, item.repeatFinished);
    item.cronExpression = NormalizeTodoCronExpression(item.cronExpression);
    if (item.scheduleKind == TodoScheduleKind::None) {
        item.anchorAt.clear();
        item.nextDueAt.clear();
        item.cronExpression.clear();
        item.repeatFinished = 0;
        ResetTodoReminderState(item);
    } else if (item.scheduleKind == TodoScheduleKind::Cron) {
        item.anchorAt.clear();
        if (!IsValidTodoCronExpression(item.cronExpression)) {
            error = L"待办 Cron 表达式无效。";
            return false;
        }
        if (item.nextDueAt.empty()) {
            item.nextDueAt = ComputeNextTodoDueAt(item, CurrentTodoTimestamp());
        }
    } else if (item.anchorAt.empty()) {
        error = L"定时待办缺少有效时间。";
        return false;
    } else if (item.nextDueAt.empty()) {
        item.cronExpression.clear();
        item.nextDueAt = ComputeNextTodoDueAt(item, CurrentTodoTimestamp());
    }
    if (assignPosition || item.pos < 0) {
        item.pos = NextTodoPosition(db, item.tagId);
    }
    if (item.createdAt.empty()) {
        item.createdAt = kStableFallbackLocalTimestamp;
    }
    if (item.updatedAt.empty()) {
        item.updatedAt = item.createdAt;
    }
    if (item.todoUid.empty()) {
        item.todoUid = GenerateStableUuid();
    }
    if (item.mergeUpdatedAtUtc.empty()) {
        item.mergeUpdatedAtUtc = kStableFallbackMergeTimestamp;
    }
    if (item.todoUid.empty()) {
        error = L"待办缺少同步标识。";
        return false;
    }
    return true;
}

void ClearTransientReminderState(TodoItem& item) {
    item.lastNotifiedDueAt.clear();
    item.lastNotifiedAt.clear();
    item.lastViewedDueAt.clear();
    item.lastViewedAt.clear();
}

bool InsertTodoRaw(sqlite3* db, TodoItem& item, bool clearReminderState, std::wstring& error) {
    if (clearReminderState) {
        ClearTransientReminderState(item);
    }
    if (!NormalizeTodoForImport(db, item, true, error)) {
        return false;
    }
    SQLiteStatement statement(db,
        L"INSERT INTO TodoItems(TagId,Title,Content,Enabled,ScheduleKind,RepeatMode,RepeatInterval,RepeatLimit,RepeatFinished,CronExpression,AnchorAt,NextDueAt,CompletedAt,LastNotifiedDueAt,LastNotifiedAt,LastViewedDueAt,LastViewedAt,IgnoredDueAt,SnoozedUntil,POS,CreatedAt,UpdatedAt,TodoUid,MergeUpdatedAtUtc) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        error = L"准备导入待办 SQL 失败。";
        return false;
    }
    statement.bindInt(1, item.tagId);
    statement.bindText(2, item.title);
    statement.bindText(3, item.content);
    statement.bindInt(4, item.enabled ? 1 : 0);
    statement.bindInt(5, static_cast<int>(item.scheduleKind));
    statement.bindInt(6, static_cast<int>(item.repeatMode));
    statement.bindInt(7, item.repeatInterval);
    statement.bindInt(8, item.repeatLimit);
    statement.bindInt(9, item.repeatFinished);
    statement.bindText(10, item.cronExpression);
    statement.bindText(11, item.anchorAt);
    statement.bindText(12, item.nextDueAt);
    statement.bindText(13, item.completedAt);
    statement.bindText(14, item.lastNotifiedDueAt);
    statement.bindText(15, item.lastNotifiedAt);
    statement.bindText(16, item.lastViewedDueAt);
    statement.bindText(17, item.lastViewedAt);
    statement.bindText(18, item.ignoredDueAt);
    statement.bindText(19, item.snoozedUntil);
    statement.bindInt(20, item.pos);
    statement.bindText(21, item.createdAt);
    statement.bindText(22, item.updatedAt);
    statement.bindText(23, item.todoUid);
    statement.bindText(24, item.mergeUpdatedAtUtc);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db);
        error = message ? static_cast<const wchar_t*>(message) : L"导入待办失败。";
        return false;
    }
    item.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    return true;
}

bool UpdateTodoRaw(sqlite3* db, TodoItem& item, int localId, int localPos, std::wstring& error) {
    item.id = localId;
    item.pos = localPos;
    if (!NormalizeTodoForImport(db, item, false, error)) {
        return false;
    }
    SQLiteStatement statement(db,
        L"UPDATE TodoItems SET TagId=?,Title=?,Content=?,Enabled=?,ScheduleKind=?,RepeatMode=?,RepeatInterval=?,RepeatLimit=?,RepeatFinished=?,CronExpression=?,AnchorAt=?,NextDueAt=?,CompletedAt=?,IgnoredDueAt=?,SnoozedUntil=?,POS=?,CreatedAt=?,UpdatedAt=?,TodoUid=?,MergeUpdatedAtUtc=? WHERE ID=?;");
    if (!statement.ok()) {
        const void* message = sqlite3_errmsg16(db);
        error = L"准备更新合并待办 SQL 失败";
        if (message) {
            error += L": ";
            error += static_cast<const wchar_t*>(message);
        }
        error += L"。";
        return false;
    }
    statement.bindInt(1, item.tagId);
    statement.bindText(2, item.title);
    statement.bindText(3, item.content);
    statement.bindInt(4, item.enabled ? 1 : 0);
    statement.bindInt(5, static_cast<int>(item.scheduleKind));
    statement.bindInt(6, static_cast<int>(item.repeatMode));
    statement.bindInt(7, item.repeatInterval);
    statement.bindInt(8, item.repeatLimit);
    statement.bindInt(9, item.repeatFinished);
    statement.bindText(10, item.cronExpression);
    statement.bindText(11, item.anchorAt);
    statement.bindText(12, item.nextDueAt);
    statement.bindText(13, item.completedAt);
    statement.bindText(14, item.ignoredDueAt);
    statement.bindText(15, item.snoozedUntil);
    statement.bindInt(16, item.pos);
    statement.bindText(17, item.createdAt);
    statement.bindText(18, item.updatedAt);
    statement.bindText(19, item.todoUid);
    statement.bindText(20, item.mergeUpdatedAtUtc);
    statement.bindInt(21, localId);
    if (statement.step() != SQLITE_DONE || sqlite3_changes(db) <= 0) {
        const void* message = sqlite3_errmsg16(db);
        error = message ? static_cast<const wchar_t*>(message) : L"更新合并待办失败。";
        return false;
    }
    return true;
}

bool DeleteTombstone(sqlite3* db, const std::wstring& todoUid, std::wstring& error) {
    SQLiteStatement statement(db, L"DELETE FROM TodoTombstones WHERE TodoUid=?;");
    if (!statement.ok()) {
        error = L"准备删除待办墓碑 SQL 失败。";
        return false;
    }
    statement.bindText(1, todoUid);
    if (statement.step() != SQLITE_DONE) {
        error = L"删除待办墓碑失败。";
        return false;
    }
    return true;
}

bool TodoBusinessEqual(const TodoItem& left, const TodoItem& right, int rightTagId) {
    return left.tagId == rightTagId &&
        left.title == right.title && left.content == right.content && left.enabled == right.enabled &&
        left.scheduleKind == right.scheduleKind && left.repeatMode == right.repeatMode &&
        left.repeatInterval == right.repeatInterval && left.repeatLimit == right.repeatLimit &&
        left.repeatFinished == right.repeatFinished && left.cronExpression == right.cronExpression &&
        left.anchorAt == right.anchorAt && left.nextDueAt == right.nextDueAt &&
        left.completedAt == right.completedAt && left.ignoredDueAt == right.ignoredDueAt &&
        left.snoozedUntil == right.snoozedUntil && left.createdAt == right.createdAt;
}

bool ShouldExportTodo(const TodoItem& todo, const TodoJsonExportOptions& options) {
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
    if (!TryParseTodoTimestamp(dueAt, due) || !TryParseTodoTimestamp(CurrentTodoTimestamp(), now)) {
        return true;
    }
    FILETIME dueFile{};
    FILETIME nowFile{};
    if (!SystemTimeToFileTime(&due, &dueFile) || !SystemTimeToFileTime(&now, &nowFile)) {
        return true;
    }
    return CompareFileTime(&dueFile, &nowFile) >= 0;
}

std::wstring CurrentIso8601Timestamp() {
    return TodoTimestampToIso8601(CurrentTodoTimestamp());
}

std::wstring BuildTodoExportJson(const AppModel& model, const TodoJsonExportOptions& options) {
    std::wstringstream out;
    out << L"{\n";
    out << L"  \"app\": \"Quattro\",\n";
    out << L"  \"exportType\": \"todo-backup\",\n";
    out << L"  \"formatVersion\": " << kTodoJsonFormatVersion << L",\n";
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
        out << L"        \"todoUid\": \"" << JsonEscape(todo.todoUid) << L"\",\n";
        out << L"        \"mergeUpdatedAtUtc\": \"" << JsonEscape(todo.mergeUpdatedAtUtc) << L"\",\n";
        out << L"        \"groupUid\": \"" << JsonEscape(parent ? parent->groupUid : L"") << L"\",\n";
        out << L"        \"tagUid\": \"" << JsonEscape(tag ? tag->groupUid : L"") << L"\",\n";
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
        out << L"        \"lastNotifiedDueAt\": \"" << JsonEscape(todo.lastNotifiedDueAt) << L"\",\n";
        out << L"        \"lastNotifiedAt\": \"" << JsonEscape(todo.lastNotifiedAt) << L"\",\n";
        out << L"        \"lastViewedDueAt\": \"" << JsonEscape(todo.lastViewedDueAt) << L"\",\n";
        out << L"        \"lastViewedAt\": \"" << JsonEscape(todo.lastViewedAt) << L"\",\n";
        out << L"        \"ignoredDueAt\": \"" << JsonEscape(todo.ignoredDueAt) << L"\",\n";
        out << L"        \"snoozedUntil\": \"" << JsonEscape(todo.snoozedUntil) << L"\",\n";
        out << L"        \"pos\": " << todo.pos << L",\n";
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

struct ParsedTodoRecord {
    TodoItem item;
    std::wstring groupName;
    std::wstring tagName;
    std::wstring groupUid;
    std::wstring tagUid;
};

struct ParsedTodoJson {
    bool ok = false;
    std::wstring message;
    std::wstring fileHash;
    std::vector<ParsedTodoRecord> records;
    std::vector<std::wstring> warnings;
    int failedCount = 0;
};

std::wstring MergeTimestampFallback(const JsonValue* quattro, const JsonValue& entry, const std::wstring& exportedAt) {
    std::wstring value = JsonStringField(quattro, L"mergeUpdatedAtUtc", JsonStringField(entry, L"mergeUpdatedAtUtc"));
    if (!value.empty()) {
        return value;
    }
    const std::wstring updatedAt = JsonStringField(quattro, L"updatedAt", JsonStringField(entry, L"updatedAt"));
    value = LocalTimestampToUtc(ImportableTodoTimestamp(updatedAt));
    if (!value.empty()) {
        return value;
    }
    value = LocalTimestampToUtc(ImportableTodoTimestamp(exportedAt));
    if (!value.empty()) {
        return value;
    }
    return kStableFallbackMergeTimestamp;
}

ParsedTodoJson ParseTodoJsonFile(const std::filesystem::path& jsonPath) {
    ParsedTodoJson parsed;
    const std::vector<std::uint8_t> bytes = ReadBinaryFile(jsonPath);
    parsed.fileHash = ContentHash(bytes);
    const std::wstring text = LoadUtf8File(jsonPath);
    if (text.empty()) {
        parsed.message = L"读取待办 JSON 失败，或文件内容为空。";
        return parsed;
    }

    JsonValue root;
    std::wstring error;
    if (!ParseJson(text, root, error)) {
        parsed.message = L"待办 JSON 解析失败: " + error;
        return parsed;
    }
    const JsonValue* todos = root.get(L"todos");
    if (!root.isObject() || !todos || !todos->isArray()) {
        parsed.message = L"待办 JSON 缺少 todos 数组。";
        return parsed;
    }

    const std::wstring exportedAt = JsonStringField(root, L"exportedAt");
    std::unordered_set<std::wstring> seenUids;
    for (std::size_t index = 0; index < todos->arrayValue.size(); ++index) {
        const JsonValue& entry = todos->arrayValue[index];
        if (!entry.isObject()) {
            ++parsed.failedCount;
            parsed.warnings.push_back(L"跳过非对象待办条目。");
            continue;
        }
        const JsonValue* quattro = ObjectField(entry, L"quattro");
        ParsedTodoRecord record;
        record.groupName = Trim(JsonStringField(entry, L"groupName", L"默认分组"));
        record.tagName = Trim(JsonStringField(entry, L"tagName", L"待办事项"));
        if (record.groupName.empty()) record.groupName = L"默认分组";
        if (record.tagName.empty()) record.tagName = L"待办事项";
        record.groupUid = JsonStringField(quattro, L"groupUid", JsonStringField(entry, L"groupUid"));
        record.tagUid = JsonStringField(quattro, L"tagUid", JsonStringField(entry, L"tagUid"));

        TodoItem& item = record.item;
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
        item.lastNotifiedDueAt = JsonStringField(quattro, L"lastNotifiedDueAt");
        item.lastNotifiedAt = JsonStringField(quattro, L"lastNotifiedAt");
        item.lastViewedDueAt = JsonStringField(quattro, L"lastViewedDueAt");
        item.lastViewedAt = JsonStringField(quattro, L"lastViewedAt");
        item.ignoredDueAt = JsonStringField(quattro, L"ignoredDueAt");
        item.snoozedUntil = JsonStringField(quattro, L"snoozedUntil");
        item.createdAt = JsonStringField(quattro, L"createdAt", JsonStringField(entry, L"createdAt"));
        item.updatedAt = JsonStringField(quattro, L"updatedAt", JsonStringField(entry, L"updatedAt"));
        item.pos = JsonIntField(quattro, L"pos", JsonIntField(entry, L"pos", -1));
        item.todoUid = JsonStringField(quattro, L"todoUid", JsonStringField(entry, L"todoUid"));
        if (item.todoUid.empty()) {
            const int originalId = JsonIntField(quattro, L"originalId", JsonIntField(entry, L"id", 0));
            item.todoUid = L"legacy-json-" + parsed.fileHash + L"-" +
                (originalId > 0 ? std::to_wstring(originalId) : (L"index-" + std::to_wstring(index)));
        }
        item.mergeUpdatedAtUtc = MergeTimestampFallback(quattro, entry, exportedAt);
        if (!quattro && item.scheduleKind == TodoScheduleKind::None && !JsonStringField(entry, L"dueAt").empty()) {
            const std::wstring dueAt = ImportableTodoTimestamp(JsonStringField(entry, L"dueAt"));
            if (!dueAt.empty()) {
                item.scheduleKind = TodoScheduleKind::Once;
                item.anchorAt = dueAt;
                item.nextDueAt = item.anchorAt;
            }
        }
        if (!quattro && item.completedAt.empty() && JsonBoolField(entry, L"completed", false)) {
            item.completedAt = ImportableTodoTimestamp(exportedAt);
            if (item.completedAt.empty()) {
                item.completedAt = kStableFallbackLocalTimestamp;
            }
        }
        if (Trim(item.title).empty()) {
            ++parsed.failedCount;
            parsed.warnings.push_back(L"跳过标题为空的待办。");
            continue;
        }
        if (!seenUids.insert(item.todoUid).second) {
            ++parsed.failedCount;
            parsed.warnings.push_back(L"跳过重复同步标识的待办: " + item.title);
            continue;
        }
        parsed.records.push_back(std::move(record));
    }

    parsed.ok = true;
    parsed.message = L"待办 JSON 预分析完成。";
    return parsed;
}

int ResolveGroup(sqlite3* db, AppModel& model, const ParsedTodoRecord& record, TodoJsonImportReport& report, std::wstring& error) {
    if (const Group* byUid = FindGroupByUid(model.groups, record.groupUid)) {
        if (byUid->parentGroup == 0) {
            return byUid->id;
        }
    }
    if (const Group* byName = FindRootGroupByName(model.groups, record.groupName)) {
        return byName->id;
    }

    Group group;
    group.name = UniqueSiblingGroupName(model.groups, 0, record.groupName.empty() ? L"默认分组" : record.groupName);
    group.groupUid = record.groupUid;
    if (!InsertGroupRaw(db, group, error)) {
        return 0;
    }
    model.groups.push_back(group);
    ++report.groupsCreated;
    return group.id;
}

int ResolveTodoTag(sqlite3* db, AppModel& model, const ParsedTodoRecord& record, int parentGroupId, TodoJsonImportReport& report, std::wstring& error) {
    if (const Group* byUid = FindGroupByUid(model.groups, record.tagUid)) {
        if (IsTodoItemsTag(*byUid)) {
            return byUid->id;
        }
    }
    if (const Group* byName = FindTodoTagByName(model.groups, parentGroupId, record.tagName)) {
        return byName->id;
    }

    Group tag;
    tag.parentGroup = parentGroupId;
    tag.name = UniqueSiblingGroupName(model.groups, parentGroupId, record.tagName.empty() ? L"待办事项" : record.tagName);
    tag.type = 4;
    tag.content = L"todoItems";
    tag.groupUid = record.tagUid;
    if (!InsertGroupRaw(db, tag, error)) {
        return 0;
    }
    model.groups.push_back(tag);
    ++report.tagsCreated;
    return tag.id;
}

void FinishReportMessage(TodoJsonImportReport& report, TodoJsonImportMode mode) {
    if (report.ok) {
        report.message = mode == TodoJsonImportMode::ReplaceAll ? L"待办 JSON 全量导入完成。" : L"待办 JSON 合并导入完成。";
    } else if (report.message.empty()) {
        report.message = L"待办 JSON 导入失败。";
    }
}

TodoJsonImportReport ApplyImport(
    const std::filesystem::path& appDirectory,
    const ParsedTodoJson& parsed,
    const TodoJsonImportOptions& options,
    AppModel targetModel) {
    TodoJsonImportReport report;
    report.todosParsed = static_cast<int>(parsed.records.size());
    report.todosFailed = parsed.failedCount;
    report.warnings = parsed.warnings;

    SQLiteDatabase targetDb(appDirectory / L"db" / L"link.db");
    if (!targetDb.ok()) {
        report.message = L"打开当前数据库进行待办导入失败: " + targetDb.Error();
        return report;
    }
    std::wstring transactionError;
    if (!Exec(targetDb.get(), "BEGIN IMMEDIATE;", transactionError)) {
        report.message = L"开始待办导入事务失败: " + transactionError;
        return report;
    }
    auto rollback = [&]() {
        std::wstring ignored;
        Exec(targetDb.get(), "ROLLBACK;", ignored);
    };

    if (options.mode == TodoJsonImportMode::ReplaceAll) {
        report.todosDeletedForReplace = static_cast<int>(targetModel.todos.size());
        if (!Exec(targetDb.get(), "DELETE FROM TodoItems; DELETE FROM TodoTombstones;", transactionError)) {
            report.message = L"清空现有待办失败: " + transactionError;
            rollback();
            return report;
        }
        targetModel.todos.clear();
        targetModel.todoTombstones.clear();
    }

    std::unordered_set<std::wstring> tombstones;
    for (const auto& item : targetModel.todoTombstones) tombstones.insert(item.todoUid);

    for (const auto& record : parsed.records) {
        TodoItem sourceTodo = record.item;
        const int parentGroupId = ResolveGroup(targetDb.get(), targetModel, record, report, transactionError);
        if (parentGroupId <= 0) {
            ++report.todosFailed;
            report.message = transactionError;
            rollback();
            return report;
        }
        const int tagId = ResolveTodoTag(targetDb.get(), targetModel, record, parentGroupId, report, transactionError);
        if (tagId <= 0) {
            ++report.todosFailed;
            report.message = transactionError;
            rollback();
            return report;
        }
        sourceTodo.tagId = tagId;

        if (options.mode == TodoJsonImportMode::ReplaceAll) {
            sourceTodo.id = 0;
            if (!InsertTodoRaw(targetDb.get(), sourceTodo, false, transactionError)) {
                ++report.todosFailed;
                report.message = transactionError;
                rollback();
                return report;
            }
            targetModel.todos.push_back(sourceTodo);
            ++report.todosAdded;
            continue;
        }

        int localIndex = -1;
        for (std::size_t index = 0; index < targetModel.todos.size(); ++index) {
            if (!sourceTodo.todoUid.empty() && targetModel.todos[index].todoUid == sourceTodo.todoUid) {
                localIndex = static_cast<int>(index);
                break;
            }
        }

        const bool wasDeleted = tombstones.find(sourceTodo.todoUid) != tombstones.end();
        if (localIndex < 0 && wasDeleted) {
            if (options.restoreDeletedPolicy == TodoJsonRestoreDeletedPolicy::KeepDeleted) {
                ++report.todosKeptDeleted;
                continue;
            }
            if (!DeleteTombstone(targetDb.get(), sourceTodo.todoUid, transactionError)) {
                ++report.todosFailed;
                report.message = transactionError;
                rollback();
                return report;
            }
            sourceTodo.id = 0;
            if (!InsertTodoRaw(targetDb.get(), sourceTodo, true, transactionError)) {
                ++report.todosFailed;
                report.message = transactionError;
                rollback();
                return report;
            }
            targetModel.todos.push_back(sourceTodo);
            tombstones.erase(sourceTodo.todoUid);
            ++report.todosRestored;
            continue;
        }

        if (localIndex < 0) {
            sourceTodo.id = 0;
            if (!InsertTodoRaw(targetDb.get(), sourceTodo, true, transactionError)) {
                ++report.todosFailed;
                report.message = transactionError;
                rollback();
                return report;
            }
            targetModel.todos.push_back(sourceTodo);
            ++report.todosAdded;
            continue;
        }

        TodoItem& local = targetModel.todos[static_cast<std::size_t>(localIndex)];
        if (sourceTodo.mergeUpdatedAtUtc > local.mergeUpdatedAtUtc) {
            sourceTodo.todoUid = local.todoUid;
            sourceTodo.lastNotifiedDueAt = local.lastNotifiedDueAt;
            sourceTodo.lastNotifiedAt = local.lastNotifiedAt;
            sourceTodo.lastViewedDueAt = local.lastViewedDueAt;
            sourceTodo.lastViewedAt = local.lastViewedAt;
            if (!UpdateTodoRaw(targetDb.get(), sourceTodo, local.id, local.pos, transactionError)) {
                ++report.todosFailed;
                report.message = transactionError;
                rollback();
                return report;
            }
            local = sourceTodo;
            ++report.todosUpdatedFromRemote;
        } else if (sourceTodo.mergeUpdatedAtUtc < local.mergeUpdatedAtUtc) {
            ++report.todosKeptLocal;
        } else if (TodoBusinessEqual(local, sourceTodo, sourceTodo.tagId)) {
            ++report.todosSkippedIdentical;
        } else {
            ++report.todosConflicted;
            report.warnings.push_back(L"待办更新时间相同但内容不同，已保留本地: " + local.title);
        }
    }

    if (!Exec(targetDb.get(), "COMMIT;", transactionError)) {
        report.message = L"提交待办导入事务失败: " + transactionError;
        rollback();
        return report;
    }

    report.ok = true;
    FinishReportMessage(report, options.mode);
    return report;
}
}

TodoJsonBackupService::TodoJsonBackupService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

std::wstring TodoJsonBackupService::DefaultFileName() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t buffer[64]{};
    swprintf_s(buffer,
        L"quattro-todos-%04u%02u%02u-%02u%02u.json",
        static_cast<unsigned>(local.wYear),
        static_cast<unsigned>(local.wMonth),
        static_cast<unsigned>(local.wDay),
        static_cast<unsigned>(local.wHour),
        static_cast<unsigned>(local.wMinute));
    return buffer;
}

bool TodoJsonBackupService::ExportJson(
    const std::filesystem::path& targetPath,
    const TodoJsonExportOptions& options,
    std::wstring& error) const {
    StorageService storage(appDirectory_);
    const AppModel model = storage.Load();
    if (!storage.sqliteAvailable()) {
        error = L"当前数据库不可用: " + storage.lastError();
        return false;
    }
    if (!SaveUtf8File(targetPath, BuildTodoExportJson(model, options))) {
        error = L"写入待办 JSON 文件失败。";
        return false;
    }
    return true;
}

TodoJsonImportReport TodoJsonBackupService::PreviewImport(
    const std::filesystem::path& jsonPath,
    const TodoJsonImportOptions& options) const {
    TodoJsonImportReport report;
    const ParsedTodoJson parsed = ParseTodoJsonFile(jsonPath);
    report.todosParsed = static_cast<int>(parsed.records.size());
    report.todosFailed = parsed.failedCount;
    report.warnings = parsed.warnings;
    if (!parsed.ok) {
        report.message = parsed.message;
        return report;
    }

    StorageService targetStorage(appDirectory_);
    const AppModel targetModel = targetStorage.Load();
    if (!targetStorage.sqliteAvailable()) {
        report.message = L"当前数据库不可用: " + targetStorage.lastError();
        return report;
    }
    if (options.mode == TodoJsonImportMode::ReplaceAll) {
        report.todosDeletedForReplace = static_cast<int>(targetModel.todos.size());
    }
    report.ok = true;
    report.message = L"待办 JSON 预分析完成。";
    return report;
}

TodoJsonImportReport TodoJsonBackupService::ImportJson(
    const std::filesystem::path& jsonPath,
    const TodoJsonImportOptions& options) const {
    ParsedTodoJson parsed = ParseTodoJsonFile(jsonPath);
    if (!parsed.ok) {
        TodoJsonImportReport report;
        report.todosParsed = static_cast<int>(parsed.records.size());
        report.todosFailed = parsed.failedCount;
        report.warnings = parsed.warnings;
        report.message = parsed.message;
        return report;
    }

    StorageService targetStorage(appDirectory_);
    const AppModel targetModel = targetStorage.Load();
    if (!targetStorage.sqliteAvailable()) {
        TodoJsonImportReport report;
        report.message = L"当前数据库不可用: " + targetStorage.lastError();
        return report;
    }

    TodoJsonImportReport backupReport;
    const std::filesystem::path backupDirectory = CreateSafetyBackup(appDirectory_, backupReport);
    TodoJsonImportReport report = ApplyImport(appDirectory_, parsed, options, targetModel);
    report.warnings.insert(report.warnings.begin(), backupReport.warnings.begin(), backupReport.warnings.end());
    if (!report.ok) {
        RestoreSafetyBackup(appDirectory_, backupDirectory, report);
        FinishReportMessage(report, options.mode);
    }
    return report;
}
