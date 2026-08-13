#include "AppLaunchLockerCore.h"
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
    AdBlockManager adBlock;
    if (request.action == L"adblock-block") {
        int succeeded = 0;
        int failed = 0;
        std::wstring lastError;
        for (const std::wstring& target : request.targets) {
            const OperationResult item = adBlock.Block(target, request.mode);
            operation.items.push_back({target, L"adblock-block", item.success, item.message, false});
            if (item.success) ++succeeded;
            else {
                ++failed;
                lastError = item.message;
            }
        }
        operation.success = failed == 0 || succeeded > 0;
        operation.partial = succeeded > 0 && failed > 0;
        operation.message = failed == 0
            ? L"已拦截 " + std::to_wstring(succeeded) + L" 个程序。"
            : L"已拦截 " + std::to_wstring(succeeded) + L" 个，" + std::to_wstring(failed) +
                L" 个失败：" + lastError;
    } else if (request.action == L"adblock-unblock") {
        operation = adBlock.Unblock(request.targets.front());
    } else if (request.action == L"adblock-unblock-all") {
        operation = adBlock.UnblockAll();
    } else if (request.action == L"adblock-repair") {
        operation = adBlock.RepairRecord(request.targets.front());
    } else {
        operation = {false, L"管理员操作动作不受支持。"};
    }

    if (!WriteElevatedOperationResult(request, operation, error)) {
        AppendAppLaunchLockerLog(L"管理员结果写入失败：" + error);
        return 4;
    }
    return operation.success ? 0 : 1;
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
        const std::wstring mode = blockMode == L"exact" ? L"精确路径" : L"同名程序";
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
    if (arguments[0] == L"version") {
        WriteOutput(JsonFormat(arguments)
            ? (L"{\"schemaVersion\":1,\"productVersion\":\"" + EscapeJson(AppLaunchLockerVersionText()) +
                L"\",\"protocolVersion\":1,\"adBlockStoreSchemaVersion\":2}\n")
            : (std::wstring(AppLaunchLockerVersionText()) + L"\n"));
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
    WriteOutput(L"未知命令。支持 version、block、unblock、unblock-all、repair、clean-stale、list-blocked。\n", true);
    return 2;
}
} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argumentCount = 0;
    LPWSTR* rawArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    // IFEO 占位：系统会把被拦截程序改为启动本 exe 的 --ifeo-noop 分支，并在此后追加
    // 被拦截程序的完整路径和原始命令行参数（argc 必然 >= 3）。必须在任何窗口/COM/CLI
    // 初始化之前判定并瞬时退出，做到零副作用、零依赖。
    if (argumentCount >= 2 && rawArguments && wcscmp(rawArguments[1], L"--ifeo-noop") == 0) {
        LocalFree(rawArguments);
        return 0;
    }
    std::vector<std::wstring> arguments;
    for (int index = 1; rawArguments && index < argumentCount; ++index) arguments.emplace_back(rawArguments[index]);
    if (rawArguments) LocalFree(rawArguments);
    if (!arguments.empty() && arguments[0] == L"runas-operation") {
        return RunElevatedOperation(arguments);
    }

    const bool adBlockMode = arguments.empty() || (arguments.size() == 1 && arguments[0] == L"--ad-block");
    if (!adBlockMode) return RunCli(arguments);

    return RunGui(L"ad-block", [&]() {
        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&controls);
        const Theme theme = Theme::Load(
            ModuleDirectory() / L"theme", L"default", instance, IDR_QUATTRO_DEFAULT_THEME);
        AdBlockWindow window(instance, theme);
        return window.Run();
    });
}
