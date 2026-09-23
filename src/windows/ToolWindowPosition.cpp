#include "ToolWindowPosition.h"

#include "../common/Utilities.h"
#include "../theme/ThemedWindowUi.h"

#include <filesystem>

namespace {
std::filesystem::path StatePath() {
    return QuattroUserConfigDirectory() / L"tool-window-state.ini";
}
}

std::optional<POINT> LoadToolWindowPosition(const std::wstring& toolId, int width, int height) {
    const auto path = StatePath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::nullopt;
    wchar_t xBuffer[32]{};
    wchar_t yBuffer[32]{};
    GetPrivateProfileStringW(toolId.c_str(), L"x", L"", xBuffer, _countof(xBuffer), path.c_str());
    GetPrivateProfileStringW(toolId.c_str(), L"y", L"", yBuffer, _countof(yBuffer), path.c_str());
    const auto x = ParseInt(xBuffer);
    const auto y = ParseInt(yBuffer);
    return x && y ? ThemedWindowUi::RestoredWindowPosition(*x, *y, width, height) : std::nullopt;
}

void SaveToolWindowPosition(const std::wstring& toolId, HWND hwnd) {
    if (toolId.empty() || !hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return;
    const auto path = StatePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    WritePrivateProfileStringW(toolId.c_str(), L"version", L"1", path.c_str());
    WritePrivateProfileStringW(toolId.c_str(), L"x", std::to_wstring(rect.left).c_str(), path.c_str());
    WritePrivateProfileStringW(toolId.c_str(), L"y", std::to_wstring(rect.top).c_str(), path.c_str());
}
