#include "FileLockQueryService.h"

#include "Utilities.h"

#include <restartmanager.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

namespace {
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

bool IsCancelled(const FileLockCancelCheck& shouldCancel) {
    return shouldCancel && shouldCancel();
}

bool IsDriveRootPath(const std::wstring& path) {
    return path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

std::wstring DirectoryResourcePath(std::wstring path) {
    if (path.empty() || IsDriveRootPath(path)) {
        return path;
    }
    const wchar_t last = path.back();
    if (last != L'\\' && last != L'/') {
        path.push_back(L'\\');
    }
    return path;
}

struct BatchResult {
    std::vector<unsigned long> processIds;
    std::wstring error;
};

constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);

struct SystemHandleEntry {
    PVOID object = nullptr;
    ULONG_PTR processId = 0;
    ULONG_PTR handleValue = 0;
    ULONG grantedAccess = 0;
    USHORT creatorBackTraceIndex = 0;
    USHORT objectTypeIndex = 0;
    ULONG handleAttributes = 0;
    ULONG reserved = 0;
};

struct SystemHandleInformation {
    ULONG_PTR handleCount = 0;
    ULONG_PTR reserved = 0;
    SystemHandleEntry handles[1];
};

using NtQuerySystemInformationFn = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);

struct HandleScanResult {
    std::vector<FileLockEvidence> evidence;
    std::size_t checkedProcesses = 0;
    std::size_t totalProcesses = 0;
    std::size_t inaccessibleProcesses = 0;
    bool supported = true;
    bool cancelled = false;
    std::wstring warning;
};

void AppendWarning(std::wstring& target, const std::wstring& value) {
    if (value.empty()) return;
    if (!target.empty()) target += L"；";
    target += value;
}

std::wstring ComparablePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        path = L"\\\\" + path.substr(8);
    } else if (path.starts_with(L"\\\\?\\")) {
        path.erase(0, 4);
    }
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

std::wstring AbsoluteComparablePath(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return ComparablePath(path);
    std::wstring absolute(static_cast<std::size_t>(required), L'\0');
    const DWORD length = GetFullPathNameW(
        path.c_str(), required, absolute.data(), nullptr);
    if (length == 0 || length >= required) return ComparablePath(path);
    absolute.resize(length);
    return ComparablePath(std::move(absolute));
}

bool EqualPathPrefix(const std::wstring& left, const std::wstring& right, int length) {
    return CompareStringOrdinal(left.c_str(), length, right.c_str(), length, TRUE) == CSTR_EQUAL;
}

bool PathIsTargetOrDescendant(const std::wstring& candidateValue, const std::wstring& targetValue) {
    const std::wstring candidate = ComparablePath(candidateValue);
    const std::wstring target = ComparablePath(targetValue);
    if (candidate.size() < target.size() ||
        !EqualPathPrefix(candidate, target, static_cast<int>(target.size()))) {
        return false;
    }
    return candidate.size() == target.size() ||
        target.ends_with(L'\\') || candidate[target.size()] == L'\\';
}

std::wstring FinalPathForHandle(HANDLE handle) {
    DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) return {};
    std::wstring path(static_cast<std::size_t>(required), L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        handle, path.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= required) return {};
    path.resize(length);
    return ComparablePath(std::move(path));
}

bool QuerySystemHandleSnapshot(
    NtQuerySystemInformationFn query,
    std::vector<unsigned char>& buffer,
    std::wstring& error) {
    ULONG size = 1u << 20;
    constexpr ULONG maxSize = 256u << 20;
    while (size <= maxSize) {
        buffer.resize(size);
        ULONG needed = 0;
        const LONG status = query(
            kSystemExtendedHandleInformation, buffer.data(), size, &needed);
        if (status >= 0) return true;
        if (status != kStatusInfoLengthMismatch) {
            error = L"读取系统句柄表失败，NTSTATUS=" +
                std::to_wstring(static_cast<unsigned long>(status));
            return false;
        }
        const ULONG grown = needed > size ? needed + (1u << 20) : size * 2;
        if (grown <= size) break;
        size = grown;
    }
    error = L"系统句柄表过大，已跳过目录句柄检查";
    return false;
}

