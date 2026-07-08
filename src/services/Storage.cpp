#include "Storage.h"

#include "AppLog.h"
#include "TodoSchedule.h"
#include "Utilities.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {
constexpr int kSchemaVersion = 20000;

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
    void bindText(int index, const std::wstring& value) {
        sqlite3_bind_text16(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bindBlob(int index, const std::vector<std::uint8_t>& value) {
        if (value.empty()) {
            sqlite3_bind_null(stmt_, index);
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
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
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

void CreateSchema(sqlite3* db, std::wstring& error) {
    Exec(db,
         "CREATE TABLE IF NOT EXISTS Version(ID INTEGER PRIMARY KEY, Ver INTEGER);"
         "CREATE TABLE IF NOT EXISTS Groups("
         "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
         "NAME CHARACTER(16) NOT NULL,"
         "TYPE INTEGER DEFAULT 0,"
         "SORT INTEGER DEFAULT 0,"
         "POS INTEGER DEFAULT 0,"
         "ParentGroup INTEGER DEFAULT 0,"
         "ICON TEXT,"
         "LAYOUT INTEGER DEFAULT 0,"
         "ICONSIZE INTEGER DEFAULT 0,"
         "FLAG INTEGER DEFAULT 0,"
         "Content TEXT);"
         "CREATE TABLE IF NOT EXISTS Links("
         "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
         "NAME TEXT NOT NULL,"
         "POS INTEGER DEFAULT 0,"
         "RunCount INTEGER DEFAULT 0,"
         "ParentGroup INTEGER DEFAULT 0,"
         "TYPE INTEGER DEFAULT 0,"
         "ICON TEXT NOT NULL DEFAULT '',"
         "PATH TEXT NOT NULL DEFAULT '',"
         "Parameter TEXT NOT NULL DEFAULT '',"
         "WorkDir TEXT NOT NULL DEFAULT '',"
         "HotKey INTEGER DEFAULT 0,"
         "ShowCmd INTEGER DEFAULT 0,"
         "IsAdmin INTEGER DEFAULT 0,"
         "IsCustomColor INTEGER DEFAULT 0,"
         "CustomColor CHARACTER(10),"
         "Remark TEXT,"
         "Pidl BLOB);"
         "CREATE TABLE IF NOT EXISTS NotePages("
         "TagId INTEGER PRIMARY KEY,"
         "Content TEXT NOT NULL DEFAULT '',"
         "UpdatedAt TEXT NOT NULL DEFAULT '');"
         "CREATE TABLE IF NOT EXISTS TodoItems("
         "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
         "TagId INTEGER NOT NULL,"
         "Title TEXT NOT NULL,"
         "Content TEXT NOT NULL DEFAULT '',"
         "Enabled INTEGER DEFAULT 1,"
         "ScheduleKind INTEGER DEFAULT 0,"
         "RepeatMode INTEGER DEFAULT 0,"
         "RepeatInterval INTEGER DEFAULT 1,"
         "RepeatLimit INTEGER DEFAULT 0,"
         "RepeatFinished INTEGER DEFAULT 0,"
         "CronExpression TEXT NOT NULL DEFAULT '',"
         "AnchorAt TEXT NOT NULL DEFAULT '',"
         "NextDueAt TEXT NOT NULL DEFAULT '',"
         "CompletedAt TEXT NOT NULL DEFAULT '',"
         "POS INTEGER DEFAULT 0,"
         "CreatedAt TEXT NOT NULL DEFAULT '',"
         "UpdatedAt TEXT NOT NULL DEFAULT '');",
         error);
}

bool HasColumn(sqlite3* db, const wchar_t* table, const wchar_t* column) {
    std::wstring sql = L"PRAGMA table_info(";
    sql += table;
    sql += L");";
    SQLiteStatement statement(db, sql.c_str());
    if (!statement.ok()) {
        return false;
    }
    while (statement.step() == SQLITE_ROW) {
        if (ToLower(statement.columnText(1)) == ToLower(column)) {
            return true;
        }
    }
    return false;
}

bool HasTable(sqlite3* db, const wchar_t* table) {
    SQLiteStatement statement(db, L"SELECT name FROM sqlite_master WHERE type='table' AND name=?;");
    if (!statement.ok()) {
        return false;
    }
    statement.bindText(1, table);
    return statement.step() == SQLITE_ROW;
}

int SchemaVersion(sqlite3* db) {
    if (!HasTable(db, L"Version")) {
        return 0;
    }
    SQLiteStatement statement(db, L"SELECT Ver FROM Version WHERE ID=1;");
    if (!statement.ok() || statement.step() != SQLITE_ROW) {
        return 0;
    }
    return statement.columnInt(0);
}

bool NeedsSchemaMigration(sqlite3* db) {
    if (SchemaVersion(db) < kSchemaVersion) {
        return true;
    }
    return !HasTable(db, L"Groups") ||
           !HasTable(db, L"Links") ||
           !HasTable(db, L"NotePages") ||
           !HasTable(db, L"TodoItems") ||
           !HasColumn(db, L"Links", L"Pidl") ||
           !HasColumn(db, L"TodoItems", L"Enabled") ||
           !HasColumn(db, L"TodoItems", L"RepeatInterval") ||
           !HasColumn(db, L"TodoItems", L"RepeatMode") ||
           !HasColumn(db, L"TodoItems", L"RepeatLimit") ||
           !HasColumn(db, L"TodoItems", L"RepeatFinished") ||
           !HasColumn(db, L"TodoItems", L"CronExpression");
}

std::wstring MigrationTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(
        buffer,
        L"%04u%02u%02u-%02u%02u%02u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
    return buffer;
}

bool BackupDatabaseBeforeMigration(const std::filesystem::path& appDirectory, const std::filesystem::path& dbPath, std::wstring& error) {
    const std::filesystem::path backupPath = appDirectory / L"backups" / L"db" / MigrationTimestamp() / L"link.db";
    std::error_code ec;
    std::filesystem::create_directories(backupPath.parent_path(), ec);
    if (ec) {
        error = L"创建数据库迁移备份目录失败: " + Utf8ToWide(ec.message().c_str());
        return false;
    }
    std::filesystem::copy_file(dbPath, backupPath, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = L"备份数据库失败: " + Utf8ToWide(ec.message().c_str());
        return false;
    }
    WriteAppLog(L"数据库迁移前已备份: " + backupPath.wstring());
    return true;
}

void MigrateSchema(sqlite3* db, std::wstring& error) {
    if (!HasColumn(db, L"Links", L"Pidl")) {
        Exec(db, "ALTER TABLE Links ADD COLUMN Pidl BLOB;", error);
    }
    Exec(db,
         "CREATE TABLE IF NOT EXISTS NotePages("
         "TagId INTEGER PRIMARY KEY,"
         "Content TEXT NOT NULL DEFAULT '',"
         "UpdatedAt TEXT NOT NULL DEFAULT '');"
         "CREATE TABLE IF NOT EXISTS TodoItems("
         "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
         "TagId INTEGER NOT NULL,"
         "Title TEXT NOT NULL,"
         "Content TEXT NOT NULL DEFAULT '',"
         "Enabled INTEGER DEFAULT 1,"
         "ScheduleKind INTEGER DEFAULT 0,"
         "RepeatMode INTEGER DEFAULT 0,"
         "RepeatInterval INTEGER DEFAULT 1,"
         "RepeatLimit INTEGER DEFAULT 0,"
         "RepeatFinished INTEGER DEFAULT 0,"
         "CronExpression TEXT NOT NULL DEFAULT '',"
         "AnchorAt TEXT NOT NULL DEFAULT '',"
         "NextDueAt TEXT NOT NULL DEFAULT '',"
         "CompletedAt TEXT NOT NULL DEFAULT '',"
         "POS INTEGER DEFAULT 0,"
         "CreatedAt TEXT NOT NULL DEFAULT '',"
         "UpdatedAt TEXT NOT NULL DEFAULT '');",
         error);
    if (!HasColumn(db, L"TodoItems", L"Enabled")) {
        Exec(db, "ALTER TABLE TodoItems ADD COLUMN Enabled INTEGER DEFAULT 1;", error);
    }
    if (!HasColumn(db, L"TodoItems", L"RepeatInterval")) {
        Exec(db, "ALTER TABLE TodoItems ADD COLUMN RepeatInterval INTEGER DEFAULT 1;", error);
    }
    if (!HasColumn(db, L"TodoItems", L"RepeatMode")) {
        Exec(db, "ALTER TABLE TodoItems ADD COLUMN RepeatMode INTEGER DEFAULT 0;", error);
        Exec(db, "UPDATE TodoItems SET RepeatMode=1 WHERE ScheduleKind IN (6,7,8);", error);
    }
    if (!HasColumn(db, L"TodoItems", L"RepeatLimit")) {
        Exec(db, "ALTER TABLE TodoItems ADD COLUMN RepeatLimit INTEGER DEFAULT 0;", error);
    }
    if (!HasColumn(db, L"TodoItems", L"RepeatFinished")) {
        Exec(db, "ALTER TABLE TodoItems ADD COLUMN RepeatFinished INTEGER DEFAULT 0;", error);
    }
    if (!HasColumn(db, L"TodoItems", L"CronExpression")) {
        Exec(db, "ALTER TABLE TodoItems ADD COLUMN CronExpression TEXT NOT NULL DEFAULT '';", error);
    }
}

int CountRows(sqlite3* db, const wchar_t* sql) {
    SQLiteStatement statement(db, sql);
    if (!statement.ok() || statement.step() != SQLITE_ROW) {
        return 0;
    }
    return statement.columnInt(0);
}

int NextLinkPosition(sqlite3* db, int parentGroup) {
    SQLiteStatement statement(db, L"SELECT COALESCE(MAX(POS), -1) + 1 FROM Links WHERE ParentGroup=?;");
    if (!statement.ok()) {
        return 0;
    }
    statement.bindInt(1, parentGroup);
    if (statement.step() != SQLITE_ROW) {
        return 0;
    }
    return statement.columnInt(0);
}

int NextGroupPosition(sqlite3* db, int parentGroup) {
    SQLiteStatement statement(db, L"SELECT COALESCE(MAX(POS), -1) + 1 FROM Groups WHERE ParentGroup=?;");
    if (!statement.ok()) {
        return 0;
    }
    statement.bindInt(1, parentGroup);
    if (statement.step() != SQLITE_ROW) {
        return 0;
    }
    return statement.columnInt(0);
}

int NextTodoPosition(sqlite3* db, int tagId) {
    SQLiteStatement statement(db, L"SELECT COALESCE(MAX(POS), -1) + 1 FROM TodoItems WHERE TagId=?;");
    if (!statement.ok()) {
        return 0;
    }
    statement.bindInt(1, tagId);
    if (statement.step() != SQLITE_ROW) {
        return 0;
    }
    return statement.columnInt(0);
}

bool NormalizeLinkForSave(sqlite3* db, Link& link) {
    link.name = Trim(link.name);
    link.path = Trim(link.path);
    link.parameter = Trim(link.parameter);
    link.workDir = Trim(link.workDir);
    link.remark = Trim(link.remark);
    if (link.name.empty() || link.path.empty()) {
        return false;
    }
    if (link.parentGroup <= 0) {
        SQLiteStatement tag(db, L"SELECT ID FROM Groups WHERE ParentGroup != 0 ORDER BY ParentGroup,POS,ID LIMIT 1;");
        if (tag.ok() && tag.step() == SQLITE_ROW) {
            link.parentGroup = tag.columnInt(0);
        }
    }
    if (link.parentGroup <= 0) {
        return false;
    }
    if (link.pos < 0) {
        link.pos = NextLinkPosition(db, link.parentGroup);
    }
    if (link.showCmd <= 0) {
        link.showCmd = SW_SHOWNORMAL;
    }
    return true;
}

bool NormalizeGroupForSave(sqlite3* db, Group& group) {
    group.name = Trim(group.name);
    group.icon = Trim(group.icon);
    group.content = Trim(group.content);
    if (group.name.empty()) {
        return false;
    }
    if (group.parentGroup < 0) {
        group.parentGroup = 0;
    }
    if (group.pos < 0) {
        group.pos = NextGroupPosition(db, group.parentGroup);
    }
    return true;
}

bool NormalizeTodoForSave(sqlite3* db, TodoItem& item, bool isNew) {
    item.title = Trim(item.title);
    item.content = Trim(item.content);
    item.anchorAt = NormalizeTodoTimestamp(item.anchorAt);
    item.nextDueAt = NormalizeTodoTimestamp(item.nextDueAt);
    item.completedAt = NormalizeTodoTimestamp(item.completedAt);
    if (item.title.empty() || item.tagId <= 0) {
        return false;
    }
    if (static_cast<int>(item.scheduleKind) < static_cast<int>(TodoScheduleKind::None) ||
        static_cast<int>(item.scheduleKind) > static_cast<int>(TodoScheduleKind::Cron)) {
        item.scheduleKind = TodoScheduleKind::None;
    }
    item.repeatInterval = std::max(1, item.repeatInterval);
    item.repeatLimit = std::max(0, item.repeatLimit);
    item.repeatFinished = std::max(0, item.repeatFinished);
    item.cronExpression = NormalizeTodoCronExpression(item.cronExpression);
    if (static_cast<int>(item.repeatMode) < static_cast<int>(TodoRepeatMode::FixedPoint) ||
        static_cast<int>(item.repeatMode) > static_cast<int>(TodoRepeatMode::Interval)) {
        item.repeatMode = TodoRepeatMode::FixedPoint;
    }
    if (item.scheduleKind == TodoScheduleKind::None) {
        item.anchorAt.clear();
        item.nextDueAt.clear();
        item.cronExpression.clear();
        item.repeatFinished = 0;
    } else if (item.scheduleKind == TodoScheduleKind::Cron) {
        item.anchorAt.clear();
        if (!IsValidTodoCronExpression(item.cronExpression)) {
            return false;
        }
        if (item.nextDueAt.empty()) {
            item.nextDueAt = ComputeNextTodoDueAt(item, CurrentTodoTimestamp());
        }
    } else if (item.anchorAt.empty()) {
        return false;
    } else if (item.nextDueAt.empty()) {
        item.cronExpression.clear();
        item.nextDueAt = ComputeNextTodoDueAt(item, CurrentTodoTimestamp());
    }
    if (item.pos < 0) {
        item.pos = NextTodoPosition(db, item.tagId);
    }
    const std::wstring now = CurrentTodoTimestamp();
    if (isNew && item.createdAt.empty()) {
        item.createdAt = now;
    }
    if (item.updatedAt.empty()) {
        item.updatedAt = now;
    }
    return true;
}
}

StorageService::StorageService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

AppModel StorageService::Load() {
    lastError_.clear();
    WriteStartupTiming(L"storage load begin");

    const std::filesystem::path dbDirectory = appDirectory_ / L"db";
    std::error_code ec;
    std::filesystem::create_directories(dbDirectory, ec);
    WriteStartupTiming(
        L"storage db directory ready",
        L"path=" + dbDirectory.wstring() + L", error=" + (ec ? Utf8ToWide(ec.message().c_str()) : L""));

    const std::filesystem::path dbPath = dbDirectory / L"link.db";
    const bool newDatabase = !std::filesystem::exists(dbPath);
    SQLiteDatabase db(dbPath);
    if (!db.ok()) {
        lastError_ = db.Error();
        sqliteAvailable_ = false;
        WriteStartupTiming(L"storage sqlite open failed", lastError_);
        return LoadFallback();
    }
    sqliteAvailable_ = true;
    WriteStartupTiming(
        L"storage sqlite opened",
        L"path=" + dbPath.wstring() + L", new=" + std::wstring(newDatabase ? L"1" : L"0"));

    const bool needsSchemaMigration = !newDatabase && NeedsSchemaMigration(db.get());
    WriteStartupTiming(L"storage schema migration checked", L"needed=" + std::wstring(needsSchemaMigration ? L"1" : L"0"));
    if (needsSchemaMigration && !BackupDatabaseBeforeMigration(appDirectory_, dbPath, lastError_)) {
        sqliteAvailable_ = false;
        WriteStartupTiming(L"storage schema migration backup failed", lastError_);
        return LoadFallback();
    }

    bool startupSchemaTransaction = false;
    if (newDatabase || needsSchemaMigration) {
        startupSchemaTransaction = Exec(db.get(), "BEGIN IMMEDIATE;", lastError_);
        WriteStartupTiming(
            L"storage schema transaction begin",
            L"ok=" + std::wstring(startupSchemaTransaction ? L"1" : L"0") +
                L", error=" + lastError_);
    }

    CreateSchema(db.get(), lastError_);
    WriteStartupTiming(L"storage schema ensured", lastError_);
    MigrateSchema(db.get(), lastError_);
    WriteStartupTiming(L"storage schema migrated", lastError_);

    const int versionRows = CountRows(db.get(), L"SELECT COUNT(*) FROM Version;");
    const int currentSchemaVersion = SchemaVersion(db.get());
    WriteStartupTiming(
        L"storage version rows counted",
        L"rows=" + std::to_wstring(versionRows) +
            L", version=" + std::to_wstring(currentSchemaVersion));
    if (versionRows == 0) {
        Exec(db.get(), ("INSERT INTO Version(ID,Ver) VALUES(1," + std::to_string(kSchemaVersion) + ");").c_str(), lastError_);
        WriteStartupTiming(L"storage version row inserted", lastError_);
    } else if (currentSchemaVersion != kSchemaVersion) {
        Exec(db.get(), ("UPDATE Version SET Ver=" + std::to_string(kSchemaVersion) + " WHERE ID=1;").c_str(), lastError_);
        WriteStartupTiming(L"storage version row updated", lastError_);
    }

    const int groupRows = CountRows(db.get(), L"SELECT COUNT(*) FROM Groups;");
    WriteStartupTiming(L"storage group rows counted", L"rows=" + std::to_wstring(groupRows));
    if (groupRows == 0) {
        Exec(db.get(),
             "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT,SORT,TYPE) VALUES(1,'新的分组',0,0,0,0,0,0);"
             "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT,SORT,TYPE) VALUES(2,'新的标签',0,1,32,1,0,0);",
             lastError_);
        WriteStartupTiming(L"storage default groups inserted", lastError_);
    }

    if (startupSchemaTransaction) {
        if (lastError_.empty()) {
            Exec(db.get(), "COMMIT;", lastError_);
            WriteStartupTiming(L"storage schema transaction committed", lastError_);
        } else {
            Exec(db.get(), "ROLLBACK;", lastError_);
            WriteStartupTiming(L"storage schema transaction rolled back", lastError_);
        }
    }

    AppModel model;
    {
        SQLiteStatement statement(db.get(),
            L"SELECT ID,NAME,ParentGroup,ICON,LAYOUT,ICONSIZE,POS,TYPE,SORT,Content,FLAG "
            L"FROM Groups ORDER BY ParentGroup,POS,ID;");
        if (statement.ok()) {
            while (statement.step() == SQLITE_ROW) {
                Group group;
                group.id = statement.columnInt(0);
                group.name = statement.columnText(1);
                group.parentGroup = statement.columnInt(2);
                group.icon = statement.columnText(3);
                group.layout = statement.columnInt(4);
                group.iconSize = statement.columnInt(5);
                group.pos = statement.columnInt(6);
                group.type = statement.columnInt(7);
                group.sort = statement.columnInt(8);
                group.content = statement.columnText(9);
                group.flag = statement.columnInt(10);
                model.groups.push_back(std::move(group));
            }
        }
    }
    WriteStartupTiming(L"storage groups loaded", L"count=" + std::to_wstring(model.groups.size()));

    {
        SQLiteStatement statement(db.get(),
            L"SELECT ID,NAME,ParentGroup,TYPE,POS,ICON,PATH,Parameter,WorkDir,"
            L"IsAdmin,IsCustomColor,CustomColor,Remark,RunCount,HotKey,ShowCmd,Pidl "
            L"FROM Links ORDER BY POS,ID;");
        if (statement.ok()) {
            while (statement.step() == SQLITE_ROW) {
                Link link;
                link.id = statement.columnInt(0);
                link.name = statement.columnText(1);
                link.parentGroup = statement.columnInt(2);
                link.type = statement.columnInt(3);
                link.pos = statement.columnInt(4);
                link.icon = statement.columnText(5);
                link.path = statement.columnText(6);
                link.parameter = statement.columnText(7);
                link.workDir = statement.columnText(8);
                link.isAdmin = statement.columnInt(9) != 0;
                link.isCustomColor = statement.columnInt(10) != 0;
                link.customColor = statement.columnText(11);
                link.remark = statement.columnText(12);
                link.runCount = statement.columnInt(13);
                link.hotKey = statement.columnInt(14);
                link.showCmd = statement.columnInt(15);
                link.pidl = statement.columnBlob(16);
                model.links.push_back(std::move(link));
            }
        }
    }
    WriteStartupTiming(L"storage links loaded", L"count=" + std::to_wstring(model.links.size()));

    {
        SQLiteStatement statement(db.get(), L"SELECT TagId,Content,UpdatedAt FROM NotePages;");
        if (statement.ok()) {
            while (statement.step() == SQLITE_ROW) {
                NotePage note;
                note.tagId = statement.columnInt(0);
                note.content = statement.columnText(1);
                note.updatedAt = statement.columnText(2);
                model.notes.push_back(std::move(note));
            }
        }
    }
    WriteStartupTiming(L"storage notes loaded", L"count=" + std::to_wstring(model.notes.size()));

    {
        SQLiteStatement statement(db.get(),
            L"SELECT ID,TagId,Title,Content,Enabled,ScheduleKind,RepeatMode,RepeatInterval,RepeatLimit,RepeatFinished,CronExpression,AnchorAt,NextDueAt,CompletedAt,POS,CreatedAt,UpdatedAt "
            L"FROM TodoItems ORDER BY TagId,CompletedAt,POS,ID;");
        if (statement.ok()) {
            while (statement.step() == SQLITE_ROW) {
                TodoItem item;
                item.id = statement.columnInt(0);
                item.tagId = statement.columnInt(1);
                item.title = statement.columnText(2);
                item.content = statement.columnText(3);
                item.enabled = statement.columnInt(4) != 0;
                item.scheduleKind = static_cast<TodoScheduleKind>(statement.columnInt(5));
                item.repeatMode = static_cast<TodoRepeatMode>(statement.columnInt(6));
                item.repeatInterval = std::max(1, statement.columnInt(7));
                item.repeatLimit = std::max(0, statement.columnInt(8));
                item.repeatFinished = std::max(0, statement.columnInt(9));
                item.cronExpression = statement.columnText(10);
                item.anchorAt = statement.columnText(11);
                item.nextDueAt = statement.columnText(12);
                item.completedAt = statement.columnText(13);
                item.pos = statement.columnInt(14);
                item.createdAt = statement.columnText(15);
                item.updatedAt = statement.columnText(16);
                model.todos.push_back(std::move(item));
            }
        }
    }
    WriteStartupTiming(L"storage todos loaded", L"count=" + std::to_wstring(model.todos.size()));

    EnsureDefaultData(model);
    WriteStartupTiming(
        L"storage default data normalized",
        L"groups=" + std::to_wstring(model.groups.size()) +
            L", links=" + std::to_wstring(model.links.size()) +
            L", notes=" + std::to_wstring(model.notes.size()) +
            L", todos=" + std::to_wstring(model.todos.size()));
    return model;
}

bool StorageService::InsertGroup(Group& group) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    group.pos = NextGroupPosition(db.get(), group.parentGroup);
    if (!NormalizeGroupForSave(db.get(), group)) {
        lastError_ = L"名称不能为空。";
        return false;
    }

    SQLiteStatement statement(db.get(),
        L"INSERT INTO Groups(NAME,TYPE,SORT,POS,ParentGroup,ICON,LAYOUT,ICONSIZE,FLAG,Content) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        lastError_ = L"新增分组 SQL 准备失败。";
        return false;
    }
    statement.bindText(1, group.name);
    statement.bindInt(2, group.type);
    statement.bindInt(3, group.sort);
    statement.bindInt(4, group.pos);
    statement.bindInt(5, group.parentGroup);
    statement.bindText(6, group.icon);
    statement.bindInt(7, group.layout);
    statement.bindInt(8, group.iconSize);
    statement.bindInt(9, group.flag);
    statement.bindText(10, group.content);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"新增分组失败。";
        return false;
    }
    group.id = static_cast<int>(sqlite3_last_insert_rowid(db.get()));
    return true;
}

bool StorageService::UpdateGroup(const Group& source) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }

    Group group = source;
    if (!NormalizeGroupForSave(db.get(), group)) {
        lastError_ = L"名称不能为空。";
        return false;
    }

    SQLiteStatement statement(db.get(),
        L"UPDATE Groups SET NAME=?,TYPE=?,SORT=?,POS=?,ParentGroup=?,ICON=?,LAYOUT=?,ICONSIZE=?,FLAG=?,Content=? WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新分组 SQL 准备失败。";
        return false;
    }
    statement.bindText(1, group.name);
    statement.bindInt(2, group.type);
    statement.bindInt(3, group.sort);
    statement.bindInt(4, group.pos);
    statement.bindInt(5, group.parentGroup);
    statement.bindText(6, group.icon);
    statement.bindInt(7, group.layout);
    statement.bindInt(8, group.iconSize);
    statement.bindInt(9, group.flag);
    statement.bindText(10, group.content);
    statement.bindInt(11, group.id);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"更新分组失败。";
        return false;
    }
    return sqlite3_changes(db.get()) > 0;
}

