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

enum class CommonPathPickerKind {
    File,
    Folder,
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

struct CommonPathPickerDialogOptions {
    HWND owner = nullptr;
    std::wstring defaultPath;
    std::wstring fileContext;
    std::wstring folderContext;
    std::wstring fileTitle;
    std::wstring folderTitle;
    const wchar_t* fileFilter = L"所有文件\0*.*\0";
    bool folderForceFileSystem = true;
    bool folderAllowShellFolderParsingName = false;
};

struct CommonPathPickerResult {
    CommonPathPickerKind kind = CommonPathPickerKind::File;
    CommonFileDialogResult dialog;
};

std::filesystem::path ResolveCommonFileDialogInitialDirectory(const std::wstring& defaultPath);
std::wstring CommonFileDialogModeName(CommonFileDialogMode mode);
bool CommonFileDialogSupportsNativeMode(CommonFileDialogMode mode);
CommonFileDialogOptions BuildCommonPathPickerDialogOptions(
    CommonPathPickerKind kind,
    const CommonPathPickerDialogOptions& options);
bool ShowCommonPathPickerDialog(
    CommonPathPickerKind kind,
    const CommonPathPickerDialogOptions& options,
    CommonPathPickerResult& result);
bool ShowCommonFileDialog(const CommonFileDialogOptions& options, CommonFileDialogResult& result);
