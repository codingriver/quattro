#include "WebDavFileIndexCache.h"

#include "JsonValue.h"
#include "Utilities.h"
#include "WebDavClient.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <windows.h>

namespace {
constexpr int kSchemaVersion = 1;

class MutexLock {
public:
    explicit MutexLock(const std::wstring& name) {
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_) locked_ = WaitForSingleObject(handle_, 5000) == WAIT_OBJECT_0;
    }
    ~MutexLock() { if (locked_) ReleaseMutex(handle_); if (handle_) CloseHandle(handle_); }
    bool locked() const { return locked_; }
private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

std::wstring Escape(const std::wstring& value) {
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

bool SaveUtf8(const std::filesystem::path& path, const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(stream);
}

std::wstring Fingerprint(const AppConfig& config) {
    return ToLower(Trim(config.webDavUrl)) + L"\n" + ToLower(Trim(config.webDavUserName)) + L"\n" +
        ToLower(Trim(WebDavClient::FilesRemotePath(config)));
}

bool ParseRecord(const JsonValue& value, WebDavFileRecord& record) {
    if (!value.isObject()) return false;
    const auto* id = value.get(L"id"); const auto* path = value.get(L"absolutePath");
    const auto* name = value.get(L"displayName"); const auto* size = value.get(L"size");
    const auto* hash = value.get(L"sha256");
    if (!id || !id->isString() || !path || !path->isString() || !size || !size->isNumber() || !hash || !hash->isString()) return false;
    record.id = id->stringValue; record.absolutePath = path->stringValue;
    record.displayName = name ? name->stringOr() : std::filesystem::path(record.absolutePath).filename().wstring();
    record.size = static_cast<std::uint64_t>(size->numberValue); record.sha256 = hash->stringValue;
    record.uploadedAtUtc = value.get(L"uploadedAtUtc") ? value.get(L"uploadedAtUtc")->stringOr() : L"";
    record.uploadState = value.get(L"uploadState") ? value.get(L"uploadState")->stringOr(L"complete") : L"complete";
    record.contentReady = value.get(L"contentReady") ? value.get(L"contentReady")->boolOr(true) : true;
    const int health = value.get(L"health") ? value.get(L"health")->intOr() : 0;
    if (health >= static_cast<int>(WebDavFileRecordHealth::Healthy) &&
        health <= static_cast<int>(WebDavFileRecordHealth::MetadataReadFailed)) {
        record.health = static_cast<WebDavFileRecordHealth>(health);
    }
    record.recordError = value.get(L"recordError") ? value.get(L"recordError")->stringOr() : L"";
    return WebDavFileService::IsRecordDirectoryName(record.id);
}
}

WebDavFileIndexCache::WebDavFileIndexCache(AppConfig config)
    : mutexName_(L"Local\\QuattroWebDavFileIndexCache_" + WebDavFileService::RecordId(Fingerprint(config))),
      path_(CachePath(config)) {}

std::filesystem::path WebDavFileIndexCache::CachePath(const AppConfig& config) {
    return QuattroUserConfigDirectory() / L"cache" / L"webdav-files" /
        (WebDavFileService::RecordId(Fingerprint(config)) + L".json");
}

bool WebDavFileIndexCache::Load(std::vector<WebDavFileRecord>& records, std::wstring& refreshedAtUtc) const {
    MutexLock lock(mutexName_); return lock.locked() && LoadUnlocked(records, refreshedAtUtc);
}

bool WebDavFileIndexCache::LoadUnlocked(std::vector<WebDavFileRecord>& records, std::wstring& refreshedAtUtc) const {
    records.clear(); refreshedAtUtc.clear();
    if (!FileExists(path_)) return false;
    JsonValue root; std::wstring error;
    if (!ParseJson(LoadUtf8File(path_), root, error) || !root.isObject()) return false;
    if (!root.get(L"schemaVersion") || root.get(L"schemaVersion")->intOr() != kSchemaVersion) return false;
    refreshedAtUtc = root.get(L"refreshedAtUtc") ? root.get(L"refreshedAtUtc")->stringOr() : L"";
    const JsonValue* values = root.get(L"records");
    if (!values || !values->isArray()) return false;
    for (const auto& value : values->arrayValue) { WebDavFileRecord record; if (ParseRecord(value, record)) records.push_back(std::move(record)); }
    return true;
}

bool WebDavFileIndexCache::SaveUnlocked(const std::vector<WebDavFileRecord>& records, const std::wstring& refreshedAtUtc) const {
    std::error_code ec; std::filesystem::create_directories(path_.parent_path(), ec); if (ec) return false;
    std::wstringstream json;
    json << L"{\"schemaVersion\":" << kSchemaVersion << L",\"refreshedAtUtc\":\"" << Escape(refreshedAtUtc) << L"\",\"records\":[";
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i) json << L",";
        const auto& record = records[i];
        json << L"{\"id\":\"" << Escape(record.id) << L"\",\"absolutePath\":\"" << Escape(record.absolutePath)
             << L"\",\"displayName\":\"" << Escape(record.displayName) << L"\",\"size\":" << record.size
             << L",\"sha256\":\"" << Escape(record.sha256) << L"\",\"uploadedAtUtc\":\"" << Escape(record.uploadedAtUtc)
             << L"\",\"uploadState\":\"" << Escape(record.uploadState) << L"\",\"contentReady\":" << (record.contentReady ? L"true" : L"false")
             << L",\"health\":" << static_cast<int>(record.health)
             << L",\"recordError\":\"" << Escape(record.recordError) << L"\"}";
    }
    json << L"]}";
    auto temp = path_; temp += L".tmp." + std::to_wstring(GetCurrentProcessId());
    if (!SaveUtf8(temp, json.str())) return false;
    if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temp, ec); return false;
    }
    return true;
}

bool WebDavFileIndexCache::Replace(const std::vector<WebDavFileRecord>& records, const std::wstring& refreshedAtUtc) const {
    MutexLock lock(mutexName_); return lock.locked() && SaveUnlocked(records, refreshedAtUtc);
}

bool WebDavFileIndexCache::Upsert(const WebDavFileRecord& record) const {
    MutexLock lock(mutexName_); if (!lock.locked()) return false;
    std::vector<WebDavFileRecord> records; std::wstring refreshed;
    LoadUnlocked(records, refreshed);
    auto existing = std::find_if(records.begin(), records.end(), [&](const auto& item) { return item.id == record.id; });
    if (existing == records.end()) records.push_back(record); else *existing = record;
    return SaveUnlocked(records, refreshed);
}

bool WebDavFileIndexCache::Remove(const std::wstring& recordId) const {
    MutexLock lock(mutexName_); if (!lock.locked()) return false;
    std::vector<WebDavFileRecord> records; std::wstring refreshed;
    if (!LoadUnlocked(records, refreshed)) return true;
    records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& item) { return item.id == recordId; }), records.end());
    return SaveUnlocked(records, refreshed);
}
