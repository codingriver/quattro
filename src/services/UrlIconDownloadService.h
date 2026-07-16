#pragma once

#include "Models.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class UrlIconDownloadService {
public:
    explicit UrlIconDownloadService(std::filesystem::path appDirectory);
    ~UrlIconDownloadService();

    UrlIconDownloadService(const UrlIconDownloadService&) = delete;
    UrlIconDownloadService& operator=(const UrlIconDownloadService&) = delete;

    void RequestInitialDownload(HWND notifyHwnd, UINT notifyMessage, Link link);
    bool RequestManualRefresh(HWND notifyHwnd, UINT notifyMessage, Link link);
    void Shutdown();

private:
    bool RequestDownload(HWND notifyHwnd, UINT notifyMessage, Link link, bool overwrite);
    bool DownloadIconForLink(const Link& link, bool overwrite);
    bool TryClaimHost(const std::wstring& host);
    void ReleaseHost(const std::wstring& host);

    std::filesystem::path appDirectory_;
    std::mutex mutex_;
    std::vector<std::thread> threads_;
    std::unordered_set<std::wstring> activeHosts_;
    bool stopping_ = false;
};
