#include "../src/domain/Config.h"
#include "../src/domain/MenuCatalog.h"
#include "../src/domain/TodoSchedule.h"
#include "../src/services/Storage.h"

#include <sqlite3.h>
#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UINT kReminderStateMessage = WM_APP + 0x70;
constexpr UINT kTrayMessage = WM_APP + 0x66;
constexpr UINT kReminderActionMessage = WM_APP + 0x71;
constexpr UINT kCompleteOverdueMessage = WM_APP + 0x72;
constexpr UINT kSystemReminderMessage = WM_APP + 0x74;
int failures = 0;

void Check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAILED: " << name << "\n";
        ++failures;
    }
}

void CheckReminderDomainRules() {
    TodoItem item;
    item.id = 1;
    item.enabled = true;
    item.scheduleKind = TodoScheduleKind::Once;
    item.nextDueAt = L"2026-06-26 09:30:00";
    Check(!IsTodoReminderDue(item, L"2026-06-26 09:29:59"), "reminder stays pending before due time");
    Check(IsTodoReminderDue(item, L"2026-06-26 09:30:00"), "reminder becomes due at boundary");
    MarkTodoReminderSent(item, L"2026-06-26 09:30:01");
    Check(IsTodoReminderDelivered(item) && GetTodoReminderStatus(item, L"2026-06-26 09:31:00") == TodoReminderStatus::Sent,
          "sent reminder is delivered");
    MarkTodoReminderViewed(item, L"2026-06-26 09:31:00");
    Check(GetTodoReminderStatus(item, L"2026-06-26 09:31:00") == TodoReminderStatus::Viewed,
          "viewed reminder status wins over sent");
    ResetTodoReminderState(item);
    Check(SnoozeTodoReminder(item, L"2026-06-26 09:31:00", 30) && item.snoozedUntil == L"2026-06-26 10:01:00",
          "snooze computes a new reminder instance");
    Check(!IsTodoReminderDue(item, L"2026-06-26 10:00:59") && IsTodoReminderDue(item, L"2026-06-26 10:01:00"),
          "snoozed reminder uses its new due time");
    IgnoreTodoReminderOccurrence(item);
    Check(IsTodoReminderDelivered(item) && GetTodoReminderStatus(item, L"2026-06-26 10:01:00") == TodoReminderStatus::Ignored,
          "ignored occurrence is suppressed");
    Check(MenuIconFor(ID_MENU_TODO_REMINDER_VIEWED, L"标记本次提醒为已查看") == MenuIconEye,
          "viewed command uses semantic icon");
    Check(MenuIconFor(ID_MENU_TODO_REMINDER_IGNORE, L"忽略本次提醒") == MenuIconClear,
          "ignore command uses semantic icon");
    Check(MenuIconFor(ID_MENU_TODO_REMINDER_SNOOZE_30, L"30 分钟后") == MenuIconHistory,
          "snooze command uses semantic icon");
    Check(MenuIconFor(ID_MENU_COMPLETE_OVERDUE_TODOS, L"标记全部逾期项为完成") == MenuIconList,
          "bulk overdue completion uses semantic icon");
}

bool Exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::cerr << "sqlite exec failed: " << (message ? message : "") << "\n";
        sqlite3_free(message);
        return false;
    }
    return true;
}

