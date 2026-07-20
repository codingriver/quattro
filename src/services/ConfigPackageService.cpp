#include "ConfigPackageService.h"

#include "Config.h"
#include "Storage.h"
#include "Utilities.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr int kPackageFormatVersion = 2;

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
    sqlite3_int64 columnInt64(int index) const { return sqlite3_column_int64(stmt_, index); }
    std::wstring columnText(int index) const {
        const void* text = sqlite3_column_text16(stmt_, index);
        return text ? static_cast<const wchar_t*>(text) : L"";
    }
    std::vector<std::uint8_t> columnBlob(int index) const {
        const void* blob = sqlite3_column_blob(stmt_, index);
        const int bytes = sqlite3_column_bytes(stmt_, index);
        if (!blob || bytes <= 0) {
            return {};
        }
        const auto* begin = static_cast<const std::uint8_t*>(blob);
        return std::vector<std::uint8_t>(begin, begin + bytes);
    }
    void bindInt(int index, int value) { sqlite3_bind_int(stmt_, index, value); }
    void bindInt64(int index, sqlite3_int64 value) { sqlite3_bind_int64(stmt_, index, value); }
    void bindText(int index, const std::wstring& value) {
        sqlite3_bind_text16(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bindBlob(int index, const std::vector<std::uint8_t>& value) {
        if (value.empty()) {
            sqlite3_bind_blob(stmt_, index, "", 0, SQLITE_TRANSIENT);
            return;
        }
        sqlite3_bind_blob(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
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

bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(file);
}

std::wstring NormalizePackagePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return path;
}

std::wstring PackagePathFor(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (ec) {
        relative = path.filename();
    }
    return NormalizePackagePath(relative.wstring());
}

bool EnsurePackageSchema(sqlite3* db, std::wstring& error) {
    return Exec(db,
        "CREATE TABLE PackageManifest(Key TEXT PRIMARY KEY,Value TEXT NOT NULL);"
        "CREATE TABLE PackageFiles("
        "Path TEXT PRIMARY KEY,"
        "Kind TEXT NOT NULL,"
        "Size INTEGER NOT NULL,"
        "Hash TEXT NOT NULL,"
        "Data BLOB NOT NULL);",
        error);
}

bool PutManifest(sqlite3* db, const std::wstring& key, const std::wstring& value, std::wstring& error) {
    SQLiteStatement statement(db,
        L"INSERT INTO PackageManifest(Key,Value) VALUES(?,?) "
        L"ON CONFLICT(Key) DO UPDATE SET Value=excluded.Value;");
    if (!statement.ok()) {
        error = L"写入配置包清单失败。";
        return false;
    }
    statement.bindText(1, key);
    statement.bindText(2, value);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db);
        error = message ? static_cast<const wchar_t*>(message) : L"写入配置包清单失败。";
        return false;
    }
    return true;
}

std::wstring GetManifest(sqlite3* db, const std::wstring& key) {
    SQLiteStatement statement(db, L"SELECT Value FROM PackageManifest WHERE Key=?;");
    if (!statement.ok()) {
        return {};
    }
    statement.bindText(1, key);
    if (statement.step() != SQLITE_ROW) {
        return {};
    }
    return statement.columnText(0);
}

std::wstring ContentHash(const std::vector<std::uint8_t>& data) {
    std::uint32_t hash = 2166136261u;
    for (std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return Hex8(hash);
}

bool PutPackageFile(sqlite3* db, const std::wstring& packagePath, const std::wstring& kind, const std::vector<std::uint8_t>& data, std::wstring& error) {
    SQLiteStatement statement(db,
        L"INSERT INTO PackageFiles(Path,Kind,Size,Hash,Data) VALUES(?,?,?,?,?) "
        L"ON CONFLICT(Path) DO UPDATE SET Kind=excluded.Kind,Size=excluded.Size,Hash=excluded.Hash,Data=excluded.Data;");
    if (!statement.ok()) {
        error = L"写入配置包文件失败。";
        return false;
    }
    statement.bindText(1, packagePath);
    statement.bindText(2, kind);
    statement.bindInt64(3, static_cast<sqlite3_int64>(data.size()));
    statement.bindText(4, ContentHash(data));
    statement.bindBlob(5, data);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db);
        error = message ? static_cast<const wchar_t*>(message) : L"写入配置包文件失败。";
        return false;
    }
    return true;
}

