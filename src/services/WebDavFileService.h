#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <stop_token>
#include <vector>

struct WebDavRemoteFile;

enum class WebDavFileRecordHealth {
    Healthy,
    MissingMetadata,
    InvalidMetadata,
    MetadataReadFailed,
};

struct WebDavFileRecord {
    std::wstring id;
    std::wstring absolutePath;
    std::wstring displayName;
    std::uint64_t size = 0;
    std::wstring sha256;
    std::wstring uploadedAtUtc;
    std::wstring uploadState = L"complete";
    bool contentReady = true;
    WebDavFileRecordHealth health = WebDavFileRecordHealth::Healthy;
    std::wstring recordError;
};

enum class WebDavFileTransferPhase {
    Preparing,
    UploadingMeta,
    UploadingContent,
    FinalizingMeta,
    DownloadingContent,
    Verifying,
};

enum class WebDavFileDeletePhase {
    DeletingContent,
    DeletingMetadata,
    DeletingDirectory,
};

using WebDavFileProgressCallback = std::function<bool(
    WebDavFileTransferPhase phase,
    std::uint64_t transferred,
    std::uint64_t total)>;
using WebDavFileDeleteProgressCallback = std::function<void(WebDavFileDeletePhase phase, bool completed)>;

struct WebDavFileOperationResult {
    bool ok = false;
    std::wstring message;
    WebDavFileRecord record;
};

struct WebDavFileEnumerationOptions {
    unsigned int maxMetadataConcurrency = 4;
    std::size_t batchSize = 20;
};

using WebDavFileBatchCallback = std::function<bool(std::vector<WebDavFileRecord>)>;

class WebDavFileService {
public:
    explicit WebDavFileService(AppConfig config);

    bool List(std::vector<WebDavFileRecord>& records, std::wstring& error);
    bool Enumerate(const WebDavFileEnumerationOptions& options, WebDavFileBatchCallback callback,
        std::stop_token stopToken, std::wstring& error);
    static unsigned int NormalizeMetadataConcurrency(unsigned int value);
    WebDavFileOperationResult Upload(const std::filesystem::path& localPath,
        WebDavFileProgressCallback progress = {}, std::stop_token stopToken = {});
    WebDavFileOperationResult Download(const WebDavFileRecord& record,
        WebDavFileProgressCallback progress = {}, std::stop_token stopToken = {});
    bool Delete(const WebDavFileRecord& record, std::wstring& error,
        WebDavFileDeleteProgressCallback progress = {});

    static std::wstring CanonicalPath(const std::filesystem::path& path);
    static bool ValidateDownloadTargetPath(const std::wstring& absolutePath,
        std::filesystem::path& target, std::wstring& error);
    static std::wstring RecordId(const std::wstring& canonicalPath);
    static std::wstring FilesDirectory(const AppConfig& config);
    static std::wstring FormatUploadedAtLocal(const std::wstring& uploadedAtUtc);
    static std::wstring FormatRecordTooltip(const WebDavFileRecord& record);
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
