#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class TaskStatus {
    Pending,
    Running,
    Completed,
    Stopped,
    Failed,
};

struct TaskProgressUpdate {
    std::wstring phase;
    std::wstring title;
    std::wstring status;
    std::wstring detail;
    std::uint64_t discovered = 0;
    std::uint64_t completed = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t skipped = 0;
    std::uint64_t failed = 0;
    std::uint64_t current = 0;
    std::uint64_t total = 0;
    std::size_t workerCount = 0;
    bool indeterminate = true;
};

struct TaskProgressSnapshot : TaskProgressUpdate {
    TaskStatus taskStatus = TaskStatus::Pending;
    bool stopRequested = false;
    std::wstring error;
};
