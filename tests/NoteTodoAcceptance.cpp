#include "../src/services/ConfigPackageService.h"
#include "../src/domain/MenuCatalog.h"
#include "../src/services/Storage.h"
#include "../src/domain/TodoSchedule.h"
#include "../src/common/Utilities.h"

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

bool SetPackageVersion(const std::filesystem::path& packagePath, int version) {
    sqlite3* db = nullptr;
    if (sqlite3_open16(packagePath.c_str(), &db) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return false;
    }
    const std::string sql = "UPDATE PackageManifest SET Value='" + std::to_string(version) + "' WHERE Key='formatVersion';";
    const bool ok = Exec(db, sql.c_str());
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
    Check(NormalizeTodoTimestamp(L"2026/06/26") == L"2026-06-26 00:00:00", "acceptance timestamp date-only normalize");
    Check(NormalizeTodoTimestamp(L"2026-02-29 09:00").empty(), "acceptance timestamp rejects invalid date");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::None, L"2026-06-26 09:30", L"2026-06-26 10:00").empty(), "acceptance no-time next due empty");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Once, L"2026-06-26 09:30", L"2030-01-01 00:00") == L"2026-06-26 09:30:00", "acceptance once keeps anchor");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Secondly, 10, L"2026-06-26 09:30:05", L"2026-06-26 09:30:12") == L"2026-06-26 09:30:15", "acceptance second interval next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Daily, L"2026-06-26 09:30", L"2026-06-26 10:00") == L"2026-06-27 09:30:00", "acceptance daily next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Weekly, L"2026-06-26 09:30", L"2026-06-27 00:00") == L"2026-07-03 09:30:00", "acceptance weekly next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Monthly, L"2024-01-31 09:00", L"2024-02-01 00:00") == L"2024-02-29 09:00:00", "acceptance monthly clamps February");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Monthly, L"2024-01-31 09:00", L"2024-03-01 00:00") == L"2024-03-31 09:00:00", "acceptance monthly returns to anchor day");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Yearly, L"2024-02-29 09:00", L"2025-01-01 00:00") == L"2025-02-28 09:00:00", "acceptance yearly clamps non-leap");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Yearly, L"2024-02-29 09:00", L"2027-03-01 00:00") == L"2028-02-29 09:00:00", "acceptance yearly returns to leap day");
    Check(IsValidTodoCronExpression(L"0 30 9 * * *") && ComputeNextTodoCronDueAt(L"0 30 9 * * *", L"2026-06-26 09:00:00") == L"2026-06-26 09:30:00", "acceptance cron daily next due");

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
    Check(storage.InsertTodoItem(once) && once.nextDueAt == L"2026-06-26 09:30:00" && once.enabled, "acceptance insert once todo");
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
    recurring.repeatMode = TodoRepeatMode::FixedPoint;
    recurring.repeatInterval = 1;
    recurring.repeatLimit = 2;
    recurring.anchorAt = L"2024-01-31 09:00";
    recurring.pos = -1;
    Check(storage.InsertTodoItem(recurring) && recurring.nextDueAt >= CurrentTodoTimestamp(), "acceptance insert recurring next due after now");
    Check(storage.SetTodoCompleted(recurring.id, true), "acceptance complete recurring todo advances");
    Check(storage.SetTodoCompleted(recurring.id, true), "acceptance complete recurring todo reaches repeat limit");

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
    Check(loadedRecurring && !loadedRecurring->completedAt.empty() && loadedRecurring->nextDueAt.empty() && loadedRecurring->repeatFinished == 2 && loadedRecurring->repeatMode == TodoRepeatMode::FixedPoint, "acceptance reload recurring stops at repeat limit");
    Check(!todoTag.groupUid.empty() && loadedOnce && !loadedOnce->todoUid.empty() && !loadedOnce->mergeUpdatedAtUtc.empty(), "acceptance stable todo and group identities");

    Check(storage.DeleteTodoItem(noTime.id), "acceptance delete todo");
    loaded = storage.Load();
    Check(!FindTodoByTitle(loaded, L"NoTime"), "acceptance deleted todo gone");
    Check(storage.DeleteGroup(noteTag.id), "acceptance delete note tag");
    loaded = storage.Load();
    Check(!FindNoteByTag(loaded, noteTag.id), "acceptance note tag cascade delete");
    Check(storage.DeleteGroup(group.id), "acceptance delete group tree");
    loaded = storage.Load();
    Check(!FindGroupByName(loaded, L"AcceptanceTodo") && !FindTodoByTitle(loaded, L"OnceEdited"), "acceptance group cascade deletes todo tag items");
    Check(loaded.todoTombstones.size() >= 3, "acceptance direct and group deletes create tombstones");
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
    Check(legacyTodo && legacyTodo->enabled && legacyTodo->repeatMode == TodoRepeatMode::FixedPoint && legacyTodo->nextDueAt == L"2026-06-26 09:30", "acceptance legacy todo default enabled after migration");
    Check(legacyTodo && !legacyTodo->todoUid.empty() && !legacyTodo->mergeUpdatedAtUtc.empty(), "acceptance legacy todo identity backfilled");
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

    ConfigPackageReport repeatImport = importer.ImportPackageMerge(packageFile, options);
    imported = targetStorage.Load();
    Check(repeatImport.ok && repeatImport.todosAdded == 0 && repeatImport.todosSkippedIdentical == 1 && imported.todos.size() == 1,
        "acceptance repeated v2 package import is idempotent");

    ConfigPackageMergePreview stalePreview = importer.PreviewPackageMerge(packageFile, options);
    Group staleStateGroup;
    staleStateGroup.name = L"PreviewStateChanged";
    staleStateGroup.parentGroup = 0;
    staleStateGroup.pos = -1;
    Check(targetStorage.InsertGroup(staleStateGroup), "acceptance mutate state after merge preview");
    ConfigPackageReport staleApply = importer.ApplyPackageMerge(
        packageFile, options, TodoRestorePolicy::KeepDeleted, stalePreview.stateToken);
    Check(!staleApply.ok && staleApply.message.find(L"重新预览") != std::wstring::npos,
        "acceptance stale merge preview rejected");
    Check(targetStorage.DeleteGroup(staleStateGroup.id), "acceptance cleanup stale preview state group");

    Sleep(5);
    AppModel updatedSource = sourceStorage.Load();
    TodoItem remoteUpdated = *FindTodoByTitle(updatedSource, L"PackageDisabledTodo");
    remoteUpdated.title = L"PackageRemoteNewer";
    remoteUpdated.content = L"remote newer content";
    Check(sourceStorage.UpdateTodoItem(remoteUpdated), "acceptance update remote todo");
    exportReport = exporter.ExportPackage(packageFile, options);
    Check(exportReport.ok, "acceptance re-export updated package");
    ConfigPackageReport remoteNewerImport = importer.ImportPackageMerge(packageFile, options);
    imported = targetStorage.Load();
    importedTodo = FindTodoByTitle(imported, L"PackageRemoteNewer");
    Check(remoteNewerImport.ok && remoteNewerImport.todosUpdatedFromRemote == 1 && importedTodo && importedTodo->content == L"remote newer content",
        "acceptance remote newer todo wins");

    Sleep(5);
    TodoItem localNewer = *importedTodo;
    localNewer.title = L"PackageLocalNewer";
    localNewer.content = L"local newer content";
    Check(targetStorage.UpdateTodoItem(localNewer), "acceptance update local todo");
    ConfigPackageReport localNewerImport = importer.ImportPackageMerge(packageFile, options);
    imported = targetStorage.Load();
    Check(localNewerImport.ok && localNewerImport.todosKeptLocal == 1 && FindTodoByTitle(imported, L"PackageLocalNewer"),
        "acceptance local newer todo wins");

    const TodoItem* deletedCandidate = FindTodoByTitle(imported, L"PackageLocalNewer");
    Check(deletedCandidate && targetStorage.DeleteTodoItem(deletedCandidate->id), "acceptance delete imported todo creates merge tombstone");
    ConfigPackageMergePreview keepPreview = importer.PreviewPackageMerge(packageFile, options);
    Check(keepPreview.ok && keepPreview.deletedTodoTitles.size() == 1, "acceptance preview reports locally deleted remote todo");
    ConfigPackageReport keepDeleted = importer.ApplyPackageMerge(
        packageFile, options, TodoRestorePolicy::KeepDeleted, keepPreview.stateToken);
    imported = targetStorage.Load();
    Check(keepDeleted.ok && keepDeleted.todosKeptDeleted == 1 && imported.todos.empty(),
        "acceptance keep deleted policy skips restoration");

    ConfigPackageMergePreview restorePreview = importer.PreviewPackageMerge(packageFile, options);
    ConfigPackageReport restoreDeleted = importer.ApplyPackageMerge(
        packageFile, options, TodoRestorePolicy::RestoreDeleted, restorePreview.stateToken);
    imported = targetStorage.Load();
    Check(restoreDeleted.ok && restoreDeleted.todosRestored == 1 && FindTodoByTitle(imported, L"PackageRemoteNewer") && imported.todoTombstones.empty(),
        "acceptance restore deleted policy restores remote todo");

    TodoItem sameTextDifferentIdentity = remoteUpdated;
    sameTextDifferentIdentity.id = 0;
    sameTextDifferentIdentity.todoUid.clear();
    sameTextDifferentIdentity.mergeUpdatedAtUtc.clear();
    Check(sourceStorage.InsertTodoItem(sameTextDifferentIdentity), "acceptance insert same text with different identity");
    Check(exporter.ExportPackage(packageFile, options).ok, "acceptance export distinct identity package");
    ConfigPackageReport distinctIdentityImport = importer.ImportPackageMerge(packageFile, options);
    imported = targetStorage.Load();
    Check(distinctIdentityImport.ok && distinctIdentityImport.todosAdded == 1 && imported.todos.size() == 2,
        "acceptance same text different uuid remains distinct");

    Check(sourceStorage.DeleteTodoItem(remoteUpdated.id), "acceptance source delete creates remote tombstone");
    Check(exporter.ExportPackage(packageFile, options).ok, "acceptance export remote tombstone");
    ConfigPackageReport remoteDeleteImport = importer.ImportPackageMerge(packageFile, options);
    imported = targetStorage.Load();
    Check(remoteDeleteImport.ok && remoteDeleteImport.todosRemoteDeleteConflicts == 1 && imported.todos.size() == 2,
        "acceptance remote tombstone does not delete local todo");

    const std::filesystem::path legacyPackageFile = std::filesystem::temp_directory_path() / L"quattro_note_todo_acceptance_v1.q4cfg";
    const std::filesystem::path legacyPackageTarget = std::filesystem::temp_directory_path() / L"quattro_note_todo_package_v1_target";
    std::filesystem::remove(legacyPackageFile, ec);
    std::filesystem::remove_all(legacyPackageTarget, ec);
    std::filesystem::copy_file(packageFile, legacyPackageFile, std::filesystem::copy_options::overwrite_existing, ec);
    Check(!ec && SetPackageVersion(legacyPackageFile, 1), "acceptance prepare v1 package");
    StorageService legacyPackageTargetStorage(legacyPackageTarget);
    legacyPackageTargetStorage.Load();
    ConfigPackageService legacyPackageImporter(legacyPackageTarget);
    ConfigPackageReport firstLegacyImport = legacyPackageImporter.ImportPackageMerge(legacyPackageFile, options);
    ConfigPackageReport secondLegacyImport = legacyPackageImporter.ImportPackageMerge(legacyPackageFile, options);
    AppModel legacyPackageImported = legacyPackageTargetStorage.Load();
    Check(firstLegacyImport.ok && secondLegacyImport.ok && secondLegacyImport.todosAdded == 0 &&
        secondLegacyImport.todosSkippedIdentical == 1 && legacyPackageImported.todos.size() == 1,
        "acceptance repeated v1 package import is idempotent");
    std::filesystem::remove(legacyPackageFile, ec);
    std::filesystem::remove_all(legacyPackageTarget, ec);
    std::filesystem::remove_all(packageSource, ec);
    std::filesystem::remove_all(packageTarget, ec);
    std::filesystem::remove(packageFile, ec);

    Check(MenuIconFor(ID_MENU_ADD_NOTE_TAG, L"新建便签") == MenuIconNotebook, "acceptance note tag icon");
    Check(MenuIconFor(ID_MENU_ADD_TODO_TAG, L"新建待办事项标签页") == MenuIconList, "acceptance todo tag icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"禁用待办事项") == MenuIconEyeOff, "acceptance todo disable icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"启用待办事项") == MenuIconEye, "acceptance todo enable icon");
    Check(MenuIconFor(ID_MENU_TODO_SORT_DUE, L"按提醒时间（推荐）") == MenuIconSort, "acceptance todo sort icon");

    if (failures == 0) {
        std::cout << "note_todo_acceptance=passed\n";
    }
    return failures == 0 ? 0 : 1;
}
