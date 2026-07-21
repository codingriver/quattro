#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <stop_token>
#include <vector>

struct WebDavRemoteFile;

struct WebDavFileRecord {
    std::wstring id;
    std::wstring absolutePath;
    std::wstring displayName;
    std::uint64_t size = 0;
    std::wstring sha256;
    std::wstring uploadedAtUtc;
    std::wstring uploadState = L"complete";
    bool contentReady = true;
};

enum class WebDavFileTransferPhase {
    Preparing,
    UploadingMeta,
    UploadingContent,
    FinalizingMeta,
    DownloadingContent,
    Verifying,
};

using WebDavFileProgressCallback = std::function<bool(
    WebDavFileTransferPhase phase,
    std::uint64_t transferred,
    std::uint64_t total)>;

struct WebDavFileOperationResult {
    bool ok = false;
    std::wstring message;
    WebDavFileRecord record;
};

class WebDavFileService {
public:
    explicit WebDavFileService(AppConfig config);

    bool List(std::vector<WebDavFileRecord>& records, std::wstring& error);
    WebDavFileOperationResult Upload(const std::filesystem::path& localPath,
        WebDavFileProgressCallback progress = {}, std::stop_token stopToken = {});
    WebDavFileOperationResult Download(const WebDavFileRecord& record,
        WebDavFileProgressCallback progress = {}, std::stop_token stopToken = {});
    bool Delete(const WebDavFileRecord& record, std::wstring& error);

    static std::wstring CanonicalPath(const std::filesystem::path& path);
    static std::wstring RecordId(const std::wstring& canonicalPath);
    static std::wstring FilesDirectory(const AppConfig& config);
    static bool IsCollectionSelfResponse(const std::wstring& remotePath, const WebDavRemoteFile& entry);
    static bool IsRecordDirectoryName(const std::wstring& name);

private:
    bool LoadPassword(std::wstring& password, std::wstring& error) const;
    bool EnsureFilesDirectory(class WebDavClient& client, std::wstring& error) const;
    bool ReadMetadata(const std::wstring& text, WebDavFileRecord& record) const;
    std::wstring MetadataJson(const WebDavFileRecord& record) const;
    bool Sha256(const std::filesystem::path& path, std::wstring& hash, std::wstring& error) const;

    AppConfig config_;
};
