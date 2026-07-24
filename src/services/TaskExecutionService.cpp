#include "TaskExecutionService.h"

namespace {
bool RunsInBackground(TaskExecutionMode mode) {
    return mode == TaskExecutionMode::BackgroundParallel ||
        mode == TaskExecutionMode::BackgroundSingle;
}
}

TaskHandle::TaskHandle(TaskOptions options)
    : options_(std::move(options)) {
    options_.progressInterval = std::max(std::chrono::milliseconds(1), options_.progressInterval);
}

TaskHandle::~TaskHandle() {
    RequestStop();
    Wait();
}

void TaskHandle::RequestStop() {
    stopSource_.request_stop();
    std::lock_guard lock(mutex_);
    snapshot_.stopRequested = true;
    publishedSnapshot_.stopRequested = true;
}

TaskProgressSnapshot TaskHandle::Snapshot() const {
    std::lock_guard lock(mutex_);
    return publishedSnapshot_;
}

TaskProgressSnapshot TaskHandle::LiveSnapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

TaskStatus TaskHandle::Status() const {
    std::lock_guard lock(mutex_);
    return snapshot_.taskStatus;
}

bool TaskHandle::IsFinished() const {
    const TaskStatus status = Status();
    return status == TaskStatus::Completed ||
        status == TaskStatus::Stopped ||
        status == TaskStatus::Failed;
}

void TaskHandle::Wait() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        return snapshot_.taskStatus == TaskStatus::Completed ||
            snapshot_.taskStatus == TaskStatus::Stopped ||
            snapshot_.taskStatus == TaskStatus::Failed;
    });
}

void TaskHandle::Run(std::function<std::any(TaskContext&)> body) {
    {
        std::lock_guard lock(mutex_);
        snapshot_.taskStatus = TaskStatus::Running;
        publishedSnapshot_ = snapshot_;
        hasPublishedProgress_ = false;
    }
    TaskContext context(this);
    try {
        std::any result = body(context);
        Complete(
            stopSource_.stop_requested() ? TaskStatus::Stopped : TaskStatus::Completed,
            std::move(result),
            {});
    } catch (const std::exception& exception) {
        Complete(
            TaskStatus::Failed,
            {},
            std::wstring(exception.what(), exception.what() + std::char_traits<char>::length(exception.what())));
    } catch (...) {
        Complete(TaskStatus::Failed, {}, L"任务执行过程中发生未处理异常。");
    }
}

void TaskHandle::Complete(TaskStatus status, std::any result, std::wstring error) {
    {
        std::lock_guard lock(mutex_);
        result_ = std::move(result);
        snapshot_.taskStatus = status;
        snapshot_.stopRequested = stopSource_.stop_requested();
        snapshot_.error = std::move(error);
        PublishProgressLocked(true);
    }
    condition_.notify_all();
    if (options_.completionCallback) {
        options_.completionCallback();
    }
}

void TaskHandle::Report(TaskProgressUpdate update) {
    std::lock_guard lock(mutex_);
    static_cast<TaskProgressUpdate&>(snapshot_) = std::move(update);
    snapshot_.taskStatus = TaskStatus::Running;
    snapshot_.stopRequested = stopSource_.stop_requested();
    PublishProgressLocked(false);
}

void TaskHandle::UpdateProgress(const std::function<void(TaskProgressUpdate&)>& update) {
    if (!update) {
        return;
    }
    std::lock_guard lock(mutex_);
    update(static_cast<TaskProgressUpdate&>(snapshot_));
    snapshot_.taskStatus = TaskStatus::Running;
    snapshot_.stopRequested = stopSource_.stop_requested();
    PublishProgressLocked(false);
}

void TaskHandle::PublishProgressLocked(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && hasPublishedProgress_ && now - lastProgressPublish_ < options_.progressInterval) {
        return;
    }
    publishedSnapshot_ = snapshot_;
    lastProgressPublish_ = now;
    hasPublishedProgress_ = true;
}

void TaskHandle::Publish(std::function<void()> callback) {
    if (!callback || stopSource_.stop_requested()) {
        return;
    }
    std::lock_guard lock(publishMutex_);
    if (!stopSource_.stop_requested()) {
        callback();
    }
}

std::size_t TaskHandle::ResolveWorkerCount(
    std::size_t itemCount,
    TaskForEachOptions options) const {
    if (itemCount == 0) {
        return 0;
    }
    const bool parallel = options.mode == TaskForEachMode::Parallel ||
        (options.mode == TaskForEachMode::Inherit && Parallel());
    if (!parallel || options.mode == TaskForEachMode::Single) {
        return 1;
    }
    const std::size_t hardware = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const std::size_t requested = options.maxWorkers != 0 ? options.maxWorkers : options_.maxWorkers;
    const std::size_t configured = requested == 0 ? hardware : requested;
    return std::max<std::size_t>(1, std::min({itemCount, hardware, configured, std::size_t{8}}));
}

bool TaskHandle::Parallel() const {
    return options_.mode == TaskExecutionMode::BackgroundParallel ||
        options_.mode == TaskExecutionMode::CallerParallel;
}

std::stop_token TaskContext::StopToken() const {
    return handle_->stopSource_.get_token();
}

bool TaskContext::StopRequested() const {
    return StopToken().stop_requested();
}

TaskProgressSnapshot TaskContext::Snapshot() const {
    return handle_->LiveSnapshot();
}

void TaskContext::RequestStop() {
    handle_->RequestStop();
}

void TaskContext::Report(TaskProgressUpdate update) {
    handle_->Report(std::move(update));
}

void TaskContext::UpdateProgress(const std::function<void(TaskProgressUpdate&)>& update) {
    handle_->UpdateProgress(update);
}

void TaskContext::Publish(std::function<void()> callback) {
    handle_->Publish(std::move(callback));
}

std::shared_ptr<TaskHandle> TaskExecutionService::Start(TaskOptions options, Body body) {
    auto task = std::shared_ptr<TaskHandle>(new TaskHandle(std::move(options)));
    if (RunsInBackground(task->options_.mode)) {
        std::thread([task, body = std::move(body)]() mutable {
            task->Run(std::move(body));
        }).detach();
    } else {
        task->Run(std::move(body));
    }
    return task;
}
