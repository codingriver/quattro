#include "QuickImportService.h"

#include "ShellItemService.h"
#include "Utilities.h"

#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <stack>
#include <system_error>
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

}

std::vector<QuickImportService::Item> QuickImportService::Scan(const std::filesystem::path& directory, std::wstring& error) const {
    error.clear();
    std::vector<Item> items;
    if (directory.empty()) {
        error = L"请输入要扫描的目录。";
        return items;
    }
    if (!directory.is_absolute()) {
        error = L"扫描目录必须是绝对路径。";
        return items;
    }
    if (!DirectoryExists(directory)) {
        error = L"扫描目录不存在或无法访问。";
        return items;
    }

    ScanRoot(directory, items);
    return items;
}

void QuickImportService::ScanRoot(const std::filesystem::path& root, std::vector<Item>& items) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }

    std::stack<std::pair<std::filesystem::path, int>> pending;
    pending.push({root, 0});
    while (!pending.empty()) {
        const auto [directory, depth] = pending.top();
        pending.pop();

        std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        for (const auto& entry : it) {
            const auto path = entry.path();
            if (entry.is_directory(ec)) {
                if (!ec && depth < kMaxDepth) {
                    pending.push({path, depth + 1});
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

            Item item;
            if (TryCreateItem(path, item)) {
                items.push_back(std::move(item));
            }
        }
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
            link.type = DirectoryExists(link.path) ? 1 : 0;
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
    error.clear();
    std::vector<Item> items;

    PIDLIST_ABSOLUTE appsFolderPidl = nullptr;
    HRESULT hr = SHParseDisplayName(L"shell:AppsFolder", nullptr, &appsFolderPidl, 0, nullptr);
    if (FAILED(hr) || !appsFolderPidl) {
        error = L"无法读取 Windows 应用列表。";
        return items;
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
        error = L"无法打开 Windows 应用列表。";
        return items;
    }

    IEnumIDList* enumList = nullptr;
    hr = appsFolder->EnumObjects(nullptr, SHCONTF_NONFOLDERS | SHCONTF_FOLDERS, &enumList);
    if (FAILED(hr) || !enumList) {
        SafeRelease(appsFolder);
        SafeRelease(desktop);
        CoTaskMemFree(appsFolderPidl);
        error = L"Windows 应用列表为空或无法枚举。";
        return items;
    }

    for (;;) {
        if (stopToken.stop_requested()) {
            break;
        }
        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        hr = enumList->Next(1, &child, &fetched);
        if (hr != S_OK || fetched == 0 || !child) {
            break;
        }

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
                        ? L"store-pidl:" + std::to_wstring(items.size())
                        : L"store:" + ToLower(ref->parseName);
                    items.push_back(std::move(item));
                }
            }
            CoTaskMemFree(absolute);
        }
        CoTaskMemFree(child);
    }

    SafeRelease(enumList);
    SafeRelease(appsFolder);
    SafeRelease(desktop);
    CoTaskMemFree(appsFolderPidl);

    std::sort(items.begin(), items.end(), [](const Item& left, const Item& right) {
        return CompareStringOrdinal(
            left.link.name.c_str(),
            -1,
            right.link.name.c_str(),
            -1,
            TRUE) == CSTR_LESS_THAN;
    });
    return items;
}
