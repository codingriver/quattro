#include "AppLaunchLockerCore.h"
#include "AppLaunchLockerWindow.h"
#include "AdBlockWindow.h"

#include "AppLog.h"
#include "AppLaunchLockerVersion.h"
#include "Theme.h"
#include "Utilities.h"
#include "Version.h"
#include "../../resources/resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::filesystem::path ModuleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!copied) return std::filesystem::current_path();
        if (copied < path.size() - 1) {
            path.resize(copied);
            return std::filesystem::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
}

std::filesystem::path GuiLogDirectory() {
    return AppLaunchLockerDataDirectory();
}

bool GuiLoggingEnabled() {
    const std::filesystem::path configPath = AppLaunchLockerDataDirectory() / L"settings.ini";
    return QuattroTestMode() ||
        GetPrivateProfileIntW(L"main", L"loggingEnabled", 0, configPath.c_str()) != 0;
}

template <typename RunWindow>
int RunGui(const wchar_t* mode, RunWindow&& runWindow) {
    const std::filesystem::path logDirectory = GuiLogDirectory();
    const bool loggingEnabled = GuiLoggingEnabled();
    InitializeAppLog(logDirectory, loggingEnabled);
    struct AppLogShutdownGuard {
        ~AppLogShutdownGuard() { ShutdownAppLog(); }
    } appLogShutdownGuard;
    WriteAppLog(
        L"AppLaunchLocker GUI 启动: mode=" + std::wstring(mode) +
        L", pid=" + std::to_wstring(GetCurrentProcessId()) +
        L", moduleDir=\"" + ModuleDirectory().wstring() + L"\"" +
        L", logRoot=\"" + logDirectory.wstring() + L"\"" +
        L", logging=" + std::wstring(loggingEnabled ? L"1" : L"0"));

    const HRESULT ole = OleInitialize(nullptr);
    WriteAppLog(
        L"AppLaunchLocker OLE 初始化: hr=0x" +
        [&]() {
            std::wostringstream output;
            output << std::hex << static_cast<unsigned long>(ole);
            return output.str();
        }());
    FlushAppLog();
    if (FAILED(ole)) {
        WriteAppLog(L"AppLaunchLocker GUI 启动失败: OLE/STA 初始化不可用");
        return 1;
    }

    const int result = runWindow();
    WriteAppLog(
        L"AppLaunchLocker GUI 退出: mode=" + std::wstring(mode) +
        L", result=" + std::to_wstring(result));
    OleUninitialize();
    return result;
}

std::wstring EscapeJson(const std::wstring& value) {
    std::wstring output;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += L"\\\\"; break;
        case L'"': output += L"\\\""; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': output += L"\\r"; break;
        case L'\t': output += L"\\t"; break;
        default: output.push_back(ch); break;
        }
    }
    return output;
}

void AttachParentConsole() {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if ((!output || output == INVALID_HANDLE_VALUE) && (!error || error == INVALID_HANDLE_VALUE)) {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
}

void WriteOutput(const std::wstring& text, bool error = false) {
    HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (handle && handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (WriteConsoleW(handle, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr)) return;
        const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (length > 0) {
            std::string utf8(static_cast<std::size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
            WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
}

std::wstring ArgumentValue(const std::vector<std::wstring>& arguments, const std::wstring& name) {
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name) return arguments[index + 1];
    }
    return {};
}

std::vector<std::wstring> ArgumentValues(const std::vector<std::wstring>& arguments, const std::wstring& name) {
    std::vector<std::wstring> values;
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name) values.push_back(arguments[index + 1]);
    }
    return values;
}

