#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

enum class SelectedPathCopyStatus {
    Success,
    UnsupportedForegroundWindow,
    NoSelection,
    NonFileSystemItem,
    ShellUnavailable,
    ClipboardUnavailable,
};

struct SelectedPathCopyResult {
    SelectedPathCopyStatus status = SelectedPathCopyStatus::ShellUnavailable;
    std::size_t copiedCount = 0;
    std::wstring detail;
};

class SelectedPathCopyService {
public:
    static std::wstring FormatPaths(const std::vector<std::wstring>& paths);
    static bool WriteClipboardText(HWND owner, const std::wstring& text, std::wstring& errorMessage);
    static SelectedPathCopyResult CopySelectedPaths(HWND foregroundWindow, HWND clipboardOwner);
};
