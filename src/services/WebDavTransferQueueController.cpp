#include "WebDavTransferQueueController.h"

#include "AppLog.h"
#include "Utilities.h"
#include "WebDavFileIndexCache.h"
#include "../../resources/resource.h"

#include <algorithm>
#include <unordered_set>

namespace {
bool Terminal(ThemedFileTransferStatus status) {
    return status == ThemedFileTransferStatus::UploadCompleted ||
        status == ThemedFileTransferStatus::DownloadCompleted ||
        status == ThemedFileTransferStatus::UploadFailed ||
        status == ThemedFileTransferStatus::DownloadFailed ||
        status == ThemedFileTransferStatus::Stopped;
}

bool Active(ThemedFileTransferStatus status) {
    return status != ThemedFileTransferStatus::Waiting && !Terminal(status);
}

std::wstring PhaseText(ThemedFileTransferStatus status) {
    switch (status) {
    case ThemedFileTransferStatus::Preparing: return L"正在准备（1 / 4）";
    case ThemedFileTransferStatus::UploadingMeta: return L"正在上传 META（2 / 4）";
    case ThemedFileTransferStatus::Uploading: return L"正在上传（3 / 4）";
    case ThemedFileTransferStatus::Confirming: return L"正在确认（4 / 4）";
    case ThemedFileTransferStatus::Downloading: return L"正在下载（1 / 2）";
    case ThemedFileTransferStatus::Verifying: return L"正在校验（2 / 2）";
    default: return L"等待传输";
    }
}

std::pair<int, int> PhasePosition(bool upload, WebDavFileTransferPhase phase) {
    if (!upload) {
        return phase == WebDavFileTransferPhase::Verifying ? std::pair{2, 2} : std::pair{1, 2};
    }
    switch (phase) {
    case WebDavFileTransferPhase::Preparing: return {1, 4};
    case WebDavFileTransferPhase::UploadingMeta: return {2, 4};
    case WebDavFileTransferPhase::UploadingContent: return {3, 4};
    case WebDavFileTransferPhase::FinalizingMeta: return {4, 4};
    default: return {0, 4};
    }
}

int Percent(std::uint64_t value, std::uint64_t total) {
    if (total == 0) return 0;
    return static_cast<int>(std::clamp(
        static_cast<double>(value) * 100.0 / static_cast<double>(total), 0.0, 100.0));
}
}

WebDavTransferQueueController::WebDavTransferQueueController(
    HWND owner, HINSTANCE instance, const Theme& theme, AppConfig config, WebDavTransferQueueOptions options)
    : owner_(owner), instance_(instance), theme_(theme), config_(std::move(config)),
      maxConcurrentTransfers_(NormalizeMaxConcurrentTransfers(options.maxConcurrentTransfers)) {
    ThemedFileTransferQueueDialogOptions dialogOptions{};
    dialogOptions.owner = owner_;
    dialogOptions.instance = instance_;
    dialogOptions.theme = theme_;
    dialogOptions.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    dialogOptions.className = L"QuattroWebDavTransferQueue_" + std::to_wstring(GetCurrentProcessId());
    dialogOptions.title = L"WebDAV 文件传输";
    dialogOptions.maxConcurrentTransfers = maxConcurrentTransfers_;
    dialogOptions.readSnapshot = [this] { return Snapshot(); };
    dialogOptions.requestStopAll = [this] { RequestStopAll(); };
    dialog_ = std::make_unique<ThemedFileTransferQueueDialog>(std::move(dialogOptions));
    for (unsigned int i = 0; i < maxConcurrentTransfers_; ++i) {
        workers_.emplace_back([this](std::stop_token token) { WorkerLoop(token); });
    }
}

unsigned int WebDavTransferQueueController::NormalizeMaxConcurrentTransfers(unsigned int value) {
    return std::clamp(value, 1u, 4u);
}

WebDavTransferQueueController::~WebDavTransferQueueController() {
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
        for (const auto& task : tasks_) task->stopRequested = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) worker.request_stop();
    workers_.clear();
}