std::wstring OperationJson(const OperationResult& operation) {
    std::wostringstream output;
    output << L"{\n"
           << L"  \"schemaVersion\": 1,\n"
           << L"  \"success\": " << (operation.success ? L"true" : L"false") << L",\n"
           << L"  \"partial\": " << (operation.partial ? L"true" : L"false") << L",\n"
           << L"  \"message\": \"" << EscapeJson(operation.message) << L"\",\n"
           << L"  \"items\": [";
    for (std::size_t index = 0; index < operation.items.size(); ++index) {
        const OperationItemResult& item = operation.items[index];
        output << (index == 0 ? L"\n" : L",\n")
               << L"    {\"targetId\":\"" << EscapeJson(item.targetId)
               << L"\",\"action\":\"" << EscapeJson(item.action)
               << L"\",\"success\":" << (item.success ? L"true" : L"false")
               << L",\"rolledBack\":" << (item.rolledBack ? L"true" : L"false")
               << L",\"message\":\"" << EscapeJson(item.message) << L"\"}";
    }
    if (!operation.items.empty()) output << L"\n  ";
    output << L"]\n}\n";
    return output.str();
}

bool JsonFormat(const std::vector<std::wstring>& arguments) {
    return ArgumentValue(arguments, L"--format") == L"json";
}

bool RunningAsAdministrator() {
    BOOL isAdministrator = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators)) {
        CheckTokenMembership(nullptr, administrators, &isAdministrator);
        FreeSid(administrators);
    }
    return isAdministrator != FALSE;
}

int RunElevatedOperation(const std::vector<std::wstring>& arguments) {
    if (!RunningAsAdministrator()) return 5;
    const std::wstring requestPath = ArgumentValue(arguments, L"--request");
    const std::wstring token = ArgumentValue(arguments, L"--token");
    if (requestPath.empty() || token.empty()) return 2;
    ElevatedOperationRequest request;
    std::wstring error;
    if (!LoadAndConsumeElevatedOperationRequest(requestPath, token, request, error)) {
        AppendAppLaunchLockerLog(L"管理员请求被拒绝：" + error);
        return 3;
    }
    SetOperationAuditContext(request.requestId, L"elevated-request", true);

    OperationResult operation;
    StartupManager startup;
    AdBlockManager adBlock;
    if (request.action == L"disable") operation = startup.Disable(request.targets.front());
    else if (request.action == L"disable-many") operation = startup.DisableMany(request.targets);
    else if (request.action == L"restore") operation = startup.Restore(request.targets.front());
    else if (request.action == L"restore-many") operation = startup.RestoreMany(request.targets);
    else if (request.action == L"adblock-block") {
        int succeeded = 0;
        int failed = 0;
        std::wstring lastError;
        for (const std::wstring& target : request.targets) {
            const OperationResult item = adBlock.Block(target, request.mode);
            operation.items.push_back({target, L"adblock-block", item.success, item.message, false});
            if (item.success) ++succeeded;
            else { ++failed; lastError = item.message; }
        }
        operation.success = failed == 0 || succeeded > 0;
        operation.partial = succeeded > 0 && failed > 0;
        operation.message = failed == 0
            ? L"已拦截 " + std::to_wstring(succeeded) + L" 个程序。"
            : L"已拦截 " + std::to_wstring(succeeded) + L" 个，" + std::to_wstring(failed) +
                L" 个失败：" + lastError;
    }
    else if (request.action == L"adblock-unblock") operation = adBlock.Unblock(request.targets.front());
    else if (request.action == L"adblock-unblock-all") operation = adBlock.UnblockAll();
    else if (request.action == L"adblock-repair") operation = adBlock.RepairRecord(request.targets.front());
    else operation = {false, L"管理员操作动作不受支持。"};

    if (!WriteElevatedOperationResult(request, operation, error)) {
        AppendAppLaunchLockerLog(L"管理员结果写入失败：" + error);
        return 4;
    }
    return operation.success ? 0 : 1;
}

const wchar_t* kStartupWindowClassName = L"AppLaunchLockerMainWindow";
const wchar_t* kStartupSingleInstanceMutexName = L"Local\\Quattro.AppLaunchLocker.StartupManager.v1";
constexpr ULONG_PTR kAppLaunchLockerIpcMessage = 0x414C4C31;

