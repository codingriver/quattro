#pragma once

#include "Models.h"
#include "Theme.h"
#include "ThemedFileTransferQueueDialog.h"
#include "WebDavFileService.h"

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct WebDavTransferQueueOptions {
    unsigned int maxConcurrentTransfers = 1;
};

class WebDavTransferQueueController final {
public:
    WebDavTransferQueueController(HWND owner, HINSTANCE instance, const Theme& theme, AppConfig config,
        WebDavTransferQueueOptions options = {});
    ~WebDavTransferQueueController();

    void EnqueueUploads(const std::vector<std::filesystem::path>& paths);
    void EnqueueDownloads(const std::vector<WebDavFileRecord>& records);
    bool Show();
    void RequestStopAll();
    bool HasRunningOrWaitingTasks() const;
    bool IsWindowVisible() const;
    bool IsWindowOpen() const;
    int FailedCount() const;
    ThemedFileTransferQueueSnapshot Snapshot() const;
    static unsigned int NormalizeMaxConcurrentTransfers(unsigned int value);

private:
    enum class Kind { Upload, Download };
    struct Task {
        std::uint64_t id = 0;
        Kind kind = Kind::Upload;
        std::filesystem::path uploadPath;
        WebDavFileRecord downloadRecord;
        std::wstring fileName;
        std::wstring absolutePath;
        std::uint64_t size = 0;
        ThemedFileTransferStatus status = ThemedFileTransferStatus::Waiting;
        WebDavFileTransferPhase phase = WebDavFileTransferPhase::Preparing;
        std::uint64_t transferred = 0;
        std::uint64_t total = 0;
        std::uint64_t contentTransferred = 0;
        std::uint64_t contentTotal = 0;
        std::wstring error;
        bool stopRequested = false;
    };

    void WorkerLoop(std::stop_token stopToken);
    std::shared_ptr<Task> TakeWaitingTask();
    void RunTask(const std::shared_ptr<Task>& task);
    void NotifyChanged();
    static ThemedFileTransferStatus StatusForPhase(Kind kind, WebDavFileTransferPhase phase);
    static double TaskProgress(const Task& task);

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Theme theme_;
    AppConfig config_;
    unsigned int maxConcurrentTransfers_ = 1;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::vector<std::shared_ptr<Task>> tasks_;
    std::vector<std::jthread> workers_;
    std::unique_ptr<ThemedFileTransferQueueDialog> dialog_;
    std::uint64_t nextTaskId_ = 1;
    bool shuttingDown_ = false;
};
