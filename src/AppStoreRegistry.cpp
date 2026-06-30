#include "AppStoreRegistry.h"

#include "Utilities.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <map>

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
    std::wstring columnText(int index) const {
        const void* text = sqlite3_column_text16(stmt_, index);
        return text ? static_cast<const wchar_t*>(text) : L"";
    }
    long long columnInt64(int index) const { return sqlite3_column_int64(stmt_, index); }
    void bindText(int index, const std::wstring& value) { sqlite3_bind_text16(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT); }
    void bindInt64(int index, long long value) { sqlite3_bind_int64(stmt_, index, value); }

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
        return false;
    }
    return true;
}
}

AppStoreRegistry::AppStoreRegistry(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

bool AppStoreRegistry::Initialize() {
    lastError_.clear();
    std::error_code ec;
    std::filesystem::create_directories(appDirectory_ / L"db", ec);
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    return EnsureSchema(db.get());
}

bool AppStoreRegistry::EnsureSchema(void* rawDb) {
    auto* db = static_cast<sqlite3*>(rawDb);
    return Exec(db,
        "CREATE TABLE IF NOT EXISTS AppTransferTasks("
        "ID TEXT PRIMARY KEY,"
        "Direction TEXT NOT NULL,"
        "AppID TEXT,"
        "Name TEXT,"
        "Version TEXT,"
        "ReleaseTag TEXT,"
        "Owner TEXT NOT NULL,"
        "Repo TEXT NOT NULL,"
        "LocalPath TEXT,"
        "InstallPath TEXT,"
        "ManifestJson TEXT,"
        "Status TEXT NOT NULL,"
        "Phase TEXT,"
        "TotalBytes INTEGER DEFAULT 0,"
        "TransferredBytes INTEGER DEFAULT 0,"
        "SplitSizeBytes INTEGER DEFAULT 268435456,"
        "ErrorMessage TEXT,"
        "CreatedAt TEXT DEFAULT CURRENT_TIMESTAMP,"
        "UpdatedAt TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS AppTransferParts("
        "TaskID TEXT NOT NULL,"
        "PartIndex INTEGER NOT NULL,"
        "AssetName TEXT NOT NULL,"
        "AssetID INTEGER DEFAULT 0,"
        "LocalPath TEXT,"
        "SizeBytes INTEGER DEFAULT 0,"
        "Sha256 TEXT,"
        "Status TEXT NOT NULL,"
        "TransferredBytes INTEGER DEFAULT 0,"
        "ErrorMessage TEXT,"
        "PRIMARY KEY(TaskID,PartIndex));"
        "CREATE TABLE IF NOT EXISTS InstalledApps("
        "AppID TEXT PRIMARY KEY,"
        "Name TEXT NOT NULL,"
        "Version TEXT NOT NULL,"
        "ReleaseTag TEXT NOT NULL,"
        "Owner TEXT NOT NULL,"
        "Repo TEXT NOT NULL,"
        "InstallPath TEXT NOT NULL,"
        "ManifestJson TEXT NOT NULL,"
        "PackageSha256 TEXT,"
        "Draft INTEGER DEFAULT 0,"
        "InstalledAt TEXT DEFAULT CURRENT_TIMESTAMP,"
        "UpdatedAt TEXT DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS InstalledAppFiles("
        "AppID TEXT NOT NULL,"
        "Path TEXT NOT NULL,"
        "Sha256 TEXT,"
        "SizeBytes INTEGER DEFAULT 0,"
        "PRIMARY KEY(AppID,Path));"
        "CREATE TABLE IF NOT EXISTS InstalledAppShortcuts("
        "AppID TEXT NOT NULL,"
        "Path TEXT NOT NULL,"
        "PRIMARY KEY(AppID,Path));",
        lastError_);
}

std::wstring AppStoreRegistry::NewTaskId() const {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return L"task-" + Hex8(StablePathHash(appDirectory_.wstring() + L"|" + std::to_wstring(now))) + L"-" + std::to_wstring(now & 0xffff);
}

bool AppStoreRegistry::AddDownloadTask(const AppStoreEntry& app, const std::wstring& owner, const std::wstring& repo, long long splitSizeBytes, std::wstring& taskId) {
    lastError_.clear();
    taskId = NewTaskId();
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok() && !Initialize()) {
        return false;
    }
    SQLiteDatabase readyDb(appDirectory_ / L"db" / L"link.db");
    if (!readyDb.ok() || !EnsureSchema(readyDb.get())) {
        if (lastError_.empty()) {
            lastError_ = readyDb.Error();
        }
        return false;
    }
    SQLiteStatement statement(readyDb.get(),
        L"INSERT INTO AppTransferTasks(ID,Direction,AppID,Name,Version,ReleaseTag,Owner,Repo,ManifestJson,Status,Phase,TotalBytes,SplitSizeBytes) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        lastError_ = L"创建下载任务失败。";
        return false;
    }
    long long total = 0;
    for (const auto& part : app.manifest.parts) {
        total += static_cast<long long>(part.size);
    }
    statement.bindText(1, taskId);
    statement.bindText(2, L"download");
    statement.bindText(3, app.manifest.appId);
    statement.bindText(4, app.manifest.name);
    statement.bindText(5, app.manifest.version);
    statement.bindText(6, app.manifest.tag);
    statement.bindText(7, owner);
    statement.bindText(8, repo);
    statement.bindText(9, app.manifest.manifestJson);
    statement.bindText(10, L"queued");
    statement.bindText(11, L"downloadingPart");
    statement.bindInt64(12, total);
    statement.bindInt64(13, splitSizeBytes);
    if (statement.step() != SQLITE_DONE) {
        lastError_ = L"写入下载任务失败。";
        return false;
    }
    for (const auto& part : app.manifest.parts) {
        SQLiteStatement partStatement(readyDb.get(),
            L"INSERT INTO AppTransferParts(TaskID,PartIndex,AssetName,SizeBytes,Sha256,Status) VALUES(?,?,?,?,?,?);");
        if (!partStatement.ok()) {
            lastError_ = L"创建下载分片任务失败。";
            return false;
        }
        partStatement.bindText(1, taskId);
        partStatement.bindInt64(2, part.index);
        partStatement.bindText(3, part.name);
        partStatement.bindInt64(4, static_cast<long long>(part.size));
        partStatement.bindText(5, part.sha256);
        partStatement.bindText(6, L"queued");
        if (partStatement.step() != SQLITE_DONE) {
            lastError_ = L"写入下载分片任务失败。";
            return false;
        }
    }
    return true;
}

