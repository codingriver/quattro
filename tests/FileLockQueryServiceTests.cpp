#include "../src/services/FileLockQueryService.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        ++failures;
    }
}
}

int wmain() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"quattro_file_lock_service_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / L"nested", error);
    for (int index = 0; index < 48; ++index) {
        const std::filesystem::path parent = index % 2 == 0 ? root : root / L"nested";
        std::ofstream(parent / (L"resource-" + std::to_wstring(index) + L".txt"), std::ios::binary) << "test";
    }

    std::vector<FileLockQueryProgress> progressEvents;
    FileLockQueryOptions parallelOptions;
    parallelOptions.batchSize = 4;
    parallelOptions.maxWorkers = 4;
    const FileLockQueryResult parallelResult = QueryFileLocks(
        root.wstring(),
        {},
        [&](const FileLockQueryProgress& progress) { progressEvents.push_back(progress); },
        parallelOptions);
    Check(parallelResult.error.empty(), "directory query succeeds");
    Check(parallelResult.directory, "directory input is identified");
    Check(parallelResult.totalPaths == 49 && parallelResult.checkedPaths == 49,
        "root and all regular files are checked");
    Check(parallelResult.workerCount >= 2 && parallelResult.workerCount <= 4,
        "directory query uses bounded parallel workers");
    Check(
        std::any_of(progressEvents.begin(), progressEvents.end(), [](const FileLockQueryProgress& progress) {
            return progress.phase == FileLockQueryPhase::Enumerating;
        }),
        "enumeration progress is reported");
    Check(
        !progressEvents.empty() && progressEvents.back().phase == FileLockQueryPhase::Completed &&
            progressEvents.back().checkedPaths == progressEvents.back().totalPaths,
        "completion progress reaches the total");

    std::atomic_bool cancelRequested{false};
    FileLockQueryOptions cancelOptions;
    cancelOptions.batchSize = 1;
    cancelOptions.maxWorkers = 2;
    cancelOptions.batchDelay = std::chrono::milliseconds(3);
    const FileLockQueryResult cancelledResult = QueryFileLocks(
        root.wstring(),
        [&]() { return cancelRequested.load(); },
        [&](const FileLockQueryProgress& progress) {
            if (progress.phase == FileLockQueryPhase::Querying && progress.checkedPaths >= 2) {
                cancelRequested.store(true);
            }
        },
        cancelOptions);
    Check(cancelledResult.cancelled, "directory query supports cancellation");
    Check(cancelledResult.checkedPaths < cancelledResult.totalPaths,
        "cancellation stops before every path is checked");

    const FileLockQueryResult fileResult = QueryFileLocks((root / L"resource-0.txt").wstring());
    Check(fileResult.error.empty() && !fileResult.directory && fileResult.totalPaths == 1 &&
            fileResult.checkedPaths == 1 && fileResult.workerCount == 1,
        "single-file query stays on the lightweight path");
    const FileLockQueryResult missingResult = QueryFileLocks((root / L"missing.txt").wstring());
    Check(!missingResult.error.empty(), "missing path reports an error");

    std::filesystem::remove_all(root, error);
    if (failures != 0) {
        return 1;
    }
    std::wcout << L"file_lock_query_service_tests=passed\n";
    return 0;
}
