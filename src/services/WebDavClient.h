#pragma once

#include "Models.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct WebDavRemoteFile {
    std::wstring name;
    std::wstring href;
    std::uint64_t size = 0;
    std::wstring lastModified;
    bool collection = false;
};

using WebDavProgressCallback = std::function<bool(std::uint64_t transferred, std::uint64_t total)>;

class WebDavClient {
public:
    enum class BodyKind {
        None,
        Upload,
        Text,
    };

    struct RequestBody {
        BodyKind kind = BodyKind::None;
        std::vector<std::uint8_t> upload;
        std::string text;
        std::size_t offset = 0;
    };

    WebDavClient(AppConfig config, std::wstring password);

    bool TestConnection();
    bool EnsureDirectory(const std::wstring& remotePath);
    bool ListFiles(const std::wstring& remotePath, std::vector<WebDavRemoteFile>& files);
    bool UploadFile(const std::filesystem::path& localPath, const std::wstring& remotePath,
        WebDavProgressCallback progress = {}, std::stop_token stopToken = {});
    bool DownloadFile(const std::wstring& remotePath, const std::filesystem::path& localPath,
        WebDavProgressCallback progress = {}, std::stop_token stopToken = {});
    bool DownloadText(const std::wstring& remotePath, std::wstring& text,
        std::stop_token stopToken = {}, long* statusCode = nullptr);
    bool DeleteRemoteFile(const std::wstring& remotePath);
    bool DeleteRemoteDirectory(const std::wstring& remotePath);

    const std::wstring& lastError() const { return lastError_; }

    static std::vector<WebDavRemoteFile> ParsePropFindResponse(const std::string& xml);
    static std::wstring FileNameFromHref(const std::wstring& href);
    static std::wstring CombineRemotePath(const std::wstring& directory, const std::wstring& fileName);
    static std::wstring BackupRemotePath(const AppConfig& config);
    static std::wstring FilesRemotePath(const AppConfig& config);

private:
    bool Request(
        const std::wstring& method,
        const std::wstring& remotePath,
        RequestBody* body,
        const std::vector<std::string>& headers,
        std::vector<std::uint8_t>* response,
        long* statusCode,
        const WebDavProgressCallback& progress = {},
        std::stop_token stopToken = {});
    std::wstring UrlForRemotePath(const std::wstring& remotePath) const;
    bool ValidateSettings();
    void SetCurlError(const std::wstring& action, int curlCode, long statusCode, const std::string& detail);

    AppConfig config_;
    std::wstring password_;
    std::wstring lastError_;
};
