#include "WebDavFileService.h"

#include "AppLog.h"
#include "JsonValue.h"
#include "ScanExecutionService.h"
#include "Utilities.h"
#include "WebDavClient.h"
#include "WebDavCredentialService.h"

#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>

namespace {
std::wstring JsonEscape(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring Hex(const std::vector<unsigned char>& bytes) {
    std::wstringstream stream;
    stream << std::hex << std::setfill(L'0');
    for (unsigned char byte : bytes) stream << std::setw(2) << static_cast<int>(byte);
    return stream.str();
}

std::wstring NowUtc() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

std::wstring FormatRecordSize(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::to_wstring((bytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull)) + L" MB";
    }
    if (bytes >= 1024ull) return std::to_wstring((bytes + 1023ull) / 1024ull) + L" KB";
    return std::to_wstring(bytes) + L" B";
}

bool SaveUtf8(const std::filesystem::path& path, const std::wstring& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(file);
}

std::wstring NormalizeRemotePathForComparison(std::wstring value) {
    const auto query = value.find_first_of(L"?#");
    if (query != std::wstring::npos) value.resize(query);
    std::replace(value.begin(), value.end(), L'\\', L'/');
    while (value.size() > 1 && value.back() == L'/') value.pop_back();
    return ToLower(value);
}
}

WebDavFileService::WebDavFileService(AppConfig config) : config_(std::move(config)) {}

std::wstring WebDavFileService::FilesDirectory(const AppConfig& config) {
    return WebDavClient::FilesRemotePath(config);
}

std::wstring WebDavFileService::FormatUploadedAtLocal(const std::wstring& uploadedAtUtc) {
    SYSTEMTIME utc{};
    wchar_t suffix = L'\0';
    const int matched = swscanf_s(
        uploadedAtUtc.c_str(),
        L"%4hu-%2hu-%2huT%2hu:%2hu:%2hu.%3hu%c",
        &utc.wYear, &utc.wMonth, &utc.wDay,
        &utc.wHour, &utc.wMinute, &utc.wSecond, &utc.wMilliseconds,
        &suffix, 1u);
    if (matched != 8 || suffix != L'Z') return {};
    FILETIME validation{};
    if (!SystemTimeToFileTime(&utc, &validation)) return {};
    DYNAMIC_TIME_ZONE_INFORMATION timeZone{};
    GetDynamicTimeZoneInformation(&timeZone);
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTimeEx(&timeZone, &utc, &local)) return {};
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
    return buffer;
}

std::wstring WebDavFileService::FormatLocalModifiedAt(const std::wstring& absolutePath) {
    const std::wstring path = Trim(absolutePath);
    if (path.empty()) return {};

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }

    FILETIME localFile{};
    SYSTEMTIME local{};
    if (!FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &localFile) ||
        !FileTimeToSystemTime(&localFile, &local)) {
        return {};
    }

    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
    return buffer;
}

std::wstring WebDavFileService::FormatRecordTooltip(const WebDavFileRecord& record) {
    if (record.health != WebDavFileRecordHealth::Healthy ||
        record.displayName.empty() || record.absolutePath.empty()) {
        return L"获取失败";
    }
    const std::wstring uploadedAt = FormatUploadedAtLocal(record.uploadedAtUtc);
    if (uploadedAt.empty()) return L"获取失败";
    return record.displayName + L"  ·  " + FormatRecordSize(record.size) + L"\n" +
        record.absolutePath + L"\n上传时间：" + uploadedAt;
}

bool WebDavFileService::IsCollectionSelfResponse(const std::wstring& remotePath, const WebDavRemoteFile& entry) {
    if (!entry.collection) return false;
    const std::wstring target = NormalizeRemotePathForComparison(remotePath);
    const std::wstring href = NormalizeRemotePathForComparison(entry.href);
    if (target.empty() || href.size() < target.size()) return false;
    const std::size_t offset = href.size() - target.size();
    return href.compare(offset, target.size(), target) == 0;
}

bool WebDavFileService::IsRecordDirectoryName(const std::wstring& name) {
    return name.size() == 64 && std::all_of(name.begin(), name.end(), [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F');
    });
}

