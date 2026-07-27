#include "../src/common/FileDialog.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <iostream>

namespace {
bool Require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        return false;
    }
    return true;
}
}

int wmain() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / L"quattro-file-dialog-tests";
    const std::filesystem::path child = root / L"child";
    const std::filesystem::path file = child / L"sample.txt";

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(child, ec);
    bool ok = Require(!ec, L"test directory should be created");

    {
        FILE* handle = nullptr;
        _wfopen_s(&handle, file.c_str(), L"wb");
        if (handle) {
            fputs("sample", handle);
            fclose(handle);
        }
    }

    ok = Require(ResolveCommonFileDialogInitialDirectory(child.wstring()) == child,
                 L"existing directory should be used directly") && ok;
    ok = Require(ResolveCommonFileDialogInitialDirectory(file.wstring()) == child,
                 L"existing file should use its parent directory") && ok;
    ok = Require(ResolveCommonFileDialogInitialDirectory((child / L"missing" / L"target.txt").wstring()) == child,
                 L"missing path should walk up to nearest existing parent") && ok;

    SetEnvironmentVariableW(L"QUATTRO_FILE_DIALOG_TEST_ROOT", child.c_str());
    ok = Require(ResolveCommonFileDialogInitialDirectory(L"%QUATTRO_FILE_DIALOG_TEST_ROOT%\\sample.txt") == child,
                 L"environment variables should be expanded before resolving") && ok;
    SetEnvironmentVariableW(L"QUATTRO_FILE_DIALOG_TEST_ROOT", nullptr);

    ok = Require(CommonFileDialogModeName(CommonFileDialogMode::FileOnly) == L"file",
                 L"file-only mode should have stable log name") && ok;
    ok = Require(CommonFileDialogModeName(CommonFileDialogMode::FolderOnly) == L"folder",
                 L"folder-only mode should have stable log name") && ok;
    ok = Require(CommonFileDialogModeName(CommonFileDialogMode::FileOrFolder) == L"file-or-folder",
                 L"file-or-folder mode should have stable log name") && ok;
    ok = Require(CommonFileDialogSupportsNativeMode(CommonFileDialogMode::FileOnly),
                 L"file-only mode should be supported by native dialog") && ok;
    ok = Require(CommonFileDialogSupportsNativeMode(CommonFileDialogMode::FolderOnly),
                 L"folder-only mode should be supported by native dialog") && ok;
    ok = Require(!CommonFileDialogSupportsNativeMode(CommonFileDialogMode::FileOrFolder),
                 L"file-or-folder mode should not claim native mixed selection support") && ok;

    CommonFileDialogOptions unsupportedOptions{};
    unsupportedOptions.mode = CommonFileDialogMode::FileOrFolder;
    unsupportedOptions.allowMultiSelect = true;
    unsupportedOptions.context = L"自动化不打开真实选择器";
    CommonFileDialogResult unsupportedResult{};
    ok = Require(!ShowCommonFileDialog(unsupportedOptions, unsupportedResult),
                 L"unsupported mixed file-or-folder mode should fail without opening a dialog") && ok;
    ok = Require(unsupportedResult.dialogResult == E_NOTIMPL,
                 L"unsupported mixed file-or-folder mode should return E_NOTIMPL") && ok;

    std::filesystem::remove_all(root, ec);
    if (!ok) {
        return 1;
    }
    std::wcout << L"File dialog tests passed.\n";
    return 0;
}
