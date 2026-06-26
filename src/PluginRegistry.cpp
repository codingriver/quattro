#include "PluginRegistry.h"

#include "Utilities.h"

#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {
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
}

PluginRegistry::PluginRegistry(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

std::vector<PluginRecord> PluginRegistry::BuiltinPlugins() {
    PluginRecord clicker;
    clicker.id = L"quattro.builtin.clicker";
    clicker.name = L"连点器";
    clicker.version = L"1.0.0";
    clicker.category = L"builtin-tools";
    clicker.kind = L"builtin-tool";
    clicker.engine = L"clicker";
    clicker.description = L"按指定坐标、次数和间隔连续执行鼠标点击。";
    clicker.permissions = L"鼠标控制, 热键停止";
    clicker.author = L"Quattro";
    clicker.license = L"Built-in";
    clicker.builtin = true;
    clicker.deletable = false;
    clicker.enabled = false;
    clicker.installed = true;

    PluginRecord timer;
    timer.id = L"quattro.builtin.timer";
    timer.name = L"计时器";
    timer.version = L"1.0.0";
    timer.category = L"builtin-tools";
    timer.kind = L"builtin-tool";
    timer.engine = L"timer";
    timer.description = L"设置倒计时，到点后弹窗提醒。";
    timer.permissions = L"提醒";
    timer.author = L"Quattro";
    timer.license = L"Built-in";
    timer.builtin = true;
    timer.deletable = false;
    timer.enabled = true;
    timer.installed = true;

    PluginRecord stopwatch;
    stopwatch.id = L"quattro.builtin.stopwatch";
    stopwatch.name = L"秒表";
    stopwatch.version = L"1.0.0";
    stopwatch.category = L"builtin-tools";
    stopwatch.kind = L"builtin-tool";
    stopwatch.engine = L"stopwatch";
    stopwatch.description = L"提供开始、暂停、重置和计次。";
    stopwatch.author = L"Quattro";
    stopwatch.license = L"Built-in";
    stopwatch.builtin = true;
    stopwatch.deletable = false;
    stopwatch.enabled = true;
    stopwatch.installed = true;

    return {
        clicker,
        timer,
        stopwatch,
    };
}

bool PluginRegistry::Initialize() {
    lastError_.clear();
    std::error_code ec;
    std::filesystem::create_directories(appDirectory_ / L"db", ec);
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    return EnsureSchema(db.get()) && UpsertBuiltinPlugins(db.get());
}

bool PluginRegistry::EnsureSchema(void* rawDb) {
    auto* db = static_cast<sqlite3*>(rawDb);
    if (!Exec(db,
        "CREATE TABLE IF NOT EXISTS Plugins("
        "ID TEXT PRIMARY KEY,"
        "Name TEXT NOT NULL,"
        "Version TEXT NOT NULL,"
        "Category TEXT NOT NULL,"
        "Kind TEXT NOT NULL,"
        "Engine TEXT,"
        "Description TEXT,"
        "Permissions TEXT,"
        "Author TEXT,"
        "License TEXT,"
        "PackageUrl TEXT,"
        "Sha256 TEXT,"
        "Builtin INTEGER DEFAULT 0,"
        "Deletable INTEGER DEFAULT 1,"
        "Enabled INTEGER DEFAULT 0,"
        "Installed INTEGER DEFAULT 0,"
        "InstallPath TEXT,"
        "CreatedAt TEXT DEFAULT CURRENT_TIMESTAMP,"
        "UpdatedAt TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS PluginSettings("
        "PluginID TEXT NOT NULL,"
        "Key TEXT NOT NULL,"
        "Value TEXT,"
        "PRIMARY KEY(PluginID,Key));"
        "CREATE TABLE IF NOT EXISTS PluginContributions("
        "PluginID TEXT NOT NULL,"
        "ObjectType TEXT NOT NULL,"
        "ObjectID INTEGER NOT NULL,"
        "ObjectPath TEXT);",
        lastError_)) {
        return false;
    }
    if (!HasColumn(db, L"Plugins", L"Author")) {
        Exec(db, "ALTER TABLE Plugins ADD COLUMN Author TEXT;", lastError_);
    }
    if (!HasColumn(db, L"Plugins", L"License")) {
        Exec(db, "ALTER TABLE Plugins ADD COLUMN License TEXT;", lastError_);
    }
    if (!HasColumn(db, L"Plugins", L"PackageUrl")) {
        Exec(db, "ALTER TABLE Plugins ADD COLUMN PackageUrl TEXT;", lastError_);
    }
    if (!HasColumn(db, L"Plugins", L"Sha256")) {
        Exec(db, "ALTER TABLE Plugins ADD COLUMN Sha256 TEXT;", lastError_);
    }
    if (!HasColumn(db, L"PluginContributions", L"ObjectPath")) {
        Exec(db, "ALTER TABLE PluginContributions ADD COLUMN ObjectPath TEXT;", lastError_);
    }
    return lastError_.empty();
}

bool PluginRegistry::UpsertBuiltinPlugins(void* rawDb) {
    auto* db = static_cast<sqlite3*>(rawDb);
    for (const auto& plugin : BuiltinPlugins()) {
        SQLiteStatement row(db,
            L"INSERT INTO Plugins(ID,Name,Version,Category,Kind,Engine,Description,Permissions,Author,License,PackageUrl,Sha256,Builtin,Deletable,Enabled,Installed) "
            L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            L"ON CONFLICT(ID) DO UPDATE SET "
            L"Name=excluded.Name,Version=excluded.Version,Category=excluded.Category,Kind=excluded.Kind,"
            L"Engine=excluded.Engine,Description=excluded.Description,Permissions=excluded.Permissions,"
            L"Author=excluded.Author,License=excluded.License,PackageUrl=excluded.PackageUrl,Sha256=excluded.Sha256,"
            L"Builtin=excluded.Builtin,Deletable=excluded.Deletable,Installed=1,UpdatedAt=CURRENT_TIMESTAMP;");
        if (!row.ok()) {
            lastError_ = L"插件注册 SQL 准备失败。";
            return false;
        }
        row.bindText(1, plugin.id);
        row.bindText(2, plugin.name);
        row.bindText(3, plugin.version);
        row.bindText(4, plugin.category);
        row.bindText(5, plugin.kind);
        row.bindText(6, plugin.engine);
        row.bindText(7, plugin.description);
        row.bindText(8, plugin.permissions);
        row.bindText(9, plugin.author);
        row.bindText(10, plugin.license);
        row.bindText(11, plugin.packageUrl);
        row.bindText(12, plugin.sha256);
        row.bindInt(13, plugin.builtin ? 1 : 0);
        row.bindInt(14, plugin.deletable ? 1 : 0);
        row.bindInt(15, plugin.enabled ? 1 : 0);
        row.bindInt(16, plugin.installed ? 1 : 0);
        if (row.step() != SQLITE_DONE) {
            const void* message = sqlite3_errmsg16(db);
            lastError_ = message ? static_cast<const wchar_t*>(message) : L"注册内置插件失败。";
            return false;
        }
    }
    return true;
}

std::vector<PluginRecord> PluginRegistry::LoadPlugins() {
    lastError_.clear();
    std::vector<PluginRecord> plugins;
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return BuiltinPlugins();
    }
    if (!EnsureSchema(db.get()) || !UpsertBuiltinPlugins(db.get())) {
        return BuiltinPlugins();
    }

    SQLiteStatement statement(db.get(),
        L"SELECT ID,Name,Version,Category,Kind,Engine,Description,Permissions,Author,License,PackageUrl,Sha256,Builtin,Deletable,Enabled,Installed "
        L"FROM Plugins ORDER BY Builtin DESC,Category,Name;");
    if (!statement.ok()) {
        lastError_ = L"读取插件列表失败。";
        return plugins;
    }
    while (statement.step() == SQLITE_ROW) {
        PluginRecord plugin;
        plugin.id = statement.columnText(0);
        plugin.name = statement.columnText(1);
        plugin.version = statement.columnText(2);
        plugin.category = statement.columnText(3);
        plugin.kind = statement.columnText(4);
        plugin.engine = statement.columnText(5);
        plugin.description = statement.columnText(6);
        plugin.permissions = statement.columnText(7);
        plugin.author = statement.columnText(8);
        plugin.license = statement.columnText(9);
        plugin.packageUrl = statement.columnText(10);
        plugin.sha256 = statement.columnText(11);
        plugin.builtin = statement.columnInt(12) != 0;
        plugin.deletable = statement.columnInt(13) != 0;
        plugin.enabled = statement.columnInt(14) != 0;
        plugin.installed = statement.columnInt(15) != 0;
        plugins.push_back(std::move(plugin));
    }
    return plugins;
}

