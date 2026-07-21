#pragma once

#include "Models.h"
#include "WebDavFileService.h"

#include <filesystem>
#include <string>
#include <vector>

class WebDavFileIndexCache final {
public:
    explicit WebDavFileIndexCache(AppConfig config);

    bool Load(std::vector<WebDavFileRecord>& records, std::wstring& refreshedAtUtc) const;
    bool Replace(const std::vector<WebDavFileRecord>& records, const std::wstring& refreshedAtUtc) const;
    bool Upsert(const WebDavFileRecord& record) const;
    bool Remove(const std::wstring& recordId) const;

    const std::filesystem::path& path() const { return path_; }
    static std::filesystem::path CachePath(const AppConfig& config);

private:
    bool LoadUnlocked(std::vector<WebDavFileRecord>& records, std::wstring& refreshedAtUtc) const;
    bool SaveUnlocked(const std::vector<WebDavFileRecord>& records, const std::wstring& refreshedAtUtc) const;
    std::wstring mutexName_;
    std::filesystem::path path_;
};