std::wstring WebDavFileService::CanonicalPath(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::wstring full(32768, L'\0');
    DWORD length = GetFullPathNameW(value.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (length > 0 && length < full.size()) value.assign(full.data(), length);
    return ToLower(value);
}

bool WebDavFileService::ValidateDownloadTargetPath(
    const std::wstring& absolutePath,
    std::filesystem::path& target,
    std::wstring& error) {
    error.clear();
    target.clear();

    const std::wstring trimmed = Trim(absolutePath);
    if (trimmed.empty()) {
        error = L"文件保存路径为空。";
        return false;
    }
    const std::filesystem::path raw(trimmed);
    if (!raw.is_absolute()) {
        error = L"文件保存路径必须是有效的绝对文件路径。";
        return false;
    }

    std::wstring full(32768, L'\0');
    DWORD length = GetFullPathNameW(trimmed.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (length == 0 || length >= full.size()) {
        error = L"文件保存路径无效。";
        return false;
    }
    full.resize(length);

    target = std::filesystem::path(full);
    if (!target.is_absolute() || target.filename().empty() || target.parent_path().empty()) {
        error = L"文件保存路径必须是有效的绝对文件路径。";
        target.clear();
        return false;
    }
    const std::wstring invalidChars = L"<>:\"|?*";
    for (const auto& part : target.relative_path()) {
        const std::wstring text = part.wstring();
        if (text.empty() || text == L"." || text == L".." ||
            text.find_first_of(invalidChars) != std::wstring::npos ||
            std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch >= 0 && ch < 32; })) {
            error = L"文件保存路径包含无效字符。";
            target.clear();
            return false;
        }
    }
    return true;
}

std::wstring WebDavFileService::RecordId(const std::wstring& canonicalPath) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return Hex8(StablePathHash(canonicalPath));
    }
    const std::string utf8 = [&]() {
        if (canonicalPath.empty()) return std::string{};
        int size = WideCharToMultiByte(CP_UTF8, 0, canonicalPath.data(), static_cast<int>(canonicalPath.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, canonicalPath.data(), static_cast<int>(canonicalPath.size()), result.data(), size, nullptr, nullptr);
        return result;
    }();
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(utf8.data())), static_cast<ULONG>(utf8.size()), 0);
    std::vector<unsigned char> digest(32);
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return Hex(digest);
}

bool WebDavFileService::LoadPassword(std::wstring& password, std::wstring& error) const {
    if (!config_.webDavEnabled) { error = L"WebDAV 未启用。"; return false; }
    if (Trim(config_.webDavUrl).empty()) { error = L"WebDAV 地址未配置。"; return false; }
    if (Trim(config_.webDavFilesPath).empty()) { error = L"WebDAV 文件目录未配置。"; return false; }
    if (Trim(config_.webDavUserName).empty()) { error = L"WebDAV 用户名未配置。"; return false; }
    if (!WebDavCredentialService::LoadPassword(config_, password, error) || password.empty()) {
        if (error.empty()) error = L"WebDAV 密码未配置。";
        return false;
    }
    return true;
}

bool WebDavFileService::EnsureFilesDirectory(WebDavClient& client, std::wstring& error) const {
    if (!client.EnsureDirectory(FilesDirectory(config_))) {
        error = client.lastError();
        return false;
    }
    return true;
}

bool WebDavFileService::Sha256(const std::filesystem::path& path, std::wstring& hash, std::wstring& error) const {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = L"无法读取文件。"; return false; }
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(algorithm, &handle, nullptr, 0, nullptr, 0, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); error = L"初始化 SHA-256 失败。"; return false;
    }
    std::vector<char> buffer(64 * 1024);
    while (file) { file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())); const auto count = file.gcount();
        if (count > 0 && BCryptHashData(handle, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) != 0) {
            BCryptDestroyHash(handle); BCryptCloseAlgorithmProvider(algorithm, 0); error = L"计算 SHA-256 失败。"; return false;
        }
    }
    std::vector<unsigned char> digest(32);
    BCryptFinishHash(handle, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(handle); BCryptCloseAlgorithmProvider(algorithm, 0); hash = Hex(digest); return true;
}

