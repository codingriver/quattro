#pragma once

#include "ConfigPackageService.h"
#include "Models.h"
#include "WebDavClient.h"

#include <filesystem>
#include <string>
#include <vector>

struct WebDavBackupReport {
    bool ok = false;
    std::wstring message;
    std::wstring remoteName;
    std::filesystem::path downloadedPackagePath;
    ConfigPackageMergePreview mergePreview;
    ConfigPackageReport importReport;
};

class WebDavBackupService {
public:
    WebDavBackupService(std::filesystem::path appDirectory, AppConfig config);

    bool TestConnection(std::wstring& error);
    bool ListBackups(std::vector<WebDavRemoteFile>& backups, std::wstring& error);
    WebDavBackupReport UploadBackup();
    WebDavBackupReport DownloadAndImportMerge(const std::wstring& remoteName);
    WebDavBackupReport DownloadAndPreviewMerge(const std::wstring& remoteName);
    WebDavBackupReport ApplyDownloadedMerge(
        const std::filesystem::path& packagePath,
        const std::wstring& remoteName,
        TodoRestorePolicy restorePolicy,
        const std::wstring& expectedStateToken);

private:
    bool LoadPassword(std::wstring& password, std::wstring& error) const;
    std::wstring BackupFileName() const;
    std::filesystem::path TempPackagePath(const std::wstring& fileName) const;
    void PruneOldBackups(WebDavClient& client, std::vector<std::wstring>& warnings);

    std::filesystem::path appDirectory_;
    AppConfig config_;
};