bool StorageService::DeleteGroup(int groupId) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }

    SQLiteStatement typeQuery(db.get(), L"SELECT ParentGroup FROM Groups WHERE ID=?;");
    if (!typeQuery.ok()) {
        lastError_ = L"查询分组失败。";
        return false;
    }
    typeQuery.bindInt(1, groupId);
    if (typeQuery.step() != SQLITE_ROW) {
        lastError_ = L"分组不存在。";
        return false;
    }
    const int parentGroup = typeQuery.columnInt(0);

    if (!Exec(db.get(), "BEGIN TRANSACTION;", lastError_)) {
        return false;
    }

    bool ok = true;
    if (parentGroup == 0) {
        SQLiteStatement deleteTodos(db.get(),
            L"DELETE FROM TodoItems WHERE TagId IN (SELECT ID FROM Groups WHERE ParentGroup=?);");
        deleteTodos.bindInt(1, groupId);
        ok = ok && deleteTodos.step() == SQLITE_DONE;

        SQLiteStatement deleteNotes(db.get(),
            L"DELETE FROM NotePages WHERE TagId IN (SELECT ID FROM Groups WHERE ParentGroup=?);");
        deleteNotes.bindInt(1, groupId);
        ok = ok && deleteNotes.step() == SQLITE_DONE;

        SQLiteStatement deleteLinks(db.get(),
            L"DELETE FROM Links WHERE ParentGroup IN (SELECT ID FROM Groups WHERE ParentGroup=?);");
        deleteLinks.bindInt(1, groupId);
        ok = ok && deleteLinks.step() == SQLITE_DONE;

        SQLiteStatement deleteTags(db.get(), L"DELETE FROM Groups WHERE ParentGroup=?;");
        deleteTags.bindInt(1, groupId);
        ok = ok && deleteTags.step() == SQLITE_DONE;
    } else {
        SQLiteStatement deleteTodos(db.get(), L"DELETE FROM TodoItems WHERE TagId=?;");
        deleteTodos.bindInt(1, groupId);
        ok = ok && deleteTodos.step() == SQLITE_DONE;

        SQLiteStatement deleteNotes(db.get(), L"DELETE FROM NotePages WHERE TagId=?;");
        deleteNotes.bindInt(1, groupId);
        ok = ok && deleteNotes.step() == SQLITE_DONE;

        SQLiteStatement deleteLinks(db.get(), L"DELETE FROM Links WHERE ParentGroup=?;");
        deleteLinks.bindInt(1, groupId);
        ok = ok && deleteLinks.step() == SQLITE_DONE;
    }

    SQLiteStatement deleteGroup(db.get(), L"DELETE FROM Groups WHERE ID=?;");
    deleteGroup.bindInt(1, groupId);
    ok = ok && deleteGroup.step() == SQLITE_DONE;

    if (!ok) {
        Exec(db.get(), "ROLLBACK;", lastError_);
        if (lastError_.empty()) {
            lastError_ = L"删除分组失败。";
        }
        return false;
    }
    return Exec(db.get(), "COMMIT;", lastError_);
}

