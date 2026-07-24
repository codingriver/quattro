#include "QuickImportService.h"

#include "ShellItemService.h"
#include "Utilities.h"

#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <stack>
#include <system_error>
#include <unordered_set>
#include <windows.h>

namespace {

std::wstring LowerExtension(const std::filesystem::path& path) {
    return ToLower(path.extension().wstring());
}

std::wstring DisplayNameFromPath(const std::filesystem::path& path) {
    std::wstring name = path.stem().wstring();
    if (Trim(name).empty()) {
        name = path.filename().wstring();
    }
    return Trim(name);
}

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

class ScopedComApartment {
public:
    explicit ScopedComApartment(DWORD mode)
        : result_(CoInitializeEx(nullptr, mode)),
          initialized_(SUCCEEDED(result_)) {}

    ~ScopedComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    HRESULT result() const { return result_; }

private:
    HRESULT result_ = E_FAIL;
    bool initialized_ = false;
};

std::wstring ShellDisplayName(IShellFolder* folder, PCUITEMID_CHILD child) {
    if (!folder || !child) {
        return {};
    }
    STRRET value{};
    if (FAILED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &value))) {
        return {};
    }
    wchar_t buffer[1024]{};
    if (FAILED(StrRetToBufW(&value, child, buffer, static_cast<UINT>(std::size(buffer))))) {
        return {};
    }
    return Trim(buffer);
}

std::wstring ReadInternetShortcutUrl(const std::filesystem::path& path) {
    std::wstring buffer(4096, L'\0');
    const DWORD copied = GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    if (copied == 0) {
        return {};
    }
    buffer.resize(copied);
    return Trim(buffer);
}

DWORD QuickImportTestItemDelayMs() {
    wchar_t testMode[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_TEST_MODE", testMode, static_cast<DWORD>(std::size(testMode))) == 0) {
        return 0;
    }
    wchar_t delayText[16]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_TEST_QUICK_IMPORT_ITEM_DELAY_MS",
            delayText,
            static_cast<DWORD>(std::size(delayText))) == 0) {
        return 0;
    }
    return (std::min<DWORD>)(wcstoul(delayText, nullptr, 10), 100);
}

}

std::vector<QuickImportService::Item> QuickImportService::Scan(const std::filesystem::path& directory, std::wstring& error) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::CallerParallel;
    ScanRequest request;
    request.source = Source::Directory;
    request.directory = directory;
    ScanResult result = ScanExecutionService::Run<ScanResult>(options,
        [request](ScanTaskContext& context) {
            return QuickImportService().RunScan(request, context);
        });
    error = std::move(result.error);
    return std::move(result.items);
}

std::shared_ptr<ScanTaskHandle> QuickImportService::StartScan(const ScanRequest& request) const {
    ScanTaskOptions options;
    options.mode = request.source == Source::StoreApps
        ? ScanExecutionMode::BackgroundSingle
        : ScanExecutionMode::BackgroundParallel;
    return ScanExecutionService::StartTyped<ScanResult>(options,
        [request](ScanTaskContext& context) {
            return QuickImportService().RunScan(request, context);
        });
}