bool PluginRegistry::UpsertStorePlugin(const PluginRecord& plugin) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    if (!EnsureSchema(db.get()) || !UpsertBuiltinPlugins(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(),
        L"INSERT INTO Plugins(ID,Name,Version,Category,Kind,Engine,Description,Permissions,Author,License,PackageUrl,Sha256,Builtin,Deletable,Enabled,Installed) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0,?,0,0) "
        L"ON CONFLICT(ID) DO UPDATE SET "
        L"Name=excluded.Name,Version=excluded.Version,Category=excluded.Category,Kind=excluded.Kind,"
        L"Engine=excluded.Engine,Description=excluded.Description,Permissions=excluded.Permissions,"
        L"Author=excluded.Author,License=excluded.License,PackageUrl=excluded.PackageUrl,Sha256=excluded.Sha256,"
        L"Deletable=excluded.Deletable,UpdatedAt=CURRENT_TIMESTAMP;");
    if (!statement.ok()) {
        lastError_ = L"更新商店插件失败。";
        return false;
    }
    statement.bindText(1, plugin.id);
    statement.bindText(2, plugin.name);
    statement.bindText(3, plugin.version);
    statement.bindText(4, plugin.category);
    statement.bindText(5, plugin.kind);
    statement.bindText(6, plugin.engine);
    statement.bindText(7, plugin.description);
    statement.bindText(8, plugin.permissions);
    statement.bindText(9, plugin.author);
    statement.bindText(10, plugin.license);
    statement.bindText(11, plugin.packageUrl);
    statement.bindText(12, plugin.sha256);
    statement.bindInt(13, plugin.deletable ? 1 : 0);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"更新商店插件失败。";
        return false;
    }
    return true;
}

