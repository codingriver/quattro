#include "../src/ConfigPackageService.h"
#include "../src/MenuCatalog.h"
#include "../src/Storage.h"
#include "../src/TodoSchedule.h"
#include "../src/Utilities.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAILED: " << name << "\n";
        ++failures;
    }
}

bool Exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite exec failed: " << (message ? message : "") << "\n";
        sqlite3_free(message);
        return false;
    }
    return true;
}

bool CreateLegacyDatabase(const std::filesystem::path& appRoot) {
    std::error_code ec;
    std::filesystem::create_directories(appRoot / L"db", ec);
    sqlite3* db = nullptr;
    if (sqlite3_open16((appRoot / L"db" / L"link.db").c_str(), &db) != SQLITE_OK || !db) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }
    const bool ok = Exec(db,
        "CREATE TABLE Version(ID INTEGER PRIMARY KEY, Ver INTEGER);"
        "INSERT INTO Version(ID,Ver) VALUES(1,19999);"
        "CREATE TABLE Groups("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        "NAME TEXT NOT NULL,"
        "TYPE INTEGER DEFAULT 0,"
        "SORT INTEGER DEFAULT 0,"
        "POS INTEGER DEFAULT 0,"
        "ParentGroup INTEGER DEFAULT 0,"
        "ICON TEXT,"
        "LAYOUT INTEGER DEFAULT 0,"
        "ICONSIZE INTEGER DEFAULT 0,"
        "FLAG INTEGER DEFAULT 0,"
        "Content TEXT);"
        "CREATE TABLE Links("
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
        "CustomColor TEXT,"
        "Remark TEXT,"
        "Pidl BLOB);"
        "CREATE TABLE NotePages(TagId INTEGER PRIMARY KEY,Content TEXT NOT NULL DEFAULT '',UpdatedAt TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE TodoItems("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        "TagId INTEGER NOT NULL,"
        "Title TEXT NOT NULL,"
        "Content TEXT NOT NULL DEFAULT '',"
        "ScheduleKind INTEGER DEFAULT 0,"
        "AnchorAt TEXT NOT NULL DEFAULT '',"
        "NextDueAt TEXT NOT NULL DEFAULT '',"
        "CompletedAt TEXT NOT NULL DEFAULT '',"
        "POS INTEGER DEFAULT 0,"
        "CreatedAt TEXT NOT NULL DEFAULT '',"
        "UpdatedAt TEXT NOT NULL DEFAULT '');"
        "INSERT INTO Groups(ID,NAME,TYPE,SORT,POS,ParentGroup,ICON,LAYOUT,ICONSIZE,FLAG,Content) VALUES"
        "(100,'LegacyGroup',0,0,0,0,'',0,0,0,''),"
        "(101,'LegacyNote',3,0,0,100,'',0,0,0,'note'),"
        "(102,'LegacyTodoItems',4,0,1,100,'',0,0,0,'todoItems'),"
        "(103,'LegacyTodoFilter',2,0,2,100,'',0,0,0,'todo');"
        "INSERT INTO NotePages(TagId,Content,UpdatedAt) VALUES(101,'legacy note','2026-06-26 09:00');"
        "INSERT INTO TodoItems(ID,TagId,Title,Content,ScheduleKind,AnchorAt,NextDueAt,CompletedAt,POS,CreatedAt,UpdatedAt) "
        "VALUES(200,102,'legacy todo','legacy content',1,'2026-06-26 09:30','2026-06-26 09:30','',0,'2026-06-26 09:00','2026-06-26 09:00');");
    sqlite3_close(db);
    return ok;
}

const Group* FindGroupByName(const AppModel& model, const std::wstring& name) {
    for (const auto& group : model.groups) {
        if (group.name == name) {
            return &group;
        }
    }
    return nullptr;
}

const TodoItem* FindTodoByTitle(const AppModel& model, const std::wstring& title) {
    for (const auto& item : model.todos) {
        if (item.title == title) {
            return &item;
        }
    }
    return nullptr;
}

const NotePage* FindNoteByTag(const AppModel& model, int tagId) {
    for (const auto& note : model.notes) {
        if (note.tagId == tagId) {
            return &note;
        }
    }
    return nullptr;
}