bool StorageService::InsertLink(Link& link) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    link.pos = NextLinkPosition(db.get(), link.parentGroup);
    if (!NormalizeLinkForSave(db.get(), link)) {
        lastError_ = L"名称和路径不能为空。";
        return false;
    }

    SQLiteStatement statement(db.get(),
        L"INSERT INTO Links(NAME,ParentGroup,TYPE,POS,ICON,PATH,Parameter,WorkDir,"
        L"IsAdmin,IsCustomColor,CustomColor,Remark,RunCount,HotKey,ShowCmd,Pidl) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        lastError_ = L"新增启动项 SQL 准备失败。";
        return false;
    }
    statement.bindText(1, link.name);
    statement.bindInt(2, link.parentGroup);
    statement.bindInt(3, link.type);
    statement.bindInt(4, link.pos);
    statement.bindText(5, link.icon);
    statement.bindText(6, link.path);
    statement.bindText(7, link.parameter);
    statement.bindText(8, link.workDir);
    statement.bindInt(9, link.isAdmin ? 1 : 0);
    statement.bindInt(10, link.isCustomColor ? 1 : 0);
    statement.bindText(11, link.customColor);
    statement.bindText(12, link.remark);
    statement.bindInt(13, link.runCount);
    statement.bindInt(14, link.hotKey);
    statement.bindInt(15, link.showCmd);
    statement.bindBlob(16, link.pidl);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"新增启动项失败。";
        return false;
    }
    link.id = static_cast<int>(sqlite3_last_insert_rowid(db.get()));
    return true;
}