bool AppStoreRegistry::AddUploadTaskPlaceholder(const std::wstring& localPath, const std::wstring& owner, const std::wstring& repo, long long splitSizeBytes, std::wstring& taskId) {
    lastError_.clear();
    taskId = NewTaskId();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok() || !EnsureSchema(db.get())) {
        if (lastError_.empty()) {
            lastError_ = db.Error();
        }
        return false;
    }
    SQLiteStatement statement(db.get(),
        L"INSERT INTO AppTransferTasks(ID,Direction,Owner,Repo,LocalPath,Status,Phase,SplitSizeBytes) VALUES(?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        lastError_ = L"创建上传任务失败。";
        return false;
    }
    statement.bindText(1, taskId);
    statement.bindText(2, L"upload");
    statement.bindText(3, owner);
    statement.bindText(4, repo);
    statement.bindText(5, localPath);
    statement.bindText(6, L"queued");
    statement.bindText(7, L"packaging");
    statement.bindInt64(8, splitSizeBytes);
    if (statement.step() != SQLITE_DONE) {
        lastError_ = L"写入上传任务失败。";
        return false;
    }
    return true;
}

std::vector<AppTransferTask> AppStoreRegistry::LoadTasks(const std::wstring& direction) {
    lastError_.clear();
    std::vector<AppTransferTask> tasks;
    if (!Initialize()) {
        return tasks;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok() || !EnsureSchema(db.get())) {
        if (lastError_.empty()) {
            lastError_ = db.Error();
        }
        return tasks;
    }
    const bool filtered = !Trim(direction).empty();
    SQLiteStatement statement(db.get(), filtered
        ? L"SELECT ID,Direction,AppID,Name,Version,ReleaseTag,Owner,Repo,LocalPath,InstallPath,ManifestJson,Status,Phase,TotalBytes,TransferredBytes,SplitSizeBytes,ErrorMessage,CreatedAt,UpdatedAt FROM AppTransferTasks WHERE Direction=? ORDER BY CreatedAt DESC;"
        : L"SELECT ID,Direction,AppID,Name,Version,ReleaseTag,Owner,Repo,LocalPath,InstallPath,ManifestJson,Status,Phase,TotalBytes,TransferredBytes,SplitSizeBytes,ErrorMessage,CreatedAt,UpdatedAt FROM AppTransferTasks ORDER BY CreatedAt DESC;");
    if (!statement.ok()) {
        lastError_ = L"读取网盘管理队列失败。";
        return tasks;
    }
    if (filtered) {
        statement.bindText(1, direction);
    }
    while (statement.step() == SQLITE_ROW) {
        AppTransferTask task;
        task.id = statement.columnText(0);
        task.direction = statement.columnText(1);
        task.appId = statement.columnText(2);
        task.name = statement.columnText(3);
        task.version = statement.columnText(4);
        task.releaseTag = statement.columnText(5);
        task.owner = statement.columnText(6);
        task.repo = statement.columnText(7);
        task.localPath = statement.columnText(8);
        task.installPath = statement.columnText(9);
        task.manifestJson = statement.columnText(10);
        task.status = statement.columnText(11);
        task.phase = statement.columnText(12);
        task.totalBytes = statement.columnInt64(13);
        task.transferredBytes = statement.columnInt64(14);
        task.splitSizeBytes = statement.columnInt64(15);
        task.errorMessage = statement.columnText(16);
        task.createdAt = statement.columnText(17);
        task.updatedAt = statement.columnText(18);
        tasks.push_back(std::move(task));
    }
    return tasks;
}