std::wstring WebDavFileService::MetadataJson(const WebDavFileRecord& record) const {
    return L"{\"schemaVersion\":2,\"absolutePath\":\"" + JsonEscape(record.absolutePath) +
        L"\",\"displayName\":\"" + JsonEscape(record.displayName) + L"\",\"size\":" +
        std::to_wstring(record.size) + L",\"sha256\":\"" + record.sha256 + L"\",\"uploadedAtUtc\":\"" +
        JsonEscape(record.uploadedAtUtc) + L"\",\"contentName\":\"content\",\"uploadState\":\"" +
        record.uploadState + L"\",\"contentReady\":" + (record.contentReady ? L"true" : L"false") + L"}";
}

bool WebDavFileService::ReadMetadata(const std::wstring& text, WebDavFileRecord& record) const {
    JsonValue root; std::wstring error;
    if (!ParseJson(text, root, error) || !root.isObject()) return false;
    const JsonValue* path = root.get(L"absolutePath"); const JsonValue* size = root.get(L"size");
    const JsonValue* hash = root.get(L"sha256");
    if (!path || !path->isString() || !size || !hash || !hash->isString()) return false;
    record.absolutePath = path->stringValue; record.displayName = root.get(L"displayName") ? root.get(L"displayName")->stringOr() : std::filesystem::path(record.absolutePath).filename().wstring();
    record.size = static_cast<std::uint64_t>(size->numberValue); record.sha256 = hash->stringValue;
    record.uploadedAtUtc = root.get(L"uploadedAtUtc") ? root.get(L"uploadedAtUtc")->stringOr() : L"";
    record.uploadState = root.get(L"uploadState") ? root.get(L"uploadState")->stringOr(L"complete") : L"complete";
    record.contentReady = root.get(L"contentReady") ? root.get(L"contentReady")->boolOr(true) : true;
    record.id = RecordId(CanonicalPath(record.absolutePath)); return true;
}