bool StorageService::UpdateLink(const Link& source) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    Link link = source;
    if (!NormalizeLinkForSave(db.get(), link)) {
        lastError_ = L"名称和路径不能为空。";
        return false;
    }

    SQLiteStatement statement(db.get(),
        L"UPDATE Links SET NAME=?,ParentGroup=?,TYPE=?,POS=?,ICON=?,PATH=?,Parameter=?,WorkDir=?,"
        L"IsAdmin=?,IsCustomColor=?,CustomColor=?,Remark=?,RunCount=?,HotKey=?,ShowCmd=?,Pidl=? WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新启动项 SQL 准备失败。";
        return false;
    }
    statement.bindText(1, link.name);
    statement.bindInt(2, link.parentGroup);
    statement.bindInt(3, link.type);
    statement.bindInt(4, link.pos);
    statement.bindText(5, link.icon);
    statement.bindText(6, link.path);
    statement.bindText(7, link.parameter);
    statement.bindText(8, link.workDir);
    statement.bindInt(9, link.isAdmin ? 1 : 0);
    statement.bindInt(10, link.isCustomColor ? 1 : 0);
    statement.bindText(11, link.customColor);
    statement.bindText(12, link.remark);
    statement.bindInt(13, link.runCount);
    statement.bindInt(14, link.hotKey);
    statement.bindInt(15, link.showCmd);
    statement.bindBlob(16, link.pidl);
    statement.bindInt(17, link.id);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"更新启动项失败。";
        return false;
    }
    return sqlite3_changes(db.get()) > 0;
}

