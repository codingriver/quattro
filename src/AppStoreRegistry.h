#pragma once

#include "AppStoreManifest.h"

#include <filesystem>
#include <string>
#include <vector>

struct AppTransferTask {
    std::wstring id;
    std::wstring direction;
    std::wstring appId;
    std::wstring name;
    std::wstring version;
    std::wstring releaseTag;
    std::wstring owner;
    std::wstring repo;
    std::wstring localPath;
    std::wstring installPath;
    std::wstring manifestJson;
    std::wstring status;
    std::wstring phase;
    long long totalBytes = 0;
    long long transferredBytes = 0;
    long long splitSizeBytes = 268435456;
    std::wstring errorMessage;
    std::wstring createdAt;
    std::wstring updatedAt;
};

struct AppTransferPartRecord {
    std::wstring taskId;
    int partIndex = 0;
    std::wstring assetName;
    long long assetId = 0;
    std::wstring localPath;
    long long sizeBytes = 0;
    std::wstring sha256;
    std::wstring status;
    long long transferredBytes = 0;
    std::wstring errorMessage;
};

struct InstalledAppRecord {
    std::wstring appId;
    std::wstring name;
    std::wstring version;
    std::wstring releaseTag;
    std::wstring owner;
    std::wstring repo;
    std::wstring installPath;
    std::wstring manifestJson;
    std::wstring packageSha256;
    bool draft = false;
};

class AppStoreRegistry {
public:
    explicit AppStoreRegistry(std::filesystem::path appDirectory);

    bool Initialize();
    bool AddDownloadTask(const AppStoreEntry& app, const std::wstring& owner, const std::wstring& repo, long long splitSizeBytes, std::wstring& taskId);
    bool AddUploadTaskPlaceholder(const std::wstring& localPath, const std::wstring& owner, const std::wstring& repo, long long splitSizeBytes, std::wstring& taskId);
    std::vector<AppTransferTask> LoadTasks(const std::wstring& direction = L"");
    bool LoadTask(const std::wstring& taskId, AppTransferTask& task);
    std::vector<AppTransferPartRecord> LoadParts(const std::wstring& taskId);
    bool SetTaskStatus(const std::wstring& taskId, const std::wstring& status, const std::wstring& phase = L"", const std::wstring& error = L"");
    bool UpdateTaskProgress(const std::wstring& taskId, long long transferredBytes, long long totalBytes = -1);
    bool UpdateTaskManifest(const std::wstring& taskId, const AppStoreManifest& manifest);
    bool UpsertPart(const AppTransferPartRecord& part);
    bool SetPartStatus(const std::wstring& taskId, int partIndex, const std::wstring& status, long long transferredBytes, long long assetId = 0, const std::wstring& error = L"");
    bool IsTaskCancelRequested(const std::wstring& taskId);
    bool MarkInterruptedTasksPaused();
    bool DeleteTask(const std::wstring& taskId);

    bool RegisterInstalledApp(const AppStoreManifest& manifest, const std::wstring& owner, const std::wstring& repo, const std::wstring& installPath, const std::vector<std::wstring>& files, const std::vector<std::wstring>& shortcuts);
    std::vector<InstalledAppRecord> LoadInstalledApps();
    bool LoadInstalledApp(const std::wstring& appId, InstalledAppRecord& app);
    std::vector<std::wstring> LoadInstalledFiles(const std::wstring& appId);
    std::vector<std::wstring> LoadInstalledShortcuts(const std::wstring& appId);
    bool DeleteInstalledAppRecord(const std::wstring& appId);

    const std::wstring& lastError() const { return lastError_; }

private:
    bool EnsureSchema(void* db);
    std::wstring NewTaskId() const;

    std::filesystem::path appDirectory_;
    std::wstring lastError_;
};
