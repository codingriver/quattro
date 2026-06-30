#pragma once

#include "AppStoreManifest.h"
#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

class AppStoreService {
public:
    AppStoreService(std::filesystem::path appDirectory, AppConfig config);

    bool Refresh();
    const std::vector<AppStoreEntry>& apps() const { return apps_; }
    const std::wstring& lastError() const { return lastError_; }

private:
    std::filesystem::path appDirectory_;
    AppConfig config_;
    std::wstring lastError_;
    std::vector<AppStoreEntry> apps_;
};