bool StorageService::DeleteLink(int linkId) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }

    SQLiteStatement statement(db.get(), L"DELETE FROM Links WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"删除启动项 SQL 准备失败。";
        return false;
    }
    statement.bindInt(1, linkId);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"删除启动项失败。";
        return false;
    }
    return sqlite3_changes(db.get()) > 0;
}

bool StorageService::IncrementRunCount(int linkId, int runCount) {
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }

    SQLiteStatement statement(db.get(), L"UPDATE Links SET RunCount=? WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新运行次数 SQL 准备失败。";
        return false;
    }
    statement.bindInt(1, runCount);
    statement.bindInt(2, linkId);
    return statement.step() == SQLITE_DONE;
}

bool StorageService::SaveNotePage(int tagId, const std::wstring& content) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    SQLiteStatement statement(db.get(),
        L"INSERT INTO NotePages(TagId,Content,UpdatedAt) VALUES(?,?,?) "
        L"ON CONFLICT(TagId) DO UPDATE SET Content=excluded.Content,UpdatedAt=excluded.UpdatedAt;");
    if (!statement.ok()) {
        lastError_ = L"保存便签 SQL 准备失败。";
        return false;
    }
    statement.bindInt(1, tagId);
    statement.bindText(2, content);
    statement.bindText(3, CurrentTodoTimestamp());
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"保存便签失败。";
        return false;
    }
    return true;
}