void WebDavTransferQueueController::EnqueueUploads(const std::vector<std::filesystem::path>& paths) {
    std::lock_guard lock(mutex_);
    std::unordered_set<std::wstring> submitted;
    for (const auto& path : paths) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) continue;
        const std::wstring canonical = WebDavFileService::CanonicalPath(path);
        if (!submitted.insert(canonical).second) continue;
        const bool duplicatePending = std::any_of(tasks_.begin(), tasks_.end(), [&](const auto& task) {
            return task->kind == Kind::Upload && !Terminal(task->status) &&
                WebDavFileService::CanonicalPath(task->uploadPath) == canonical;
        });
        if (duplicatePending) continue;
        auto task = std::make_shared<Task>();
        task->id = nextTaskId_++;
        task->kind = Kind::Upload;
        task->uploadPath = path;
        task->fileName = path.filename().wstring();
        task->absolutePath = canonical;
        task->size = std::filesystem::file_size(path, ec);
        if (ec) task->size = 0;
        task->contentTotal = task->size;
        tasks_.push_back(std::move(task));
    }
    condition_.notify_all();
    NotifyChanged();
}

void WebDavTransferQueueController::EnqueueDownloads(const std::vector<WebDavFileRecord>& records) {
    std::lock_guard lock(mutex_);
    for (const auto& record : records) {
        const bool duplicatePending = std::any_of(tasks_.begin(), tasks_.end(), [&](const auto& task) {
            return task->kind == Kind::Download && !Terminal(task->status) && task->downloadRecord.id == record.id;
        });
        if (duplicatePending) continue;
        auto task = std::make_shared<Task>();
        task->id = nextTaskId_++;
        task->kind = Kind::Download;
        task->downloadRecord = record;
        task->fileName = record.displayName;
        task->absolutePath = record.absolutePath;
        task->size = record.size;
        task->contentTotal = task->size;
        tasks_.push_back(std::move(task));
    }
    condition_.notify_all();
    NotifyChanged();
}

bool WebDavTransferQueueController::Show() { return dialog_ && dialog_->Show(); }

void WebDavTransferQueueController::RequestStopAll() {
    {
        std::lock_guard lock(mutex_);
        for (const auto& task : tasks_) {
            if (task->status == ThemedFileTransferStatus::Waiting) task->status = ThemedFileTransferStatus::Stopped;
            else if (Active(task->status)) task->stopRequested = true;
        }
    }
    condition_.notify_all(); NotifyChanged();
}

bool WebDavTransferQueueController::HasRunningOrWaitingTasks() const {
    std::lock_guard lock(mutex_);
    return std::any_of(tasks_.begin(), tasks_.end(), [](const auto& task) { return !Terminal(task->status); });
}
bool WebDavTransferQueueController::IsWindowVisible() const { return dialog_ && dialog_->IsVisible(); }
bool WebDavTransferQueueController::IsWindowOpen() const { return dialog_ && dialog_->IsOpen(); }
int WebDavTransferQueueController::FailedCount() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(std::count_if(tasks_.begin(), tasks_.end(), [](const auto& task) {
        return task->status == ThemedFileTransferStatus::UploadFailed || task->status == ThemedFileTransferStatus::DownloadFailed;
    }));
}

std::shared_ptr<WebDavTransferQueueController::Task> WebDavTransferQueueController::TakeWaitingTask() {
    auto it = std::find_if(tasks_.begin(), tasks_.end(), [](const auto& task) {
        return task->status == ThemedFileTransferStatus::Waiting;
    });
    if (it == tasks_.end()) return {};
    (*it)->status = ThemedFileTransferStatus::Preparing;
    return *it;
}

void WebDavTransferQueueController::WorkerLoop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stopToken, [this] {
                return shuttingDown_ || std::any_of(tasks_.begin(), tasks_.end(), [](const auto& item) {
                    return item->status == ThemedFileTransferStatus::Waiting;
                });
            });
            if (shuttingDown_ || stopToken.stop_requested()) return;
            task = TakeWaitingTask();
        }
        if (task) { NotifyChanged(); RunTask(task); }
    }
}