void CheckReminderSchemaMigration(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    StorageService initial(root);
    initial.Load();
    sqlite3* db = nullptr;
    Check(sqlite3_open16((root / L"db" / L"link.db").c_str(), &db) == SQLITE_OK && db,
          "open reminder migration database");
    if (!db) return;
    const bool downgraded =
        Exec(db, "ALTER TABLE TodoItems DROP COLUMN LastNotifiedDueAt;") &&
        Exec(db, "ALTER TABLE TodoItems DROP COLUMN LastNotifiedAt;") &&
        Exec(db, "ALTER TABLE TodoItems DROP COLUMN LastViewedDueAt;") &&
        Exec(db, "ALTER TABLE TodoItems DROP COLUMN LastViewedAt;") &&
        Exec(db, "ALTER TABLE TodoItems DROP COLUMN IgnoredDueAt;") &&
        Exec(db, "ALTER TABLE TodoItems DROP COLUMN SnoozedUntil;") &&
        Exec(db, "UPDATE Version SET Ver=20002 WHERE ID=1;") &&
        Exec(db,
            "INSERT INTO TodoItems(TagId,Title,Content,Enabled,ScheduleKind,RepeatMode,RepeatInterval,RepeatLimit,RepeatFinished,CronExpression,AnchorAt,NextDueAt,CompletedAt,POS,CreatedAt,UpdatedAt) "
            "VALUES(2,'MigratedReminder','',1,1,0,1,0,0,'','2020-01-01 09:00:00','2020-01-01 09:00:00','',0,'2020-01-01 08:00:00','2020-01-01 08:00:00');");
    sqlite3_close(db);
    Check(downgraded, "prepare pre-reminder schema");

    StorageService migrated(root);
    const AppModel model = migrated.Load();
    const TodoItem* reminder = nullptr;
    for (const auto& item : model.todos) {
        if (item.title == L"MigratedReminder") reminder = &item;
    }
    Check(reminder && reminder->lastNotifiedDueAt.empty() && reminder->lastNotifiedAt.empty() &&
              reminder->lastViewedDueAt.empty() && reminder->lastViewedAt.empty() &&
              reminder->ignoredDueAt.empty() && reminder->snoozedUntil.empty(),
          "pre-reminder database migrates with empty lifecycle state");
    std::filesystem::remove_all(root, ec);
}

std::filesystem::path ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

HWND FindProcessWindow(DWORD processId) {
    struct Context {
        DWORD processId = 0;
        HWND result = nullptr;
    } context{processId, nullptr};
    EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
        auto* context = reinterpret_cast<Context*>(value);
        DWORD ownerProcessId = 0;
        GetWindowThreadProcessId(hwnd, &ownerProcessId);
        if (ownerProcessId != context->processId) {
            return TRUE;
        }
        wchar_t className[128]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        if (std::wstring(className) == L"QuattroMainWindow") {
            context->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.result;
}

HWND WaitForMainWindow(DWORD processId, DWORD timeoutMs = 10000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        if (HWND hwnd = FindProcessWindow(processId)) {
            return hwnd;
        }
        Sleep(50);
    }
    return nullptr;
}

HWND FindProcessWindowByClass(DWORD processId, const std::wstring& expectedClass, const std::wstring& expectedTitle = {}) {
    struct Context {
        DWORD processId = 0;
        const std::wstring* expectedClass = nullptr;
        const std::wstring* expectedTitle = nullptr;
        HWND result = nullptr;
    } context{processId, &expectedClass, &expectedTitle, nullptr};
    EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
        auto* context = reinterpret_cast<Context*>(value);
        DWORD ownerProcessId = 0;
        GetWindowThreadProcessId(hwnd, &ownerProcessId);
        if (ownerProcessId != context->processId) return TRUE;
        wchar_t className[128]{};
        wchar_t title[256]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        if (std::wstring(className) == *context->expectedClass &&
            (context->expectedTitle->empty() || std::wstring(title) == *context->expectedTitle)) {
            context->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.result;
}

HWND WaitForProcessWindowByClass(
    DWORD processId,
    const std::wstring& expectedClass,
    const std::wstring& expectedTitle,
    DWORD timeoutMs = 5000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        if (HWND hwnd = FindProcessWindowByClass(processId, expectedClass, expectedTitle)) return hwnd;
        Sleep(50);
    }
    return nullptr;
}

bool WaitForReminderState(HWND hwnd, int channel, int count, DWORD timeoutMs = 10000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        if (SendMessageW(hwnd, kReminderStateMessage, 0, 0) == channel &&
            SendMessageW(hwnd, kReminderStateMessage, 1, 0) == count) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

bool WaitForPendingCount(HWND hwnd, int count, DWORD timeoutMs = 5000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        if (SendMessageW(hwnd, kReminderStateMessage, 2, 0) == count) return true;
        Sleep(50);
    }
    return false;
}

struct DesktopState {
    HWND foreground = nullptr;
    HWND focus = nullptr;
};

DesktopState CaptureDesktopState() {
    DesktopState state;
    state.foreground = GetForegroundWindow();
    const DWORD threadId = state.foreground ? GetWindowThreadProcessId(state.foreground, nullptr) : 0;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (threadId != 0 && GetGUIThreadInfo(threadId, &info)) {
        state.focus = info.hwndFocus;
    }
    return state;
}

struct ChildProcess {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
};

