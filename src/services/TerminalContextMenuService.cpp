#include "TerminalContextMenuService.h"

#include "Utilities.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {
bool IsRunnableExecutable(const std::wstring& value) {
    if (Trim(value).empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(value), ec)) {
        return false;
    }
    DWORD binaryType = 0;
    return GetBinaryTypeW(value.c_str(), &binaryType) != FALSE;
}

std::wstring AppPathValue(HKEY root, const std::wstring& executable, REGSAM view) {
    HKEY key = nullptr;
    const std::wstring subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + executable;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key, nullptr, nullptr, &type,
            reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    value = ExpandEnvironmentStringsSafe(Trim(value));
    return IsRunnableExecutable(value) ? value : std::wstring{};
}

std::wstring FindProgram(const std::vector<std::wstring>& executableNames) {
    for (const auto& name : executableNames) {
        const std::wstring expanded = ExpandEnvironmentStringsSafe(name);
        if (IsRunnableExecutable(expanded)) {
            return std::filesystem::absolute(expanded).wstring();
        }
        std::array<wchar_t, 32768> found{};
        if (SearchPathW(nullptr, name.c_str(), nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr) > 0 &&
            IsRunnableExecutable(found.data())) {
            return found.data();
        }
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            for (REGSAM view : {REGSAM{0}, REGSAM{KEY_WOW64_64KEY}, REGSAM{KEY_WOW64_32KEY}}) {
                if (std::wstring path = AppPathValue(root, name, view); !path.empty()) {
                    return path;
                }
            }
        }
    }
    return {};
}

struct ProcessResult {
    bool started = false;
    bool completed = false;
    DWORD exitCode = ERROR_GEN_FAILURE;
    std::vector<unsigned char> output;
};

ProcessResult RunHiddenProbe(
    const std::wstring& executable,
    const std::wstring& arguments,
    DWORD timeoutMs) {
    ProcessResult result;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring commandLine = QuoteForCommandLine(executable);
    if (!arguments.empty()) {
        commandLine += L" " + arguments;
    }
    result.started = CreateProcessW(
        executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(writePipe);
    if (!result.started) {
        CloseHandle(readPipe);
        return result;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMs);
    result.completed = wait == WAIT_OBJECT_0;
    if (!result.completed) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 1000);
    }
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    std::array<unsigned char, 4096> buffer{};
    DWORD available = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available != 0) {
        DWORD read = 0;
        const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(readPipe, buffer.data(), requested, &read, nullptr) || read == 0) {
            break;
        }
        result.output.insert(result.output.end(), buffer.begin(), buffer.begin() + read);
        if (result.output.size() >= 64 * 1024) {
            break;
        }
    }
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

bool HasVisibleOutput(const std::vector<unsigned char>& output) {
    return std::any_of(output.begin(), output.end(), [](unsigned char value) {
        return value != 0 && !std::isspace(value) && value != 0xFF && value != 0xFE;
    });
}

bool ContainsCdOption(const std::vector<unsigned char>& output) {
    constexpr std::array<unsigned char, 4> ascii{{'-', '-', 'c', 'd'}};
    constexpr std::array<unsigned char, 8> utf16{{'-', 0, '-', 0, 'c', 0, 'd', 0}};
    return std::search(output.begin(), output.end(), ascii.begin(), ascii.end()) != output.end() ||
        std::search(output.begin(), output.end(), utf16.begin(), utf16.end()) != output.end();
}

void AddProgram(
    TerminalContextMenuRefreshContext& context,
    std::wstring id,
    std::wstring text,
    std::wstring executable) {
    if (executable.empty()) {
        return;
    }
    TerminalContextMenuProgram program;
    program.id = std::move(id);
    program.text = std::move(text);
    program.executable = std::move(executable);
    program.iconTemplate.providerId = ShellContextMenuProviderId::Terminal;
    program.iconTemplate.verb = program.id;
    ShellItemService::LoadExecutableMenuIcon(program.executable, program.iconTemplate);
    context.programs.push_back(std::move(program));
}

bool IsUrl(const Link& link) {
    if (link.type == 2) {
        return true;
    }
    const std::wstring lower = ToLower(Trim(link.path));
    return lower.rfind(L"http://", 0) == 0 ||
        lower.rfind(L"https://", 0) == 0 ||
        lower.rfind(L"ftp://", 0) == 0 ||
        lower.rfind(L"www.", 0) == 0;
}

std::optional<std::wstring> TerminalArguments(
    const std::wstring& actionId,
    const std::wstring& workingDirectory) {
    if (actionId == L"cmd") return L"/K";
    if (actionId == L"powershell") return L"-NoExit";
    if (actionId == L"wsl") return L"--cd " + QuoteForCommandLine(workingDirectory);
    if (actionId == L"wezterm") return L"start --cwd " + QuoteForCommandLine(workingDirectory);
    if (actionId == L"alacritty") return L"--working-directory " + QuoteForCommandLine(workingDirectory);
    if (actionId == L"cmder") return L"/START " + QuoteForCommandLine(workingDirectory);
    if (actionId == L"conemu") return L"-Dir " + QuoteForCommandLine(workingDirectory);
    return std::nullopt;
}

bool MatchesTerminalExecutable(const std::wstring& actionId, const std::wstring& executable) {
    const std::wstring name = ToLower(std::filesystem::path(executable).filename().wstring());
    if (actionId == L"cmd") return name == L"cmd.exe";
    if (actionId == L"powershell") return name == L"pwsh.exe" || name == L"powershell.exe";
    if (actionId == L"wsl") return name == L"wsl.exe";
    if (actionId == L"wezterm") return name == L"wezterm.exe";
    if (actionId == L"alacritty") return name == L"alacritty.exe";
    if (actionId == L"cmder") return name == L"cmder.exe";
    if (actionId == L"conemu") return name == L"conemu64.exe" || name == L"conemu.exe";
    return false;
}
}