bool StorageService::InsertTodoItem(TodoItem& item) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    item.pos = NextTodoPosition(db.get(), item.tagId);
    if (!NormalizeTodoForSave(db.get(), item, true)) {
        lastError_ = L"待办事项标题不能为空，且有时间的事项必须填写有效时间。";
        return false;
    }

    SQLiteStatement statement(db.get(),
        L"INSERT INTO TodoItems(TagId,Title,Content,Enabled,ScheduleKind,RepeatMode,RepeatInterval,RepeatLimit,RepeatFinished,CronExpression,AnchorAt,NextDueAt,CompletedAt,POS,CreatedAt,UpdatedAt) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        lastError_ = L"新增待办事项 SQL 准备失败。";
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
    statement.bindInt(14, item.pos);
    statement.bindText(15, item.createdAt);
    statement.bindText(16, item.updatedAt);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"新增待办事项失败。";
        return false;
    }
    item.id = static_cast<int>(sqlite3_last_insert_rowid(db.get()));
    return true;
}

bool StorageService::UpdateTodoItem(const TodoItem& source) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    TodoItem item = source;
    item.updatedAt = CurrentTodoTimestamp();
    if (!NormalizeTodoForSave(db.get(), item, false)) {
        lastError_ = L"待办事项标题不能为空，且有时间的事项必须填写有效时间。";
        return false;
    }

    SQLiteStatement statement(db.get(),
        L"UPDATE TodoItems SET TagId=?,Title=?,Content=?,Enabled=?,ScheduleKind=?,RepeatMode=?,RepeatInterval=?,RepeatLimit=?,RepeatFinished=?,CronExpression=?,AnchorAt=?,NextDueAt=?,"
        L"CompletedAt=?,POS=?,CreatedAt=?,UpdatedAt=? WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新待办事项 SQL 准备失败。";
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
    statement.bindInt(14, item.pos);
    statement.bindText(15, item.createdAt);
    statement.bindText(16, item.updatedAt);
    statement.bindInt(17, item.id);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"更新待办事项失败。";
        return false;
    }
    return sqlite3_changes(db.get()) > 0;
}