void SetChildEnvironment(const std::filesystem::path& root, const std::wstring& runId) {
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", root.c_str());
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    SetEnvironmentVariableW(L"QUATTRO_TEST_RUN_ID", runId.c_str());
    SetEnvironmentVariableW(L"QUATTRO_TEST_SUPPRESS_TRAY", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_REMINDER_FAIL_ONCE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_REMINDER_RETRY_MS", L"200");
}

void ClearChildEnvironment() {
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_RUN_ID", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_SUPPRESS_TRAY", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_REMINDER_FAIL_ONCE", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_REMINDER_RETRY_MS", nullptr);
}

ChildProcess StartQuattro(const std::filesystem::path& root, int sequence) {
    ChildProcess child;
    const std::filesystem::path sourceExe = ModuleDirectory() / L"Quattro.exe";
    const std::filesystem::path exe = root / L"Quattro.exe";
    std::error_code copyError;
    std::filesystem::copy_file(sourceExe, exe, std::filesystem::copy_options::overwrite_existing, copyError);
    if (copyError) {
        return child;
    }
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    SetChildEnvironment(root, std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence));
    const BOOL created = CreateProcessW(
        exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        root.c_str(), &startup, &child.process);
    ClearChildEnvironment();
    if (!created) {
        return child;
    }
    child.window = WaitForMainWindow(child.process.dwProcessId);
    return child;
}

void StopQuattro(ChildProcess& child) {
    if (child.window) {
        PostMessageW(child.window, WM_COMMAND, MAKEWPARAM(ID_MENU_EXIT, 0), 0);
    }
    if (child.process.hProcess && WaitForSingleObject(child.process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(child.process.hProcess, 2);
        WaitForSingleObject(child.process.hProcess, 2000);
    }
    if (child.process.hThread) CloseHandle(child.process.hThread);
    if (child.process.hProcess) CloseHandle(child.process.hProcess);
    child = {};
}

std::vector<TodoItem> SeedReminderRoot(const std::filesystem::path& root, bool hideOnStart) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    std::ofstream marker(root / L".quattro-test-root", std::ios::binary | std::ios::trunc);
    marker << "todo_reminder_acceptance=1\n";

    StorageService storage(root);
    storage.Load();
    Group group;
    group.name = L"ReminderAcceptance";
    group.pos = -1;
    Check(storage.InsertGroup(group), "reminder acceptance seed group");
    Group tag;
    tag.name = L"ReminderTodos";
    tag.parentGroup = group.id;
    tag.type = 4;
    tag.content = L"todoItems";
    tag.pos = -1;
    Check(storage.InsertGroup(tag), "reminder acceptance seed tag");

    std::vector<TodoItem> todos;
    for (int index = 0; index < 2; ++index) {
        TodoItem item;
        item.tagId = tag.id;
        item.title = index == 0 ? L"First overdue reminder" : L"Second overdue reminder";
        item.content = L"Reminder acceptance content";
        item.scheduleKind = TodoScheduleKind::Once;
        item.anchorAt = L"2020-01-01 09:00:00";
        item.nextDueAt = item.anchorAt;
        item.pos = -1;
        Check(storage.InsertTodoItem(item), "reminder acceptance seed todo");
        todos.push_back(item);
    }

    ConfigService configService(root / L"conf.ini");
    AppConfig config = configService.Load();
    config.currentGroupId = group.id;
    config.currentTagId = tag.id;
    config.hideOnStart = hideOnStart;
    config.autoDock = false;
    config.hideWhenInactive = false;
    config.globalHotKeysEnabled = false;
    config.httpServerAutoStart = false;
    config.width = 720;
    config.height = 560;
    configService.Save(config);
    return todos;
}