void CheckScheduleRules() {
    Check(NormalizeTodoTimestamp(L"2026/06/26") == L"2026-06-26 00:00", "acceptance timestamp date-only normalize");
    Check(NormalizeTodoTimestamp(L"2026-02-29 09:00").empty(), "acceptance timestamp rejects invalid date");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::None, L"2026-06-26 09:30", L"2026-06-26 10:00").empty(), "acceptance no-time next due empty");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Once, L"2026-06-26 09:30", L"2030-01-01 00:00") == L"2026-06-26 09:30", "acceptance once keeps anchor");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Daily, L"2026-06-26 09:30", L"2026-06-26 10:00") == L"2026-06-27 09:30", "acceptance daily next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Weekly, L"2026-06-26 09:30", L"2026-06-27 00:00") == L"2026-07-03 09:30", "acceptance weekly next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Monthly, L"2024-01-31 09:00", L"2024-02-01 00:00") == L"2024-02-29 09:00", "acceptance monthly clamps February");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Monthly, L"2024-01-31 09:00", L"2024-03-01 00:00") == L"2024-03-31 09:00", "acceptance monthly returns to anchor day");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Yearly, L"2024-02-29 09:00", L"2025-01-01 00:00") == L"2025-02-28 09:00", "acceptance yearly clamps non-leap");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Yearly, L"2024-02-29 09:00", L"2027-03-01 00:00") == L"2028-02-29 09:00", "acceptance yearly returns to leap day");
}
}