bool StorageService::DeleteTodoItem(int todoId) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }

    SQLiteStatement statement(db.get(), L"DELETE FROM TodoItems WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"删除待办事项 SQL 准备失败。";
        return false;
    }
    statement.bindInt(1, todoId);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"删除待办事项失败。";
        return false;
    }
    return sqlite3_changes(db.get()) > 0;
}

bool StorageService::SetTodoCompleted(int todoId, bool completed) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    TodoItem item;
    {
        SQLiteStatement query(db.get(),
            L"SELECT ID,TagId,Title,Content,Enabled,ScheduleKind,RepeatMode,RepeatInterval,RepeatLimit,RepeatFinished,CronExpression,AnchorAt,NextDueAt,CompletedAt,POS,CreatedAt,UpdatedAt "
            L"FROM TodoItems WHERE ID=?;");
        if (!query.ok()) {
            lastError_ = L"查询待办事项 SQL 准备失败。";
            return false;
        }
        query.bindInt(1, todoId);
        if (query.step() != SQLITE_ROW) {
            lastError_ = L"待办事项不存在。";
            return false;
        }
        item.id = query.columnInt(0);
        item.tagId = query.columnInt(1);
        item.title = query.columnText(2);
        item.content = query.columnText(3);
        item.enabled = query.columnInt(4) != 0;
        item.scheduleKind = static_cast<TodoScheduleKind>(query.columnInt(5));
        item.repeatMode = static_cast<TodoRepeatMode>(query.columnInt(6));
        item.repeatInterval = std::max(1, query.columnInt(7));
        item.repeatLimit = std::max(0, query.columnInt(8));
        item.repeatFinished = std::max(0, query.columnInt(9));
        item.cronExpression = query.columnText(10);
        item.anchorAt = query.columnText(11);
        item.nextDueAt = query.columnText(12);
        item.completedAt = query.columnText(13);
        item.pos = query.columnInt(14);
        item.createdAt = query.columnText(15);
        item.updatedAt = query.columnText(16);
    }

    const std::wstring now = CurrentTodoTimestamp();
    if (completed && IsRecurringTodoSchedule(item.scheduleKind)) {
        ++item.repeatFinished;
        if (item.repeatLimit > 0 && item.repeatFinished >= item.repeatLimit) {
            item.completedAt = now;
            item.nextDueAt.clear();
        } else {
            item.completedAt.clear();
            item.nextDueAt = ComputeNextTodoDueAt(item, now);
        }
    } else {
        item.completedAt = completed ? now : L"";
        if (!completed && IsRecurringTodoSchedule(item.scheduleKind) && item.nextDueAt.empty()) {
            item.nextDueAt = ComputeNextTodoDueAt(item, now);
        }
    }
    item.updatedAt = now;

    SQLiteStatement statement(db.get(), L"UPDATE TodoItems SET NextDueAt=?,CompletedAt=?,RepeatFinished=?,UpdatedAt=? WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新待办完成状态 SQL 准备失败。";
        return false;
    }
    statement.bindText(1, item.nextDueAt);
    statement.bindText(2, item.completedAt);
    statement.bindInt(3, item.repeatFinished);
    statement.bindText(4, item.updatedAt);
    statement.bindInt(5, item.id);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"更新待办完成状态失败。";
        return false;
    }
    return sqlite3_changes(db.get()) > 0;
}

