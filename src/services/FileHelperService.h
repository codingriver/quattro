#pragma once

#include <filesystem>
#include <string>
#include <string_view>
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
    FileHelperResult OpenFile(HWND owner, std::wstring_view input) const;
    FileHelperResult OpenFolder(HWND owner, std::wstring_view input) const;
    FileHelperResult CreateFile(std::wstring_view input, bool overwrite) const;
    FileHelperResult CreateFolder(std::wstring_view input) const;
    FileHelperResult OpenContainingLocation(HWND owner, std::wstring_view input) const;

private:
    friend class FileHelperDialog;

    static FileHelperResult ResolvePath(std::wstring_view input);
    static bool IsExistingPath(std::wstring_view input);
};