bool ActivateExistingStartupWindow() {
    HWND existing = FindWindowW(kStartupWindowClassName, nullptr);
    if (!existing) return false;
    const std::wstring requestId = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::wstring payload = L"{\"protocolVersion\":1,\"requestId\":\"" +
        EscapeJson(requestId) + L"\",\"intent\":\"show-main\"}";
    COPYDATASTRUCT data{};
    data.dwData = kAppLaunchLockerIpcMessage;
    data.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(payload.c_str());
    DWORD_PTR response = 0;
    return SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &response) && response == 1;
}
bool IsStartupGuiLaunchIntent(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) return true;
    bool sawLaunchSource = false;
    bool sawProtocolVersion = false;
    for (std::size_t index = 0; index < arguments.size();) {
        if (arguments[index] == L"--launch-source" && index + 1 < arguments.size()) {
            if (arguments[index + 1] != L"quattro" && arguments[index + 1] != L"direct") return false;
            sawLaunchSource = true;
            index += 2;
            continue;
        }
        if (arguments[index] == L"--protocol-version" && index + 1 < arguments.size()) {
            if (arguments[index + 1] != L"1") return false;
            sawProtocolVersion = true;
            index += 2;
            continue;
        }
        return false;
    }
    return sawLaunchSource && sawProtocolVersion;
}

std::wstring ScanPlain(const ScanResult& result) {
    std::wostringstream output;
    for (const auto& item : result.items) {
        output << item.id << L"\t" << item.name << L"\t" << StartupSourceText(item.source)
               << L"\t" << (item.canDisable ? L"可禁用" : L"仅查看") << L"\t" << item.location << L"\n";
    }
    for (const auto& warning : result.warnings) output << L"警告：" << warning << L"\n";
    return output.str();
}

std::wstring ScanJson(const ScanResult& result, int schemaVersion) {
    std::wostringstream output;
    output << L"{\n  \"schemaVersion\": " << schemaVersion << L",\n  \"items\": [";
    for (std::size_t index = 0; index < result.items.size(); ++index) {
        const auto& item = result.items[index];
        output << (index == 0 ? L"\n" : L",\n")
               << L"    {\"id\":\"" << EscapeJson(item.id)
               << L"\",\"name\":\"" << EscapeJson(item.name)
               << L"\",\"source\":\"" << StartupSourceKey(item.source)
               << L"\",\"location\":\"" << EscapeJson(item.location)
               << L"\",\"command\":\"" << EscapeJson(item.command)
               << L"\",\"canDisable\":" << (item.canDisable ? L"true" : L"false")
               << L",\"requiresAdmin\":" << (item.requiresAdmin ? L"true" : L"false");
        if (schemaVersion >= 2) {
            output << L",\"readOnly\":" << (item.readOnly ? L"true" : L"false")
                   << L",\"state\":\"" << (item.readOnly ? L"readonly" : L"enabled") << L"\"";
        }
        output << L"}";
    }
    if (!result.items.empty()) output << L"\n  ";
    output << L"],\n  \"warnings\": [";
    for (std::size_t index = 0; index < result.warnings.size(); ++index) {
        output << (index == 0 ? L"" : L",") << L"\"" << EscapeJson(result.warnings[index]) << L"\"";
    }
    output << L"]\n}\n";
    return output.str();
}

std::wstring DiffJson(const StartupSnapshotDiff& diff) {
    std::wostringstream output;
    output << L"{\n  \"schemaVersion\": 1,\n";
    const auto writeEntries = [&](const wchar_t* name, const std::vector<StartupSnapshotEntry>& entries, bool comma) {
        output << L"  \"" << name << L"\": [";
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            output << (index ? L"," : L"") << L"{\"entryId\":\"" << EscapeJson(entry.entryId)
                   << L"\",\"source\":\"" << StartupSourceKey(entry.source)
                   << L"\",\"displayName\":\"" << EscapeJson(entry.displayName)
                   << L"\",\"targetPath\":\"" << EscapeJson(entry.targetPath)
                   << L"\",\"state\":\"" << EscapeJson(entry.state) << L"\"}";
        }
        output << L"]" << (comma ? L",\n" : L"\n");
    };
    writeEntries(L"added", diff.added, true);
    writeEntries(L"removed", diff.removed, true);
    writeEntries(L"changed", diff.changed, true);
    writeEntries(L"stateChanged", diff.stateChanged, false);
    output << L"}\n";
    return output.str();
}

