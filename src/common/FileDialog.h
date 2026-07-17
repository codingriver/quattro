#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

enum class CommonFileDialogMode {
    FileOnly,
    FolderOnly,
    FileOrFolder,
};

struct CommonFileDialogOptions {
    HWND owner = nullptr;
    CommonFileDialogMode mode = CommonFileDialogMode::FileOnly;
    bool allowMultiSelect = false;
    std::wstring context;
    std::wstring title;
    std::wstring defaultPath;
    const wchar_t* legacyFilter = nullptr;
    std::wstring defaultExtension;
    bool forceFileSystem = true;
    bool pathMustExist = true;
    bool fileMustExist = true;
    bool allowShellFolderParsingName = false;
};

struct CommonFileDialogResult {
    bool accepted = false;
    std::wstring path;
    std::wstring displayName;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> displayNames;
    HRESULT dialogResult = S_FALSE;
    long long elapsedMs = 0;
};

std::filesystem::path ResolveCommonFileDialogInitialDirectory(const std::wstring& defaultPath);
std::wstring CommonFileDialogModeName(CommonFileDialogMode mode);
bool CommonFileDialogSupportsNativeMode(CommonFileDialogMode mode);
bool ShowCommonFileDialog(const CommonFileDialogOptions& options, CommonFileDialogResult& result);
