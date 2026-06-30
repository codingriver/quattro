#pragma once

#include "AppStoreManifest.h"
#include "Models.h"

#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

class GitHubReleaseClient {
public:
    using TransferProgressCallback = std::function<bool(std::uint64_t transferred, std::uint64_t total)>;

    GitHubReleaseClient(AppConfig config, std::wstring token);

    bool TestConnection();
    bool ListReleases(std::vector<AppStoreRelease>& releases);
    bool GetReleaseByTag(const std::wstring& tag, AppStoreRelease& release);
    bool CreateRelease(const AppStoreManifest& manifest, AppStoreRelease& release);
    bool DeleteRelease(long long releaseId);
    bool DeleteAsset(long long assetId);
    bool UploadReleaseAsset(const AppStoreRelease& release, const std::filesystem::path& path, const std::wstring& assetName, AppStoreReleaseAsset& asset, TransferProgressCallback progress = {});
    bool DownloadAssetText(long long assetId, std::wstring& text);
    bool DownloadAssetBytes(long long assetId, std::vector<std::uint8_t>& bytes);
    bool DownloadAssetFile(long long assetId, const std::filesystem::path& path, TransferProgressCallback progress = {});
    bool UpdateReleaseMetadata(long long releaseId, const std::wstring& name, const std::wstring& body);

    const std::wstring& lastError() const { return lastError_; }

private:
    bool RequestJson(const std::wstring& url, std::wstring& text);
    bool RequestJson(const std::wstring& method, const std::wstring& url, const std::string& body, std::wstring& text, long* statusOut = nullptr);
    bool RequestBytes(const std::wstring& url, const std::wstring& acceptHeader, std::vector<std::uint8_t>& bytes);
    bool RequestBytes(const std::wstring& method, const std::wstring& url, const std::wstring& acceptHeader, const std::vector<std::uint8_t>& body, std::vector<std::uint8_t>& bytes, long* statusOut = nullptr);
    bool RequestFile(const std::wstring& url, const std::wstring& acceptHeader, const std::filesystem::path& path, TransferProgressCallback progress = {});
    bool UploadFileRequest(const std::wstring& url, const std::filesystem::path& path, const std::wstring& acceptHeader, std::vector<std::uint8_t>& response, TransferProgressCallback progress = {});
    std::wstring ApiUrl(const std::wstring& path) const;
    bool ValidateSettings();

    AppConfig config_;
    std::wstring token_;
    std::wstring lastError_;
};