bool PluginRegistry::MarkInstalled(const PluginRecord& plugin, const std::wstring& installPath) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    if (!EnsureSchema(db.get()) || !UpsertBuiltinPlugins(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(),
        L"INSERT INTO Plugins(ID,Name,Version,Category,Kind,Engine,Description,Permissions,Author,License,PackageUrl,Sha256,Builtin,Deletable,Enabled,Installed,InstallPath) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0,?,1,1,?) "
        L"ON CONFLICT(ID) DO UPDATE SET "
        L"Name=excluded.Name,Version=excluded.Version,Category=excluded.Category,Kind=excluded.Kind,"
        L"Engine=excluded.Engine,Description=excluded.Description,Permissions=excluded.Permissions,"
        L"Author=excluded.Author,License=excluded.License,PackageUrl=excluded.PackageUrl,Sha256=excluded.Sha256,"
        L"Deletable=excluded.Deletable,Enabled=1,Installed=1,InstallPath=excluded.InstallPath,UpdatedAt=CURRENT_TIMESTAMP;");
    if (!statement.ok()) {
        lastError_ = L"标记插件已安装失败。";
        return false;
    }
    statement.bindText(1, plugin.id);
    statement.bindText(2, plugin.name);
    statement.bindText(3, plugin.version);
    statement.bindText(4, plugin.category);
    statement.bindText(5, plugin.kind);
    statement.bindText(6, plugin.engine);
    statement.bindText(7, plugin.description);
    statement.bindText(8, plugin.permissions);
    statement.bindText(9, plugin.author);
    statement.bindText(10, plugin.license);
    statement.bindText(11, plugin.packageUrl);
    statement.bindText(12, plugin.sha256);
    statement.bindInt(13, plugin.deletable ? 1 : 0);
    statement.bindText(14, installPath);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"标记插件已安装失败。";
        return false;
    }
    return true;
}

bool PluginRegistry::RemovePlugin(const std::wstring& pluginId) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    if (!EnsureSchema(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(),
        L"UPDATE Plugins SET Enabled=0,Installed=0,InstallPath='',UpdatedAt=CURRENT_TIMESTAMP "
        L"WHERE ID=? AND Builtin=0;");
    if (!statement.ok()) {
        lastError_ = L"删除插件记录失败。";
        return false;
    }
    statement.bindText(1, pluginId);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"删除插件记录失败。";
        return false;
    }
    return true;
}

