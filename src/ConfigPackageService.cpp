#include "ConfigPackageService.h"

#include "Config.h"
#include "PluginRegistry.h"
#include "Storage.h"
#include "Utilities.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace {
constexpr int kPackageFormatVersion = 1;

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

std::wstring UniqueGroupNameForParent(const AppModel& model, const std::wstring& sourceName, int parentGroup, const std::unordered_map<int, int>& pendingParents) {
    auto exists = [&](const std::wstring& name) {
        for (const auto& group : model.groups) {
            if (group.parentGroup == parentGroup && group.name == name) {
                return true;
            }
        }
        for (const auto& item : pendingParents) {
            (void)item;
        }
        return false;
    };

    std::wstring name = Trim(sourceName).empty() ? L"导入项目" : Trim(sourceName);
    if (!exists(name)) {
        return name;
    }
    std::wstring base = name + L" (导入";
    for (int index = 1; index < 1000; ++index) {
        std::wstring candidate = index == 1 ? name + L" (导入)" : base + std::to_wstring(index) + L")";
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return name + L" (导入)";
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

bool TableExists(sqlite3* db, const wchar_t* table) {
    SQLiteStatement statement(db, L"SELECT name FROM sqlite_master WHERE type='table' AND name=?;");
    if (!statement.ok()) {
        return false;
    }
    statement.bindText(1, table);
    return statement.step() == SQLITE_ROW;
}

void MergePluginSettings(sqlite3* sourceDb, sqlite3* targetDb, ConfigPackageReport& report) {
    if (!TableExists(sourceDb, L"PluginSettings") || !TableExists(targetDb, L"PluginSettings")) {
        return;
    }
    SQLiteStatement query(sourceDb, L"SELECT PluginID,Key,Value FROM PluginSettings ORDER BY PluginID,Key;");
    if (!query.ok()) {
        report.warnings.push_back(L"读取导入包工具设置失败。");
        return;
    }
    while (query.step() == SQLITE_ROW) {
        const std::wstring pluginId = query.columnText(0);
        const std::wstring key = query.columnText(1);
        const std::wstring value = query.columnText(2);

        SQLiteStatement exists(targetDb, L"SELECT Value FROM PluginSettings WHERE PluginID=? AND Key=?;");
        if (!exists.ok()) {
            report.warnings.push_back(L"检查工具设置冲突失败。");
            return;
        }
        exists.bindText(1, pluginId);
        exists.bindText(2, key);
        if (exists.step() == SQLITE_ROW) {
            continue;
        }

        SQLiteStatement insert(targetDb, L"INSERT INTO PluginSettings(PluginID,Key,Value) VALUES(?,?,?);");
        if (!insert.ok()) {
            report.warnings.push_back(L"写入工具设置失败。");
            return;
        }
        insert.bindText(1, pluginId);
        insert.bindText(2, key);
        insert.bindText(3, value);
        if (insert.step() == SQLITE_DONE) {
            ++report.pluginSettingsAdded;
        }
    }
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

ConfigPackageReport MergeImportedData(const std::filesystem::path& appDirectory, const std::filesystem::path& importRoot, const ConfigPackageOptions& options) {
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

    std::unordered_map<int, int> groupIdMap;
    std::vector<Group> addedGroups;
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

    std::unordered_map<int, int> emptyParents;
    for (const auto& sourceGroup : sourceMajorGroups) {
        Group group = sourceGroup;
        group.id = 0;
        group.parentGroup = 0;
        group.pos = -1;
        group.name = UniqueGroupNameForParent(targetModel, group.name, 0, emptyParents);
        if (!targetStorage.InsertGroup(group)) {
            report.message = L"导入分组失败: " + targetStorage.lastError();
            return report;
        }
        groupIdMap[sourceGroup.id] = group.id;
        targetModel.groups.push_back(group);
        addedGroups.push_back(group);
        ++report.groupsAdded;
    }

    for (const auto& sourceTag : sourceTags) {
        const auto parent = groupIdMap.find(sourceTag.parentGroup);
        if (parent == groupIdMap.end()) {
            report.warnings.push_back(L"跳过缺少父分组的标签: " + sourceTag.name);
            continue;
        }
        Group tag = sourceTag;
        tag.id = 0;
        tag.parentGroup = parent->second;
        tag.pos = -1;
        tag.name = UniqueGroupNameForParent(targetModel, tag.name, tag.parentGroup, emptyParents);
        if (!targetStorage.InsertGroup(tag)) {
            report.message = L"导入标签失败: " + targetStorage.lastError();
            return report;
        }
        groupIdMap[sourceTag.id] = tag.id;
        targetModel.groups.push_back(tag);
        addedGroups.push_back(tag);
        ++report.tagsAdded;
    }

    for (auto sourceLink : sourceModel.links) {
        const auto parent = groupIdMap.find(sourceLink.parentGroup);
        if (parent == groupIdMap.end()) {
            report.warnings.push_back(L"跳过缺少父标签的启动项: " + sourceLink.name);
            continue;
        }
        sourceLink.id = 0;
        sourceLink.parentGroup = parent->second;
        sourceLink.pos = -1;
        if (!targetStorage.InsertLink(sourceLink)) {
            report.message = L"导入启动项失败: " + targetStorage.lastError();
            return report;
        }
        ++report.linksAdded;
    }

    for (const auto& sourceNote : sourceModel.notes) {
        const auto tag = groupIdMap.find(sourceNote.tagId);
        if (tag == groupIdMap.end()) {
            continue;
        }
        if (!targetStorage.SaveNotePage(tag->second, sourceNote.content)) {
            report.message = L"导入便签失败: " + targetStorage.lastError();
            return report;
        }
        ++report.notesAdded;
    }

    for (auto sourceTodo : sourceModel.todos) {
        const auto tag = groupIdMap.find(sourceTodo.tagId);
        if (tag == groupIdMap.end()) {
            continue;
        }
        sourceTodo.id = 0;
        sourceTodo.tagId = tag->second;
        sourceTodo.pos = -1;
        if (!targetStorage.InsertTodoItem(sourceTodo)) {
            report.message = L"导入待办失败: " + targetStorage.lastError();
            return report;
        }
        ++report.todosAdded;
    }

    if (options.includePluginSettings) {
        PluginRegistry targetPlugins(appDirectory);
        targetPlugins.Initialize();
        SQLiteDatabase sourceDb(importRoot / L"db" / L"link.db");
        SQLiteDatabase targetDb(appDirectory / L"db" / L"link.db");
        if (sourceDb.ok() && targetDb.ok()) {
            MergePluginSettings(sourceDb.get(), targetDb.get(), report);
        }
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
    ConfigPackageReport report;
    SQLiteDatabase packageDb(packagePath);
    if (!packageDb.ok()) {
        report.message = packageDb.Error();
        return report;
    }
    if (GetManifest(packageDb.get(), L"app") != L"Quattro" ||
        GetManifest(packageDb.get(), L"formatVersion") != std::to_wstring(kPackageFormatVersion)) {
        report.message = L"不是有效的 Quattro快速启动器 配置包。";
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
        report = MergeImportedData(appDirectory_, tempDirectory, options);
        report.warnings.insert(report.warnings.begin(), backupWarnings.begin(), backupWarnings.end());
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
