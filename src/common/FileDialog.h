#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

enum class CommonFileDialogKind {
    OpenFile,
    PickFolder,
};

struct CommonFileDialogOptions {
    HWND owner = nullptr;
    CommonFileDialogKind kind = CommonFileDialogKind::OpenFile;
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
    HRESULT dialogResult = S_FALSE;
    long long elapsedMs = 0;
};

std::filesystem::path ResolveCommonFileDialogInitialDirectory(const std::wstring& defaultPath);
std::wstring CommonFileDialogKindName(CommonFileDialogKind kind);
bool ShowCommonFileDialog(const CommonFileDialogOptions& options, CommonFileDialogResult& result);

