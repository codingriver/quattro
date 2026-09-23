#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#ifdef CreateFile
#undef CreateFile
#endif

enum class FileHelperStatus {
    Success,
    AlreadyExists,
    Failed,
};

struct FileHelperResult {
    FileHelperStatus status = FileHelperStatus::Failed;
    std::filesystem::path path;
    std::wstring message;
};

class FileHelperDialog;

class FileHelperService {
public:
    static constexpr std::size_t HistoryLimit = 15;

    explicit FileHelperService(std::filesystem::path historyPath = {});

    FileHelperResult OpenFile(HWND owner, std::wstring_view input) const;
    FileHelperResult OpenFolder(HWND owner, std::wstring_view input) const;
    FileHelperResult CreateFile(std::wstring_view input, bool overwrite) const;
    FileHelperResult CreateFolder(std::wstring_view input) const;
    FileHelperResult OpenContainingLocation(HWND owner, std::wstring_view input) const;
    std::vector<std::filesystem::path> LoadHistory() const;
    bool RememberPath(const std::filesystem::path& path) const;
    const std::filesystem::path& historyPath() const noexcept { return historyPath_; }

private:
    friend class FileHelperDialog;

    static FileHelperResult ResolvePath(std::wstring_view input);
    static bool IsExistingPath(std::wstring_view input);

    std::filesystem::path historyPath_;
};