QuickImportService::ScanResult QuickImportService::RunScan(
    const ScanRequest& request,
    ScanTaskContext& context,
    std::stop_token externalStopToken) const {
    if (request.source == Source::StoreApps) {
        return ScanStoreAppsCore(context, externalStopToken);
    }

    ScanResult result;
    if (request.directory.empty()) {
        result.error = L"请输入要扫描的目录。";
        return result;
    }
    if (!request.directory.is_absolute()) {
        result.error = L"扫描目录必须是绝对路径。";
        return result;
    }
    if (!DirectoryExists(request.directory)) {
        result.error = L"扫描目录不存在或无法访问。";
        return result;
    }

    context.Report(ScanProgressUpdate{
        L"enumerating", L"快速导入扫描进度", L"正在扫描目录", request.directory.wstring()});
    std::vector<std::filesystem::path> candidates;
    EnumerateRoot(request.directory, candidates, context);
    if (context.StopRequested() || externalStopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }

    ScanProgressUpdate progress;
    progress.phase = L"resolving";
    progress.title = L"快速导入扫描进度";
    progress.status = L"正在解析可导入项目";
    progress.detail = L"已发现 " + std::to_wstring(candidates.size()) + L" 个候选文件";
    progress.discovered = candidates.size();
    progress.total = candidates.size();
    progress.indeterminate = false;
    context.Report(std::move(progress));
    const DWORD testItemDelayMs = QuickImportTestItemDelayMs();

    using LocalItems = std::vector<Item>;
    context.ForEach<std::filesystem::path, LocalItems>(
        candidates,
        [] { return LocalItems{}; },
        [externalStopToken, testItemDelayMs](const std::filesystem::path& path, LocalItems& local, ScanTaskContext& workerContext) {
            if (workerContext.StopRequested() || externalStopToken.stop_requested()) {
                return;
            }
            if (testItemDelayMs > 0) {
                Sleep(testItemDelayMs);
            }
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Item item;
            const bool accepted = QuickImportService().TryCreateItem(path, item);
            if (SUCCEEDED(comResult)) {
                CoUninitialize();
            }
            if (accepted) {
                local.push_back(std::move(item));
            }
            workerContext.UpdateProgress([accepted, path](ScanProgressUpdate& value) {
                ++value.completed;
                value.current = value.completed;
                if (accepted) ++value.succeeded;
                else ++value.skipped;
                value.detail = path.wstring();
            });
        },
        [&result](LocalItems&& local) {
            result.items.insert(
                result.items.end(),
                std::make_move_iterator(local.begin()),
                std::make_move_iterator(local.end()));
        });

    result.cancelled = context.StopRequested() || externalStopToken.stop_requested();
    std::unordered_set<std::wstring> seen;
    std::erase_if(result.items, [&seen](const Item& item) {
        return !seen.insert(item.stableKey).second;
    });
    std::sort(result.items.begin(), result.items.end(), [](const Item& left, const Item& right) {
        const int name = CompareStringOrdinal(
            left.link.name.c_str(), -1, right.link.name.c_str(), -1, TRUE);
        if (name != CSTR_EQUAL) return name == CSTR_LESS_THAN;
        return CompareStringOrdinal(
            left.link.path.c_str(), -1, right.link.path.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    context.UpdateProgress([&result](ScanProgressUpdate& value) {
        value.status = result.cancelled ? L"扫描已停止" : L"扫描完成";
        value.detail = L"发现 " + std::to_wstring(result.items.size()) + L" 个可导入项目";
        value.current = value.total;
    });
    return result;
}

void QuickImportService::EnumerateRoot(
    const std::filesystem::path& root,
    std::vector<std::filesystem::path>& candidates,
    ScanTaskContext& context) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }

    std::stack<std::filesystem::path> pending;
    pending.push(root);
    while (!pending.empty()) {
        if (context.StopRequested()) return;
        const auto directory = pending.top();
        pending.pop();

        std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        for (const auto& entry : it) {
            if (context.StopRequested()) return;
            const auto path = entry.path();
            if (entry.is_directory(ec)) {
                const auto status = entry.symlink_status(ec);
                if (!ec && (status.type() != std::filesystem::file_type::symlink) &&
                    (entry.status(ec).permissions() != std::filesystem::perms::unknown)) {
                    const DWORD attributes = GetFileAttributesW(path.c_str());
                    if (attributes != INVALID_FILE_ATTRIBUTES &&
                        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                        pending.push(path);
                    }
                }
                ec.clear();
                continue;
            }
            ec.clear();

            if (!entry.is_regular_file(ec)) {
                ec.clear();
                continue;
            }
            ec.clear();

            const std::wstring extension = LowerExtension(path);
            if (extension == L".lnk" || extension == L".url" || extension == L".exe") {
                candidates.push_back(path);
            }
        }
        context.UpdateProgress([&candidates, directory](ScanProgressUpdate& value) {
            ++value.completed;
            value.discovered = candidates.size();
            value.detail = directory.wstring();
        });
    }
}

bool QuickImportService::TryCreateItem(const std::filesystem::path& path, Item& item) const {
    const std::wstring extension = LowerExtension(path);
    if (extension == L".lnk") {
        return TryCreateShortcutItem(path, item);
    }
    if (extension == L".url") {
        return TryCreateUrlItem(path, item);
    }
    if (extension == L".exe") {
        return TryCreateExecutableItem(path, item);
    }
    return false;
}

bool QuickImportService::TryCreateShortcutItem(const std::filesystem::path& path, Item& item) const {
    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) || !shellLink) {
        return false;
    }

    IPersistFile* persistFile = nullptr;
    if (FAILED(shellLink->QueryInterface(IID_PPV_ARGS(&persistFile))) || !persistFile) {
        shellLink->Release();
        return false;
    }

    bool ok = false;
    if (SUCCEEDED(persistFile->Load(path.c_str(), STGM_READ))) {
        std::wstring target(MAX_PATH * 4, L'\0');
        std::wstring args(2048, L'\0');
        std::wstring workDir(MAX_PATH * 4, L'\0');
        WIN32_FIND_DATAW findData{};
        if (SUCCEEDED(shellLink->GetPath(target.data(), static_cast<int>(target.size()), &findData, SLGP_UNCPRIORITY)) && !Trim(target).empty()) {
            shellLink->GetArguments(args.data(), static_cast<int>(args.size()));
            shellLink->GetWorkingDirectory(workDir.data(), static_cast<int>(workDir.size()));
            target.resize(wcsnlen_s(target.c_str(), target.size()));
            args.resize(wcsnlen_s(args.c_str(), args.size()));
            workDir.resize(wcsnlen_s(workDir.c_str(), workDir.size()));

            Link link;
            link.name = DisplayNameFromPath(path);
            link.path = Trim(target);
            link.parameter = Trim(args);
            link.workDir = Trim(workDir);
            if (DirectoryExists(link.path)) {
                persistFile->Release();
                shellLink->Release();
                return false;
            }
            link.type = 0;
            link.pos = -1;
            link.showCmd = SW_SHOWNORMAL;
            item.link = std::move(link);
            item.sourcePath = path;
            item.sourceName = L"快捷方式";
            item.status = L"可导入";
            item.stableKey = L"shortcut:" + ToLower(item.link.path) + L"\n" + ToLower(item.link.parameter);
            ok = true;
        }
    }

    persistFile->Release();
    shellLink->Release();
    return ok;
}

