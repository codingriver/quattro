#pragma once

#include "Theme.h"
#include "ThemedTaskProgressDialog.h"
#include "WebDavFileService.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class WebDavTransferProgressController {
public:
    WebDavTransferProgressController(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        AppConfig config);
    ~WebDavTransferProgressController();

    bool StartUpload(std::vector<std::filesystem::path> paths);
    bool StartDownload(WebDavFileRecord record);
    void ShowProgress();
    void RequestStop();
    bool IsRunning() const;
    int RunUntilFinished();

    const WebDavFileOperationResult& downloadResult() const { return downloadResult_; }
    int succeeded() const { return succeeded_; }
    int failed() const { return failed_; }

private:
    struct State {
        WebDavFileTransferPhase phase = WebDavFileTransferPhase::Preparing;
        std::wstring currentFileName;
        std::size_t currentFileIndex = 0;
        std::size_t fileCount = 0;
        std::uint64_t currentBytes = 0;
        std::uint64_t currentTotal = 0;
        std::uint64_t currentFileSize = 0;
        std::uint64_t batchBytes = 0;
        std::uint64_t batchTotal = 0;
        std::uint64_t completedBytes = 0;
        int succeeded = 0;
        int failed = 0;
        bool stopRequested = false;
        bool finished = false;
        std::wstring lastError;
    };

    void StartWorker();
    void UpdatePhase(WebDavFileTransferPhase phase, std::uint64_t transferred, std::uint64_t total);
    ThemedTaskProgressSnapshot Snapshot() const;
    void PostFinished();
    void JoinWorker();

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    AppConfig config_;
    std::vector<std::filesystem::path> uploadPaths_;
    std::optional<WebDavFileRecord> downloadRecord_;
    std::unique_ptr<ThemedTaskProgressDialog> progressDialog_;
    std::jthread worker_;
    mutable std::mutex mutex_;
    State state_;
    WebDavFileOperationResult downloadResult_;
    int succeeded_ = 0;
    int failed_ = 0;
    bool uploadMode_ = false;
    bool downloadMode_ = false;
    DWORD messageThreadId_ = 0;
};
