#include "AppLog.h"

#include <windows.h>

#include <fstream>

namespace {
std::filesystem::path g_logPath;

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
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}
}

void InitializeAppLog(const std::filesystem::path& appDirectory) {
    g_logPath = appDirectory / L"logs" / L"app.log";
    std::error_code ec;
    std::filesystem::create_directories(g_logPath.parent_path(), ec);
}

void WriteAppLog(const std::wstring& message) {
    if (g_logPath.empty()) {
        return;
    }
    std::ofstream file(g_logPath, std::ios::binary | std::ios::app);
    if (!file) {
        return;
    }
    file << WideToUtf8(Timestamp() + L" " + message + L"\n");
}