bool AppStoreRegistry::LoadTask(const std::wstring& taskId, AppTransferTask& task) {
    lastError_.clear();
    task = {};
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(),
        L"SELECT ID,Direction,AppID,Name,Version,ReleaseTag,Owner,Repo,LocalPath,InstallPath,ManifestJson,Status,Phase,TotalBytes,TransferredBytes,SplitSizeBytes,ErrorMessage,CreatedAt,UpdatedAt FROM AppTransferTasks WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"读取队列任务失败。";
        return false;
    }
    statement.bindText(1, taskId);
    if (statement.step() != SQLITE_ROW) {
        lastError_ = L"队列任务不存在。";
        return false;
    }
    task.id = statement.columnText(0);
    task.direction = statement.columnText(1);
    task.appId = statement.columnText(2);
    task.name = statement.columnText(3);
    task.version = statement.columnText(4);
    task.releaseTag = statement.columnText(5);
    task.owner = statement.columnText(6);
    task.repo = statement.columnText(7);
    task.localPath = statement.columnText(8);
    task.installPath = statement.columnText(9);
    task.manifestJson = statement.columnText(10);
    task.status = statement.columnText(11);
    task.phase = statement.columnText(12);
    task.totalBytes = statement.columnInt64(13);
    task.transferredBytes = statement.columnInt64(14);
    task.splitSizeBytes = statement.columnInt64(15);
    task.errorMessage = statement.columnText(16);
    task.createdAt = statement.columnText(17);
    task.updatedAt = statement.columnText(18);
    return true;
}

std::vector<AppTransferPartRecord> AppStoreRegistry::LoadParts(const std::wstring& taskId) {
    lastError_.clear();
    std::vector<AppTransferPartRecord> parts;
    if (!Initialize()) {
        return parts;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(),
        L"SELECT TaskID,PartIndex,AssetName,AssetID,LocalPath,SizeBytes,Sha256,Status,TransferredBytes,ErrorMessage FROM AppTransferParts WHERE TaskID=? ORDER BY PartIndex;");
    if (!statement.ok()) {
        lastError_ = L"读取队列分片失败。";
        return parts;
    }
    statement.bindText(1, taskId);
    while (statement.step() == SQLITE_ROW) {
        AppTransferPartRecord part;
        part.taskId = statement.columnText(0);
        part.partIndex = static_cast<int>(statement.columnInt64(1));
        part.assetName = statement.columnText(2);
        part.assetId = statement.columnInt64(3);
        part.localPath = statement.columnText(4);
        part.sizeBytes = statement.columnInt64(5);
        part.sha256 = statement.columnText(6);
        part.status = statement.columnText(7);
        part.transferredBytes = statement.columnInt64(8);
        part.errorMessage = statement.columnText(9);
        parts.push_back(std::move(part));
    }
    return parts;
}

bool AppStoreRegistry::SetTaskStatus(const std::wstring& taskId, const std::wstring& status, const std::wstring& phase, const std::wstring& error) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(), L"UPDATE AppTransferTasks SET Status=?,Phase=?,ErrorMessage=?,UpdatedAt=CURRENT_TIMESTAMP WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新任务状态失败。";
        return false;
    }
    statement.bindText(1, status);
    statement.bindText(2, phase);
    statement.bindText(3, error);
    statement.bindText(4, taskId);
    if (statement.step() != SQLITE_DONE) {
        lastError_ = L"写入任务状态失败。";
        return false;
    }
    return true;
}