void WebDavTransferQueueController::RunTask(const std::shared_ptr<Task>& task) {
    WriteAppLog(std::wstring(L"WebDAV 队列任务开始：") + task->fileName);
    WebDavFileService service(config_);
    auto progress = [this, task](WebDavFileTransferPhase phase, std::uint64_t transferred, std::uint64_t total) {
        {
            std::lock_guard lock(mutex_);
            task->phase = phase;
            task->status = StatusForPhase(task->kind, phase);
            task->transferred = transferred;
            task->total = total;
            if (phase == WebDavFileTransferPhase::UploadingContent ||
                phase == WebDavFileTransferPhase::DownloadingContent) {
                task->contentTransferred = transferred;
                task->contentTotal = total > 0 ? total : task->size;
            }
            if (task->stopRequested) return false;
        }
        NotifyChanged();
        return true;
    };
    WebDavFileOperationResult result = task->kind == Kind::Upload
        ? service.Upload(task->uploadPath, progress)
        : service.Download(task->downloadRecord, progress);
    if (result.ok && task->kind == Kind::Upload) WebDavFileIndexCache(config_).Upsert(result.record);
    std::wstring finalState;
    std::wstring finalMessage;
    {
        std::lock_guard lock(mutex_);
        if (task->stopRequested) { task->status = ThemedFileTransferStatus::Stopped; finalState = L"停止"; }
        else if (result.ok) task->status = task->kind == Kind::Upload
            ? ThemedFileTransferStatus::UploadCompleted : ThemedFileTransferStatus::DownloadCompleted;
        else {
            task->status = task->kind == Kind::Upload
                ? ThemedFileTransferStatus::UploadFailed : ThemedFileTransferStatus::DownloadFailed;
            task->error = result.message;
        }
        if (finalState.empty()) finalState = result.ok ? L"成功" : L"失败";
        finalMessage = result.message;
        if (result.ok) {
            task->transferred = task->size;
            task->total = task->size;
            task->contentTransferred = task->size;
            task->contentTotal = task->size;
        }
    }
    WriteAppLog(std::wstring(L"WebDAV 队列任务结束：") + task->fileName + L"，状态=" +
        finalState + (finalMessage.empty() ? L"" : L"，详情=" + finalMessage));
    NotifyChanged();
}

ThemedFileTransferStatus WebDavTransferQueueController::StatusForPhase(Kind kind, WebDavFileTransferPhase phase) {
    if (kind == Kind::Download) return phase == WebDavFileTransferPhase::Verifying
        ? ThemedFileTransferStatus::Verifying : ThemedFileTransferStatus::Downloading;
    switch (phase) {
    case WebDavFileTransferPhase::Preparing: return ThemedFileTransferStatus::Preparing;
    case WebDavFileTransferPhase::UploadingMeta: return ThemedFileTransferStatus::UploadingMeta;
    case WebDavFileTransferPhase::UploadingContent: return ThemedFileTransferStatus::Uploading;
    case WebDavFileTransferPhase::FinalizingMeta: return ThemedFileTransferStatus::Confirming;
    default: return ThemedFileTransferStatus::Preparing;
    }
}

double WebDavTransferQueueController::TaskProgress(const Task& task) {
    if (task.status == ThemedFileTransferStatus::UploadCompleted || task.status == ThemedFileTransferStatus::DownloadCompleted) return 1.0;
    if (task.kind == Kind::Download) {
        if (task.status == ThemedFileTransferStatus::Verifying) return 0.5;
        return task.total > 0
            ? std::clamp(static_cast<double>(task.transferred) / task.total * 0.5, 0.0, 0.5)
            : 0.0;
    }
    const double ratio = task.total > 0 ? std::clamp(static_cast<double>(task.transferred) / task.total, 0.0, 1.0) : 0.0;
    switch (task.status) {
    case ThemedFileTransferStatus::Preparing: return 0.0;
    case ThemedFileTransferStatus::UploadingMeta: return 0.25 + ratio * 0.25;
    case ThemedFileTransferStatus::Uploading: return 0.50 + ratio * 0.25;
    case ThemedFileTransferStatus::Confirming: return 0.75 + ratio * 0.25;
    default: return 0.0;
    }
}

