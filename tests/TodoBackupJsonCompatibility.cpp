#include "../src/common/JsonValue.h"
#include "../src/common/Utilities.h"
#include "../src/services/Storage.h"
#include "../src/services/TodoJsonBackupService.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
bool Require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        return false;
    }
    return true;
}

bool ValidateFixture(const std::filesystem::path& path) {
    std::wstring error;
    JsonValue root;
    if (!ParseJson(LoadUtf8File(path), root, error)) {
        std::wcerr << L"FAIL: parse " << path.wstring() << L": " << error << L"\n";
        return false;
    }
    const JsonValue* todos = root.get(L"todos");
    if (!Require(root.isObject(), L"root must be object") ||
        !Require(todos && todos->isArray(), L"todos must be array") ||
        !Require(!todos->arrayValue.empty(), L"todos must not be empty")) {
        return false;
    }

    const JsonValue& first = todos->arrayValue.front();
    return Require(first.isObject(), L"todo item must be object") &&
        Require(first.get(L"title") && first.get(L"title")->isString(), L"title must be string") &&
        Require(first.get(L"groupName") && first.get(L"groupName")->isString(), L"groupName must be string") &&
        Require(first.get(L"tagName") && first.get(L"tagName")->isString(), L"tagName must be string");
}

std::filesystem::path TestRoot(const wchar_t* name) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (std::wstring(name) + L"_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

bool WriteUtf8File(const std::filesystem::path& path, const std::wstring& text) {
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

bool SeedTodoRoot(const std::filesystem::path& root, const std::wstring& title, TodoItem& todo) {
    std::error_code ec;
    std::filesystem::create_directories(root / L"db", ec);
    StorageService storage(root);
    Group group;
    group.name = L"待办测试";
    group.pos = -1;
    if (!storage.InsertGroup(group)) return false;
    Group tag;
    tag.parentGroup = group.id;
    tag.name = L"待办事项";
    tag.type = 4;
    tag.content = L"todoItems";
    tag.pos = -1;
    if (!storage.InsertGroup(tag)) return false;
    todo.tagId = tag.id;
    todo.title = title;
    todo.content = L"details";
    todo.scheduleKind = TodoScheduleKind::Once;
    todo.anchorAt = L"2026-06-26 09:30";
    todo.pos = -1;
    return storage.InsertTodoItem(todo);
}

bool ValidateExportContainsMergeIdentity() {
    const std::filesystem::path root = TestRoot(L"quattro_todo_json_export");
    TodoItem todo;
    if (!Require(SeedTodoRoot(root, L"导出身份", todo), L"seed export todo")) return false;

    const std::filesystem::path jsonPath = root / L"todos.json";
    TodoJsonBackupService service(root);
    TodoJsonExportOptions options;
    std::wstring error;
    if (!Require(service.ExportJson(jsonPath, options, error), L"export todo json")) {
        std::wcerr << error << L"\n";
        return false;
    }
    JsonValue json;
    if (!Require(ParseJson(LoadUtf8File(jsonPath), json, error), L"parse exported json")) return false;
    const JsonValue* todos = json.get(L"todos");
    if (!Require(todos && todos->isArray() && !todos->arrayValue.empty(), L"exported todos exists")) return false;
    const JsonValue* quattro = todos->arrayValue.front().get(L"quattro");
    return Require(quattro && quattro->isObject(), L"exported quattro object exists") &&
        Require(quattro->get(L"todoUid") && quattro->get(L"todoUid")->isString(), L"todoUid exported") &&
        Require(quattro->get(L"mergeUpdatedAtUtc") && quattro->get(L"mergeUpdatedAtUtc")->isString(), L"mergeUpdatedAtUtc exported") &&
        Require(quattro->get(L"groupUid") && quattro->get(L"groupUid")->isString(), L"groupUid exported") &&
        Require(quattro->get(L"tagUid") && quattro->get(L"tagUid")->isString(), L"tagUid exported") &&
        Require(todos->arrayValue.front().get(L"title") && todos->arrayValue.front().get(L"title")->isString(), L"apple-friendly title remains");
}

bool ValidateMergeAndReplaceImport() {
    const std::filesystem::path sourceRoot = TestRoot(L"quattro_todo_json_source");
    TodoItem sourceTodo;
    if (!Require(SeedTodoRoot(sourceRoot, L"合并源", sourceTodo), L"seed source todo")) return false;
    const std::filesystem::path jsonPath = sourceRoot / L"todos.json";
    TodoJsonBackupService sourceService(sourceRoot);
    TodoJsonExportOptions exportOptions;
    std::wstring error;
    if (!Require(sourceService.ExportJson(jsonPath, exportOptions, error), L"export source json")) return false;

    const std::filesystem::path targetRoot = TestRoot(L"quattro_todo_json_target");
    TodoJsonBackupService targetService(targetRoot);
    TodoJsonImportOptions mergeOptions;
    mergeOptions.mode = TodoJsonImportMode::Merge;
    TodoJsonImportReport first = targetService.ImportJson(jsonPath, mergeOptions);
    TodoJsonImportReport second = targetService.ImportJson(jsonPath, mergeOptions);
    StorageService targetStorage(targetRoot);
    AppModel targetModel = targetStorage.Load();
    if (!Require(first.ok && first.todosAdded == 1, L"first merge adds todo") ||
        !Require(second.ok && second.todosAdded == 0 && second.todosSkippedIdentical == 1, L"second merge is idempotent") ||
        !Require(targetModel.todos.size() == 1, L"merge target has one todo")) {
        return false;
    }

    std::wstring remoteNewerJson = LoadUtf8File(jsonPath);
    remoteNewerJson = ReplaceAll(remoteNewerJson, L"合并源", L"远端较新");
    remoteNewerJson = ReplaceAll(remoteNewerJson, sourceTodo.mergeUpdatedAtUtc, L"2999-01-01T00:00:00.000Z");
    const std::filesystem::path remoteNewerPath = sourceRoot / L"todos-newer.json";
    if (!Require(WriteUtf8File(remoteNewerPath, remoteNewerJson), L"write remote newer json")) return false;
    TodoJsonImportReport newer = targetService.ImportJson(remoteNewerPath, mergeOptions);
    targetModel = targetStorage.Load();
    const bool updated = std::any_of(targetModel.todos.begin(), targetModel.todos.end(), [](const TodoItem& item) {
        return item.title == L"远端较新";
    });
    if (!Require(newer.ok && newer.todosUpdatedFromRemote == 1 && updated, L"remote newer updates local todo")) return false;

    TodoItem extra;
    if (!Require(SeedTodoRoot(targetRoot, L"替换前本地项", extra), L"seed extra target todo")) return false;
    TodoJsonImportOptions replaceOptions;
    replaceOptions.mode = TodoJsonImportMode::ReplaceAll;
    TodoJsonImportReport replace = targetService.ImportJson(jsonPath, replaceOptions);
    targetModel = targetStorage.Load();
    const bool onlySource = targetModel.todos.size() == 1 && targetModel.todos.front().title == L"合并源";
    return Require(replace.ok && replace.todosDeletedForReplace >= 2 && onlySource, L"replace all swaps todo set");
}

bool ValidateFixtureImports(const std::filesystem::path& fixtures) {
    const std::filesystem::path root = TestRoot(L"quattro_todo_json_fixtures");
    TodoJsonBackupService service(root);
    TodoJsonImportOptions options;
    options.mode = TodoJsonImportMode::Merge;
    bool ok = true;
    for (const auto& name : {L"todo-backup-v1.json", L"todo-backup-v2.json", L"todo-backup-apple-simple.json"}) {
        const TodoJsonImportReport report = service.ImportJson(fixtures / name, options);
        ok = Require(report.ok && report.todosAdded > 0, L"fixture imports") && ok;
    }
    StorageService storage(root);
    ok = Require(!storage.Load().todos.empty(), L"fixture import stored todos") && ok;
    return ok;
}
}

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
    const std::filesystem::path fixtures = root / L"tests" / L"fixtures";
    bool ok = true;
    ok = ValidateFixture(fixtures / L"todo-backup-v1.json") && ok;
    ok = ValidateFixture(fixtures / L"todo-backup-v2.json") && ok;
    ok = ValidateFixture(fixtures / L"todo-backup-apple-simple.json") && ok;
    ok = ValidateFixtureImports(fixtures) && ok;
    ok = ValidateExportContainsMergeIdentity() && ok;
    ok = ValidateMergeAndReplaceImport() && ok;
    if (!ok) {
        return 1;
    }
    std::wcout << L"Todo backup JSON compatibility fixtures passed.\n";
    return 0;
}