bool AppStoreRegistry::UpdateTaskProgress(const std::wstring& taskId, long long transferredBytes, long long totalBytes) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(), totalBytes >= 0
        ? L"UPDATE AppTransferTasks SET TransferredBytes=?,TotalBytes=?,UpdatedAt=CURRENT_TIMESTAMP WHERE ID=?;"
        : L"UPDATE AppTransferTasks SET TransferredBytes=?,UpdatedAt=CURRENT_TIMESTAMP WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新任务进度失败。";
        return false;
    }
    statement.bindInt64(1, transferredBytes);
    if (totalBytes >= 0) {
        statement.bindInt64(2, totalBytes);
        statement.bindText(3, taskId);
    } else {
        statement.bindText(2, taskId);
    }
    if (statement.step() != SQLITE_DONE) {
        lastError_ = L"写入任务进度失败。";
        return false;
    }
    return true;
}

bool AppStoreRegistry::UpdateTaskManifest(const std::wstring& taskId, const AppStoreManifest& manifest) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(),
        L"UPDATE AppTransferTasks SET AppID=?,Name=?,Version=?,ReleaseTag=?,ManifestJson=?,TotalBytes=?,UpdatedAt=CURRENT_TIMESTAMP WHERE ID=?;");
    if (!statement.ok()) {
        lastError_ = L"更新任务 manifest 失败。";
        return false;
    }
    statement.bindText(1, manifest.appId);
    statement.bindText(2, manifest.name);
    statement.bindText(3, manifest.version);
    statement.bindText(4, manifest.tag);
    statement.bindText(5, manifest.manifestJson);
    statement.bindInt64(6, static_cast<long long>(manifest.totalSize));
    statement.bindText(7, taskId);
    if (statement.step() != SQLITE_DONE) {
        lastError_ = L"写入任务 manifest 失败。";
        return false;
    }
    return true;
}

bool AppStoreRegistry::UpsertPart(const AppTransferPartRecord& part) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(),
        L"INSERT INTO AppTransferParts(TaskID,PartIndex,AssetName,AssetID,LocalPath,SizeBytes,Sha256,Status,TransferredBytes,ErrorMessage) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?) "
        L"ON CONFLICT(TaskID,PartIndex) DO UPDATE SET AssetName=excluded.AssetName,AssetID=excluded.AssetID,LocalPath=excluded.LocalPath,SizeBytes=excluded.SizeBytes,Sha256=excluded.Sha256,Status=excluded.Status,TransferredBytes=excluded.TransferredBytes,ErrorMessage=excluded.ErrorMessage;");
    if (!statement.ok()) {
        lastError_ = L"写入分片记录失败。";
        return false;
    }
    statement.bindText(1, part.taskId);
    statement.bindInt64(2, part.partIndex);
    statement.bindText(3, part.assetName);
    statement.bindInt64(4, part.assetId);
    statement.bindText(5, part.localPath);
    statement.bindInt64(6, part.sizeBytes);
    statement.bindText(7, part.sha256);
    statement.bindText(8, part.status);
    statement.bindInt64(9, part.transferredBytes);
    statement.bindText(10, part.errorMessage);
    if (statement.step() != SQLITE_DONE) {
        lastError_ = L"保存分片记录失败。";
        return false;
    }
    return true;
}

bool AppStoreRegistry::SetPartStatus(const std::wstring& taskId, int partIndex, const std::wstring& status, long long transferredBytes, long long assetId, const std::wstring& error) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(), assetId > 0
        ? L"UPDATE AppTransferParts SET Status=?,TransferredBytes=?,AssetID=?,ErrorMessage=? WHERE TaskID=? AND PartIndex=?;"
        : L"UPDATE AppTransferParts SET Status=?,TransferredBytes=?,ErrorMessage=? WHERE TaskID=? AND PartIndex=?;");
    if (!statement.ok()) {
        lastError_ = L"更新分片状态失败。";
        return false;
    }
    statement.bindText(1, status);
    statement.bindInt64(2, transferredBytes);
    if (assetId > 0) {
        statement.bindInt64(3, assetId);
        statement.bindText(4, error);
        statement.bindText(5, taskId);
        statement.bindInt64(6, partIndex);
    } else {
        statement.bindText(3, error);
        statement.bindText(4, taskId);
        statement.bindInt64(5, partIndex);
    }
    if (statement.step() != SQLITE_DONE) {
        lastError_ = L"写入分片状态失败。";
        return false;
    }
    return true;
}