const TodoItem* FindTodo(const AppModel& model, int id) {
    for (const auto& item : model.todos) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

void CheckBatchCompletion(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    StorageService storage(root);
    storage.Load();
    Group group;
    group.name = L"BatchCompletion";
    group.pos = -1;
    Check(storage.InsertGroup(group), "batch completion seed group");
    Group tag;
    tag.name = L"BatchTodos";
    tag.parentGroup = group.id;
    tag.type = 4;
    tag.content = L"todoItems";
    tag.pos = -1;
    Check(storage.InsertGroup(tag), "batch completion seed tag");

    auto insertOnce = [&](const wchar_t* title, const wchar_t* dueAt, bool enabled = true) {
        TodoItem item;
        item.tagId = tag.id;
        item.title = title;
        item.enabled = enabled;
        item.scheduleKind = TodoScheduleKind::Once;
        item.anchorAt = dueAt;
        item.nextDueAt = dueAt;
        item.pos = -1;
        Check(storage.InsertTodoItem(item), "batch completion seed once todo");
        return item;
    };

    TodoItem overdue = insertOnce(L"BatchOverdue", L"2020-01-01 09:00:00");
    overdue.lastNotifiedDueAt = overdue.nextDueAt;
    overdue.lastNotifiedAt = L"2020-01-01 09:00:01";
    Check(storage.UpdateTodoReminderState(overdue), "batch completion seed reminder state");
    TodoItem snoozed = insertOnce(L"BatchSnoozed", L"2020-01-02 09:00:00");
    snoozed.snoozedUntil = L"2099-01-01 09:00:00";
    Check(storage.UpdateTodoReminderState(snoozed), "batch completion seed snoozed state");
    TodoItem disabled = insertOnce(L"BatchDisabled", L"2020-01-03 09:00:00", false);
    TodoItem future = insertOnce(L"BatchFuture", L"2099-01-01 09:00:00");
    TodoItem boundary = insertOnce(L"BatchBoundary", L"2026-07-18 12:00:00");
    TodoItem alreadyDone = insertOnce(L"BatchAlreadyDone", L"2020-01-04 09:00:00");
    Check(storage.SetTodoCompleted(alreadyDone.id, true), "batch completion seed completed todo");

    TodoItem recurring;
    recurring.tagId = tag.id;
    recurring.title = L"BatchRecurring";
    recurring.scheduleKind = TodoScheduleKind::Daily;
    recurring.repeatMode = TodoRepeatMode::FixedPoint;
    recurring.anchorAt = L"2020-01-05 09:00:00";
    recurring.nextDueAt = recurring.anchorAt;
    recurring.lastNotifiedDueAt = recurring.nextDueAt;
    recurring.lastNotifiedAt = L"2020-01-05 09:00:01";
    recurring.pos = -1;
    Check(storage.InsertTodoItem(recurring), "batch completion seed recurring todo");

    TodoBatchCompleteResult result;
    const std::wstring now = L"2026-07-18 12:00:00";
    Check(storage.CompleteOverdueTodos(tag.id, now, result), "batch completion transaction succeeds");
    Check(result.updatedItems.size() == 3 && result.completedCount == 2 && result.advancedRecurringCount == 1,
          "batch completion reports completed and advanced counts");

    AppModel model = storage.Load();
    const TodoItem* loadedOverdue = FindTodo(model, overdue.id);
    const TodoItem* loadedSnoozed = FindTodo(model, snoozed.id);
    const TodoItem* loadedRecurring = FindTodo(model, recurring.id);
    const TodoItem* loadedDisabled = FindTodo(model, disabled.id);
    const TodoItem* loadedFuture = FindTodo(model, future.id);
    const TodoItem* loadedBoundary = FindTodo(model, boundary.id);
    Check(loadedOverdue && !loadedOverdue->completedAt.empty() && loadedOverdue->lastNotifiedDueAt.empty(),
          "batch completion completes one-time overdue todo and clears reminder state");
    Check(loadedSnoozed && !loadedSnoozed->completedAt.empty() && loadedSnoozed->snoozedUntil.empty(),
          "batch completion includes snoozed todo whose schedule is overdue");
    Check(loadedRecurring && loadedRecurring->completedAt.empty() && loadedRecurring->repeatFinished == 1 &&
              loadedRecurring->nextDueAt > now && loadedRecurring->lastNotifiedDueAt.empty(),
          "batch completion advances recurring todo once and clears reminder state");
    Check(loadedDisabled && loadedDisabled->completedAt.empty(), "batch completion excludes disabled todo");
    Check(loadedFuture && loadedFuture->completedAt.empty(), "batch completion excludes future todo");
    Check(loadedBoundary && loadedBoundary->completedAt.empty(), "batch completion excludes exact-time boundary");

    TodoItem rollbackA = insertOnce(L"BatchRollbackA", L"2020-02-01 09:00:00");
    TodoItem rollbackB = insertOnce(L"BatchRollbackB", L"2020-02-02 09:00:00");
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_TODO_BATCH_FAIL_AFTER", L"1");
    TodoBatchCompleteResult failedResult;
    const bool failed = storage.CompleteOverdueTodos(tag.id, now, failedResult);
    SetEnvironmentVariableW(L"QUATTRO_TEST_TODO_BATCH_FAIL_AFTER", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", nullptr);
    Check(!failed && failedResult.updatedItems.empty(), "batch completion injected failure returns no result");
    model = storage.Load();
    Check(FindTodo(model, rollbackA.id) && FindTodo(model, rollbackA.id)->completedAt.empty() &&
              FindTodo(model, rollbackB.id) && FindTodo(model, rollbackB.id)->completedAt.empty(),
          "batch completion rolls back every update after failure");
    std::filesystem::remove_all(root, ec);
}

bool WaitForDeliveredState(const std::filesystem::path& root, const std::vector<TodoItem>& todos, DWORD timeoutMs = 5000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        StorageService storage(root);
        const AppModel model = storage.Load();
        bool allDelivered = true;
        for (const auto& seed : todos) {
            const TodoItem* item = FindTodo(model, seed.id);
            allDelivered = allDelivered && item && item->lastNotifiedDueAt == item->nextDueAt;
        }
        if (allDelivered) return true;
        Sleep(50);
    }
    return false;
}

bool WaitForViewedState(const std::filesystem::path& root, const std::vector<TodoItem>& todos, DWORD timeoutMs = 5000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        StorageService storage(root);
        const AppModel model = storage.Load();
        bool allViewed = true;
        for (const auto& seed : todos) {
            const TodoItem* item = FindTodo(model, seed.id);
            allViewed = allViewed && item && item->lastViewedDueAt == item->nextDueAt;
        }
        if (allViewed) return true;
        Sleep(50);
    }
    return false;
}

