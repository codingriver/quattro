#include "FileDialog.h"

#include "AppLog.h"
#include "Utilities.h"

#include <shobjidl.h>

#include <cwchar>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace {
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

class FileDialogOpenEventLogger final : public IFileDialogEvents {
public:
    FileDialogOpenEventLogger(std::wstring prefix, LARGE_INTEGER started, LARGE_INTEGER frequency)
        : prefix_(std::move(prefix)), started_(started), frequency_(frequency) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) {
            *object = static_cast<IFileDialogEvents*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&referenceCount_);
        if (remaining == 0) {
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override {
        if (InterlockedCompareExchange(&openReadyObserved_, 1, 0) == 0) {
            LARGE_INTEGER ready{};
            QueryPerformanceCounter(&ready);
            openElapsedMs_ = ElapsedMilliseconds(started_, ready, frequency_);
            WriteAppLog(prefix_ + L" 打开完成: elapsedMs=" + std::to_wstring(openElapsedMs_) +
                        L", readyEvent=folderChanged");
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (response) {
            *response = FDESVR_DEFAULT;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        if (response) {
            *response = FDEOR_DEFAULT;
        }
        return S_OK;
    }

    bool openReadyObserved() const {
        return openReadyObserved_ != 0;
    }

    long long openElapsedMs() const { return openElapsedMs_; }

private:
    ~FileDialogOpenEventLogger() = default;

    volatile LONG referenceCount_ = 1;
    volatile LONG openReadyObserved_ = 0;
    std::wstring prefix_;
    LARGE_INTEGER started_{};
    LARGE_INTEGER frequency_{};
    long long openElapsedMs_ = 0;
};

bool AppendShellItemResult(
    IShellItem* item,
    bool allowShellFolderParsingName,
    std::vector<std::wstring>& paths,
    std::vector<std::wstring>& displayNames) {
    if (!item) {
        return false;
    }

    PWSTR path = nullptr;
    HRESULT nameResult = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(nameResult) && allowShellFolderParsingName) {
        nameResult = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path);
    }
    if (FAILED(nameResult) || !path || !*path) {
        if (path) {
            CoTaskMemFree(path);
        }
        return false;
    }

    paths.emplace_back(path);
    CoTaskMemFree(path);

    PWSTR displayName = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName)) && displayName) {
        displayNames.emplace_back(displayName);
        CoTaskMemFree(displayName);
    } else {
        displayNames.emplace_back();
    }
    return true;
}

bool CollectSingleResult(IFileOpenDialog* dialog, const CommonFileDialogOptions& options, CommonFileDialogResult& result) {
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item) {
        return false;
    }
    const bool ok = AppendShellItemResult(item, options.allowShellFolderParsingName, result.paths, result.displayNames);
    item->Release();
    return ok;
}

bool CollectMultiSelectResults(IFileOpenDialog* dialog, const CommonFileDialogOptions& options, CommonFileDialogResult& result) {
    IShellItemArray* items = nullptr;
    if (FAILED(dialog->GetResults(&items)) || !items) {
        return false;
    }

    DWORD count = 0;
    items->GetCount(&count);
    result.paths.reserve(count);
    result.displayNames.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(items->GetItemAt(index, &item)) && item) {
            AppendShellItemResult(item, options.allowShellFolderParsingName, result.paths, result.displayNames);
            item->Release();
        }
    }
    items->Release();
    return !result.paths.empty();
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

std::wstring CommonFileDialogModeName(CommonFileDialogMode mode) {
    switch (mode) {
    case CommonFileDialogMode::FolderOnly:
        return L"folder";
    case CommonFileDialogMode::FileOrFolder:
        return L"file-or-folder";
    case CommonFileDialogMode::FileOnly:
    default:
        return L"file";
    }
}

bool CommonFileDialogSupportsNativeMode(CommonFileDialogMode mode) {
    return mode == CommonFileDialogMode::FileOnly || mode == CommonFileDialogMode::FolderOnly;
}

