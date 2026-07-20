#include "AppLog.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {
LARGE_INTEGER g_startCounter{};
LARGE_INTEGER g_lastCounter{};
LARGE_INTEGER g_counterFrequency{};
bool g_startupTimingActive = false;

long long ElapsedMilliseconds(LARGE_INTEGER start, LARGE_INTEGER end) {
    if (g_counterFrequency.QuadPart <= 0) {
        return 0;
    }
    return (end.QuadPart - start.QuadPart) * 1000LL / g_counterFrequency.QuadPart;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), output.data(), length, nullptr, nullptr);
    return output;
}

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    return buffer;
}

enum class AppLogCommandType {
    Line,
    Flush,
    Close,
    Stop,
};

struct AppLogCommand {
    AppLogCommandType type = AppLogCommandType::Line;
    std::string line;
    std::shared_ptr<std::promise<void>> completion;
};

class AsyncAppLogger final {
public:
    ~AsyncAppLogger() {
        Shutdown();
    }

    void Initialize(const std::filesystem::path& appDirectory, bool enabled) {
        Shutdown();

        const std::filesystem::path path = appDirectory / L"logs" / L"app.log";
        if (enabled) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        std::lock_guard lock(mutex_);
        path_ = path;
        enabled_.store(enabled, std::memory_order_release);
        if (enabled) {
            StartWorkerLocked();
        }
    }

