#include "WebDavBackupService.h"

#include "Utilities.h"
#include "WebDavCredentialService.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace {
ConfigPackageOptions BackupPackageOptions() {
    ConfigPackageOptions options;
    options.includeConfig = true;
    options.includeData = true;
    options.includeUrlIcons = true;
    options.includePluginSettings = true;
    return options;
}

bool IsBackupPackage(const WebDavRemoteFile& file) {
    return !file.collection && ToLower(file.name).ends_with(L".q4cfg");
}

std::wstring JoinWarnings(const std::vector<std::wstring>& warnings) {
    std::wstring text;
    for (const auto& warning : warnings) {
        if (!text.empty()) {
            text += L"\n";
        }
        text += L"- " + warning;
    }
    return text;
}
}

WebDavBackupService::WebDavBackupService(std::filesystem::path appDirectory, AppConfig config)
    : appDirectory_(std::move(appDirectory)), config_(std::move(config)) {
}

bool WebDavBackupService::TestConnection(std::wstring& error) {
    std::wstring password;
    if (!LoadPassword(password, error)) {
        return false;
    }
    WebDavClient client(config_, password);
    if (!client.TestConnection()) {
        error = client.lastError();
        return false;
    }
    return true;
}

bool WebDavBackupService::ListBackups(std::vector<WebDavRemoteFile>& backups, std::wstring& error) {
    backups.clear();
    std::wstring password;
    if (!LoadPassword(password, error)) {
        return false;
    }
    WebDavClient client(config_, password);
    std::vector<WebDavRemoteFile> files;
    if (!client.ListFiles(config_.webDavRemotePath, files)) {
        error = client.lastError();
        return false;
    }
    for (const auto& file : files) {
        if (IsBackupPackage(file)) {
            backups.push_back(file);
        }
    }
    std::sort(backups.begin(), backups.end(), [](const auto& left, const auto& right) {
        return left.name > right.name;
    });
    return true;
}

WebDavBackupReport WebDavBackupService::UploadBackup() {
    WebDavBackupReport report;
    std::wstring password;
    std::wstring error;
    if (!LoadPassword(password, error)) {
        report.message = error;
        return report;
    }

    const std::wstring fileName = BackupFileName();
    const std::filesystem::path packagePath = TempPackagePath(fileName);
    ConfigPackageService packageService(appDirectory_);
    ConfigPackageReport exportReport = packageService.ExportPackage(packagePath, BackupPackageOptions());
    if (!exportReport.ok) {
        report.message = exportReport.message;
        return report;
    }

    WebDavClient client(config_, password);
    if (!client.EnsureDirectory(config_.webDavRemotePath)) {
        report.message = client.lastError();
        std::error_code ec;
        std::filesystem::remove(packagePath, ec);
        return report;
    }
    const std::wstring remotePath = WebDavClient::CombineRemotePath(config_.webDavRemotePath, fileName);
    if (!client.UploadFile(packagePath, remotePath)) {
        report.message = client.lastError();
        std::error_code ec;
        std::filesystem::remove(packagePath, ec);
        return report;
    }

    std::vector<std::wstring> warnings;
    PruneOldBackups(client, warnings);

    std::error_code ec;
    std::filesystem::remove(packagePath, ec);
    report.ok = true;
    report.remoteName = fileName;
    report.message = L"WebDAV 备份上传完成: " + fileName;
    if (!warnings.empty()) {
        report.message += L"\n\n清理旧备份时出现警告:\n" + JoinWarnings(warnings);
    }
    return report;
}

WebDavBackupReport WebDavBackupService::DownloadAndImportMerge(const std::wstring& remoteName) {
    WebDavBackupReport report;
    std::wstring name = Trim(remoteName);
    if (name.empty() || !ToLower(name).ends_with(L".q4cfg") || name.find_first_of(L"/\\") != std::wstring::npos) {
        report.message = L"请选择有效的 .q4cfg 备份文件。";
        return report;
    }

    std::wstring password;
    std::wstring error;
    if (!LoadPassword(password, error)) {
        report.message = error;
        return report;
    }

    const std::filesystem::path packagePath = TempPackagePath(name);
    WebDavClient client(config_, password);
    const std::wstring remotePath = WebDavClient::CombineRemotePath(config_.webDavRemotePath, name);
    if (!client.DownloadFile(remotePath, packagePath)) {
        report.message = client.lastError();
        return report;
    }

    ConfigPackageOptions options;
    options.includeConfig = false;
    options.includeData = true;
    options.includeUrlIcons = true;
    options.includePluginSettings = true;
    ConfigPackageService packageService(appDirectory_);
    report.importReport = packageService.ImportPackageMerge(packagePath, options);
    std::error_code ec;
    std::filesystem::remove(packagePath, ec);
    report.ok = report.importReport.ok;
    report.remoteName = name;
    report.message = report.importReport.message;
    return report;
}

bool WebDavBackupService::LoadPassword(std::wstring& password, std::wstring& error) const {
    if (!config_.webDavEnabled) {
        error = L"WebDAV 备份未启用。";
        return false;
    }
    if (Trim(config_.webDavUrl).empty()) {
        error = L"WebDAV 地址未配置。";
        return false;
    }
    if (Trim(config_.webDavRemotePath).empty()) {
        error = L"WebDAV 远端目录未配置。";
        return false;
    }
    if (Trim(config_.webDavUserName).empty()) {
        error = L"WebDAV 用户名未配置。";
        return false;
    }
    if (!WebDavCredentialService::LoadPassword(config_, password, error)) {
        return false;
    }
    if (password.empty()) {
        error = L"WebDAV 密码未配置。";
        return false;
    }
    return true;
}

std::wstring WebDavBackupService::BackupFileName() const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &value);
    wchar_t buffer[64]{};
    wcsftime(buffer, std::size(buffer), L"quattro-backup-%Y%m%d-%H%M%S.q4cfg", &local);
    return buffer;
}

std::filesystem::path WebDavBackupService::TempPackagePath(const std::wstring& fileName) const {
    return std::filesystem::temp_directory_path() /
        (L"quattro_webdav_" + std::to_wstring(GetCurrentProcessId()) + L"_" + fileName);
}

void WebDavBackupService::PruneOldBackups(WebDavClient& client, std::vector<std::wstring>& warnings) {
    std::vector<WebDavRemoteFile> files;
    if (!client.ListFiles(config_.webDavRemotePath, files)) {
        warnings.push_back(client.lastError());
        return;
    }
    std::vector<WebDavRemoteFile> backups;
    for (const auto& file : files) {
        if (IsBackupPackage(file)) {
            backups.push_back(file);
        }
    }
    std::sort(backups.begin(), backups.end(), [](const auto& left, const auto& right) {
        return left.name > right.name;
    });
    const int keepCount = std::max(1, std::min(100, config_.webDavKeepCount));
    for (std::size_t i = static_cast<std::size_t>(keepCount); i < backups.size(); ++i) {
        const std::wstring remotePath = WebDavClient::CombineRemotePath(config_.webDavRemotePath, backups[i].name);
        if (!client.DeleteRemoteFile(remotePath)) {
            warnings.push_back(client.lastError());
        }
    }
}
