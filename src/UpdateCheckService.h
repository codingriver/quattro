#pragma once

#include <filesystem>
#include <string>

struct UpdateReleaseInfo {
    std::wstring currentVersion;
    std::wstring latestVersion;
    std::wstring releaseUrl;
    std::wstring releaseNotes;
    std::wstring assetName;
    std::wstring assetDownloadUrl;
    std::wstring checksumDownloadUrl;
    bool updateAvailable = false;
};

struct UpdateDownloadResult {
    std::filesystem::path filePath;
    bool checksumVerified = false;
    std::wstring checksumMessage;
};

class UpdateCheckService {
public:
    explicit UpdateCheckService(std::filesystem::path appDirectory, std::wstring releaseApiUrl = L"");

    bool CheckLatest(UpdateReleaseInfo& info, std::wstring& error) const;
    bool DownloadUpdate(const UpdateReleaseInfo& info, UpdateDownloadResult& result, std::wstring& error) const;

    static int CompareVersions(const std::wstring& left, const std::wstring& right);

private:
    std::filesystem::path appDirectory_;
    std::wstring releaseApiUrl_;
};