bool WebDavFileService::List(std::vector<WebDavFileRecord>& records, std::wstring& error) {
    records.clear();
    return Enumerate({}, [&](std::vector<WebDavFileRecord> batch) {
        records.insert(records.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        return true;
    }, {}, error);
}

unsigned int WebDavFileService::NormalizeMetadataConcurrency(unsigned int value) {
    return std::clamp(value, 1u, 8u);
}

bool WebDavFileService::Enumerate(const WebDavFileEnumerationOptions& options,
    WebDavFileBatchCallback callback, std::stop_token stopToken, std::wstring& error) {
    ScanTaskOptions scanOptions;
    scanOptions.mode = ScanExecutionMode::CallerParallel;
    scanOptions.maxWorkers = NormalizeMetadataConcurrency(options.maxMetadataConcurrency);
    struct EnumerationResult {
        bool ok = false;
        std::wstring error;
    };
    EnumerationResult result = ScanExecutionService::Run<EnumerationResult>(scanOptions,
        [&](ScanTaskContext& context) {
            EnumerationResult value;
            value.ok = EnumerateCore(options, callback, stopToken, value.error, context);
            return value;
        });
    error = std::move(result.error);
    return result.ok;
}

bool WebDavFileService::EnumerateCore(const WebDavFileEnumerationOptions& options,
    WebDavFileBatchCallback callback, std::stop_token stopToken, std::wstring& error,
    ScanTaskContext& context) {
    std::wstring password;
    if (!LoadPassword(password, error)) return false;
    WebDavClient listingClient(config_, password);
    const auto filesDirectory = FilesDirectory(config_);
    std::vector<WebDavRemoteFile> entries;
    if (!listingClient.ListFiles(filesDirectory, entries)) { error = listingClient.lastError(); return false; }
    std::vector<std::wstring> recordIds;
    for (const auto& entry : entries) {
        if (entry.collection && !entry.name.empty() && !IsCollectionSelfResponse(filesDirectory, entry) &&
            IsRecordDirectoryName(entry.name)) recordIds.push_back(entry.name);
    }
    std::sort(recordIds.begin(), recordIds.end());
    recordIds.erase(std::unique(recordIds.begin(), recordIds.end()), recordIds.end());
    WriteAppLog(L"WebDAV 文件枚举发现记录目录: " + std::to_wstring(recordIds.size()));
    std::vector<WebDavFileRecord> pending;
    const std::size_t batchSize = std::max<std::size_t>(1, options.batchSize);
    auto lastFlush = std::chrono::steady_clock::now();
    bool cancelled = false;
    auto flush = [&](bool force) {
        const bool intervalElapsed =
            std::chrono::steady_clock::now() - lastFlush >= std::chrono::milliseconds(100);
        if (pending.empty() || (!force && pending.size() < batchSize && !intervalElapsed)) return true;
        std::vector<WebDavFileRecord> batch;
        batch.swap(pending);
        lastFlush = std::chrono::steady_clock::now();
        if (callback && !callback(std::move(batch))) { cancelled = true; context.RequestStop(); return false; }
        return true;
    };
    auto enqueue = [&](WebDavFileRecord record) {
        pending.push_back(std::move(record));
        return flush(false);
    };
    auto errorRecord = [](const std::wstring& id, WebDavFileRecordHealth health,
                           const std::wstring& error) {
        WebDavFileRecord record;
        record.id = id;
        record.displayName = L"异常记录 · " + id.substr(0, std::min<std::size_t>(12, id.size()));
        record.uploadState = L"invalid";
        record.contentReady = false;
        record.health = health;
        record.recordError = error;
        return record;
    };
    struct MetadataLocalResult {
        explicit MetadataLocalResult(const AppConfig& config, const std::wstring& password)
            : client(std::make_unique<WebDavClient>(config, password)) {}
        std::unique_ptr<WebDavClient> client;
        std::size_t loaded = 0;
        std::size_t downloadFailures = 0;
        std::size_t missingMetadata = 0;
        std::size_t invalidMetadata = 0;
    };
    std::size_t loaded = 0;
    std::size_t downloadFailures = 0;
    std::size_t missingMetadata = 0;
    std::size_t invalidMetadata = 0;
    ScanProgressUpdate progress;
    progress.phase = L"webdav-metadata";
    progress.title = L"WebDAV 文件扫描进度";
    progress.status = L"正在读取远端文件记录";
    progress.detail = L"已发现 " + std::to_wstring(recordIds.size()) + L" 个记录目录";
    progress.discovered = recordIds.size();
    progress.total = recordIds.size();
    progress.indeterminate = false;
    context.Report(std::move(progress));
    context.ForEach<std::wstring, MetadataLocalResult>(
        recordIds,
        [&] { return MetadataLocalResult(config_, password); },
        [&](const std::wstring& recordId, MetadataLocalResult& local, ScanTaskContext& workerContext) {
                if (stopToken.stop_requested() || workerContext.StopRequested()) {
                    workerContext.RequestStop();
                    return;
                }
                const auto base = WebDavClient::CombineRemotePath(filesDirectory, recordId);
                std::wstring text;
                long metadataStatus = 0;
                WebDavFileRecord output;
                if (!local.client->DownloadText(WebDavClient::CombineRemotePath(base, L"metadata.json"),
                        text, stopToken, &metadataStatus)) {
                    if (metadataStatus == 404) {
                        ++local.missingMetadata;
                        WriteAppLog(L"WebDAV 文件记录缺少 Meta，已作为异常记录显示: " + recordId);
                        output = errorRecord(recordId, WebDavFileRecordHealth::MissingMetadata,
                            L"metadata.json 缺失");
                    } else {
                        ++local.downloadFailures;
                        WriteAppLog(L"WebDAV 文件 Meta 读取失败: " + recordId + L"，" + local.client->lastError());
                        output = errorRecord(recordId, WebDavFileRecordHealth::MetadataReadFailed,
                            local.client->lastError().empty() ? L"metadata.json 读取失败" : local.client->lastError());
                    }
                } else if (!ReadMetadata(text, output)) {
                    ++local.invalidMetadata;
                    WriteAppLog(L"WebDAV 文件 Meta 格式无效: " + recordId);
                    output = errorRecord(recordId, WebDavFileRecordHealth::InvalidMetadata,
                        L"metadata.json 格式无效");
                } else {
                    output.id = recordId;
                    ++local.loaded;
                }
                workerContext.Publish([&]() { enqueue(std::move(output)); });
                workerContext.UpdateProgress([&](ScanProgressUpdate& value) {
                    ++value.completed;
                    value.current = value.completed;
                    value.detail = recordId;
                });
        },
        [&](MetadataLocalResult&& local) {
            loaded += local.loaded;
            downloadFailures += local.downloadFailures;
            missingMetadata += local.missingMetadata;
            invalidMetadata += local.invalidMetadata;
        });
    if (stopToken.stop_requested() || cancelled || context.StopRequested()) { error = L"WebDAV 文件刷新已取消。"; return false; }
    if (!flush(true)) { error = L"WebDAV 文件刷新已取消。"; return false; }
    WriteAppLog(L"WebDAV 文件枚举完成: 目录 " + std::to_wstring(recordIds.size()) +
        L"，有效 " + std::to_wstring(loaded) +
        L"，读取失败 " + std::to_wstring(downloadFailures) +
        L"，缺少 Meta " + std::to_wstring(missingMetadata) +
        L"，无效 Meta " + std::to_wstring(invalidMetadata));
    if (downloadFailures > 0) {
        error = L"有 " + std::to_wstring(downloadFailures) +
            L" 个远端文件记录读取失败，已保留现有缓存，请稍后刷新重试。";
        return false;
    }
    return true;
}

WebDavFileOperationResult WebDavFileService::Upload(const std::filesystem::path& localPath,
    WebDavFileProgressCallback progress, std::stop_token stopToken) {
    WebDavFileOperationResult result; std::error_code ec;
    if (!std::filesystem::is_regular_file(localPath, ec)) { result.message = L"不是普通文件。"; return result; }
    std::wstring password, error; if (!LoadPassword(password, error)) { result.message = error; return result; }
    WebDavFileRecord record; record.absolutePath = CanonicalPath(localPath); record.displayName = localPath.filename().wstring(); record.id = RecordId(record.absolutePath);
    record.size = std::filesystem::file_size(localPath, ec); if (ec || !Sha256(localPath, record.sha256, error)) { result.message = ec ? L"无法读取文件大小。" : error; return result; }
    record.uploadedAtUtc = NowUtc(); record.uploadState = L"pending"; record.contentReady = false;
    if (progress && !progress(WebDavFileTransferPhase::Preparing, 0, record.size)) { result.message = L"上传已停止。"; return result; }
    WebDavClient client(config_, password); if (!EnsureFilesDirectory(client, error)) { result.message = error; return result; }
    const auto base = WebDavClient::CombineRemotePath(FilesDirectory(config_), record.id);
    const auto meta = std::filesystem::temp_directory_path() / (L"quattro_webdav_meta_" + record.id + L".json");
    if (!client.EnsureDirectory(base) || !SaveUtf8(meta, MetadataJson(record))) { result.message = L"写入上传元数据失败。"; return result; }
    if (progress && !progress(WebDavFileTransferPhase::UploadingMeta, 0, 0)) { std::filesystem::remove(meta, ec); result.message = L"上传已停止。"; return result; }
    const bool metaOk = client.UploadFile(meta, WebDavClient::CombineRemotePath(base, L"metadata.json"),
        [&](std::uint64_t transferred, std::uint64_t total) { return !progress || progress(WebDavFileTransferPhase::UploadingMeta, transferred, total); }, stopToken);
    if (!metaOk) { std::filesystem::remove(meta, ec); result.message = client.lastError(); return result; }
    if (stopToken.stop_requested()) { std::filesystem::remove(meta, ec); result.message = L"上传已停止。"; return result; }
    const bool contentOk = client.UploadFile(localPath, WebDavClient::CombineRemotePath(base, L"content"),
        [&](std::uint64_t transferred, std::uint64_t total) { return !progress || progress(WebDavFileTransferPhase::UploadingContent, transferred, total); }, stopToken);
    if (!contentOk) { std::filesystem::remove(meta, ec); result.message = client.lastError(); return result; }
    record.uploadState = L"complete"; record.contentReady = true;
    if (progress && !progress(WebDavFileTransferPhase::FinalizingMeta, 0, 0)) { std::filesystem::remove(meta, ec); result.message = L"上传已停止。"; return result; }
    if (!SaveUtf8(meta, MetadataJson(record)) || !client.UploadFile(meta, WebDavClient::CombineRemotePath(base, L"metadata.json"),
            [&](std::uint64_t transferred, std::uint64_t total) { return !progress || progress(WebDavFileTransferPhase::FinalizingMeta, transferred, total); }, stopToken)) {
        std::filesystem::remove(meta, ec); result.message = client.lastError().empty() ? L"上传完成标记失败。" : client.lastError(); return result;
    }
    std::filesystem::remove(meta, ec); result.ok = true; result.record = record; result.message = L"上传完成。"; return result;
}

WebDavFileOperationResult WebDavFileService::Download(const WebDavFileRecord& record,
    WebDavFileProgressCallback progress, std::stop_token stopToken) {
    WebDavFileOperationResult result; std::wstring password, error; if (!LoadPassword(password, error)) { result.message = error; return result; }
    if (!record.contentReady || ToLower(record.uploadState) != L"complete") { result.message = L"远端文件尚未上传完成。"; return result; }
    std::filesystem::path target; if (!ValidateDownloadTargetPath(record.absolutePath, target, error)) { result.message = error; return result; }
    std::error_code ec; std::filesystem::create_directories(target.parent_path(), ec); if (ec) { result.message = L"无法创建目标文件夹。"; return result; }
    WebDavClient client(config_, password); const auto base = WebDavClient::CombineRemotePath(FilesDirectory(config_), record.id);
    const auto temp = target.wstring() + L".quattro-download.tmp";
    if (!client.DownloadFile(WebDavClient::CombineRemotePath(base, L"content"), temp,
            [&](std::uint64_t transferred, std::uint64_t total) { return !progress || progress(WebDavFileTransferPhase::DownloadingContent, transferred, total); }, stopToken)) { std::filesystem::remove(temp, ec); result.message = client.lastError(); return result; }
    if (progress && !progress(WebDavFileTransferPhase::Verifying, 0, 0)) { std::filesystem::remove(temp, ec); result.message = L"下载已停止。"; return result; }
    std::wstring actual; if (std::filesystem::file_size(temp, ec) != record.size || !Sha256(temp, actual, error) || ToLower(actual) != ToLower(record.sha256)) { std::filesystem::remove(temp, ec); result.message = L"下载文件校验失败。"; return result; }
    std::filesystem::remove(target, ec); std::filesystem::rename(temp, target, ec); if (ec) { result.message = L"替换本地文件失败。"; return result; }
    result.ok = true; result.record = record; result.message = L"下载完成。"; return result;
}

bool WebDavFileService::Delete(const WebDavFileRecord& record, std::wstring& error,
    WebDavFileDeleteProgressCallback progress) {
    std::wstring password; if (!LoadPassword(password, error)) return false; WebDavClient client(config_, password);
    const auto base = WebDavClient::CombineRemotePath(FilesDirectory(config_), record.id);
    if (progress) progress(WebDavFileDeletePhase::DeletingContent, false);
    if (!client.DeleteRemoteFile(WebDavClient::CombineRemotePath(base, L"content"))) { error = client.lastError(); return false; }
    if (progress) progress(WebDavFileDeletePhase::DeletingContent, true);
    if (progress) progress(WebDavFileDeletePhase::DeletingMetadata, false);
    if (!client.DeleteRemoteFile(WebDavClient::CombineRemotePath(base, L"metadata.json"))) { error = client.lastError(); return false; }
    if (progress) progress(WebDavFileDeletePhase::DeletingMetadata, true);
    if (progress) progress(WebDavFileDeletePhase::DeletingDirectory, false);
    if (!client.DeleteRemoteDirectory(base)) { error = client.lastError(); return false; }
    if (progress) progress(WebDavFileDeletePhase::DeletingDirectory, true);
    return true;
}
