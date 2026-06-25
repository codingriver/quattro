#include "Storage.h"

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
         "Pidl BLOB);",
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

void MigrateSchema(sqlite3* db, std::wstring& error) {
    if (!HasColumn(db, L"Links", L"Pidl")) {
        Exec(db, "ALTER TABLE Links ADD COLUMN Pidl BLOB;", error);
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
}

StorageService::StorageService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

AppModel StorageService::Load() {
    lastError_.clear();

    const std::filesystem::path dbDirectory = appDirectory_ / L"db";
    std::error_code ec;
    std::filesystem::create_directories(dbDirectory, ec);

    SQLiteDatabase db(dbDirectory / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        sqliteAvailable_ = false;
        return LoadFallback();
    }
    sqliteAvailable_ = true;

    CreateSchema(db.get(), lastError_);
    MigrateSchema(db.get(), lastError_);

    if (CountRows(db.get(), L"SELECT COUNT(*) FROM Version;") == 0) {
        Exec(db.get(), ("INSERT INTO Version(ID,Ver) VALUES(1," + std::to_string(kSchemaVersion) + ");").c_str(), lastError_);
    }

    if (CountRows(db.get(), L"SELECT COUNT(*) FROM Groups;") == 0) {
        Exec(db.get(),
             "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT,SORT,TYPE) VALUES(1,'新的分组',0,0,0,0,0,0);"
             "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT,SORT,TYPE) VALUES(2,'新的标签',0,1,32,1,0,0);",
             lastError_);
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

    EnsureDefaultData(model);
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
        SQLiteStatement deleteLinks(db.get(),
            L"DELETE FROM Links WHERE ParentGroup IN (SELECT ID FROM Groups WHERE ParentGroup=?);");
        deleteLinks.bindInt(1, groupId);
        ok = ok && deleteLinks.step() == SQLITE_DONE;

        SQLiteStatement deleteTags(db.get(), L"DELETE FROM Groups WHERE ParentGroup=?;");
        deleteTags.bindInt(1, groupId);
        ok = ok && deleteTags.step() == SQLITE_DONE;
    } else {
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