bool PluginRegistry::SetEnabled(const std::wstring& pluginId, bool enabled) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    if (!EnsureSchema(db.get()) || !UpsertBuiltinPlugins(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(), L"UPDATE Plugins SET Enabled=?,UpdatedAt=CURRENT_TIMESTAMP WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新插件状态失败。";
        return false;
    }
    statement.bindInt(1, enabled ? 1 : 0);
    statement.bindText(2, pluginId);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"更新插件状态失败。";
        return false;
    }
    return true;
}

bool PluginRegistry::IsEnabled(const std::wstring& pluginId) {
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        return false;
    }
    if (!EnsureSchema(db.get()) || !UpsertBuiltinPlugins(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(), L"SELECT Enabled FROM Plugins WHERE ID=?;");
    if (!statement.ok()) {
        return false;
    }
    statement.bindText(1, pluginId);
    return statement.step() == SQLITE_ROW && statement.columnInt(0) != 0;
}

bool PluginRegistry::RecordContribution(const std::wstring& pluginId, const std::wstring& objectType, int objectId, const std::wstring& objectPath) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    if (!EnsureSchema(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(), L"INSERT INTO PluginContributions(PluginID,ObjectType,ObjectID,ObjectPath) VALUES(?,?,?,?);");
    if (!statement.ok()) {
        lastError_ = L"记录插件贡献失败。";
        return false;
    }
    statement.bindText(1, pluginId);
    statement.bindText(2, objectType);
    statement.bindInt(3, objectId);
    statement.bindText(4, objectPath);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"记录插件贡献失败。";
        return false;
    }
    return true;
}

std::vector<PluginContribution> PluginRegistry::LoadContributions(const std::wstring& pluginId) {
    lastError_.clear();
    std::vector<PluginContribution> contributions;
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return contributions;
    }
    if (!EnsureSchema(db.get())) {
        return contributions;
    }
    SQLiteStatement statement(db.get(), L"SELECT PluginID,ObjectType,ObjectID,ObjectPath FROM PluginContributions WHERE PluginID=? ORDER BY rowid DESC;");
    if (!statement.ok()) {
        lastError_ = L"读取插件贡献失败。";
        return contributions;
    }
    statement.bindText(1, pluginId);
    while (statement.step() == SQLITE_ROW) {
        PluginContribution item;
        item.pluginId = statement.columnText(0);
        item.objectType = statement.columnText(1);
        item.objectId = statement.columnInt(2);
        item.objectPath = statement.columnText(3);
        contributions.push_back(std::move(item));
    }
    return contributions;
}

bool PluginRegistry::ClearContributions(const std::wstring& pluginId) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    if (!EnsureSchema(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(), L"DELETE FROM PluginContributions WHERE PluginID=?;");
    if (!statement.ok()) {
        lastError_ = L"清理插件贡献记录失败。";
        return false;
    }
    statement.bindText(1, pluginId);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"清理插件贡献记录失败。";
        return false;
    }
    return true;
}

std::wstring PluginRegistry::GetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& fallback) {
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        return fallback;
    }
    if (!EnsureSchema(db.get())) {
        return fallback;
    }
    SQLiteStatement statement(db.get(), L"SELECT Value FROM PluginSettings WHERE PluginID=? AND Key=?;");
    if (!statement.ok()) {
        return fallback;
    }
    statement.bindText(1, pluginId);
    statement.bindText(2, key);
    if (statement.step() != SQLITE_ROW) {
        return fallback;
    }
    return statement.columnText(0);
}

bool PluginRegistry::SetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& value) {
    lastError_.clear();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    if (!EnsureSchema(db.get())) {
        return false;
    }
    SQLiteStatement statement(db.get(),
        L"INSERT INTO PluginSettings(PluginID,Key,Value) VALUES(?,?,?) "
        L"ON CONFLICT(PluginID,Key) DO UPDATE SET Value=excluded.Value;");
    if (!statement.ok()) {
        lastError_ = L"保存插件配置失败。";
        return false;
    }
    statement.bindText(1, pluginId);
    statement.bindText(2, key);
    statement.bindText(3, value);
    if (statement.step() != SQLITE_DONE) {
        const void* message = sqlite3_errmsg16(db.get());
        lastError_ = message ? static_cast<const wchar_t*>(message) : L"保存插件配置失败。";
        return false;
    }
    return true;
}