int wmain() {
    std::error_code ec;
    CheckScheduleRules();

    const std::filesystem::path root = std::filesystem::temp_directory_path() / L"quattro_note_todo_acceptance";
    std::filesystem::remove_all(root, ec);
    StorageService storage(root);
    AppModel initial = storage.Load();
    Check(storage.sqliteAvailable(), "acceptance sqlite available");

    Group group;
    group.name = L"AcceptanceGroup";
    group.parentGroup = 0;
    group.pos = -1;
    Check(storage.InsertGroup(group) && group.id > 0, "acceptance create group");

    Group noteTag;
    noteTag.name = L"AcceptanceNote";
    noteTag.parentGroup = group.id;
    noteTag.type = 3;
    noteTag.content = L"note";
    noteTag.pos = -1;
    Check(storage.InsertGroup(noteTag) && noteTag.id > 0, "acceptance create note tag");
    Check(storage.SaveNotePage(noteTag.id, L"first line\r\nsecond line"), "acceptance save note multiline");
    Check(storage.SaveNotePage(noteTag.id, L"updated note\nthird line"), "acceptance update note replaces content");
    noteTag.name = L"RenamedNote";
    Check(storage.UpdateGroup(noteTag), "acceptance rename note tag");

    Group todoTag;
    todoTag.name = L"AcceptanceTodo";
    todoTag.parentGroup = group.id;
    todoTag.type = 4;
    todoTag.content = L"todoItems";
    todoTag.sort = 0;
    todoTag.pos = -1;
    Check(storage.InsertGroup(todoTag) && todoTag.id > 0, "acceptance create todo tag");

    TodoItem invalid;
    invalid.tagId = todoTag.id;
    invalid.scheduleKind = TodoScheduleKind::Once;
    invalid.anchorAt = L"2026-06-26 09:30";
    Check(!storage.InsertTodoItem(invalid), "acceptance rejects blank todo title");

    TodoItem noTime;
    noTime.tagId = todoTag.id;
    noTime.title = L"NoTime";
    noTime.content = L"no time content";
    noTime.enabled = false;
    noTime.scheduleKind = TodoScheduleKind::None;
    noTime.anchorAt = L"2026-06-26 09:30";
    noTime.nextDueAt = L"2026-06-26 09:30";
    noTime.pos = -1;
    Check(storage.InsertTodoItem(noTime) && noTime.id > 0 && noTime.anchorAt.empty() && noTime.nextDueAt.empty() && !noTime.enabled, "acceptance insert disabled no-time todo");

    TodoItem once;
    once.tagId = todoTag.id;
    once.title = L"Once";
    once.content = L"once content";
    once.scheduleKind = TodoScheduleKind::Once;
    once.anchorAt = L"2026/06/26 09:30";
    once.pos = -1;
    Check(storage.InsertTodoItem(once) && once.nextDueAt == L"2026-06-26 09:30" && once.enabled, "acceptance insert once todo");
    once.title = L"OnceEdited";
    once.content = L"edited content";
    once.enabled = false;
    Check(storage.UpdateTodoItem(once), "acceptance edit todo including disabled state");
    Check(storage.SetTodoEnabled(once.id, true), "acceptance enable todo");
    Check(storage.SetTodoCompleted(once.id, true), "acceptance complete once todo");

    TodoItem recurring;
    recurring.tagId = todoTag.id;
    recurring.title = L"Recurring";
    recurring.scheduleKind = TodoScheduleKind::Monthly;
    recurring.anchorAt = L"2024-01-31 09:00";
    recurring.pos = -1;
    Check(storage.InsertTodoItem(recurring) && recurring.nextDueAt >= CurrentTodoTimestamp(), "acceptance insert recurring next due after now");
    const std::wstring recurringBeforeComplete = recurring.nextDueAt;
    Check(storage.SetTodoCompleted(recurring.id, true), "acceptance complete recurring todo advances");

    AppModel loaded = storage.Load();
    const Group* renamedNote = FindGroupByName(loaded, L"RenamedNote");
    Check(renamedNote && renamedNote->type == 3 && renamedNote->content == L"note", "acceptance renamed note keeps special type");
    const NotePage* note = renamedNote ? FindNoteByTag(loaded, renamedNote->id) : nullptr;
    Check(note && note->content == L"updated note\nthird line" && !note->updatedAt.empty(), "acceptance reload note content");
    const TodoItem* loadedNoTime = FindTodoByTitle(loaded, L"NoTime");
    Check(loadedNoTime && !loadedNoTime->enabled && loadedNoTime->anchorAt.empty() && loadedNoTime->nextDueAt.empty(), "acceptance reload disabled no-time todo");
    const TodoItem* loadedOnce = FindTodoByTitle(loaded, L"OnceEdited");
    Check(loadedOnce && loadedOnce->enabled && !loadedOnce->completedAt.empty() && loadedOnce->content == L"edited content", "acceptance reload completed enabled todo");
    const TodoItem* loadedRecurring = FindTodoByTitle(loaded, L"Recurring");
    Check(loadedRecurring && loadedRecurring->completedAt.empty() && loadedRecurring->nextDueAt >= recurringBeforeComplete, "acceptance reload recurring remains open");

    Check(storage.DeleteTodoItem(noTime.id), "acceptance delete todo");
    loaded = storage.Load();
    Check(!FindTodoByTitle(loaded, L"NoTime"), "acceptance deleted todo gone");
    Check(storage.DeleteGroup(noteTag.id), "acceptance delete note tag");
    loaded = storage.Load();
    Check(!FindNoteByTag(loaded, noteTag.id), "acceptance note tag cascade delete");
    Check(storage.DeleteGroup(group.id), "acceptance delete group tree");
    loaded = storage.Load();
    Check(!FindGroupByName(loaded, L"AcceptanceTodo") && !FindTodoByTitle(loaded, L"OnceEdited"), "acceptance group cascade deletes todo tag items");
    std::filesystem::remove_all(root, ec);

    const std::filesystem::path legacyRoot = std::filesystem::temp_directory_path() / L"quattro_note_todo_legacy_acceptance";
    std::filesystem::remove_all(legacyRoot, ec);
    Check(CreateLegacyDatabase(legacyRoot), "acceptance create legacy database");
    StorageService legacyStorage(legacyRoot);
    AppModel legacy = legacyStorage.Load();
    const Group* legacyFilter = FindGroupByName(legacy, L"LegacyTodoFilter");
    const TodoItem* legacyTodo = FindTodoByTitle(legacy, L"legacy todo");
    const Group* legacyNote = FindGroupByName(legacy, L"LegacyNote");
    Check(legacyFilter && legacyFilter->type == 2 && legacyFilter->content == L"todo", "acceptance legacy type2 todo filter unchanged");
    Check(legacyTodo && legacyTodo->enabled && legacyTodo->nextDueAt == L"2026-06-26 09:30", "acceptance legacy todo default enabled after migration");
    Check(legacyNote && FindNoteByTag(legacy, legacyNote->id), "acceptance legacy note survives migration");
    Check(legacyStorage.SetTodoEnabled(200, false), "acceptance migrated enabled column writable");
    legacy = legacyStorage.Load();
    legacyTodo = FindTodoByTitle(legacy, L"legacy todo");
    Check(legacyTodo && !legacyTodo->enabled, "acceptance migrated enabled persisted");
    std::filesystem::remove_all(legacyRoot, ec);

    const std::filesystem::path packageSource = std::filesystem::temp_directory_path() / L"quattro_note_todo_package_source";
    const std::filesystem::path packageTarget = std::filesystem::temp_directory_path() / L"quattro_note_todo_package_target";
    const std::filesystem::path packageFile = std::filesystem::temp_directory_path() / L"quattro_note_todo_acceptance.q4cfg";
    std::filesystem::remove_all(packageSource, ec);
    std::filesystem::remove_all(packageTarget, ec);
    std::filesystem::remove(packageFile, ec);
    StorageService sourceStorage(packageSource);
    sourceStorage.Load();
    Group packageGroup;
    packageGroup.name = L"PackageGroup";
    packageGroup.parentGroup = 0;
    packageGroup.pos = -1;
    Check(sourceStorage.InsertGroup(packageGroup), "acceptance package create group");
    Group packageTodoTag;
    packageTodoTag.name = L"PackageTodo";
    packageTodoTag.parentGroup = packageGroup.id;
    packageTodoTag.type = 4;
    packageTodoTag.content = L"todoItems";
    packageTodoTag.pos = -1;
    Check(sourceStorage.InsertGroup(packageTodoTag), "acceptance package create todo tag");
    TodoItem packageTodo;
    packageTodo.tagId = packageTodoTag.id;
    packageTodo.title = L"PackageDisabledTodo";
    packageTodo.enabled = false;
    packageTodo.scheduleKind = TodoScheduleKind::Once;
    packageTodo.anchorAt = L"2026-06-26 09:30";
    packageTodo.pos = -1;
    Check(sourceStorage.InsertTodoItem(packageTodo), "acceptance package insert disabled todo");
    ConfigPackageService exporter(packageSource);
    ConfigPackageOptions options;
    ConfigPackageReport exportReport = exporter.ExportPackage(packageFile, options);
    Check(exportReport.ok, "acceptance export package");
    StorageService targetStorage(packageTarget);
    targetStorage.Load();
    ConfigPackageService importer(packageTarget);
    ConfigPackageReport importReport = importer.ImportPackageMerge(packageFile, options);
    Check(importReport.ok && importReport.todosAdded == 1, "acceptance import package todo");
    AppModel imported = targetStorage.Load();
    const TodoItem* importedTodo = FindTodoByTitle(imported, L"PackageDisabledTodo");
    Check(importedTodo && !importedTodo->enabled, "acceptance package preserves todo enabled state");
    std::filesystem::remove_all(packageSource, ec);
    std::filesystem::remove_all(packageTarget, ec);
    std::filesystem::remove(packageFile, ec);

    Check(MenuIconFor(ID_MENU_ADD_NOTE_TAG, L"新建便签标签页") == MenuIconNotebook, "acceptance note tag icon");
    Check(MenuIconFor(ID_MENU_ADD_TODO_TAG, L"新建待办事项标签页") == MenuIconList, "acceptance todo tag icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"禁用待办事项") == MenuIconEyeOff, "acceptance todo disable icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"启用待办事项") == MenuIconEye, "acceptance todo enable icon");
    Check(MenuIconFor(ID_MENU_TODO_SORT_DUE, L"按提醒时间（推荐）") == MenuIconSort, "acceptance todo sort icon");

    if (failures == 0) {
        std::cout << "note_todo_acceptance=passed\n";
    }
    return failures == 0 ? 0 : 1;
}
