#include "UpdateInstaller.h"

#include "AppLog.h"
#include "EmbeddedExecutableManager.h"
#include "Utilities.h"

#include <windows.h>

#include <system_error>

namespace {
std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute;
}

std::wstring FileSizeText(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? L"unknown" : std::to_wstring(size);
}

std::wstring BuildCommandLine(const std::filesystem::path& updater, const UpdateInstallPlan& plan) {
    const std::filesystem::path backup = plan.currentExe.wstring() + L".bak";
    std::wstring command = QuoteForCommandLine(updater.wstring());
    command += L" --pid " + std::to_wstring(GetCurrentProcessId());
    command += L" --source " + QuoteForCommandLine(plan.downloadedExe.wstring());
    command += L" --target " + QuoteForCommandLine(plan.currentExe.wstring());
    command += L" --backup " + QuoteForCommandLine(backup.wstring());
    command += L" --restart " + QuoteForCommandLine(plan.currentExe.wstring());
    command += L" --restart-args " + QuoteForCommandLine(L"--quattro-update-restart");
    if (!plan.latestVersion.empty()) {
        command += L" --version " + QuoteForCommandLine(plan.latestVersion);
    }
    if (!plan.assetName.empty()) {
        command += L" --asset " + QuoteForCommandLine(plan.assetName);
    }
    if (plan.assetSizeBytes != 0) {
        command += L" --asset-size " + std::to_wstring(plan.assetSizeBytes);
    }
    if (!plan.logPath.empty()) {
        command += L" --log " + QuoteForCommandLine(plan.logPath.wstring());
    }
    return command;
}
}

bool LaunchEmbeddedUpdater(const UpdateInstallPlan& plan, std::wstring& error) {
    if (plan.downloadedExe.empty() || !FileExists(plan.downloadedExe)) {
        error = L"新版文件不存在。";
        WriteAppLog(
            L"启动更新器失败: " + error +
            L"，downloaded=" + AbsolutePath(plan.downloadedExe).wstring());
        return false;
    }
    if (plan.currentExe.empty()) {
        error = L"当前程序路径为空。";
        WriteAppLog(L"启动更新器失败: " + error);
        return false;
    }

    const EmbeddedExecutablePrepareResult prepared = PrepareEmbeddedExecutable(
        L"quattro-updater", {QuattroEmbeddedExecutableRootDirectory()});
    if (!prepared.success) {
        error = prepared.message;
        WriteAppLog(L"启动更新器失败: " + error);
        return false;
    }
    const std::filesystem::path updater = prepared.path;

    std::wstring command = BuildCommandLine(updater, plan);
    WriteAppLog(
        L"准备启动更新器。version=" + plan.latestVersion +
        L"，asset=" + plan.assetName +
        L"，asset_size=" + std::to_wstring(plan.assetSizeBytes) +
        L"，downloaded=" + AbsolutePath(plan.downloadedExe).wstring() +
        L"，downloaded_size=" + FileSizeText(plan.downloadedExe) +
        L"，current=" + AbsolutePath(plan.currentExe).wstring() +
        L"，updater=" + AbsolutePath(updater).wstring() +
        L"，log=" + AbsolutePath(plan.logPath).wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            updater.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            updater.parent_path().c_str(),
            &startup,
            &process)) {
        error = L"启动更新器失败: " + FormatLastError(GetLastError());
        WriteAppLog(L"启动更新器失败: " + error);
        return false;
    }

    WriteAppLog(L"更新器已启动。pid=" + std::to_wstring(process.dwProcessId));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