std::wstring DisabledPlain(const std::vector<DisabledRecord>& records) {
    std::wostringstream output;
    for (const auto& record : records) {
        output << record.recordId << L"\t" << record.name << L"\t" << StartupSourceText(record.source)
               << L"\t" << record.disabledAt << L"\n";
    }
    return output.str();
}

std::wstring DisabledJson(const std::vector<DisabledRecord>& records) {
    std::wostringstream output;
    output << L"{\n  \"schemaVersion\": 1,\n  \"items\": [";
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        output << (index == 0 ? L"\n" : L",\n")
               << L"    {\"recordId\":\"" << EscapeJson(record.recordId)
               << L"\",\"itemId\":\"" << EscapeJson(record.itemId)
               << L"\",\"name\":\"" << EscapeJson(record.name)
               << L"\",\"source\":\"" << StartupSourceKey(record.source)
               << L"\",\"disabledAt\":\"" << EscapeJson(record.disabledAt) << L"\"}";
    }
    if (!records.empty()) output << L"\n  ";
    output << L"]\n}\n";
    return output.str();
}

std::wstring MapField(const std::map<std::wstring, std::wstring>& values, const wchar_t* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::wstring{} : found->second;
}

std::wstring BlockedPlain(const std::vector<DisabledRecord>& records) {
    std::wostringstream output;
    AdBlockManager adBlock;
    for (const auto& record : records) {
        const std::wstring blockMode = MapField(record.original, L"blockMode");
        const std::wstring mode = blockMode == L"exact" ? L"精确路径"
            : blockMode == L"startup" ? L"禁止自启" : L"同名程序";
        const AdBlockRecordStatus status = adBlock.CheckRecordStatus(record);
        output << record.recordId << L"\t" << record.name << L"\t" << mode
               << L"\t" << AdBlockRecordStateText(status.state)
               << L"\t" << MapField(record.original, L"targetPath") << L"\t" << record.disabledAt << L"\n";
    }
    return output.str();
}

std::wstring BlockedJson(const std::vector<DisabledRecord>& records) {
    std::wostringstream output;
    AdBlockManager adBlock;
    output << L"{\n  \"schemaVersion\": 1,\n  \"items\": [";
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        const AdBlockRecordStatus status = adBlock.CheckRecordStatus(record);
        output << (index == 0 ? L"\n" : L",\n")
               << L"    {\"recordId\":\"" << EscapeJson(record.recordId)
               << L"\",\"name\":\"" << EscapeJson(record.name)
               << L"\",\"mode\":\"" << EscapeJson(MapField(record.original, L"blockMode"))
               << L"\",\"state\":\"" << EscapeJson(AdBlockRecordStateKey(status.state))
               << L"\",\"stateText\":\"" << EscapeJson(AdBlockRecordStateText(status.state))
               << L"\",\"stateMessage\":\"" << EscapeJson(status.message)
               << L"\",\"targetPath\":\"" << EscapeJson(MapField(record.original, L"targetPath"))
               << L"\",\"imageName\":\"" << EscapeJson(MapField(record.original, L"ifeoImageName"))
               << L"\",\"disabledAt\":\"" << EscapeJson(record.disabledAt) << L"\"}";
    }
    if (!records.empty()) output << L"\n  ";
    output << L"]\n}\n";
    return output.str();
}