TerminalContextMenuRefreshContext TerminalContextMenuService::DetectAvailablePrograms() {
    TerminalContextMenuRefreshContext context;

    std::wstring commandPrompt = ExpandEnvironmentStringsSafe(Trim([] {
        std::array<wchar_t, 32768> value{};
        const DWORD length = GetEnvironmentVariableW(L"COMSPEC", value.data(), static_cast<DWORD>(value.size()));
        return length > 0 && length < value.size() ? std::wstring(value.data(), length) : std::wstring{};
    }()));
    if (!IsRunnableExecutable(commandPrompt)) {
        commandPrompt = FindProgram({L"cmd.exe"});
    }
    AddProgram(context, L"cmd", L"此处打开 CMD 窗口", commandPrompt);

    std::wstring powershell = FindProgram({
        L"pwsh.exe",
        L"%ProgramFiles%\\PowerShell\\7\\pwsh.exe",
        L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\pwsh.exe",
    });
    if (powershell.empty()) {
        powershell = FindProgram({L"powershell.exe"});
    }
    AddProgram(context, L"powershell", L"此处打开 PowerShell 窗口", powershell);

    const std::wstring wsl = FindProgram({L"wsl.exe"});
    if (!wsl.empty()) {
        const ProcessResult distributions = RunHiddenProbe(wsl, L"--list --quiet", 4000);
        const ProcessResult help = RunHiddenProbe(wsl, L"--help", 2000);
        if (distributions.completed && distributions.exitCode == 0 && HasVisibleOutput(distributions.output) &&
            help.completed && help.exitCode == 0 && ContainsCdOption(help.output)) {
            AddProgram(context, L"wsl", L"此处打开 WSL 窗口", wsl);
        }
    }

    AddProgram(
        context, L"wezterm", L"此处打开 WezTerm", FindProgram({
            L"wezterm.exe",
            L"%ProgramFiles%\\WezTerm\\wezterm.exe",
            L"%LOCALAPPDATA%\\Programs\\WezTerm\\wezterm.exe",
        }));
    AddProgram(
        context, L"alacritty", L"此处打开 Alacritty", FindProgram({
            L"alacritty.exe",
            L"%ProgramFiles%\\Alacritty\\alacritty.exe",
            L"%LOCALAPPDATA%\\Programs\\Alacritty\\alacritty.exe",
        }));
    AddProgram(
        context, L"cmder", L"此处打开 Cmder", FindProgram({
            L"Cmder.exe",
            L"%CMDER_ROOT%\\Cmder.exe",
            L"%ProgramFiles%\\Cmder\\Cmder.exe",
            L"%LOCALAPPDATA%\\Programs\\Cmder\\Cmder.exe",
        }));
    AddProgram(
        context, L"conemu", L"此处打开 ConEmu", FindProgram({
            L"ConEmu64.exe",
            L"ConEmu.exe",
            L"%ProgramFiles%\\ConEmu\\ConEmu64.exe",
            L"%ProgramFiles%\\ConEmu\\ConEmu.exe",
            L"%LOCALAPPDATA%\\Programs\\ConEmu\\ConEmu64.exe",
        }));
    return context;
}

std::optional<std::filesystem::path> TerminalContextMenuService::WorkingDirectoryFor(const Link& link) {
    if (IsUrl(link) || ShellItemService::IsShellParseName(link.path)) {
        return std::nullopt;
    }
    const std::filesystem::path target(ExpandEnvironmentStringsSafe(Trim(link.path)));
    if (target.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(target, ec)) {
        return std::filesystem::absolute(target, ec);
    }
    ec.clear();
    if (std::filesystem::is_regular_file(target, ec)) {
        const auto parent = target.parent_path();
        return parent.empty() ? std::nullopt : std::optional<std::filesystem::path>(std::filesystem::absolute(parent, ec));
    }
    return std::nullopt;
}

std::vector<ShellContextMenuItem> TerminalContextMenuService::ItemsFor(
    const Link& link,
    const TerminalContextMenuRefreshContext& context) {
    const auto directory = WorkingDirectoryFor(link);
    if (!directory) {
        return {};
    }
    std::vector<ShellContextMenuItem> items;
    items.reserve(context.programs.size());
    for (const auto& program : context.programs) {
        ShellContextMenuItem item = program.iconTemplate;
        item.providerId = ShellContextMenuProviderId::Terminal;
        item.text = program.text;
        item.verb = program.id;
        item.actionKind = ShellContextMenuActionKind::Terminal;
        item.actionId = program.id;
        item.executable = program.executable;
        item.workingDirectory = directory->wstring();
        item.arguments = TerminalArguments(program.id, item.workingDirectory).value_or(L"");
        items.push_back(std::move(item));
    }
    return items;
}

bool TerminalContextMenuService::Invoke(
    HWND owner,
    const ShellContextMenuLocator& locator,
    std::wstring& errorMessage) {
    errorMessage.clear();
    const auto arguments = TerminalArguments(locator.actionId, locator.workingDirectory);
    if (locator.actionKind != ShellContextMenuActionKind::Terminal ||
        locator.executable.empty() || locator.workingDirectory.empty() ||
        !arguments || !MatchesTerminalExecutable(locator.actionId, locator.executable)) {
        errorMessage = L"终端菜单缓存无效，请点击刷新。";
        return false;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = locator.executable.c_str();
    info.lpParameters = arguments->empty() ? nullptr : arguments->c_str();
    info.lpDirectory = locator.workingDirectory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        errorMessage = L"终端程序已失效或无法启动，请点击刷新。\n\n" + FormatLastError(GetLastError());
        return false;
    }
    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    return true;
}