bool QuickImportService::TryCreateUrlItem(const std::filesystem::path& path, Item& item) const {
    const std::wstring url = ReadInternetShortcutUrl(path);
    if (url.empty()) {
        return false;
    }

    Link link;
    link.name = DisplayNameFromPath(path);
    link.path = NormalizeUrl(url);
    link.type = 2;
    link.icon = L"#url";
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    item.link = std::move(link);
    item.sourcePath = path;
    item.sourceName = L"网址";
    item.status = L"可导入";
    item.stableKey = L"url:" + ToLower(item.link.path);
    return true;
}

bool QuickImportService::TryCreateExecutableItem(const std::filesystem::path& path, Item& item) const {
    Link link;
    link.name = DisplayNameFromPath(path);
    link.path = path.wstring();
    link.type = 0;
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    item.link = std::move(link);
    item.sourcePath = path;
    item.sourceName = L"程序";
    item.status = L"可导入";
    item.stableKey = L"exe:" + ToLower(item.link.path);
    return true;
}

std::vector<QuickImportService::Item> QuickImportService::ScanStoreApps(
    std::wstring& error,
    std::stop_token stopToken) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::CallerSingle;
    ScanRequest request;
    request.source = Source::StoreApps;
    ScanResult result = ScanExecutionService::Run<ScanResult>(options,
        [request, stopToken](ScanTaskContext& context) {
            return QuickImportService().RunScan(request, context, stopToken);
        });
    error = std::move(result.error);
    return std::move(result.items);
}