int RunCli(const std::vector<std::wstring>& arguments) {
    AttachParentConsole();
    if (arguments.empty()) return 2;
    SetOperationAuditContext(
        L"cli-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()),
        L"cli",
        RunningAsAdministrator());
    StartupManager manager;
    if (arguments[0] == L"version") {
        WriteOutput(JsonFormat(arguments)
            ? (L"{\"schemaVersion\":1,\"productVersion\":\"" + EscapeJson(AppLaunchLockerVersionText()) +
                L"\",\"protocolVersion\":1,\"disabledStoreSchemaVersion\":2,\"snapshotSchemaVersion\":1}\n")
            : (std::wstring(AppLaunchLockerVersionText()) + L"\n"));
        return 0;
    }
    if (arguments[0] == L"scan") {
        const ScanResult result = manager.ScanAll();
        for (const auto& warning : result.warnings) AppendAppLaunchLockerLog(L"扫描警告：" + warning);
        const int schemaVersion = ArgumentValue(arguments, L"--schema-version") == L"2" ? 2 : 1;
        WriteOutput(JsonFormat(arguments) ? ScanJson(result, schemaVersion) : ScanPlain(result));
        return 0;
    }
    if (arguments[0] == L"diff") {
        StartupSnapshot previous;
        std::wstring error;
        if (!StartupSnapshotStore().Load(previous, error)) {
            WriteOutput(error + L"\n", true);
            return 1;
        }
        std::vector<DisabledRecord> disabled;
        if (!manager.LoadDisabled(disabled, error)) {
            WriteOutput(error + L"\n", true);
            return 1;
        }
        const StartupSnapshot current = BuildStartupSnapshot(manager.ScanAll(), disabled);
        const StartupSnapshotDiff diff = DiffStartupSnapshots(previous, current);
        if (JsonFormat(arguments)) WriteOutput(DiffJson(diff));
        else WriteOutput(L"新增 " + std::to_wstring(diff.added.size()) + L"，移除 " +
            std::to_wstring(diff.removed.size()) + L"，变化 " + std::to_wstring(diff.changed.size()) +
            L"，状态变化 " + std::to_wstring(diff.stateChanged.size()) + L"\n");
        return 0;
    }
    if (arguments[0] == L"list-disabled") {
        std::vector<DisabledRecord> records;
        std::wstring error;
        if (!manager.LoadDisabled(records, error)) {
            AppendAppLaunchLockerLog(error);
            WriteOutput(error + L"\n", true);
            return 1;
        }
        WriteOutput(JsonFormat(arguments) ? DisabledJson(records) : DisabledPlain(records));
        return 0;
    }
    if (arguments[0] == L"list-blocked" || arguments[0] == L"adblock-list" || arguments[0] == L"--adblock-list" ||
        arguments[0] == L"adblock-diagnose" || arguments[0] == L"--adblock-diagnose") {
        AdBlockManager adBlock;
        std::vector<DisabledRecord> records;
        std::wstring error;
        if (!adBlock.ListBlocked(records, error)) {
            AppendAppLaunchLockerLog(error);
            WriteOutput(error + L"\n", true);
            return 1;
        }
        WriteOutput(JsonFormat(arguments) ? BlockedJson(records) : BlockedPlain(records));
        return 0;
    }
    if (arguments[0] == L"block") {
        const std::wstring path = ArgumentValue(arguments, L"--path");
        std::wstring mode = ArgumentValue(arguments, L"--mode");
        if (mode.empty()) mode = L"exact";
        if (path.empty()) {
            WriteOutput(L"缺少 --path。\n", true);
            return 2;
        }
        const OperationResult operation = AdBlockManager().Block(path, mode);
        WriteOutput(operation.message + L"\n", !operation.success);
        if (!operation.success) AppendAppLaunchLockerLog(L"拦截失败：" + operation.message);
        return operation.success ? 0 : 1;
    }
    if (arguments[0] == L"unblock" || arguments[0] == L"adblock-unblock" || arguments[0] == L"--adblock-unblock") {
        const std::wstring id = ArgumentValue(arguments, L"--record-id");
        const std::wstring positionalId = arguments.size() >= 2 ? arguments[1] : std::wstring{};
        const std::wstring recordId = id.empty() ? positionalId : id;
        if (recordId.empty()) {
            WriteOutput(L"缺少 --record-id。\n", true);
            return 2;
        }
        const OperationResult operation = AdBlockManager().Unblock(recordId);
        WriteOutput(operation.message + L"\n", !operation.success);
        if (!operation.success) AppendAppLaunchLockerLog(L"解除拦截失败：" + operation.message);
        return operation.success ? 0 : 1;
    }
    if (arguments[0] == L"unblock-all" || arguments[0] == L"adblock-unblock-all" ||
        arguments[0] == L"--adblock-unblock-all") {
        const OperationResult operation = AdBlockManager().UnblockAll();
        WriteOutput(operation.message + L"\n", !operation.success);
        if (!operation.success) AppendAppLaunchLockerLog(L"全部解除拦截失败：" + operation.message);
        return operation.success ? 0 : 1;
    }
    if (arguments[0] == L"repair" || arguments[0] == L"adblock-repair" || arguments[0] == L"--adblock-repair") {
        const std::wstring id = ArgumentValue(arguments, L"--record-id");
        const std::wstring positionalId = arguments.size() >= 2 ? arguments[1] : std::wstring{};
        const std::wstring recordId = id.empty() ? positionalId : id;
        if (recordId.empty()) {
            WriteOutput(L"缺少 --record-id。\n", true);
            return 2;
        }
        const OperationResult operation = AdBlockManager().RepairRecord(recordId);
        WriteOutput(operation.message + L"\n", !operation.success);
        if (!operation.success) AppendAppLaunchLockerLog(L"修复拦截失败：" + operation.message);
        return operation.success ? 0 : 1;
    }
    if (arguments[0] == L"clean-stale" || arguments[0] == L"adblock-clean-stale" ||
        arguments[0] == L"--adblock-clean-stale") {
        const OperationResult operation = AdBlockManager().CleanStaleRecords();
        WriteOutput(operation.message + L"\n", !operation.success);
        if (!operation.success) AppendAppLaunchLockerLog(L"清理失效拦截记录失败：" + operation.message);
        return operation.success ? 0 : 1;
    }
    OperationResult operation;
    if (arguments[0] == L"disable-application" || arguments[0] == L"restore-application") {
        const std::wstring appId = ArgumentValue(arguments, L"--app-id");
        if (appId.empty()) {
            WriteOutput(L"缺少 --app-id。\n", true);
            return 2;
        }
        std::vector<DisabledRecord> disabled;
        std::wstring error;
        if (!manager.LoadDisabled(disabled, error)) {
            WriteOutput(error + L"\n", true);
            return 1;
        }
        const std::vector<StartupApplication> applications = BuildStartupApplications(manager.ScanAll(), disabled);
        const auto found = std::find_if(applications.begin(), applications.end(), [&](const StartupApplication& app) {
            return app.appId == appId;
        });
        if (found == applications.end()) {
            WriteOutput(L"未找到指定应用，请重新扫描。\n", true);
            return 1;
        }
        std::vector<std::wstring> ids;
        const bool restoreApplication = arguments[0] == L"restore-application";
        for (const StartupApplicationEntry& entry : found->entries) {
            if (restoreApplication && entry.state == StartupEntryState::DisabledByTool && entry.canRestore) ids.push_back(entry.entryId);
            if (!restoreApplication && entry.state == StartupEntryState::Enabled && entry.canDisable) ids.push_back(entry.entryId);
        }
        operation = restoreApplication ? manager.RestoreMany(ids) : manager.DisableMany(ids);
    } else if (arguments[0] == L"disable") {
        const std::wstring id = ArgumentValue(arguments, L"--id");
        if (id.empty()) {
            WriteOutput(L"缺少 --id。\n", true);
            return 2;
        }
        operation = manager.Disable(id);
    } else if (arguments[0] == L"disable-many") {
        std::vector<std::wstring> ids = ArgumentValues(arguments, L"--id");
        if (ids.empty()) {
            WriteOutput(L"缺少 --id。\n", true);
            return 2;
        }
        operation = manager.DisableMany(ids);
    } else if (arguments[0] == L"restore") {
        const std::wstring id = ArgumentValue(arguments, L"--record-id");
        if (id.empty()) {
            WriteOutput(L"缺少 --record-id。\n", true);
            return 2;
        }
        operation = manager.Restore(id);
    } else if (arguments[0] == L"restore-many") {
        std::vector<std::wstring> ids = ArgumentValues(arguments, L"--record-id");
        if (ids.empty()) {
            WriteOutput(L"缺少 --record-id。\n", true);
            return 2;
        }
        operation = manager.RestoreMany(ids);
    } else {
        WriteOutput(L"未知命令。支持 version、scan、diff、list-disabled、disable、disable-many、disable-application、restore、restore-many、restore-application、block、unblock、unblock-all、repair、clean-stale、list-blocked。\n", true);
        return 2;
    }
    WriteOutput(JsonFormat(arguments) ? OperationJson(operation) : operation.message + L"\n", !operation.success);
    if (!operation.success) AppendAppLaunchLockerLog(L"CLI 操作失败：" + operation.message);
    return operation.success ? 0 : 1;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argumentCount = 0;
    LPWSTR* rawArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    // IFEO 占位：系统会把被拦截程序改为启动本 exe 的 --ifeo-noop 分支。必须在任何
    // 窗口/COM/CLI 初始化之前判定并瞬时退出，做到零副作用、零依赖。
    if (argumentCount == 2 && rawArguments && wcscmp(rawArguments[1], L"--ifeo-noop") == 0) {
        LocalFree(rawArguments);
        return 0;
    }
    std::vector<std::wstring> arguments;
    for (int index = 1; rawArguments && index < argumentCount; ++index) arguments.emplace_back(rawArguments[index]);
    if (rawArguments) LocalFree(rawArguments);
    if (!arguments.empty() && arguments[0] == L"runas-operation") {
        return RunElevatedOperation(arguments);
    }
    const bool adBlockMode = arguments.size() == 1 && arguments[0] == L"--ad-block";
    const bool startupGuiMode = IsStartupGuiLaunchIntent(arguments);
    if (!adBlockMode && !startupGuiMode) return RunCli(arguments);

    HANDLE startupInstanceMutex = nullptr;
    if (startupGuiMode) {
        startupInstanceMutex = CreateMutexW(nullptr, FALSE, kStartupSingleInstanceMutexName);
        if (startupInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            if (ActivateExistingStartupWindow()) {
                CloseHandle(startupInstanceMutex);
                return 0;
            }
        }
    }

    // 广告拦截简化窗口入口：工具箱「广告拦截」以 --ad-block 拉起本 exe。
    if (adBlockMode) {
        return RunGui(L"ad-block", [&]() {
            INITCOMMONCONTROLSEX adControls{};
            adControls.dwSize = sizeof(adControls);
            adControls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS;
            InitCommonControlsEx(&adControls);
            const Theme adTheme = Theme::Load(
                ModuleDirectory() / L"theme", L"default", instance, IDR_QUATTRO_DEFAULT_THEME);
            AdBlockWindow adWindow(instance, adTheme);
            return adWindow.Run();
        });
    }

    const int result = RunGui(L"startup-manager", [&]() {
        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&controls);
        const Theme theme = Theme::Load(
            ModuleDirectory() / L"theme", L"default", instance, IDR_QUATTRO_DEFAULT_THEME);
        AppLaunchLockerWindow window(instance, theme);
        return window.Run();
    });
    if (startupInstanceMutex) CloseHandle(startupInstanceMutex);
    return result;
}