bool WaitForUserActionState(const std::filesystem::path& root, int snoozedId, int ignoredId, DWORD timeoutMs = 5000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        StorageService storage(root);
        const AppModel model = storage.Load();
        const TodoItem* snoozed = FindTodo(model, snoozedId);
        const TodoItem* ignored = FindTodo(model, ignoredId);
        if (snoozed && !snoozed->snoozedUntil.empty() && ignored && ignored->ignoredDueAt == ignored->nextDueAt) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

bool WaitForCompletedState(const std::filesystem::path& root, const std::vector<TodoItem>& todos, DWORD timeoutMs = 5000) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeoutMs) {
        StorageService storage(root);
        const AppModel model = storage.Load();
        bool allCompleted = true;
        for (const auto& seed : todos) {
            const TodoItem* item = FindTodo(model, seed.id);
            allCompleted = allCompleted && item && !item->completedAt.empty();
        }
        if (allCompleted) return true;
        Sleep(50);
    }
    return false;
}
}

int wmain() {
    CheckReminderDomainRules();
    const DesktopState desktopBefore = CaptureDesktopState();
    const std::filesystem::path base = std::filesystem::temp_directory_path() /
        (L"quattro_todo_reminder_acceptance_" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path visibleRoot = base / L"visible";
    const std::filesystem::path hiddenRoot = base / L"hidden";
    CheckReminderSchemaMigration(base / L"migration");
    CheckBatchCompletion(base / L"batch");
    const auto visibleTodos = SeedReminderRoot(visibleRoot, false);

    ChildProcess visible = StartQuattro(visibleRoot, 1);
    Check(visible.window != nullptr, "visible reminder main window exists");
    if (visible.window) {
        Check(WaitForReminderState(visible.window, 1, 2), "visible reminders use one app-toast batch");
        Check(SendMessageW(visible.window, kReminderStateMessage, 3, 0) == TRUE,
              "visible reminder retries after injected delivery failure");
        Check(WaitForDeliveredState(visibleRoot, visibleTodos),
              "visible reminder retry eventually persists delivery");
    }
    StorageService visibleStorage(visibleRoot);
    AppModel visibleModel = visibleStorage.Load();
    std::vector<std::wstring> firstNotificationTimes;
    for (const auto& seed : visibleTodos) {
        const TodoItem* item = FindTodo(visibleModel, seed.id);
        Check(item && item->lastNotifiedDueAt == item->nextDueAt && !item->lastNotifiedAt.empty(),
              "visible reminder delivery persisted");
        firstNotificationTimes.push_back(item ? item->lastNotifiedAt : L"");
    }
    StopQuattro(visible);

    ChildProcess restarted = StartQuattro(visibleRoot, 2);
    Check(restarted.window != nullptr, "restart reminder main window exists");
    if (restarted.window) {
        Sleep(1500);
        Check(SendMessageW(restarted.window, kReminderStateMessage, 1, 0) == 0,
              "restart does not emit delivered reminder batch");
    }
    visibleModel = visibleStorage.Load();
    for (std::size_t index = 0; index < visibleTodos.size(); ++index) {
        const TodoItem* item = FindTodo(visibleModel, visibleTodos[index].id);
        Check(item && item->lastNotifiedAt == firstNotificationTimes[index],
              "restart preserves original notification timestamp");
    }
    StopQuattro(restarted);

    const auto hiddenTodos = SeedReminderRoot(hiddenRoot, true);
    ChildProcess hidden = StartQuattro(hiddenRoot, 3);
    Check(hidden.window != nullptr, "hidden reminder main window exists");
    if (hidden.window) {
        Check(WaitForReminderState(hidden.window, 2, 2), "hidden reminders use one system-notification intent batch");
        Check(SendMessageW(hidden.window, kReminderStateMessage, 3, 0) == TRUE,
              "hidden reminder retries after injected delivery failure");
        Check(WaitForDeliveredState(hiddenRoot, hiddenTodos),
              "hidden reminder retry eventually persists delivery");
        Check(SendMessageW(hidden.window, kSystemReminderMessage, 0, 0) == TRUE,
              "isolated system reminder semantic entry succeeds");
        Check(WaitForPendingCount(hidden.window, 2),
              "hidden reminder batch retains notification targets");
        SendMessageW(hidden.window, kTrayMessage, 0, static_cast<LPARAM>(NIN_BALLOONUSERCLICK));
        Check(WaitForViewedState(hiddenRoot, hiddenTodos), "notification click persists viewed state");
        SendMessageW(hidden.window, kReminderActionMessage, 4, hiddenTodos[0].id);
        SendMessageW(hidden.window, kReminderActionMessage, 2, hiddenTodos[1].id);
        Check(WaitForUserActionState(hiddenRoot, hiddenTodos[0].id, hiddenTodos[1].id),
              "snooze and ignore commands persist user reminder semantics");
        std::thread confirmationThread([&]() {
            SendMessageW(hidden.window, WM_COMMAND, MAKEWPARAM(ID_MENU_COMPLETE_OVERDUE_TODOS, 0), 0);
        });
        HWND confirmation = WaitForProcessWindowByClass(
            hidden.process.dwProcessId, L"QuattroThemedMessageDialog", L"完成全部逾期待办");
        Check(confirmation != nullptr, "bulk overdue completion shows confirmation dialog");
        if (confirmation) SendMessageW(confirmation, WM_COMMAND, MAKEWPARAM(IDNO, 0), 0);
        confirmationThread.join();
        StorageService cancelledStorage(hiddenRoot);
        const AppModel cancelledModel = cancelledStorage.Load();
        Check(FindTodo(cancelledModel, hiddenTodos[0].id) &&
                  FindTodo(cancelledModel, hiddenTodos[0].id)->completedAt.empty() &&
                  FindTodo(cancelledModel, hiddenTodos[1].id) &&
                  FindTodo(cancelledModel, hiddenTodos[1].id)->completedAt.empty(),
              "cancelling bulk overdue confirmation leaves todos unchanged");
        SendMessageW(hidden.window, kCompleteOverdueMessage, hiddenTodos[0].tagId, 0);
        Check(WaitForCompletedState(hiddenRoot, hiddenTodos),
              "main window bulk command completes every overdue todo in the tag");
        Check(WaitForPendingCount(hidden.window, 0),
              "main window bulk command removes completed todos from pending notification targets");
    }
    StopQuattro(hidden);

    const DesktopState desktopAfter = CaptureDesktopState();
    Check(desktopAfter.foreground == desktopBefore.foreground, "acceptance keeps foreground window unchanged");
    Check(desktopAfter.focus == desktopBefore.focus, "acceptance keeps focus unchanged");

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    if (failures == 0) {
        std::cout << "todo_reminder_acceptance=passed\n";
    }
    return failures == 0 ? 0 : 1;
}
