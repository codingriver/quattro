#pragma once

#include "domain/TaskProgress.h"

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

enum class TaskExecutionMode {
    BackgroundParallel,
    BackgroundSingle,
    CallerParallel,
    CallerSingle,
};

enum class TaskForEachMode {
    Inherit,
    Parallel,
    Single,
};

struct TaskOptions {
    TaskExecutionMode mode = TaskExecutionMode::BackgroundParallel;
    std::size_t maxWorkers = 0;
    std::chrono::milliseconds progressInterval{80};
    // Runs on the task completion thread. UI callers may only use it to post
    // a message or signal another thread-safe completion channel.
    std::function<void()> completionCallback;
};

struct TaskForEachOptions {
    TaskForEachMode mode = TaskForEachMode::Inherit;
    std::size_t maxWorkers = 0;
};

class TaskContext;

class TaskHandle final : public std::enable_shared_from_this<TaskHandle> {
public:
    ~TaskHandle();

    void RequestStop();
    TaskProgressSnapshot Snapshot() const;
    TaskStatus Status() const;
    bool IsFinished() const;
    void Wait();

    template<class Result>
    Result ResultCopy() const {
        std::lock_guard lock(mutex_);
        return std::any_cast<Result>(result_);
    }

private:
    friend class TaskExecutionService;
    friend class TaskContext;

    explicit TaskHandle(TaskOptions options);
    void Run(std::function<std::any(TaskContext&)> body);
    void Complete(TaskStatus status, std::any result, std::wstring error);
    TaskProgressSnapshot LiveSnapshot() const;
    void Report(TaskProgressUpdate update);
    void UpdateProgress(const std::function<void(TaskProgressUpdate&)>& update);
    void Publish(std::function<void()> callback);
    void PublishProgressLocked(bool force);
    std::size_t ResolveWorkerCount(std::size_t itemCount, TaskForEachOptions options) const;
    bool Parallel() const;

    TaskOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::stop_source stopSource_;
    TaskProgressSnapshot snapshot_{};
    TaskProgressSnapshot publishedSnapshot_{};
    std::chrono::steady_clock::time_point lastProgressPublish_{};
    bool hasPublishedProgress_ = false;
    std::any result_;
    std::mutex publishMutex_;
};

class TaskContext final {
public:
    std::stop_token StopToken() const;
    bool StopRequested() const;
    TaskProgressSnapshot Snapshot() const;
    void RequestStop();
    void Report(TaskProgressUpdate update);
    void UpdateProgress(const std::function<void(TaskProgressUpdate&)>& update);
    void Publish(std::function<void()> callback);

    template<class Item, class LocalResult, class LocalFactory, class Processor, class Merger>
    void ForEach(
        std::span<const Item> items,
        LocalFactory createLocal,
        Processor process,
        Merger merge) {
        ForEach<Item, LocalResult>(
            items, TaskForEachOptions{}, std::move(createLocal), std::move(process), std::move(merge));
    }

    template<class Item, class LocalResult, class LocalFactory, class Processor, class Merger>
    void ForEach(
        std::span<const Item> items,
        TaskForEachOptions options,
        LocalFactory createLocal,
        Processor process,
        Merger merge) {
        if (items.empty() || StopRequested()) {
            return;
        }

        const std::size_t workerCount = handle_->ResolveWorkerCount(items.size(), options);
        {
            TaskProgressUpdate progress = handle_->LiveSnapshot();
            progress.workerCount = workerCount;
            Report(std::move(progress));
        }

        std::vector<LocalResult> localResults;
        localResults.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            localResults.push_back(createLocal());
        }

        std::atomic_size_t nextIndex{0};
        auto runWorker = [&](std::size_t workerIndex) {
            LocalResult& local = localResults[workerIndex];
            while (!StopRequested()) {
                const std::size_t itemIndex = nextIndex.fetch_add(1);
                if (itemIndex >= items.size()) {
                    break;
                }
                process(items[itemIndex], local, *this);
            }
        };

        if (workerCount <= 1) {
            runWorker(0);
        } else {
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t index = 0; index < workerCount; ++index) {
                workers.emplace_back(runWorker, index);
            }
            for (std::thread& worker : workers) {
                worker.join();
            }
        }

        for (LocalResult& local : localResults) {
            merge(std::move(local));
        }
    }

private:
    friend class TaskExecutionService;
    friend class TaskHandle;
    explicit TaskContext(TaskHandle* handle) : handle_(handle) {}
    TaskHandle* handle_ = nullptr;
};

class TaskExecutionService final {
public:
    using Body = std::function<std::any(TaskContext&)>;

    static std::shared_ptr<TaskHandle> Start(TaskOptions options, Body body);

    template<class Result, class BodyFunction>
    static std::shared_ptr<TaskHandle> StartTyped(
        TaskOptions options,
        BodyFunction body) {
        return Start(std::move(options), [body = std::move(body)](TaskContext& context) mutable {
            return std::any(body(context));
        });
    }

    template<class Result, class BodyFunction>
    static Result Run(TaskOptions options, BodyFunction body) {
        if (options.mode == TaskExecutionMode::BackgroundParallel) {
            options.mode = TaskExecutionMode::CallerParallel;
        } else if (options.mode == TaskExecutionMode::BackgroundSingle) {
            options.mode = TaskExecutionMode::CallerSingle;
        }
        const auto task = StartTyped<Result>(std::move(options), std::move(body));
        task->Wait();
        return task->ResultCopy<Result>();
    }
};
