#include "FileDialog.h"

#include "AppLog.h"
#include "Utilities.h"

#include <shobjidl.h>

#include <sstream>
#include <system_error>
#include <vector>
#include <cwchar>

namespace {
struct ComReleaser {
    template <typename T>
    void operator()(T* value) const {
        if (value) value->Release();
    }
};

std::wstring HResultText(HRESULT hr) {
    std::wstringstream stream;
    stream << L"0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

long long ElapsedMilliseconds(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency) {
    if (frequency.QuadPart <= 0) {
        return 0;
    }
    return (end.QuadPart - start.QuadPart) * 1000LL / frequency.QuadPart;
}

bool ExistingDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_directory(path, ec);
}

bool ExistingRegularPath(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

std::vector<COMDLG_FILTERSPEC> ParseLegacyFilter(const wchar_t* legacyFilter, std::vector<std::wstring>& storage) {
    std::vector<std::pair<std::wstring, std::wstring>> parsed;
    if (!legacyFilter || !*legacyFilter) {
        return {};
    }

    const wchar_t* cursor = legacyFilter;
    while (*cursor) {
        const wchar_t* name = cursor;
        const std::size_t nameLength = wcslen(name);
        cursor += nameLength + 1;
        if (!*cursor) {
            break;
        }
        const wchar_t* spec = cursor;
        const std::size_t specLength = wcslen(spec);
        cursor += specLength + 1;

        parsed.emplace_back(std::wstring(name, nameLength), std::wstring(spec, specLength));
    }

    storage.reserve(parsed.size() * 2);
    std::vector<COMDLG_FILTERSPEC> filters;
    filters.reserve(parsed.size());
    for (const auto& [name, spec] : parsed) {
        storage.push_back(name);
        storage.push_back(spec);
        filters.push_back(COMDLG_FILTERSPEC{storage[storage.size() - 2].c_str(), storage[storage.size() - 1].c_str()});
    }
    return filters;
}

void SetDefaultFolder(IFileDialog* dialog, const std::filesystem::path& initialDirectory) {
    if (!dialog || initialDirectory.empty()) {
        return;
    }

    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(initialDirectory.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
        dialog->SetDefaultFolder(folder);
        dialog->SetFolder(folder);
        folder->Release();
    }
}

std::wstring DialogLogPrefix(const CommonFileDialogOptions& options) {
    return L"文件选择器[" + (options.context.empty() ? L"未命名" : options.context) + L"]";
}
}

std::filesystem::path ResolveCommonFileDialogInitialDirectory(const std::wstring& defaultPath) {
    const std::wstring trimmed = Trim(defaultPath);
    if (trimmed.empty()) {
        return {};
    }

    const std::filesystem::path path(ExpandEnvironmentStringsSafe(trimmed));
    if (ExistingDirectory(path)) {
        return path;
    }
    if (ExistingRegularPath(path) && ExistingDirectory(path.parent_path())) {
        return path.parent_path();
    }

    std::filesystem::path parent = path.parent_path();
    while (!parent.empty()) {
        if (ExistingDirectory(parent)) {
            return parent;
        }
        const std::filesystem::path next = parent.parent_path();
        if (next == parent) {
            break;
        }
        parent = next;
    }
    return {};
}

std::wstring CommonFileDialogKindName(CommonFileDialogKind kind) {
    switch (kind) {
    case CommonFileDialogKind::PickFolder:
        return L"folder";
    case CommonFileDialogKind::OpenFile:
    default:
        return L"file";
    }
}

bool ShowCommonFileDialog(const CommonFileDialogOptions& options, CommonFileDialogResult& result) {
    result = {};
    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);

    const std::filesystem::path initialDirectory = ResolveCommonFileDialogInitialDirectory(options.defaultPath);
    WriteAppLog(
        DialogLogPrefix(options) + L" 打开请求: kind=" + CommonFileDialogKindName(options.kind) +
        L", defaultPath=\"" + options.defaultPath + L"\", initialDir=\"" + initialDirectory.wstring() + L"\"");

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        LARGE_INTEGER ended{};
        QueryPerformanceCounter(&ended);
        result.dialogResult = hr;
        result.elapsedMs = ElapsedMilliseconds(started, ended, frequency);
        WriteAppLog(DialogLogPrefix(options) + L" 创建失败: hr=" + HResultText(hr) +
                    L", elapsedMs=" + std::to_wstring(result.elapsedMs));
        return false;
    }

    DWORD flags = 0;
    dialog->GetOptions(&flags);
    if (options.kind == CommonFileDialogKind::PickFolder) {
        flags |= FOS_PICKFOLDERS;
    }
    if (options.forceFileSystem) {
        flags |= FOS_FORCEFILESYSTEM;
    }
    if (options.pathMustExist) {
        flags |= FOS_PATHMUSTEXIST;
    }
    if (options.kind == CommonFileDialogKind::OpenFile && options.fileMustExist) {
        flags |= FOS_FILEMUSTEXIST;
    }
    dialog->SetOptions(flags);

    if (!options.title.empty()) {
        dialog->SetTitle(options.title.c_str());
    }
    if (!options.defaultExtension.empty()) {
        dialog->SetDefaultExtension(options.defaultExtension.c_str());
    }
    if (!initialDirectory.empty()) {
        SetDefaultFolder(dialog, initialDirectory);
    }

    std::vector<std::wstring> filterStorage;
    const std::vector<COMDLG_FILTERSPEC> filters = ParseLegacyFilter(options.legacyFilter, filterStorage);
    if (!filters.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
    }

    if (options.kind == CommonFileDialogKind::OpenFile && !options.defaultPath.empty()) {
        const std::filesystem::path defaultFile(ExpandEnvironmentStringsSafe(Trim(options.defaultPath)));
        if (!defaultFile.filename().empty()) {
            dialog->SetFileName(defaultFile.filename().c_str());
        }
    }

    hr = dialog->Show(options.owner);
    result.dialogResult = hr;
    result.accepted = SUCCEEDED(hr);
    if (result.accepted) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            HRESULT nameResult = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (FAILED(nameResult) && options.allowShellFolderParsingName) {
                nameResult = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path);
            }
            if (SUCCEEDED(nameResult) && path) {
                result.path = path;
                CoTaskMemFree(path);
            }

            PWSTR displayName = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName)) && displayName) {
                result.displayName = displayName;
                CoTaskMemFree(displayName);
            }
            item->Release();
        }
    }
    dialog->Release();

    LARGE_INTEGER ended{};
    QueryPerformanceCounter(&ended);
    result.elapsedMs = ElapsedMilliseconds(started, ended, frequency);
    WriteAppLog(
        DialogLogPrefix(options) + L" 关闭: accepted=" + std::wstring(result.accepted ? L"1" : L"0") +
        L", hr=" + HResultText(result.dialogResult) +
        L", elapsedMs=" + std::to_wstring(result.elapsedMs) +
        L", selected=\"" + result.path + L"\"");
    return result.accepted && !result.path.empty();
}