    void SetEnabled(bool enabled) {
        std::filesystem::path path;
        {
            std::lock_guard lock(mutex_);
            path = path_;
        }
        if (enabled && !path.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        {
            std::lock_guard lock(mutex_);
            enabled_.store(enabled, std::memory_order_release);
            if (enabled) {
                StartWorkerLocked();
            } else if (worker_.joinable()) {
                commands_.push_back(AppLogCommand{AppLogCommandType::Close});
            }
        }
        condition_.notify_one();
    }

    bool IsEnabled() const {
        return enabled_.load(std::memory_order_acquire);
    }

    void Write(const std::wstring& message) {
        if (!IsEnabled()) {
            return;
        }

        AppLogCommand command;
        command.type = AppLogCommandType::Line;
        command.line = WideToUtf8(Timestamp() + L" " + message + L"\n");

        {
            std::lock_guard lock(mutex_);
            if (!enabled_.load(std::memory_order_relaxed) || path_.empty() || !StartWorkerLocked()) {
                return;
            }
            commands_.push_back(std::move(command));
        }
        condition_.notify_one();
    }

    void Flush() {
        std::shared_ptr<std::promise<void>> completion;
        std::future<void> completed;
        {
            std::lock_guard lock(mutex_);
            if (!worker_.joinable()) {
                return;
            }
            completion = std::make_shared<std::promise<void>>();
            completed = completion->get_future();
            commands_.push_back(AppLogCommand{AppLogCommandType::Flush, {}, completion});
        }
        condition_.notify_one();
        completed.wait();
    }

    void Shutdown() {
        std::thread worker;
        {
            std::lock_guard lock(mutex_);
            enabled_.store(false, std::memory_order_release);
            if (!worker_.joinable()) {
                commands_.clear();
                return;
            }
            commands_.push_back(AppLogCommand{AppLogCommandType::Stop});
            worker = std::move(worker_);
        }
        condition_.notify_one();
        worker.join();

        std::lock_guard lock(mutex_);
        commands_.clear();
    }

private:
    bool StartWorkerLocked() {
        if (worker_.joinable()) {
            return true;
        }
        try {
            worker_ = std::thread([this]() { Run(); });
            return true;
        } catch (...) {
            enabled_.store(false, std::memory_order_release);
            return false;
        }
    }

    HANDLE OpenFile() const {
        std::filesystem::path path;
        {
            std::lock_guard lock(mutex_);
            path = path_;
        }
        if (path.empty()) {
            return INVALID_HANDLE_VALUE;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        return CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }

    static bool WriteAll(HANDLE file, const std::string& data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const std::size_t remaining = data.size() - offset;
            const DWORD requested = static_cast<DWORD>(
                (std::min)(remaining, static_cast<std::size_t>(MAXDWORD)));
            DWORD written = 0;
            if (!WriteFile(file, data.data() + offset, requested, &written, nullptr) || written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    static void FlushAndClose(HANDLE& file) {
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        FlushFileBuffers(file);
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }

    void Run() {
        HANDLE file = INVALID_HANDLE_VALUE;
        for (;;) {
            AppLogCommand command;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return !commands_.empty(); });
                command = std::move(commands_.front());
                commands_.pop_front();
            }

            if (command.type == AppLogCommandType::Line) {
                std::vector<std::string> lines;
                lines.push_back(std::move(command.line));
                std::size_t byteCount = lines.front().size();
                {
                    std::lock_guard lock(mutex_);
                    while (!commands_.empty() &&
                           commands_.front().type == AppLogCommandType::Line &&
                           byteCount < 64 * 1024) {
                        byteCount += commands_.front().line.size();
                        lines.push_back(std::move(commands_.front().line));
                        commands_.pop_front();
                    }
                }

                std::string batch;
                batch.reserve(byteCount);
                for (const auto& line : lines) {
                    batch += line;
                }
                if (file == INVALID_HANDLE_VALUE) {
                    file = OpenFile();
                }
                if (file != INVALID_HANDLE_VALUE && !WriteAll(file, batch)) {
                    CloseHandle(file);
                    file = INVALID_HANDLE_VALUE;
                }
                continue;
            }

            if (command.type == AppLogCommandType::Flush) {
                if (file != INVALID_HANDLE_VALUE) {
                    FlushFileBuffers(file);
                }
                if (command.completion) {
                    command.completion->set_value();
                }
                continue;
            }

            if (command.type == AppLogCommandType::Close) {
                FlushAndClose(file);
                continue;
            }

            FlushAndClose(file);
            return;
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<AppLogCommand> commands_;
    std::thread worker_;
    std::filesystem::path path_;
    std::atomic_bool enabled_{true};
};

AsyncAppLogger& AppLogger() {
    static AsyncAppLogger logger;
    return logger;
}
}

void InitializeAppLog(const std::filesystem::path& appDirectory, bool enabled) {
    AppLogger().Initialize(appDirectory, enabled);
}

void SetAppLogEnabled(bool enabled) {
    AppLogger().SetEnabled(enabled);
}

bool IsAppLogEnabled() {
    return AppLogger().IsEnabled();
}

void WriteAppLog(const std::wstring& message) {
    AppLogger().Write(message);
}

void FlushAppLog() {
    AppLogger().Flush();
}

void ShutdownAppLog() {
    AppLogger().Shutdown();
}

void ResetStartupTiming() {
    QueryPerformanceFrequency(&g_counterFrequency);
    QueryPerformanceCounter(&g_startCounter);
    g_lastCounter = g_startCounter;
    g_startupTimingActive = true;
}

void FinishStartupTiming() {
    g_startupTimingActive = false;
}

bool IsStartupTimingActive() {
    return g_startupTimingActive;
}

void WriteStartupTiming(const std::wstring& stage) {
    WriteStartupTiming(stage, L"");
}

void WriteStartupTiming(const std::wstring& stage, const std::wstring& detail) {
    if (!IsAppLogEnabled() || !g_startupTimingActive) {
        return;
    }
    if (g_startCounter.QuadPart == 0 || g_lastCounter.QuadPart == 0 || g_counterFrequency.QuadPart == 0) {
        ResetStartupTiming();
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const long long totalMs = ElapsedMilliseconds(g_startCounter, now);
    const long long deltaMs = ElapsedMilliseconds(g_lastCounter, now);
    g_lastCounter = now;

    std::wstringstream message;
    message << L"[startup] +" << totalMs << L"ms";
    message << L" delta=" << deltaMs << L"ms ";
    message << stage;
    if (!detail.empty()) {
        message << L" | " << detail;
    }
    WriteAppLog(message.str());
}