bool AppStoreRegistry::IsTaskCancelRequested(const std::wstring& taskId) {
    AppTransferTask task;
    if (!LoadTask(taskId, task)) {
        return false;
    }
    return task.status == L"canceling" || task.status == L"paused";
}

bool AppStoreRegistry::MarkInterruptedTasksPaused() {
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    if (!db.ok()) {
        lastError_ = db.Error();
        return false;
    }
    return Exec(db.get(),
        "UPDATE AppTransferTasks SET Status='paused',ErrorMessage='上次任务中断，已暂停。',UpdatedAt=CURRENT_TIMESTAMP WHERE Status IN ('running','canceling');"
        "UPDATE AppTransferParts SET Status='paused' WHERE Status='running';",
        lastError_);
}

bool AppStoreRegistry::DeleteTask(const std::wstring& taskId) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement parts(db.get(), L"DELETE FROM AppTransferParts WHERE TaskID=?;");
    SQLiteStatement task(db.get(), L"DELETE FROM AppTransferTasks WHERE ID=?;");
    if (!parts.ok() || !task.ok()) {
        lastError_ = L"删除任务失败。";
        return false;
    }
    parts.bindText(1, taskId);
    task.bindText(1, taskId);
    if (parts.step() != SQLITE_DONE || task.step() != SQLITE_DONE) {
        lastError_ = L"写入删除任务失败。";
        return false;
    }
    return true;
}

bool AppStoreRegistry::RegisterInstalledApp(const AppStoreManifest& manifest, const std::wstring& owner, const std::wstring& repo, const std::wstring& installPath, const std::vector<std::wstring>& files, const std::vector<std::wstring>& shortcuts) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement app(db.get(),
        L"INSERT INTO InstalledApps(AppID,Name,Version,ReleaseTag,Owner,Repo,InstallPath,ManifestJson,PackageSha256,Draft,UpdatedAt) "
        L"VALUES(?,?,?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP) "
        L"ON CONFLICT(AppID) DO UPDATE SET Name=excluded.Name,Version=excluded.Version,ReleaseTag=excluded.ReleaseTag,Owner=excluded.Owner,Repo=excluded.Repo,InstallPath=excluded.InstallPath,ManifestJson=excluded.ManifestJson,PackageSha256=excluded.PackageSha256,Draft=excluded.Draft,UpdatedAt=CURRENT_TIMESTAMP;");
    if (!app.ok()) {
        lastError_ = L"登记已安装应用失败。";
        return false;
    }
    app.bindText(1, manifest.appId);
    app.bindText(2, manifest.name);
    app.bindText(3, manifest.version);
    app.bindText(4, manifest.tag);
    app.bindText(5, owner);
    app.bindText(6, repo);
    app.bindText(7, installPath);
    app.bindText(8, manifest.manifestJson);
    app.bindText(9, manifest.packageSha256);
    app.bindInt64(10, manifest.draft ? 1 : 0);
    if (app.step() != SQLITE_DONE) {
        lastError_ = L"写入已安装应用失败。";
        return false;
    }
    SQLiteStatement clearFiles(db.get(), L"DELETE FROM InstalledAppFiles WHERE AppID=?;");
    SQLiteStatement clearShortcuts(db.get(), L"DELETE FROM InstalledAppShortcuts WHERE AppID=?;");
    clearFiles.bindText(1, manifest.appId);
    clearShortcuts.bindText(1, manifest.appId);
    clearFiles.step();
    clearShortcuts.step();
    for (const auto& file : files) {
        SQLiteStatement insert(db.get(), L"INSERT OR REPLACE INTO InstalledAppFiles(AppID,Path) VALUES(?,?);");
        insert.bindText(1, manifest.appId);
        insert.bindText(2, file);
        insert.step();
    }
    for (const auto& shortcut : shortcuts) {
        SQLiteStatement insert(db.get(), L"INSERT OR REPLACE INTO InstalledAppShortcuts(AppID,Path) VALUES(?,?);");
        insert.bindText(1, manifest.appId);
        insert.bindText(2, shortcut);
        insert.step();
    }
    return true;
}

