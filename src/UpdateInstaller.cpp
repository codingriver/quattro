#include "UpdateInstaller.h"

#include "EmbeddedUpdater.h"
#include "Utilities.h"

#include <windows.h>

#include <fstream>

namespace {
std::filesystem::path UpdaterDirectory() {
    return QuattroUserConfigDirectory() / L"updater";
}

std::filesystem::path ExtractedUpdaterPath() {
    return UpdaterDirectory() / L"QuattroUpdater.exe";
}

bool WriteEmbeddedUpdater(const std::filesystem::path& path, std::wstring& error) {
    const unsigned char* data = EmbeddedUpdaterData();
    const std::size_t size = EmbeddedUpdaterSize();
    if (!data || size == 0) {
        error = L"当前构建没有嵌入更新器。";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = L"创建更新器目录失败: " + path.parent_path().wstring();
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"写入更新器失败: " + path.wstring();
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file.good()) {
        error = L"写入更新器失败: " + path.wstring();
        return false;
    }
    return true;
}

std::wstring BuildCommandLine(const std::filesystem::path& updater, const UpdateInstallPlan& plan) {
    const std::filesystem::path backup = plan.currentExe.wstring() + L".bak";
    std::wstring command = QuoteForCommandLine(updater.wstring());
    command += L" --pid " + std::to_wstring(GetCurrentProcessId());
    command += L" --source " + QuoteForCommandLine(plan.downloadedExe.wstring());
    command += L" --target " + QuoteForCommandLine(plan.currentExe.wstring());
    command += L" --backup " + QuoteForCommandLine(backup.wstring());
    command += L" --restart " + QuoteForCommandLine(plan.currentExe.wstring());
    if (!plan.logPath.empty()) {
        command += L" --log " + QuoteForCommandLine(plan.logPath.wstring());
    }
    return command;
}
}

bool LaunchEmbeddedUpdater(const UpdateInstallPlan& plan, std::wstring& error) {
    if (plan.downloadedExe.empty() || !FileExists(plan.downloadedExe)) {
        error = L"新版文件不存在。";
        return false;
    }
    if (plan.currentExe.empty()) {
        error = L"当前程序路径为空。";
        return false;
    }

    const std::filesystem::path updater = ExtractedUpdaterPath();
    if (!WriteEmbeddedUpdater(updater, error)) {
        return false;
    }

    std::wstring command = BuildCommandLine(updater, plan);
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
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
