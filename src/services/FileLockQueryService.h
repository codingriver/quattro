#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

enum class FileLockQueryPhase {
    Validating,
    Enumerating,
    Querying,
    ScanningHandles,
    Completed,
    Cancelled,
};

struct FileLockQueryProgress {
    FileLockQueryPhase phase = FileLockQueryPhase::Validating;
    std::size_t discoveredPaths = 0;
    std::size_t checkedPaths = 0;
    std::size_t totalPaths = 0;
    std::size_t workerCount = 0;
    std::size_t checkedProcesses = 0;
    std::size_t totalProcesses = 0;
    std::size_t inaccessibleProcesses = 0;
};

enum class FileLockSource {
    RestartManager,
    SystemHandle,
};

struct FileLockEvidence {
    unsigned long processId = 0;
    FileLockSource source = FileLockSource::RestartManager;
    std::wstring lockedPath;
    bool directory = false;
};

struct FileLockQueryResult {
    std::vector<unsigned long> processIds;
    std::vector<FileLockEvidence> evidence;
    std::size_t checkedPaths = 0;
    std::size_t totalPaths = 0;
    std::size_t workerCount = 0;
    std::size_t checkedProcesses = 0;
    std::size_t totalProcesses = 0;
    std::size_t inaccessibleProcesses = 0;
    bool directory = false;
    bool cancelled = false;
    bool handleScanSupported = true;
    std::wstring warning;
    std::wstring error;
};

struct FileLockQueryOptions {
    std::size_t batchSize = 32;
    std::size_t maxWorkers = 8;
    std::size_t maxHandleWorkers = 4;
    // Acceptance tests may use this to keep the progress window observable.
    std::chrono::milliseconds batchDelay{0};
};

using FileLockCancelCheck = std::function<bool()>;
using FileLockProgressCallback = std::function<void(const FileLockQueryProgress&)>;

FileLockQueryResult QueryFileLocks(
    const std::wstring& rawPath,
    const FileLockCancelCheck& shouldCancel = {},
    const FileLockProgressCallback& reportProgress = {},
    FileLockQueryOptions options = {});
