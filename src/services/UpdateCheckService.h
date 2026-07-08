#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <string>

struct UpdateReleaseInfo {
    std::wstring currentVersion;
    std::wstring latestVersion;
    std::wstring releaseUrl;
    std::wstring releaseNotes;
    std::wstring assetName;
    std::wstring assetDownloadUrl;
    std::wstring checksumDownloadUrl;
    std::uint64_t assetSizeBytes = 0;
    bool updateAvailable = false;
};

struct UpdateDownloadResult {
    std::filesystem::path filePath;
    bool checksumVerified = false;
    std::wstring checksumMessage;
};

struct UpdateDownloadProgress {
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
};

using UpdateProgressCallback = std::function<void(const UpdateDownloadProgress&)>;
using UpdateCancelCallback = std::function<bool()>;

class UpdateCheckService {
public:
    explicit UpdateCheckService(std::filesystem::path appDirectory, std::wstring releaseApiUrl = L"");

    bool CheckLatest(UpdateReleaseInfo& info, std::wstring& error) const;
    bool DownloadUpdate(const UpdateReleaseInfo& info, UpdateDownloadResult& result, std::wstring& error) const;
    bool DownloadUpdate(
        const UpdateReleaseInfo& info,
        UpdateDownloadResult& result,
        std::wstring& error,
        const UpdateProgressCallback& progress,
        const UpdateCancelCallback& cancel) const;

    static int CompareVersions(const std::wstring& left, const std::wstring& right);

private:
    std::filesystem::path appDirectory_;
    std::wstring releaseApiUrl_;
};