bool AddFileToPackage(sqlite3* db, const std::filesystem::path& source, const std::wstring& packagePath, const std::wstring& kind, std::wstring& error) {
    if (!FileExists(source)) {
        return true;
    }
    return PutPackageFile(db, packagePath, kind, ReadBinaryFile(source), error);
}

std::filesystem::path UniqueTempDirectory(const wchar_t* prefix) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::wstring(prefix) + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(ticks));
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

std::filesystem::path CreateSafetyBackup(const std::filesystem::path& appDirectory, ConfigPackageReport& report) {
    const std::filesystem::path backupRoot = appDirectory / L"backups";
    const std::filesystem::path backupDirectory = backupRoot / (L"merge-import-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    std::filesystem::create_directories(backupDirectory / L"db", ec);
    if (FileExists(appDirectory / L"conf.ini")) {
        std::filesystem::copy_file(appDirectory / L"conf.ini", backupDirectory / L"conf.ini", std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            report.warnings.push_back(L"备份 conf.ini 失败: " + Utf8ToWide(ec.message().c_str()));
        }
    }
    std::wstring error;
    if (!BackupDatabase(appDirectory / L"db" / L"link.db", backupDirectory / L"db" / L"link.db", error) && !error.empty()) {
        report.warnings.push_back(L"备份 link.db 失败: " + error);
    }
    return backupDirectory;
}

void RestoreSafetyBackup(const std::filesystem::path& appDirectory, const std::filesystem::path& backupDirectory, ConfigPackageReport& report) {
    std::error_code ec;
    std::filesystem::create_directories(appDirectory / L"db", ec);

    const std::filesystem::path backupConfig = backupDirectory / L"conf.ini";
    const std::filesystem::path targetConfig = appDirectory / L"conf.ini";
    if (FileExists(backupConfig)) {
        std::filesystem::copy_file(backupConfig, targetConfig, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            report.warnings.push_back(L"导入失败后恢复 conf.ini 失败。");
        }
    }

    const std::filesystem::path backupDb = backupDirectory / L"db" / L"link.db";
    const std::filesystem::path targetDb = appDirectory / L"db" / L"link.db";
    if (FileExists(backupDb)) {
        std::filesystem::copy_file(backupDb, targetDb, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            report.warnings.push_back(L"导入失败后恢复 link.db 失败。");
        }
    }
}

bool ExtractPackageFile(sqlite3* packageDb, const std::wstring& packagePath, const std::filesystem::path& targetPath, std::wstring& error) {
    SQLiteStatement statement(packageDb, L"SELECT Size,Hash,Data FROM PackageFiles WHERE Path=?;");
    if (!statement.ok()) {
        error = L"读取配置包文件失败。";
        return false;
    }
    statement.bindText(1, packagePath);
    if (statement.step() != SQLITE_ROW) {
        error = L"配置包缺少 " + packagePath + L"。";
        return false;
    }
    const sqlite3_int64 size = statement.columnInt64(0);
    const std::wstring hash = statement.columnText(1);
    const std::vector<std::uint8_t> data = statement.columnBlob(2);
    if (size != static_cast<sqlite3_int64>(data.size()) || hash != ContentHash(data)) {
        error = L"配置包文件校验失败: " + packagePath;
        return false;
    }
    if (!WriteBinaryFile(targetPath, data)) {
        error = L"写入临时导入文件失败: " + targetPath.wstring();
        return false;
    }
    return true;
}

std::wstring NormalizedNameKey(const std::wstring& value) {
    return ToLower(Trim(value));
}

std::wstring NormalizedPathKey(const std::wstring& value) {
    return ToLower(Trim(value));
}

int FindMatchingGroup(const AppModel& model, const Group& sourceGroup, int targetParentGroup, bool allowLegacyNameFallback) {
    if (!sourceGroup.groupUid.empty()) {
        for (const auto& group : model.groups) {
            if (group.groupUid == sourceGroup.groupUid) return group.id;
        }
        if (!allowLegacyNameFallback) return 0;
    }
    const std::wstring name = NormalizedNameKey(sourceGroup.name);
    if (name.empty()) {
        return 0;
    }
    for (const auto& group : model.groups) {
        if (group.parentGroup == targetParentGroup &&
            group.type == sourceGroup.type &&
            NormalizedNameKey(group.name) == name) {
            return group.id;
        }
    }
    return 0;
}

bool IsDuplicateLinkInTag(const AppModel& model, const Link& sourceLink, int targetTagId) {
    const std::wstring path = NormalizedPathKey(sourceLink.path);
    const std::wstring parameter = Trim(sourceLink.parameter);
    const std::wstring workDir = NormalizedPathKey(sourceLink.workDir);
    for (const auto& link : model.links) {
        if (link.parentGroup == targetTagId &&
            link.type == sourceLink.type &&
            NormalizedPathKey(link.path) == path &&
            Trim(link.parameter) == parameter &&
            NormalizedPathKey(link.workDir) == workDir &&
            link.pidl == sourceLink.pidl) {
            return true;
        }
    }
    return false;
}

std::wstring NoteContentForTag(const AppModel& model, int tagId) {
    for (const auto& note : model.notes) {
        if (note.tagId == tagId) {
            return note.content;
        }
    }
    return {};
}

void UpsertNoteContent(AppModel& model, int tagId, const std::wstring& content) {
    for (auto& note : model.notes) {
        if (note.tagId == tagId) {
            note.content = content;
            return;
        }
    }
    NotePage note;
    note.tagId = tagId;
    note.content = content;
    model.notes.push_back(std::move(note));
}

std::wstring MergeNoteContent(const std::wstring& existing, const std::wstring& imported) {
    if (Trim(existing).empty() || existing == imported) {
        return imported;
    }
    if (Trim(imported).empty()) {
        return existing;
    }
    return existing + L"\n\n--- 导入便签 ---\n" + imported;
}

std::filesystem::path UniqueImportedIconPath(const std::filesystem::path& targetPath) {
    if (!FileExists(targetPath)) {
        return targetPath;
    }
    const std::filesystem::path directory = targetPath.parent_path();
    const std::wstring stem = targetPath.stem().wstring();
    const std::wstring extension = targetPath.extension().wstring();
    for (int index = 1; index < 1000; ++index) {
        std::filesystem::path candidate = directory / (stem + L".imported" + (index == 1 ? L"" : std::to_wstring(index)) + extension);
        if (!FileExists(candidate)) {
            return candidate;
        }
    }
    return directory / (stem + L".imported" + extension);
}

void MergeUrlIcons(sqlite3* packageDb, const std::filesystem::path& appDirectory, ConfigPackageReport& report) {
    SQLiteStatement query(packageDb, L"SELECT Path,Size,Hash,Data FROM PackageFiles WHERE Kind='urlIcon' ORDER BY Path;");
    if (!query.ok()) {
        report.warnings.push_back(L"读取配置包 URL 图标失败。");
        return;
    }
    while (query.step() == SQLITE_ROW) {
        const std::wstring packagePath = query.columnText(0);
        const sqlite3_int64 size = query.columnInt64(1);
        const std::wstring hash = query.columnText(2);
        const std::vector<std::uint8_t> data = query.columnBlob(3);
        if (size != static_cast<sqlite3_int64>(data.size()) || hash != ContentHash(data)) {
            report.warnings.push_back(L"跳过校验失败的 URL 图标: " + packagePath);
            continue;
        }
        std::wstring relative = packagePath;
        const std::wstring prefix = L"icons/url/";
        if (relative.rfind(prefix, 0) == 0) {
            relative = relative.substr(prefix.size());
        }
        relative = ReplaceAll(relative, L"/", L"\\");
        if (relative.find(L"..") != std::wstring::npos) {
            report.warnings.push_back(L"跳过非法 URL 图标路径: " + packagePath);
            continue;
        }
        const std::filesystem::path target = UniqueImportedIconPath(appDirectory / L"icons" / L"url" / relative);
        if (WriteBinaryFile(target, data)) {
            ++report.urlIconsAdded;
        } else {
            report.warnings.push_back(L"写入 URL 图标失败: " + target.wstring());
        }
    }
}

void NormalizeLegacySourceIdentities(AppModel& model, const std::wstring& packageHash) {
    for (auto& group : model.groups) {
        group.groupUid = L"legacy-group-" + packageHash + L"-" + std::to_wstring(group.id);
    }
    for (auto& todo : model.todos) {
        todo.todoUid = L"legacy-todo-" + packageHash + L"-" + std::to_wstring(todo.id);
    }
}

std::wstring MergeStateToken(const std::filesystem::path& packagePath, const AppModel& model) {
    std::uint32_t hash = 2166136261u;
    auto append = [&](const std::wstring& value) {
        for (wchar_t ch : value) {
            hash ^= static_cast<std::uint32_t>(ch);
            hash *= 16777619u;
        }
        hash ^= 0xffu;
        hash *= 16777619u;
    };
    append(ContentHash(ReadBinaryFile(packagePath)));
    for (const auto& group : model.groups) {
        append(group.groupUid);
        append(std::to_wstring(group.id));
    }
    for (const auto& todo : model.todos) {
        append(todo.todoUid);
        append(todo.mergeUpdatedAtUtc);
        append(std::to_wstring(todo.tagId));
    }
    for (const auto& tombstone : model.todoTombstones) {
        append(tombstone.todoUid);
        append(tombstone.deletedAtUtc);
    }
    return Hex8(hash);
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

int NextTodoPosition(sqlite3* db, int tagId) {
    SQLiteStatement query(db, L"SELECT COALESCE(MAX(POS),-1)+1 FROM TodoItems WHERE TagId=?;");
    query.bindInt(1, tagId);
    return query.ok() && query.step() == SQLITE_ROW ? query.columnInt(0) : 0;
}

bool InsertMergedTodo(sqlite3* db, TodoItem& item, std::wstring& error) {
    item.pos = NextTodoPosition(db, item.tagId);
    item.lastNotifiedDueAt.clear();
    item.lastNotifiedAt.clear();
    item.lastViewedDueAt.clear();
    item.lastViewedAt.clear();
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

bool UpdateMergedTodo(sqlite3* db, const TodoItem& item, int localId, std::wstring& error) {
    SQLiteStatement statement(db,
        L"UPDATE TodoItems SET TagId=?,Title=?,Content=?,Enabled=?,ScheduleKind=?,RepeatMode=?,RepeatInterval=?,RepeatLimit=?,RepeatFinished=?,CronExpression=?,AnchorAt=?,NextDueAt=?,CompletedAt=?,IgnoredDueAt=?,SnoozedUntil=?,POS=?,CreatedAt=?,UpdatedAt=?,TodoUid=?,MergeUpdatedAtUtc=? WHERE ID=?;");
    if (!statement.ok()) {
        error = L"准备更新合并待办 SQL 失败。";
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

ConfigPackageReport MergeImportedData(
    const std::filesystem::path& appDirectory,
    const std::filesystem::path& importRoot,
    int packageVersion,
    const std::wstring& packageHash,
    TodoRestorePolicy restorePolicy) {
    ConfigPackageReport report;

    StorageService targetStorage(appDirectory);
    AppModel targetModel = targetStorage.Load();
    if (!targetStorage.sqliteAvailable()) {
        report.message = L"当前数据库不可用: " + targetStorage.lastError();
        return report;
    }

    StorageService sourceStorage(importRoot);
    AppModel sourceModel = sourceStorage.Load();
    if (!sourceStorage.sqliteAvailable()) {
        report.message = L"导入包数据库不可用: " + sourceStorage.lastError();
        return report;
    }
    const bool legacyPackage = packageVersion == 1;
    if (legacyPackage) NormalizeLegacySourceIdentities(sourceModel, packageHash);

    std::unordered_map<int, int> groupIdMap;
    std::vector<Group> sourceMajorGroups;
    std::vector<Group> sourceTags;
    for (const auto& group : sourceModel.groups) {
        if (group.parentGroup == 0) {
            sourceMajorGroups.push_back(group);
        } else {
            sourceTags.push_back(group);
        }
    }

    std::sort(sourceMajorGroups.begin(), sourceMajorGroups.end(), [](const Group& left, const Group& right) {
        return left.pos == right.pos ? left.id < right.id : left.pos < right.pos;
    });
    std::sort(sourceTags.begin(), sourceTags.end(), [](const Group& left, const Group& right) {
        if (left.parentGroup != right.parentGroup) {
            return left.parentGroup < right.parentGroup;
        }
        return left.pos == right.pos ? left.id < right.id : left.pos < right.pos;
    });

    for (const auto& sourceGroup : sourceMajorGroups) {
        const int existingGroupId = FindMatchingGroup(targetModel, sourceGroup, 0, legacyPackage);
        if (existingGroupId > 0) {
            groupIdMap[sourceGroup.id] = existingGroupId;
            ++report.groupsMerged;
            continue;
        }

        Group group = sourceGroup;
        group.id = 0;
        group.parentGroup = 0;
        group.pos = -1;
        if (!targetStorage.InsertGroup(group)) {
            report.message = L"导入分组失败: " + targetStorage.lastError();
            return report;
        }
        groupIdMap[sourceGroup.id] = group.id;
        targetModel.groups.push_back(group);
        ++report.groupsAdded;
    }

    for (const auto& sourceTag : sourceTags) {
        const auto parent = groupIdMap.find(sourceTag.parentGroup);
        if (parent == groupIdMap.end()) {
            report.warnings.push_back(L"跳过缺少父分组的标签: " + sourceTag.name);
            continue;
        }
        const int existingTagId = FindMatchingGroup(targetModel, sourceTag, parent->second, legacyPackage);
        if (existingTagId > 0) {
            groupIdMap[sourceTag.id] = existingTagId;
            ++report.tagsMerged;
            continue;
        }

        Group tag = sourceTag;
        tag.id = 0;
        tag.parentGroup = parent->second;
        tag.pos = -1;
        if (!targetStorage.InsertGroup(tag)) {
            report.message = L"导入标签失败: " + targetStorage.lastError();
            return report;
        }
        groupIdMap[sourceTag.id] = tag.id;
        targetModel.groups.push_back(tag);
        ++report.tagsAdded;
    }

    for (auto sourceLink : sourceModel.links) {
        const auto parent = groupIdMap.find(sourceLink.parentGroup);
        if (parent == groupIdMap.end()) {
            report.warnings.push_back(L"跳过缺少父标签的启动项: " + sourceLink.name);
            continue;
        }
        if (IsDuplicateLinkInTag(targetModel, sourceLink, parent->second)) {
            ++report.linksSkippedDuplicate;
            continue;
        }
        sourceLink.id = 0;
        sourceLink.parentGroup = parent->second;
        sourceLink.pos = -1;
        if (!targetStorage.InsertLink(sourceLink)) {
            report.message = L"导入启动项失败: " + targetStorage.lastError();
            return report;
        }
        targetModel.links.push_back(sourceLink);
        ++report.linksAdded;
    }

    for (const auto& sourceNote : sourceModel.notes) {
        const auto tag = groupIdMap.find(sourceNote.tagId);
        if (tag == groupIdMap.end()) {
            continue;
        }
        const std::wstring existingContent = NoteContentForTag(targetModel, tag->second);
        const std::wstring mergedContent = MergeNoteContent(existingContent, sourceNote.content);
        if (mergedContent == existingContent) {
            continue;
        }
        if (!targetStorage.SaveNotePage(tag->second, mergedContent)) {
            report.message = L"导入便签失败: " + targetStorage.lastError();
            return report;
        }
        UpsertNoteContent(targetModel, tag->second, mergedContent);
        if (Trim(existingContent).empty()) {
            ++report.notesAdded;
        } else {
            ++report.notesMerged;
        }
    }

    SQLiteDatabase targetDb(appDirectory / L"db" / L"link.db");
    if (!targetDb.ok()) {
        report.message = L"打开当前数据库进行待办合并失败: " + targetDb.Error();
        return report;
    }
    std::wstring transactionError;
    if (!Exec(targetDb.get(), "BEGIN IMMEDIATE;", transactionError)) {
        report.message = L"开始待办合并事务失败: " + transactionError;
        return report;
    }
    auto rollback = [&]() {
        std::wstring ignored;
        Exec(targetDb.get(), "ROLLBACK;", ignored);
    };

    std::unordered_set<std::wstring> tombstones;
    for (const auto& item : targetModel.todoTombstones) tombstones.insert(item.todoUid);

    for (auto sourceTodo : sourceModel.todos) {
        const auto tag = groupIdMap.find(sourceTodo.tagId);
        if (tag == groupIdMap.end()) {
            ++report.todosFailed;
            report.message = L"待办“" + sourceTodo.title + L"”找不到对应标签，已回滚本次导入。";
            rollback();
            return report;
        }
        const int sourceId = sourceTodo.id;
        sourceTodo.tagId = tag->second;

        int localIndex = -1;
        for (std::size_t index = 0; index < targetModel.todos.size(); ++index) {
            if (!sourceTodo.todoUid.empty() && targetModel.todos[index].todoUid == sourceTodo.todoUid) {
                localIndex = static_cast<int>(index);
                break;
            }
        }

        if (localIndex < 0 && legacyPackage) {
            std::vector<int> candidates;
            for (std::size_t index = 0; index < targetModel.todos.size(); ++index) {
                const auto& local = targetModel.todos[index];
                if (local.tagId != sourceTodo.tagId || local.createdAt != sourceTodo.createdAt) continue;
                if (local.id == sourceId || NormalizedNameKey(local.title) == NormalizedNameKey(sourceTodo.title)) {
                    candidates.push_back(static_cast<int>(index));
                }
            }
            if (candidates.size() == 1) {
                localIndex = candidates.front();
            } else if (candidates.size() > 1) {
                ++report.todosConflicted;
                report.warnings.push_back(L"旧格式待办身份不唯一，已跳过: " + sourceTodo.title);
                continue;
            }
        }

        const bool wasDeleted = tombstones.find(sourceTodo.todoUid) != tombstones.end();
        if (localIndex < 0 && wasDeleted) {
            if (restorePolicy == TodoRestorePolicy::KeepDeleted) {
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
            if (!InsertMergedTodo(targetDb.get(), sourceTodo, transactionError)) {
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
            if (!InsertMergedTodo(targetDb.get(), sourceTodo, transactionError)) {
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
            sourceTodo.id = local.id;
            sourceTodo.todoUid = local.todoUid;
            sourceTodo.lastNotifiedDueAt = local.lastNotifiedDueAt;
            sourceTodo.lastNotifiedAt = local.lastNotifiedAt;
            sourceTodo.lastViewedDueAt = local.lastViewedDueAt;
            sourceTodo.lastViewedAt = local.lastViewedAt;
            if (!UpdateMergedTodo(targetDb.get(), sourceTodo, local.id, transactionError)) {
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

    for (const auto& remoteTombstone : sourceModel.todoTombstones) {
        const auto local = std::find_if(targetModel.todos.begin(), targetModel.todos.end(), [&](const TodoItem& item) {
            return item.todoUid == remoteTombstone.todoUid;
        });
        if (local != targetModel.todos.end()) {
            ++report.todosRemoteDeleteConflicts;
            report.warnings.push_back(L"远端已删除但本地仍保留待办: " + local->title);
        }
    }

    if (!Exec(targetDb.get(), "COMMIT;", transactionError)) {
        report.message = L"提交待办合并事务失败: " + transactionError;
        rollback();
        return report;
    }

    report.ok = true;
    report.message = L"合并导入完成。";
    return report;
}
}

ConfigPackageService::ConfigPackageService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

ConfigPackageReport ConfigPackageService::ExportPackage(const std::filesystem::path& targetPath, const ConfigPackageOptions& options) {
    ConfigPackageReport report;
    std::wstring error;

    if (options.includeData) {
        StorageService storage(appDirectory_);
        storage.Load();
        if (!storage.sqliteAvailable()) {
            report.message = L"导出前升级数据库失败: " + storage.lastError();
            return report;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(targetPath.parent_path(), ec);
    std::filesystem::remove(targetPath, ec);

    SQLiteDatabase packageDb(targetPath);
    if (!packageDb.ok()) {
        report.message = packageDb.Error();
        return report;
    }
    if (!EnsurePackageSchema(packageDb.get(), error)) {
        report.message = error;
        return report;
    }
    if (!PutManifest(packageDb.get(), L"app", L"Quattro", error) ||
        !PutManifest(packageDb.get(), L"formatVersion", std::to_wstring(kPackageFormatVersion), error)) {
        report.message = error;
        return report;
    }

    if (options.includeConfig) {
        if (!AddFileToPackage(packageDb.get(), appDirectory_ / L"conf.ini", L"conf.ini", L"config", error)) {
            report.message = error;
            return report;
        }
    }

    const std::filesystem::path tempDirectory = UniqueTempDirectory(L"quattro_export");
    const std::filesystem::path snapshotPath = tempDirectory / L"db" / L"link.db";
    if (options.includeData) {
        if (!BackupDatabase(appDirectory_ / L"db" / L"link.db", snapshotPath, error)) {
            report.message = error;
            return report;
        }
        if (!AddFileToPackage(packageDb.get(), snapshotPath, L"db/link.db", L"data", error)) {
            report.message = error;
            std::filesystem::remove_all(tempDirectory, ec);
            return report;
        }
    }
    std::filesystem::remove_all(tempDirectory, ec);

    if (options.includeUrlIcons) {
        const std::filesystem::path iconRoot = appDirectory_ / L"icons" / L"url";
        if (DirectoryExists(iconRoot)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(iconRoot, ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_regular_file()) {
                    continue;
                }
                const std::wstring extension = ToLower(entry.path().extension().wstring());
                if (extension != L".png" && extension != L".ico") {
                    continue;
                }
                const std::wstring packagePath = L"icons/url/" + PackagePathFor(iconRoot, entry.path());
                if (!AddFileToPackage(packageDb.get(), entry.path(), packagePath, L"urlIcon", error)) {
                    report.message = error;
                    return report;
                }
            }
        }
    }

    report.ok = true;
    report.message = L"配置包导出完成。";
    return report;
}

ConfigPackageReport ConfigPackageService::ImportPackageMerge(const std::filesystem::path& packagePath, const ConfigPackageOptions& options) {
    const ConfigPackageMergePreview preview = PreviewPackageMerge(packagePath, options);
    if (!preview.ok) {
        ConfigPackageReport report;
        report.message = preview.message;
        report.warnings = preview.warnings;
        return report;
    }
    return ApplyPackageMerge(packagePath, options, TodoRestorePolicy::KeepDeleted, preview.stateToken);
}

ConfigPackageMergePreview ConfigPackageService::PreviewPackageMerge(
    const std::filesystem::path& packagePath,
    const ConfigPackageOptions& options) {
    ConfigPackageMergePreview preview;
    SQLiteDatabase packageDb(packagePath);
    if (!packageDb.ok()) {
        preview.message = packageDb.Error();
        return preview;
    }
    const auto version = ParseInt(GetManifest(packageDb.get(), L"formatVersion"));
    if (GetManifest(packageDb.get(), L"app") != L"Quattro" ||
        !version.has_value() || (*version != 1 && *version != kPackageFormatVersion)) {
        preview.message = L"不是受支持的 Quattro快速启动器 配置包。";
        return preview;
    }
    preview.packageFormatVersion = *version;

    StorageService targetStorage(appDirectory_);
    const AppModel targetModel = targetStorage.Load();
    if (!targetStorage.sqliteAvailable()) {
        preview.message = L"当前数据库不可用: " + targetStorage.lastError();
        return preview;
    }
    preview.stateToken = MergeStateToken(packagePath, targetModel);

    if (!options.includeData) {
        preview.ok = true;
        preview.message = L"配置包预分析完成。";
        return preview;
    }

    const std::filesystem::path tempDirectory = UniqueTempDirectory(L"quattro_preview");
    std::wstring error;
    if (!ExtractPackageFile(packageDb.get(), L"db/link.db", tempDirectory / L"db" / L"link.db", error)) {
        preview.message = error;
        std::error_code ec;
        std::filesystem::remove_all(tempDirectory, ec);
        return preview;
    }
    StorageService sourceStorage(tempDirectory);
    AppModel sourceModel = sourceStorage.Load();
    if (!sourceStorage.sqliteAvailable()) {
        preview.message = L"导入包数据库不可用: " + sourceStorage.lastError();
        std::error_code ec;
        std::filesystem::remove_all(tempDirectory, ec);
        return preview;
    }
    const std::wstring packageHash = ContentHash(ReadBinaryFile(packagePath));
    if (*version == 1) {
        NormalizeLegacySourceIdentities(sourceModel, packageHash);
        preview.warnings.push_back(L"这是旧格式备份；只能保证同一备份文件重复导入时保持幂等。不同旧快照之间无法完全确认待办身份。");
    }
    std::unordered_set<std::wstring> tombstones;
    for (const auto& item : targetModel.todoTombstones) tombstones.insert(item.todoUid);
    for (const auto& item : sourceModel.todos) {
        if (tombstones.find(item.todoUid) != tombstones.end()) {
            preview.deletedTodoTitles.push_back(item.title);
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(tempDirectory, ec);
    preview.ok = true;
    preview.message = L"配置包预分析完成。";
    return preview;
}

ConfigPackageReport ConfigPackageService::ApplyPackageMerge(
    const std::filesystem::path& packagePath,
    const ConfigPackageOptions& options,
    TodoRestorePolicy restorePolicy,
    const std::wstring& expectedStateToken) {
    ConfigPackageReport report;
    const ConfigPackageMergePreview preview = PreviewPackageMerge(packagePath, options);
    if (!preview.ok) {
        report.message = preview.message;
        report.warnings = preview.warnings;
        return report;
    }
    if (!expectedStateToken.empty() && preview.stateToken != expectedStateToken) {
        report.message = L"本地数据在确认后发生变化，请重新预览并确认本次合并。";
        return report;
    }

    SQLiteDatabase packageDb(packagePath);
    if (!packageDb.ok()) {
        report.message = packageDb.Error();
        return report;
    }

    const std::filesystem::path backupDirectory = CreateSafetyBackup(appDirectory_, report);

    const std::filesystem::path tempDirectory = UniqueTempDirectory(L"quattro_import");
    std::wstring error;
    if (options.includeData) {
        if (!ExtractPackageFile(packageDb.get(), L"db/link.db", tempDirectory / L"db" / L"link.db", error)) {
            report.message = error;
            std::error_code ec;
            std::filesystem::remove_all(tempDirectory, ec);
            return report;
        }
        std::vector<std::wstring> backupWarnings = report.warnings;
        const std::wstring packageHash = ContentHash(ReadBinaryFile(packagePath));
        report = MergeImportedData(
            appDirectory_, tempDirectory, preview.packageFormatVersion, packageHash, restorePolicy);
        report.warnings.insert(report.warnings.begin(), backupWarnings.begin(), backupWarnings.end());
        report.warnings.insert(report.warnings.begin(), preview.warnings.begin(), preview.warnings.end());
        if (!report.ok) {
            RestoreSafetyBackup(appDirectory_, backupDirectory, report);
        }
    } else {
        report.ok = true;
    }

    if (report.ok && options.includeUrlIcons) {
        MergeUrlIcons(packageDb.get(), appDirectory_, report);
    }

    std::error_code ec;
    std::filesystem::remove_all(tempDirectory, ec);
    return report;
}