HandleScanResult QueryDirectoryHandles(
    const std::wstring& targetPath,
    const FileLockCancelCheck& shouldCancel,
    const FileLockProgressCallback& reportProgress,
    const FileLockQueryOptions& options) {
    HandleScanResult result;
    if (IsCancelled(shouldCancel)) {
        result.cancelled = true;
        return result;
    }
    const std::wstring comparableTarget = AbsoluteComparablePath(targetPath);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = ntdll
        ? reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"))
        : nullptr;
    if (!query) {
        result.supported = false;
        result.warning = L"当前系统不支持目录句柄检查";
        return result;
    }

    HANDLE targetHandle = CreateFileW(
        targetPath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (targetHandle == INVALID_HANDLE_VALUE) {
        result.supported = false;
        result.warning = L"无法打开目标目录进行句柄检查：" + FormatLastError(GetLastError());
        return result;
    }

    std::vector<unsigned char> snapshot;
    std::wstring snapshotError;
    if (!QuerySystemHandleSnapshot(query, snapshot, snapshotError)) {
        CloseHandle(targetHandle);
        result.supported = false;
        result.warning = std::move(snapshotError);
        return result;
    }
    if (IsCancelled(shouldCancel)) {
        CloseHandle(targetHandle);
        result.cancelled = true;
        return result;
    }

    const auto* information = reinterpret_cast<const SystemHandleInformation*>(snapshot.data());
    const ULONG_PTR ownPid = static_cast<ULONG_PTR>(GetCurrentProcessId());
    const ULONG_PTR ownHandle = reinterpret_cast<ULONG_PTR>(targetHandle);
    USHORT fileTypeIndex = 0;
    for (ULONG_PTR index = 0; index < information->handleCount; ++index) {
        const SystemHandleEntry& entry = information->handles[index];
        if (entry.processId == ownPid && entry.handleValue == ownHandle) {
            fileTypeIndex = entry.objectTypeIndex;
            break;
        }
    }
    CloseHandle(targetHandle);
    if (fileTypeIndex == 0) {
        result.supported = false;
        result.warning = L"无法识别系统文件句柄类型，已跳过目录句柄检查";
        return result;
    }

    std::map<DWORD, std::vector<ULONG_PTR>> handlesByProcess;
    for (ULONG_PTR index = 0; index < information->handleCount; ++index) {
        const SystemHandleEntry& entry = information->handles[index];
        if (entry.objectTypeIndex != fileTypeIndex || entry.processId == ownPid ||
            entry.processId == 0 || entry.processId > MAXDWORD) {
            continue;
        }
        const DWORD pid = static_cast<DWORD>(entry.processId);
        handlesByProcess[pid].push_back(entry.handleValue);
    }

    DWORD currentSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSession);
    std::vector<std::pair<DWORD, std::vector<ULONG_PTR>>> processes;
    processes.reserve(handlesByProcess.size());
    for (auto& entry : handlesByProcess) {
        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.first, &session) || session != currentSession) {
            continue;
        }
        processes.push_back(std::move(entry));
    }
    result.totalProcesses = processes.size();
    if (reportProgress) {
        FileLockQueryProgress progress{};
        progress.phase = FileLockQueryPhase::ScanningHandles;
        progress.totalProcesses = result.totalProcesses;
        reportProgress(progress);
    }
    if (processes.empty()) return result;

    const std::size_t hardware = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const std::size_t configured = options.maxHandleWorkers == 0
        ? hardware
        : std::max<std::size_t>(1, options.maxHandleWorkers);
    const std::size_t workerCount = std::min({processes.size(), hardware, configured});
    std::atomic_size_t nextProcess{0};
    std::atomic_size_t checkedProcesses{0};
    std::atomic_size_t inaccessibleProcesses{0};
    std::mutex resultMutex;
    std::mutex progressMutex;
    std::set<std::tuple<unsigned long, std::wstring, bool>> evidenceKeys;

    auto worker = [&]() {
        while (!IsCancelled(shouldCancel)) {
            const std::size_t index = nextProcess.fetch_add(1);
            if (index >= processes.size()) break;
            const DWORD pid = processes[index].first;
            HANDLE process = OpenProcess(
                PROCESS_DUP_HANDLE,
                FALSE,
                pid);
            if (!process) {
                if (GetLastError() == ERROR_ACCESS_DENIED) {
                    inaccessibleProcesses.fetch_add(1);
                }
            } else {
                for (ULONG_PTR handleValue : processes[index].second) {
                    if (IsCancelled(shouldCancel)) break;
                    HANDLE duplicate = nullptr;
                    if (!DuplicateHandle(
                            process,
                            reinterpret_cast<HANDLE>(handleValue),
                            GetCurrentProcess(),
                            &duplicate,
                            0,
                            FALSE,
                            DUPLICATE_SAME_ACCESS)) {
                        continue;
                    }
                    if (GetFileType(duplicate) == FILE_TYPE_DISK) {
                        const std::wstring path = FinalPathForHandle(duplicate);
                        if (!path.empty() && PathIsTargetOrDescendant(path, comparableTarget)) {
                            FILE_STANDARD_INFO standard{};
                            const bool directory = GetFileInformationByHandleEx(
                                duplicate,
                                FileStandardInfo,
                                &standard,
                                sizeof(standard)) && standard.Directory;
                            std::lock_guard lock(resultMutex);
                            const auto key = std::make_tuple(
                                static_cast<unsigned long>(pid), path, directory);
                            if (evidenceKeys.insert(key).second) {
                                result.evidence.push_back(FileLockEvidence{
                                    static_cast<unsigned long>(pid),
                                    FileLockSource::SystemHandle,
                                    path,
                                    directory});
                            }
                        }
                    }
                    CloseHandle(duplicate);
                }
                CloseHandle(process);
            }

            const std::size_t checked = checkedProcesses.fetch_add(1) + 1;
            if (reportProgress) {
                std::lock_guard lock(progressMutex);
                FileLockQueryProgress progress{};
                progress.phase = FileLockQueryPhase::ScanningHandles;
                progress.checkedProcesses = checked;
                progress.totalProcesses = result.totalProcesses;
                progress.inaccessibleProcesses = inaccessibleProcesses.load();
                progress.workerCount = workerCount;
                reportProgress(progress);
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }
    for (std::thread& thread : workers) thread.join();

    result.checkedProcesses = std::min(checkedProcesses.load(), result.totalProcesses);
    result.inaccessibleProcesses = inaccessibleProcesses.load();
    result.cancelled = IsCancelled(shouldCancel) &&
        result.checkedProcesses < result.totalProcesses;
    if (result.inaccessibleProcesses > 0) {
        result.warning = L"有 " + std::to_wstring(result.inaccessibleProcesses) +
            L" 个高权限进程无法检查，结果可能不完整";
    }
    return result;
}

BatchResult QueryBatch(const std::vector<std::wstring>& resources, std::size_t begin, std::size_t end) {
    BatchResult batch;
    DWORD session = 0;
    WCHAR sessionKey[CCH_RM_SESSION_KEY + 1]{};
    DWORD result = RmStartSession(&session, 0, sessionKey);
    if (result != ERROR_SUCCESS) {
        batch.error = L"启动文件占用检查失败：" + FormatLastError(result);
        return batch;
    }

    std::vector<LPCWSTR> pointers;
    pointers.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        pointers.push_back(resources[index].c_str());
    }

    result = RmRegisterResources(
        session,
        static_cast<UINT>(pointers.size()),
        pointers.data(),
        0,
        nullptr,
        0,
        nullptr);
    if (result != ERROR_SUCCESS) {
        RmEndSession(session);
        batch.error = L"注册待检查路径失败：" + FormatLastError(result);
        return batch;
    }

    UINT needed = 0;
    UINT count = 0;
    DWORD rebootReasons = 0;
    result = RmGetList(session, &needed, &count, nullptr, &rebootReasons);
    std::vector<RM_PROCESS_INFO> affected;
    if (result == ERROR_MORE_DATA && needed > 0) {
        affected.resize(needed);
        count = needed;
        result = RmGetList(session, &needed, &count, affected.data(), &rebootReasons);
    }
    RmEndSession(session);

    if (result != ERROR_SUCCESS) {
        batch.error = L"读取占用进程失败：" + FormatLastError(result);
        return batch;
    }

    batch.processIds.reserve(count);
    for (DWORD index = 0; index < count && index < affected.size(); ++index) {
        const DWORD pid = affected[index].Process.dwProcessId;
        if (pid != 0) {
            batch.processIds.push_back(pid);
        }
    }
    return batch;
}

