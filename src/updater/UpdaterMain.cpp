#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>

namespace {
struct UpdatePlan {
    DWORD pid = 0;
    std::filesystem::path source;
    std::filesystem::path target;
    std::filesystem::path backup;
    std::filesystem::path restart;
    std::wstring restartArgs;
    std::filesystem::path logPath;
    DWORD waitTimeoutMs = 60000;
};

std::wstring TrimDash(std::wstring value) {
    while (!value.empty() && value.front() == L'-') {
        value.erase(value.begin());
    }
    return value;
}

std::map<std::wstring, std::wstring> ParseArguments() {
    std::map<std::wstring, std::wstring> args;
    const int count = __argc;
    for (int i = 1; i < count; ++i) {
        std::wstring key = TrimDash(__wargv[i]);
        if (key.empty()) {
            continue;
        }
        std::wstring value = L"1";
        if (i + 1 < count && __wargv[i + 1][0] != L'-') {
            value = __wargv[++i];
        }
        args[key] = value;
    }
    return args;
}

std::wstring FormatLastErrorMessage(DWORD error = GetLastError()) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);
    if (length == 0 || !message) {
        return L"Windows error " + std::to_wstring(error);
    }
    std::wstring text(message, length);
    LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string bytes(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
    return bytes;
}

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

void WriteLog(const UpdatePlan& plan, const std::wstring& message) {
    if (plan.logPath.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(plan.logPath.parent_path(), ec);
    std::ofstream file(plan.logPath, std::ios::binary | std::ios::app);
    if (!file) {
        return;
    }
    file << WideToUtf8(Timestamp() + L" " + message + L"\n");
}

bool ParsePlan(UpdatePlan& plan, std::wstring& error) {
    const auto args = ParseArguments();
    auto find = [&](const wchar_t* key) -> std::wstring {
        const auto it = args.find(key);
        return it == args.end() ? L"" : it->second;
    };

    const std::wstring pid = find(L"pid");
    if (!pid.empty()) {
        try {
            plan.pid = static_cast<DWORD>(std::stoul(pid));
        } catch (...) {
            error = L"pid 参数无效。";
            return false;
        }
    }

    const std::wstring timeout = find(L"timeout-ms");
    if (!timeout.empty()) {
        try {
            plan.waitTimeoutMs = static_cast<DWORD>(std::stoul(timeout));
        } catch (...) {
            error = L"timeout-ms 参数无效。";
            return false;
        }
    }

    plan.source = find(L"source");
    plan.target = find(L"target");
    plan.backup = find(L"backup");
    plan.restart = find(L"restart");
    plan.restartArgs = find(L"restart-args");
    plan.logPath = find(L"log");

    if (plan.source.empty() || plan.target.empty()) {
        error = L"source 和 target 参数不能为空。";
        return false;
    }
    if (plan.backup.empty()) {
        plan.backup = plan.target;
        plan.backup += L".bak";
    }
    return true;
}

bool WaitForProcessExit(const UpdatePlan& plan, std::wstring& error) {
    if (plan.pid == 0) {
        return true;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, plan.pid);
    if (!process) {
        WriteLog(plan, L"目标进程已不存在或无法打开，继续更新。");
        return true;
    }
    const DWORD result = WaitForSingleObject(process, plan.waitTimeoutMs);
    CloseHandle(process);
    if (result == WAIT_OBJECT_0) {
        return true;
    }
    error = result == WAIT_TIMEOUT ? L"等待目标进程退出超时。" : L"等待目标进程失败: " + FormatLastErrorMessage();
    return false;
}

bool RetryDelete(const std::filesystem::path& path) {
    if (path.empty()) {
        return true;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    for (int i = 0; i < 20; ++i) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(path.c_str())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}

bool RetryMove(const std::filesystem::path& from, const std::filesystem::path& to) {
    for (int i = 0; i < 20; ++i) {
        SetFileAttributesW(from.c_str(), FILE_ATTRIBUTE_NORMAL);
        SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}

bool RetryCopy(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    for (int i = 0; i < 20; ++i) {
        SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}

bool ReplaceTarget(const UpdatePlan& plan, std::wstring& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(plan.source, ec)) {
        error = L"新版文件不存在: " + plan.source.wstring();
        return false;
    }

    const bool hadTarget = std::filesystem::exists(plan.target, ec);
    if (hadTarget) {
        RetryDelete(plan.backup);
        if (!RetryMove(plan.target, plan.backup)) {
            error = L"备份旧版本失败: " + FormatLastErrorMessage();
            return false;
        }
        WriteLog(plan, L"已备份旧版本: " + plan.backup.wstring());
    }

    if (!RetryCopy(plan.source, plan.target)) {
        error = L"复制新版失败: " + FormatLastErrorMessage();
        if (hadTarget && std::filesystem::exists(plan.backup, ec)) {
            RetryMove(plan.backup, plan.target);
        }
        return false;
    }

    WriteLog(plan, L"已替换目标文件: " + plan.target.wstring());
    return true;
}

std::wstring QuoteForCommandLine(const std::wstring& value) {
    if (value.empty()) {
        return L"\"\"";
    }
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') {
            result += L'\\';
        }
        result += ch;
    }
    result += L'"';
    return result;
}

bool RestartTarget(const UpdatePlan& plan, std::wstring& error) {
    if (plan.restart.empty()) {
        return true;
    }
    std::wstring command = QuoteForCommandLine(plan.restart.wstring());
    if (!plan.restartArgs.empty()) {
        command += L" ";
        command += plan.restartArgs;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, plan.restart.parent_path().c_str(), &startup, &process)) {
        error = L"重启新版失败: " + FormatLastErrorMessage();
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    WriteLog(plan, L"已启动新版。");
    return true;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    UpdatePlan plan;
    std::wstring error;
    if (!ParsePlan(plan, error)) {
        MessageBoxW(nullptr, error.c_str(), L"Quattro 更新器", MB_OK | MB_ICONWARNING);
        return 2;
    }

    WriteLog(plan, L"更新器启动。");
    if (!WaitForProcessExit(plan, error) ||
        !ReplaceTarget(plan, error) ||
        !RestartTarget(plan, error)) {
        WriteLog(plan, L"更新失败: " + error);
        MessageBoxW(nullptr, error.c_str(), L"Quattro 更新器", MB_OK | MB_ICONWARNING);
        return 1;
    }

    RetryDelete(plan.source);
    WriteLog(plan, L"更新完成。");
    return 0;
}
