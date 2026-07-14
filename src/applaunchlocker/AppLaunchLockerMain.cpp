#include "AppLaunchLockerCore.h"
#include "AppLaunchLockerWindow.h"

#include "Theme.h"
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

std::wstring ScanPlain(const ScanResult& result) {
    std::wostringstream output;
    for (const auto& item : result.items) {
        output << item.id << L"\t" << item.name << L"\t" << StartupSourceText(item.source)
               << L"\t" << (item.canDisable ? L"可禁用" : L"仅查看") << L"\t" << item.location << L"\n";
    }
    for (const auto& warning : result.warnings) output << L"警告：" << warning << L"\n";
    return output.str();
}

std::wstring ScanJson(const ScanResult& result) {
    std::wostringstream output;
    output << L"{\n  \"items\": [";
    for (std::size_t index = 0; index < result.items.size(); ++index) {
        const auto& item = result.items[index];
        output << (index == 0 ? L"\n" : L",\n")
               << L"    {\"id\":\"" << EscapeJson(item.id)
               << L"\",\"name\":\"" << EscapeJson(item.name)
               << L"\",\"source\":\"" << StartupSourceKey(item.source)
               << L"\",\"location\":\"" << EscapeJson(item.location)
               << L"\",\"command\":\"" << EscapeJson(item.command)
               << L"\",\"canDisable\":" << (item.canDisable ? L"true" : L"false")
               << L",\"requiresAdmin\":" << (item.requiresAdmin ? L"true" : L"false") << L"}";
    }
    if (!result.items.empty()) output << L"\n  ";
    output << L"],\n  \"warnings\": [";
    for (std::size_t index = 0; index < result.warnings.size(); ++index) {
        output << (index == 0 ? L"" : L",") << L"\"" << EscapeJson(result.warnings[index]) << L"\"";
    }
    output << L"]\n}\n";
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
    output << L"{\n  \"items\": [";
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

int RunCli(const std::vector<std::wstring>& arguments) {
    AttachParentConsole();
    if (arguments.empty()) return 2;
    StartupManager manager;
    if (arguments[0] == L"version") {
        WriteOutput(JsonFormat(arguments)
            ? (L"{\"version\":\"" + EscapeJson(QuattroVersionText()) + L"\"}\n")
            : (std::wstring(QuattroVersionText()) + L"\n"));
        return 0;
    }
    if (arguments[0] == L"scan") {
        const ScanResult result = manager.ScanAll();
        for (const auto& warning : result.warnings) AppendAppLaunchLockerLog(L"扫描警告：" + warning);
        WriteOutput(JsonFormat(arguments) ? ScanJson(result) : ScanPlain(result));
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
    OperationResult operation;
    if (arguments[0] == L"disable") {
        const std::wstring id = ArgumentValue(arguments, L"--id");
        if (id.empty()) {
            WriteOutput(L"缺少 --id。\n", true);
            return 2;
        }
        operation = manager.Disable(id);
    } else if (arguments[0] == L"restore") {
        const std::wstring id = ArgumentValue(arguments, L"--record-id");
        if (id.empty()) {
            WriteOutput(L"缺少 --record-id。\n", true);
            return 2;
        }
        operation = manager.Restore(id);
    } else {
        WriteOutput(L"未知命令。支持 version、scan、list-disabled、disable、restore。\n", true);
        return 2;
    }
    WriteOutput(operation.message + L"\n", !operation.success);
    if (!operation.success) AppendAppLaunchLockerLog(L"CLI 操作失败：" + operation.message);
    return operation.success ? 0 : 1;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argumentCount = 0;
    LPWSTR* rawArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::vector<std::wstring> arguments;
    for (int index = 1; rawArguments && index < argumentCount; ++index) arguments.emplace_back(rawArguments[index]);
    if (rawArguments) LocalFree(rawArguments);
    if (!arguments.empty()) return RunCli(arguments);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);
    const Theme theme = Theme::Load(
        ModuleDirectory() / L"theme", L"default", instance, IDR_QUATTRO_DEFAULT_THEME);
    AppLaunchLockerWindow window(instance, theme);
    return window.Run();
}