std::vector<InstalledAppRecord> AppStoreRegistry::LoadInstalledApps() {
    lastError_.clear();
    std::vector<InstalledAppRecord> apps;
    if (!Initialize()) {
        return apps;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(), L"SELECT AppID,Name,Version,ReleaseTag,Owner,Repo,InstallPath,ManifestJson,PackageSha256,Draft FROM InstalledApps ORDER BY Name;");
    if (!statement.ok()) {
        lastError_ = L"读取已安装应用失败。";
        return apps;
    }
    while (statement.step() == SQLITE_ROW) {
        InstalledAppRecord app;
        app.appId = statement.columnText(0);
        app.name = statement.columnText(1);
        app.version = statement.columnText(2);
        app.releaseTag = statement.columnText(3);
        app.owner = statement.columnText(4);
        app.repo = statement.columnText(5);
        app.installPath = statement.columnText(6);
        app.manifestJson = statement.columnText(7);
        app.packageSha256 = statement.columnText(8);
        app.draft = statement.columnInt64(9) != 0;
        apps.push_back(std::move(app));
    }
    return apps;
}

bool AppStoreRegistry::LoadInstalledApp(const std::wstring& appId, InstalledAppRecord& app) {
    lastError_.clear();
    app = {};
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(), L"SELECT AppID,Name,Version,ReleaseTag,Owner,Repo,InstallPath,ManifestJson,PackageSha256,Draft FROM InstalledApps WHERE AppID=?;");
    if (!statement.ok()) {
        lastError_ = L"读取已安装应用失败。";
        return false;
    }
    statement.bindText(1, appId);
    if (statement.step() != SQLITE_ROW) {
        lastError_ = L"应用未安装。";
        return false;
    }
    app.appId = statement.columnText(0);
    app.name = statement.columnText(1);
    app.version = statement.columnText(2);
    app.releaseTag = statement.columnText(3);
    app.owner = statement.columnText(4);
    app.repo = statement.columnText(5);
    app.installPath = statement.columnText(6);
    app.manifestJson = statement.columnText(7);
    app.packageSha256 = statement.columnText(8);
    app.draft = statement.columnInt64(9) != 0;
    return true;
}

std::vector<std::wstring> AppStoreRegistry::LoadInstalledFiles(const std::wstring& appId) {
    std::vector<std::wstring> files;
    if (!Initialize()) {
        return files;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(), L"SELECT Path FROM InstalledAppFiles WHERE AppID=? ORDER BY Path;");
    if (!statement.ok()) {
        lastError_ = L"读取应用文件记录失败。";
        return files;
    }
    statement.bindText(1, appId);
    while (statement.step() == SQLITE_ROW) {
        files.push_back(statement.columnText(0));
    }
    return files;
}

std::vector<std::wstring> AppStoreRegistry::LoadInstalledShortcuts(const std::wstring& appId) {
    std::vector<std::wstring> shortcuts;
    if (!Initialize()) {
        return shortcuts;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement statement(db.get(), L"SELECT Path FROM InstalledAppShortcuts WHERE AppID=? ORDER BY Path;");
    if (!statement.ok()) {
        lastError_ = L"读取应用快捷方式记录失败。";
        return shortcuts;
    }
    statement.bindText(1, appId);
    while (statement.step() == SQLITE_ROW) {
        shortcuts.push_back(statement.columnText(0));
    }
    return shortcuts;
}

bool AppStoreRegistry::DeleteInstalledAppRecord(const std::wstring& appId) {
    lastError_.clear();
    if (!Initialize()) {
        return false;
    }
    SQLiteDatabase db(appDirectory_ / L"db" / L"link.db");
    SQLiteStatement files(db.get(), L"DELETE FROM InstalledAppFiles WHERE AppID=?;");
    SQLiteStatement shortcuts(db.get(), L"DELETE FROM InstalledAppShortcuts WHERE AppID=?;");
    SQLiteStatement app(db.get(), L"DELETE FROM InstalledApps WHERE AppID=?;");
    if (!files.ok() || !shortcuts.ok() || !app.ok()) {
        lastError_ = L"删除安装登记失败。";
        return false;
    }
    files.bindText(1, appId);
    shortcuts.bindText(1, appId);
    app.bindText(1, appId);
    files.step();
    shortcuts.step();
    if (app.step() != SQLITE_DONE) {
        lastError_ = L"写入删除安装登记失败。";
        return false;
    }
    return true;
}
