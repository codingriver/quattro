#include "../src/Storage.h"
#include "../src/Utilities.h"

#include <sqlite3.h>
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {
std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), output.data(), length, nullptr, nullptr);
    return output;
}

void PrintField(const char* key, const std::wstring& value) {
    std::cout << key << "=" << WideToUtf8(value) << "\n";
}

class SQLiteStatement {
public:
    SQLiteStatement(sqlite3* db, const wchar_t* sql) {
        sqlite3_prepare16_v2(db, sql, -1, &statement_, nullptr);
    }

    ~SQLiteStatement() {
        if (statement_) {
            sqlite3_finalize(statement_);
        }
    }

    SQLiteStatement(const SQLiteStatement&) = delete;
    SQLiteStatement& operator=(const SQLiteStatement&) = delete;

    bool ok() const { return statement_ != nullptr; }
    int step() { return sqlite3_step(statement_); }
    int columnInt(int index) const { return sqlite3_column_int(statement_, index); }
    std::wstring columnText(int index) const {
        const void* text = sqlite3_column_text16(statement_, index);
        return text ? static_cast<const wchar_t*>(text) : L"";
    }
    void bindText(int index, const std::wstring& value) {
        sqlite3_bind_text16(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }

private:
    sqlite3_stmt* statement_ = nullptr;
};

bool TableExists(sqlite3* db, const std::wstring& name) {
    SQLiteStatement statement(db, L"SELECT name FROM sqlite_master WHERE type='table' AND name=?;");
    if (!statement.ok()) {
        return false;
    }
    statement.bindText(1, name);
    return statement.step() == SQLITE_ROW;
}

void PrintPluginTables(const std::filesystem::path& appDirectory) {
    sqlite3* db = nullptr;
    const std::filesystem::path dbPath = appDirectory / L"db" / L"link.db";
    if (sqlite3_open16(dbPath.c_str(), &db) != SQLITE_OK || !db) {
        if (db) {
            sqlite3_close(db);
        }
        return;
    }

    if (TableExists(db, L"Plugins")) {
        SQLiteStatement plugins(db,
            L"SELECT ID,Name,Kind,Engine,Builtin,Deletable,Enabled,Installed,InstallPath "
            L"FROM Plugins ORDER BY Builtin DESC,Category,Name;");
        while (plugins.ok() && plugins.step() == SQLITE_ROW) {
            std::cout << "PLUGIN"
                      << "\tid=" << WideToUtf8(plugins.columnText(0))
                      << "\tname=" << WideToUtf8(plugins.columnText(1))
                      << "\tkind=" << WideToUtf8(plugins.columnText(2))
                      << "\tengine=" << WideToUtf8(plugins.columnText(3))
                      << "\tbuiltin=" << plugins.columnInt(4)
                      << "\tdeletable=" << plugins.columnInt(5)
                      << "\tenabled=" << plugins.columnInt(6)
                      << "\tinstalled=" << plugins.columnInt(7)
                      << "\tinstallPath=" << WideToUtf8(plugins.columnText(8))
                      << "\n";
        }
    }

    if (TableExists(db, L"PluginSettings")) {
        SQLiteStatement settings(db, L"SELECT PluginID,Key,Value FROM PluginSettings ORDER BY PluginID,Key;");
        while (settings.ok() && settings.step() == SQLITE_ROW) {
            std::cout << "PLUGIN_SETTING"
                      << "\tplugin=" << WideToUtf8(settings.columnText(0))
                      << "\tkey=" << WideToUtf8(settings.columnText(1))
                      << "\tvalue=" << WideToUtf8(settings.columnText(2))
                      << "\n";
        }
    }

    if (TableExists(db, L"PluginContributions")) {
        SQLiteStatement contributions(db,
            L"SELECT PluginID,ObjectType,ObjectID,ObjectPath FROM PluginContributions ORDER BY PluginID,ObjectType,ObjectID;");
        while (contributions.ok() && contributions.step() == SQLITE_ROW) {
            std::cout << "PLUGIN_CONTRIBUTION"
                      << "\tplugin=" << WideToUtf8(contributions.columnText(0))
                      << "\ttype=" << WideToUtf8(contributions.columnText(1))
                      << "\tid=" << contributions.columnInt(2)
                      << "\tpath=" << WideToUtf8(contributions.columnText(3))
                      << "\n";
        }
    }

    sqlite3_close(db);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << "usage: QuattroDbProbe <app-directory>\n";
        return 2;
    }

    StorageService storage{std::filesystem::path(argv[1])};
    AppModel model = storage.Load();
    int majorGroups = 0;
    int tags = 0;
    for (const auto& group : model.groups) {
        if (group.parentGroup == 0) {
            ++majorGroups;
        } else {
            ++tags;
        }
    }

    std::cout << "sqlite=" << (storage.sqliteAvailable() ? 1 : 0) << "\n";
    std::cout << "groups=" << model.groups.size() << "\n";
    std::cout << "major_groups=" << majorGroups << "\n";
    std::cout << "tags=" << tags << "\n";
    std::cout << "links=" << model.links.size() << "\n";

    for (const auto& group : model.groups) {
        std::cout << "GROUP"
                  << "\tid=" << group.id
                  << "\tparent=" << group.parentGroup
                  << "\tpos=" << group.pos
                  << "\ttype=" << group.type
                  << "\tsort=" << group.sort
                  << "\tlayout=" << group.layout
                  << "\ticonSize=" << group.iconSize
                  << "\tname=" << WideToUtf8(group.name)
                  << "\n";
    }

    for (const auto& link : model.links) {
        std::cout << "LINK"
                  << "\tid=" << link.id
                  << "\tparent=" << link.parentGroup
                  << "\tpos=" << link.pos
                  << "\ttype=" << link.type
                  << "\trunCount=" << link.runCount
                  << "\tshowCmd=" << link.showCmd
                  << "\tname=" << WideToUtf8(link.name)
                  << "\tpath=" << WideToUtf8(link.path)
                  << "\tremark=" << WideToUtf8(link.remark)
                  << "\n";
    }

    if (!storage.lastError().empty()) {
        PrintField("message", storage.lastError());
    }
    PrintPluginTables(std::filesystem::path(argv[1]));
    return storage.sqliteAvailable() ? 0 : 1;
}