QuickImportService::ScanResult QuickImportService::ScanStoreAppsCore(
    ScanTaskContext& context,
    std::stop_token externalStopToken) const {
    ScanResult result;
    context.Report(ScanProgressUpdate{
        L"enumerating", L"快速导入扫描进度", L"正在读取 Windows 应用", L"Windows 已安装应用"});

    ScopedComApartment com(COINIT_APARTMENTTHREADED);
    if (FAILED(com.result()) && com.result() != RPC_E_CHANGED_MODE) {
        result.error = L"无法初始化 Windows 应用读取环境。";
        return result;
    }

    PIDLIST_ABSOLUTE appsFolderPidl = nullptr;
    HRESULT hr = SHParseDisplayName(L"shell:AppsFolder", nullptr, &appsFolderPidl, 0, nullptr);
    if (FAILED(hr) || !appsFolderPidl) {
        result.error = L"无法读取 Windows 应用列表。";
        return result;
    }

    IShellFolder* desktop = nullptr;
    IShellFolder* appsFolder = nullptr;
    hr = SHGetDesktopFolder(&desktop);
    if (SUCCEEDED(hr) && desktop) {
        hr = desktop->BindToObject(appsFolderPidl, nullptr, IID_PPV_ARGS(&appsFolder));
    }
    if (FAILED(hr) || !appsFolder) {
        SafeRelease(desktop);
        CoTaskMemFree(appsFolderPidl);
        result.error = L"无法打开 Windows 应用列表。";
        return result;
    }

    IEnumIDList* enumList = nullptr;
    hr = appsFolder->EnumObjects(nullptr, SHCONTF_NONFOLDERS, &enumList);
    if (FAILED(hr) || !enumList) {
        SafeRelease(appsFolder);
        SafeRelease(desktop);
        CoTaskMemFree(appsFolderPidl);
        result.error = L"Windows 应用列表为空或无法枚举。";
        return result;
    }

    std::vector<PITEMID_CHILD> children;
    auto freeChildren = [&children]() {
        for (PITEMID_CHILD child : children) {
            CoTaskMemFree(child);
        }
        children.clear();
    };

    for (;;) {
        if (context.StopRequested() || externalStopToken.stop_requested()) {
            result.cancelled = true;
            break;
        }
        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        hr = enumList->Next(1, &child, &fetched);
        if (hr != S_OK || fetched == 0 || !child) {
            break;
        }

        children.push_back(child);
    }

    SafeRelease(enumList);

    if (!result.cancelled) {
        context.Report(ScanProgressUpdate{
            L"resolving",
            L"快速导入扫描进度",
            L"正在解析 Windows 应用",
            L"已发现 " + std::to_wstring(children.size()) + L" 个应用",
            static_cast<std::uint64_t>(children.size()),
            0,
            0,
            0,
            0,
            0,
            static_cast<std::uint64_t>(children.size()),
            1,
            false});
    }

    for (std::size_t index = 0; index < children.size() && !result.cancelled; ++index) {
        if (context.StopRequested() || externalStopToken.stop_requested()) {
            result.cancelled = true;
            break;
        }

        PITEMID_CHILD child = children[index];
        bool accepted = false;
        std::wstring detail;

        PIDLIST_ABSOLUTE absolute = ILCombine(appsFolderPidl, child);
        if (absolute) {
            if (auto ref = ShellItemService::FromAbsolutePidl(absolute)) {
                Link link;
                link.name = ShellDisplayName(appsFolder, child);
                if (link.name.empty()) {
                    link.name = ref->displayName;
                }
                link.path = ref->parseName.empty() ? L"shell:AppsFolder" : ref->parseName;
                link.type = 3;
                link.pos = -1;
                link.showCmd = SW_SHOWNORMAL;
                link.pidl = ref->pidl;

                if (!Trim(link.name).empty() && !link.pidl.empty()) {
                    Item item;
                    item.link = std::move(link);
                    item.sourceName = L"商店应用";
                    item.status = L"可导入";
                    item.stableKey = ref->parseName.empty()
                        ? L"store-pidl:" + std::to_wstring(result.items.size())
                        : L"store:" + ToLower(ref->parseName);
                    detail = item.link.name;
                    result.items.push_back(std::move(item));
                    accepted = true;
                }
            }
            CoTaskMemFree(absolute);
        }
        if (detail.empty()) {
            detail = L"正在解析 Windows 应用";
        }
        CoTaskMemFree(child);
        children[index] = nullptr;

        const std::uint64_t completed = static_cast<std::uint64_t>(index + 1);
        const std::uint64_t total = static_cast<std::uint64_t>(children.size());
        context.UpdateProgress([accepted, completed, total, detail](ScanProgressUpdate& value) {
            value.phase = L"resolving";
            value.status = L"正在解析 Windows 应用";
            value.completed = completed;
            value.current = completed;
            value.total = total;
            value.discovered = total;
            value.indeterminate = false;
            if (accepted) {
                ++value.succeeded;
            } else {
                ++value.skipped;
            }
            value.detail = detail;
        });
    }
    freeChildren();

    SafeRelease(appsFolder);
    SafeRelease(desktop);
    CoTaskMemFree(appsFolderPidl);

    std::sort(result.items.begin(), result.items.end(), [](const Item& left, const Item& right) {
        return CompareStringOrdinal(
            left.link.name.c_str(),
            -1,
            right.link.name.c_str(),
            -1,
            TRUE) == CSTR_LESS_THAN;
    });
    context.UpdateProgress([&result](ScanProgressUpdate& value) {
        value.status = result.cancelled ? L"扫描已停止" : L"扫描完成";
        value.detail = L"发现 " + std::to_wstring(result.items.size()) + L" 个可导入应用";
        value.current = value.total;
        value.completed = value.total;
        value.indeterminate = value.total == 0;
    });
    return result;
}