ThemedFileTransferQueueSnapshot WebDavTransferQueueController::Snapshot() const {
    std::lock_guard lock(mutex_);
    ThemedFileTransferQueueSnapshot snapshot;
    snapshot.title = L"WebDAV 文件传输";
    snapshot.rows.reserve(tasks_.size());
    std::uint64_t totalBytes = 0;
    std::uint64_t transferredBytes = 0;
    int completed = 0, failed = 0, stopped = 0, waiting = 0, active = 0;
    const Task* current = nullptr;
    for (const auto& task : tasks_) {
        const bool taskActive = Active(task->status);
        const auto [phaseIndex, phaseCount] = PhasePosition(task->kind == Kind::Upload, task->phase);
        ThemedFileTransferRow row;
        row.id = task->id;
        row.fileName = task->fileName;
        row.absolutePath = task->absolutePath;
        row.size = task->size;
        row.status = task->status;
        row.direction = task->kind == Kind::Upload
            ? ThemedFileTransferDirection::Upload : ThemedFileTransferDirection::Download;
        row.phaseIndex = taskActive ? phaseIndex : 0;
        row.phaseCount = taskActive ? phaseCount : 0;
        row.phaseTransferred = task->transferred;
        row.phaseTotal = task->total;
        row.contentTransferred = task->contentTransferred;
        row.contentTotal = task->contentTotal;
        row.error = task->error;
        row.active = taskActive;
        snapshot.rows.push_back(std::move(row));

        totalBytes += task->size;
        if (task->status == ThemedFileTransferStatus::UploadCompleted ||
            task->status == ThemedFileTransferStatus::DownloadCompleted) {
            ++completed;
            transferredBytes += task->size;
        } else {
            transferredBytes += std::min(task->contentTransferred, task->size);
            if (task->status == ThemedFileTransferStatus::UploadFailed ||
                task->status == ThemedFileTransferStatus::DownloadFailed) ++failed;
            else if (task->status == ThemedFileTransferStatus::Stopped) ++stopped;
            else if (task->status == ThemedFileTransferStatus::Waiting) ++waiting;
            else if (taskActive) {
                ++active;
                if (!current) current = task.get();
            }
        }
    }
    snapshot.progress = totalBytes > 0
        ? std::clamp(static_cast<double>(transferredBytes) / static_cast<double>(totalBytes), 0.0, 1.0)
        : (tasks_.empty() ? 0.0 : static_cast<double>(completed) / static_cast<double>(tasks_.size()));
    snapshot.running = std::any_of(tasks_.begin(), tasks_.end(), [](const auto& task) { return !Terminal(task->status); });
    snapshot.stopRequested = snapshot.running && std::any_of(tasks_.begin(), tasks_.end(), [](const auto& task) { return task->stopRequested; });
    snapshot.status = L"总进度 " + FormatByteSizeForDisplay(transferredBytes) + L" / " +
        FormatByteSizeForDisplay(totalBytes) + L"（" + std::to_wstring(Percent(transferredBytes, totalBytes)) +
        L"%） · 成功 " + std::to_wstring(completed) + L" · 失败 " + std::to_wstring(failed) +
        L" · 处理中 " + std::to_wstring(active) + L" · 等待 " + std::to_wstring(waiting);
    if (stopped > 0) snapshot.status += L" · 停止 " + std::to_wstring(stopped);
    if (current) {
        const auto index = std::find_if(tasks_.begin(), tasks_.end(), [&](const auto& item) { return item->id == current->id; });
        snapshot.detail = L"当前 " + std::to_wstring(std::distance(tasks_.begin(), index) + 1) + L" / " +
            std::to_wstring(tasks_.size()) + L" · " + current->fileName + L" · " + PhaseText(current->status);
        if (current->total > 0 && current->status != ThemedFileTransferStatus::Preparing) {
            snapshot.detail += L" · " + FormatByteSizeForDisplay(current->transferred) + L" / " +
                FormatByteSizeForDisplay(current->total) + L"（" +
                std::to_wstring(Percent(current->transferred, current->total)) + L"%）";
        }
        if (active > 1) snapshot.detail += L" · 并行任务 " + std::to_wstring(active);
        snapshot.currentProgress = TaskProgress(*current);
        snapshot.currentProgressVisible = true;
    } else if (!tasks_.empty()) {
        snapshot.detail = snapshot.running
            ? L"等待下一项传输任务开始。"
            : (failed > 0 ? L"传输结束，部分文件失败。" :
                (stopped > 0 ? L"传输已停止。" : L"全部文件传输完成。"));
    } else {
        snapshot.status = L"等待传输";
        snapshot.detail = L"尚未添加文件。";
    }
    return snapshot;
}

void WebDavTransferQueueController::NotifyChanged() { if (dialog_) dialog_->NotifyChanged(); }
