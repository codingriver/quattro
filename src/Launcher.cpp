#include "Launcher.h"

#include "ShellItemService.h"
#include "Utilities.h"

#include <shellapi.h>
#include <shlwapi.h>

namespace {
int NormalizeShowCmd(int showCmd) {
    if (showCmd <= 0) {
        return SW_SHOWNORMAL;
    }
    return showCmd;
}
}

Launcher::Launcher(std::filesystem::path appDirectory, AppConfig* config)
    : appDirectory_(std::move(appDirectory)), config_(config) {
}

bool Launcher::IsUrlTarget(const Link& link) {
    const std::wstring path = Trim(link.path);
    if (link.type == 2) {
        return true;
    }
    const std::wstring lower = ToLower(path);
    return lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

bool Launcher::IsShellTarget(const Link& link) {
    return link.type == 3 || ShellItemService::IsShellParseName(link.path);
}

std::wstring Launcher::BuildOpenDirCommand(const std::wstring& commandTemplate, const Link& link) {
    std::wstring command = commandTemplate;
    const std::wstring quotedPath = QuoteForCommandLine(link.path);
    if (command.find(L"{path}") != std::wstring::npos) {
        command = ReplaceAll(command, L"{path}", quotedPath);
    } else {
        command += L" ";
        command += quotedPath;
    }
    command = ReplaceAll(command, L"{name}", QuoteForCommandLine(link.name));
    return command;
}

bool Launcher::Run(Link& link, std::wstring& errorMessage) const {
    errorMessage.clear();
    if (Trim(link.path).empty()) {
        errorMessage = L"启动项路径为空。";
        return false;
    }

    if (link.type == 1 && config_ && !Trim(config_->openDirCommand).empty()) {
        return ExecuteOpenDirCommand(link, errorMessage);
    }

    std::wstring file = ExpandEnvironmentStringsSafe(link.path);
    std::wstring parameters = link.parameter;
    std::wstring directory = link.workDir;
    std::wstring verb = link.isAdmin ? L"runas" : L"open";

    if (IsUrlTarget(link)) {
        file = NormalizeUrl(file);
        if (link.isAdmin) {
            const std::wstring privateArguments = L"-inprivate " + QuoteForCommandLine(file);
            SHELLEXECUTEINFOW privateInfo{};
            privateInfo.cbSize = sizeof(privateInfo);
            privateInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
            privateInfo.lpVerb = L"open";
            privateInfo.lpFile = L"msedge.exe";
            privateInfo.lpParameters = privateArguments.c_str();
            privateInfo.nShow = NormalizeShowCmd(link.showCmd);
            if (ShellExecuteExW(&privateInfo)) {
                if (privateInfo.hProcess) {
                    CloseHandle(privateInfo.hProcess);
                }
                return true;
            }
        }
        parameters.clear();
        directory.clear();
        verb = L"open";
    } else if (IsShellTarget(link)) {
        return ShellItemService::OpenShellTarget(nullptr, link, link.showCmd, errorMessage);
    } else if (directory.empty()) {
        std::filesystem::path path(file);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            directory = path.parent_path().wstring();
        }
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = verb.c_str();
    info.lpFile = file.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    info.nShow = NormalizeShowCmd(link.showCmd);

    if (!ShellExecuteExW(&info)) {
        const DWORD lastError = GetLastError();
        if (!link.pidl.empty() && !link.isAdmin && Trim(link.parameter).empty()) {
            std::wstring shellError;
            if (ShellItemService::OpenShellTarget(nullptr, link, link.showCmd, shellError)) {
                return true;
            }
        }
        errorMessage = FormatLastError(lastError);
        return false;
    }

    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    return true;
}

bool Launcher::ExecuteOpenDirCommand(const Link& link, std::wstring& errorMessage) const {
    const std::wstring command = BuildOpenDirCommand(config_ ? config_->openDirCommand : L"", link);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, appDirectory_.c_str(), &startup, &process)) {
        errorMessage = FormatLastError(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