std::size_t ResolveWorkerCount(std::size_t totalPaths, const FileLockQueryOptions& options, bool directory) {
    const std::size_t batchSize = std::max<std::size_t>(1, options.batchSize);
    const std::size_t batchCount = (totalPaths + batchSize - 1) / batchSize;
    if (batchCount == 0) {
        return 0;
    }
    const std::size_t hardware = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const std::size_t configured = options.maxWorkers == 0 ? hardware : options.maxWorkers;
    const std::size_t desired = directory ? std::max<std::size_t>(1, configured) : 1;
    return std::max<std::size_t>(1, std::min({batchCount, hardware, desired}));
}
}

FileLockQueryResult QueryFileLocks(
    const std::wstring& rawPath,
    const FileLockCancelCheck& shouldCancel,
    const FileLockProgressCallback& reportProgress,
    FileLockQueryOptions options) {
    FileLockQueryResult result;
    if (reportProgress) {
        reportProgress(FileLockQueryProgress{FileLockQueryPhase::Validating});
    }

    const DWORD attributes = GetFileAttributesW(rawPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        result.error = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
            ? L"路径不存在。"
            : L"读取路径属性失败：" + FormatLastError(code);
        return result;
    }

    result.directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::vector<std::wstring> resources;
    if (!result.directory) {
        resources.push_back(std::filesystem::path(rawPath).wstring());
    } else {
        resources.push_back(DirectoryResourcePath(std::filesystem::path(rawPath).wstring()));
        if (reportProgress) {
            reportProgress(FileLockQueryProgress{
                FileLockQueryPhase::Enumerating,
                resources.size(),
                0,
                0,
                0});
        }

        std::error_code ec;
        std::filesystem::recursive_directory_iterator iterator(
            std::filesystem::path(rawPath),
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        const std::filesystem::recursive_directory_iterator end;
        std::size_t lastReported = 0;
        while (!ec && iterator != end) {
            if (IsCancelled(shouldCancel)) {
                result.cancelled = true;
                result.totalPaths = resources.size();
                if (reportProgress) {
                    reportProgress(FileLockQueryProgress{
                        FileLockQueryPhase::Cancelled,
                        resources.size(),
                        0,
                        resources.size(),
                        0});
                }
                return result;
            }
            const std::filesystem::directory_entry& entry = *iterator;
            std::error_code entryError;
            if (entry.is_regular_file(entryError)) {
                resources.push_back(entry.path().wstring());
                if (reportProgress && resources.size() - lastReported >= 64) {
                    lastReported = resources.size();
                    reportProgress(FileLockQueryProgress{
                        FileLockQueryPhase::Enumerating,
                        resources.size(),
                        0,
                        0,
                        0});
                }
            }
            iterator.increment(ec);
        }
        if (ec) {
            result.warning = resources.size() <= 1
                ? L"目录不可枚举，仅检查目录本身：" + Utf8ToWide(ec.message())
                : L"目录部分内容不可枚举，已检查可读取的路径：" + Utf8ToWide(ec.message());
        }
    }

    result.totalPaths = resources.size();
    if (resources.empty()) {
        result.error = L"没有可检查的文件。";
        return result;
    }
    if (IsCancelled(shouldCancel)) {
        result.cancelled = true;
        if (reportProgress) {
            reportProgress(FileLockQueryProgress{
                FileLockQueryPhase::Cancelled,
                result.totalPaths,
                0,
                result.totalPaths,
                0});
        }
        return result;
    }

    options.batchSize = std::max<std::size_t>(1, options.batchSize);
    result.workerCount = ResolveWorkerCount(resources.size(), options, result.directory);
    if (reportProgress) {
        reportProgress(FileLockQueryProgress{
            FileLockQueryPhase::Querying,
            result.totalPaths,
            0,
            result.totalPaths,
            result.workerCount});
    }

    std::atomic_size_t nextIndex{0};
    std::atomic_size_t checkedPaths{0};
    std::mutex resultMutex;
    std::mutex progressMutex;
    std::set<unsigned long> processIds;
    std::wstring firstBatchError;
    std::size_t failedBatchCount = 0;

    auto worker = [&]() {
        while (!IsCancelled(shouldCancel)) {
            const std::size_t begin = nextIndex.fetch_add(options.batchSize);
            if (begin >= resources.size()) {
                break;
            }
            const std::size_t finish = std::min(resources.size(), begin + options.batchSize);
            if (options.batchDelay.count() > 0) {
                std::this_thread::sleep_for(options.batchDelay);
            }
            if (IsCancelled(shouldCancel)) {
                break;
            }

            BatchResult batch = QueryBatch(resources, begin, finish);
            {
                std::lock_guard lock(resultMutex);
                processIds.insert(batch.processIds.begin(), batch.processIds.end());
                if (!batch.error.empty()) {
                    if (firstBatchError.empty()) {
                        firstBatchError = batch.error;
                    }
                    ++failedBatchCount;
                }
            }
            checkedPaths.fetch_add(finish - begin);
            if (reportProgress) {
                std::lock_guard progressLock(progressMutex);
                reportProgress(FileLockQueryProgress{
                    FileLockQueryPhase::Querying,
                    result.totalPaths,
                    checkedPaths.load(),
                    result.totalPaths,
                    result.workerCount});
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(result.workerCount);
    for (std::size_t index = 0; index < result.workerCount; ++index) {
        workers.emplace_back(worker);
    }
    for (std::thread& thread : workers) {
        thread.join();
    }

    result.checkedPaths = std::min(checkedPaths.load(), result.totalPaths);
    result.cancelled = IsCancelled(shouldCancel) && result.checkedPaths < result.totalPaths;
    result.processIds.assign(processIds.begin(), processIds.end());
    for (unsigned long pid : result.processIds) {
        result.evidence.push_back(FileLockEvidence{
            pid,
            FileLockSource::RestartManager,
            {},
            false});
    }
    if (failedBatchCount > 0) {
        const std::wstring queryWarning = L"有 " + std::to_wstring(failedBatchCount) +
            L" 个查询批次失败，首个错误：" + firstBatchError;
        AppendWarning(result.warning, queryWarning);
    }

    if (!result.cancelled && result.directory) {
        HandleScanResult handles = QueryDirectoryHandles(
            std::filesystem::path(rawPath).wstring(),
            shouldCancel,
            reportProgress,
            options);
        result.checkedProcesses = handles.checkedProcesses;
        result.totalProcesses = handles.totalProcesses;
        result.inaccessibleProcesses = handles.inaccessibleProcesses;
        result.handleScanSupported = handles.supported;
        result.cancelled = handles.cancelled;
        AppendWarning(result.warning, handles.warning);
        result.evidence.insert(
            result.evidence.end(),
            std::make_move_iterator(handles.evidence.begin()),
            std::make_move_iterator(handles.evidence.end()));
        for (const FileLockEvidence& evidence : result.evidence) {
            if (evidence.processId != 0) processIds.insert(evidence.processId);
        }
        result.processIds.assign(processIds.begin(), processIds.end());
    }

    if (reportProgress) {
        FileLockQueryProgress progress{};
        progress.phase = result.cancelled
            ? FileLockQueryPhase::Cancelled
            : FileLockQueryPhase::Completed;
        progress.discoveredPaths = result.totalPaths;
        progress.checkedPaths = result.checkedPaths;
        progress.totalPaths = result.totalPaths;
        progress.workerCount = result.workerCount;
        progress.checkedProcesses = result.checkedProcesses;
        progress.totalProcesses = result.totalProcesses;
        progress.inaccessibleProcesses = result.inaccessibleProcesses;
        reportProgress(progress);
    }
    return result;
}