bool ShowCommonFileDialog(const CommonFileDialogOptions& options, CommonFileDialogResult& result) {
    result = {};
    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);

    const std::filesystem::path initialDirectory = ResolveCommonFileDialogInitialDirectory(options.defaultPath);
    WriteAppLog(
        DialogLogPrefix(options) + L" 打开请求: mode=" + CommonFileDialogModeName(options.mode) +
        L", multiSelect=" + std::wstring(options.allowMultiSelect ? L"1" : L"0") +
        L", defaultPath=\"" + options.defaultPath + L"\", initialDir=\"" + initialDirectory.wstring() + L"\"");

    if (!CommonFileDialogSupportsNativeMode(options.mode)) {
        LARGE_INTEGER ended{};
        QueryPerformanceCounter(&ended);
        result.dialogResult = E_NOTIMPL;
        result.elapsedMs = ElapsedMilliseconds(started, ended, frequency);
        WriteAppLog(DialogLogPrefix(options) +
                    L" 不支持: 原生 IFileOpenDialog 不能在同一对话框中同时选择文件和文件夹" +
                    L", elapsedMs=" + std::to_wstring(result.elapsedMs));
        return false;
    }

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
    if (options.mode == CommonFileDialogMode::FolderOnly) {
        flags |= FOS_PICKFOLDERS;
    }
    if (options.allowMultiSelect) {
        flags |= FOS_ALLOWMULTISELECT;
    }
    if (options.forceFileSystem) {
        flags |= FOS_FORCEFILESYSTEM;
    }
    if (options.pathMustExist) {
        flags |= FOS_PATHMUSTEXIST;
    }
    if (options.mode == CommonFileDialogMode::FileOnly && options.fileMustExist) {
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

    if (options.mode == CommonFileDialogMode::FileOnly && !options.defaultPath.empty()) {
        const std::filesystem::path defaultFile(ExpandEnvironmentStringsSafe(Trim(options.defaultPath)));
        if (!defaultFile.filename().empty()) {
            dialog->SetFileName(defaultFile.filename().c_str());
        }
    }

    FileDialogOpenEventLogger* openEventLogger =
        new FileDialogOpenEventLogger(DialogLogPrefix(options), started, frequency);
    DWORD eventCookie = 0;
    const HRESULT adviseResult = dialog->Advise(openEventLogger, &eventCookie);
    if (FAILED(adviseResult)) {
        WriteAppLog(DialogLogPrefix(options) + L" 打开事件监听注册失败: hr=" + HResultText(adviseResult));
    }

    hr = dialog->Show(options.owner);
    result.dialogResult = hr;
    result.accepted = SUCCEEDED(hr);
    if (result.accepted) {
        if (options.allowMultiSelect) {
            CollectMultiSelectResults(dialog, options, result);
        } else {
            CollectSingleResult(dialog, options, result);
        }
        if (!result.paths.empty()) {
            result.path = result.paths.front();
        }
        if (!result.displayNames.empty()) {
            result.displayName = result.displayNames.front();
        }
    }
    const bool openReadyObserved = openEventLogger->openReadyObserved();
    const long long openElapsedMs = openEventLogger->openElapsedMs();
    if (SUCCEEDED(adviseResult)) {
        dialog->Unadvise(eventCookie);
    }
    openEventLogger->Release();
    dialog->Release();

    LARGE_INTEGER ended{};
    QueryPerformanceCounter(&ended);
    result.elapsedMs = ElapsedMilliseconds(started, ended, frequency);
    WriteAppLog(
        DialogLogPrefix(options) + L" 关闭: accepted=" + std::wstring(result.accepted ? L"1" : L"0") +
        L", hr=" + HResultText(result.dialogResult) +
        L", elapsedMs=" + std::to_wstring(result.elapsedMs) +
        L", openReadyObserved=" + std::wstring(openReadyObserved ? L"1" : L"0") +
        L", openElapsedMs=" + std::to_wstring(openElapsedMs) +
        L", selectedCount=" + std::to_wstring(result.paths.size()) +
        L", selected=\"" + result.path + L"\"");
    return result.accepted && !result.path.empty();
}
