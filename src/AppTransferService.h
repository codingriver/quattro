#pragma once

#include "AppStoreRegistry.h"
#include "Models.h"

#include <filesystem>
#include <string>

struct AppTransferRunReport {
    bool ok = false;
    std::wstring message;
};

class AppTransferService {
public:
    AppTransferService(std::filesystem::path appDirectory, AppConfig config);

    AppTransferRunReport RunTask(const AppTransferTask& task);
    AppTransferRunReport RunUploadTask(const AppTransferTask& task);
    AppTransferRunReport RunDownloadTask(const AppTransferTask& task);
    AppTransferRunReport DeleteRemoteForUploadTask(const AppTransferTask& task);

private:
    bool ShouldStop(const std::wstring& taskId, AppStoreRegistry& registry, std::wstring& message);
    bool ExpandZip(const std::filesystem::path& zipPath, const std::filesystem::path& targetDirectory, std::wstring& error);
    std::filesystem::path DownloadPathFor(const AppStoreManifest& manifest) const;

    std::filesystem::path appDirectory_;
    AppConfig config_;
};
