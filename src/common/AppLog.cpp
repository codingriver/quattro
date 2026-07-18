#include "AppLog.h"

#include <windows.h>

#include <fstream>
#include <sstream>

namespace {
std::filesystem::path g_logPath;
LARGE_INTEGER g_startCounter{};
LARGE_INTEGER g_lastCounter{};
LARGE_INTEGER g_counterFrequency{};
bool g_startupTimingActive = false;
bool g_logEnabled = true;

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
}

void InitializeAppLog(const std::filesystem::path& appDirectory, bool enabled) {
    g_logEnabled = enabled;
    g_logPath = appDirectory / L"logs" / L"app.log";
    if (!g_logEnabled) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(g_logPath.parent_path(), ec);
}

void SetAppLogEnabled(bool enabled) {
    g_logEnabled = enabled;
    if (g_logEnabled && !g_logPath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_logPath.parent_path(), ec);
    }
}

bool IsAppLogEnabled() {
    return g_logEnabled;
}

void WriteAppLog(const std::wstring& message) {
    if (!g_logEnabled || g_logPath.empty()) {
        return;
    }
    std::ofstream file(g_logPath, std::ios::binary | std::ios::app);
    if (!file) {
        return;
    }
    file << WideToUtf8(Timestamp() + L" " + message + L"\n");
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
    if (!g_logEnabled || !g_startupTimingActive) {
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