bool StorageService::SetTodoEnabled(int todoId, bool enabled) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    SQLiteStatement statement(db.get(), L"UPDATE TodoItems SET Enabled=?,UpdatedAt=? WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新待办启用状态 SQL 准备失败。";
        return false;
    }
    statement.bindInt(1, enabled ? 1 : 0);
    statement.bindText(2, CurrentTodoTimestamp());
    statement.bindInt(3, todoId);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"更新待办启用状态失败。";
        return false;
    }
    return sqlite3_changes(db.get()) > 0;
}

AppModel StorageService::LoadFallback() const {
    AppModel model;
    model.groups.push_back(Group{1, L"默认分组", 0, L"", 0, 0, 0, 0, 0, L"", 0});
    model.groups.push_back(Group{2, L"默认标签", 1, L"", 1, 32, 0, 0, 0, L"", 0});
    return model;
}

void StorageService::EnsureDefaultData(AppModel& model) const {
    if (model.groups.empty()) {
        model = LoadFallback();
        return;
    }

    bool hasMajor = false;
    for (const auto& group : model.groups) {
        if (group.parentGroup == 0) {
            hasMajor = true;
            break;
        }
    }
    if (!hasMajor) {
        model.groups.insert(model.groups.begin(), Group{1, L"默认分组", 0, L"", 0, 0, 0, 0, 0, L"", 0});
    }

    bool hasTag = false;
    for (const auto& group : model.groups) {
        if (group.parentGroup != 0) {
            hasTag = true;
            break;
        }
    }
    if (!hasTag) {
        int parent = 1;
        for (const auto& group : model.groups) {
            if (group.parentGroup == 0) {
                parent = group.id;
                break;
            }
        }
        model.groups.push_back(Group{2, L"默认标签", parent, L"", 1, 32, 0, 0, 0, L"", 0});
    }
}
