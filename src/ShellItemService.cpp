#include "ShellItemService.h"

#include "Utilities.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>

namespace {
constexpr std::size_t kMaxPidlBytes = 256 * 1024;

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

struct PidlHolder {
    PIDLIST_ABSOLUTE value = nullptr;

    explicit PidlHolder(PIDLIST_ABSOLUTE pidl = nullptr) : value(pidl) {}
    ~PidlHolder() {
        if (value) {
            CoTaskMemFree(value);
        }
    }

    PidlHolder(const PidlHolder&) = delete;
    PidlHolder& operator=(const PidlHolder&) = delete;
};

std::wstring TakeShellString(PWSTR value) {
    std::wstring result;
    if (value) {
        result = value;
        CoTaskMemFree(value);
    }
    return result;
}

int NormalizeShowCmd(int showCmd) {
    return showCmd > 0 ? showCmd : SW_SHOWNORMAL;
}

bool IsUrlLink(const Link& link) {
    const std::wstring path = Trim(link.path);
    if (link.type == 2) {
        return true;
    }
    const std::wstring lower = ToLower(path);
    return lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

std::vector<std::uint8_t> SerializePidl(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) {
        return {};
    }
    const UINT bytes = ILGetSize(pidl);
    if (bytes < 2 || bytes > kMaxPidlBytes) {
        return {};
    }
    std::vector<std::uint8_t> result(bytes);
    std::memcpy(result.data(), pidl, bytes);
    if (!ShellItemService::IsPidlBlobPlausible(result)) {
        return {};
    }
    return result;
}

std::wstring PidlName(PCIDLIST_ABSOLUTE pidl, SIGDN mode) {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, mode, &value))) {
        return TakeShellString(value);
    }
    return {};
}

std::optional<ShellItemRef> BuildRefFromPidl(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) {
        return std::nullopt;
    }

    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
    if (FAILED(hr)) {
        return std::nullopt;
    }
    SafeRelease(item);

    ShellItemRef ref;
    ref.pidl = SerializePidl(pidl);
    if (ref.pidl.empty()) {
        return std::nullopt;
    }
    ref.displayName = PidlName(pidl, SIGDN_NORMALDISPLAY);
    ref.parseName = PidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING);
    ref.fileSystemPath = PidlName(pidl, SIGDN_FILESYSPATH);

    if (ref.fileSystemPath.empty()) {
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidl, path)) {
            ref.fileSystemPath = path;
        }
    }

    ref.isFileSystem = !Trim(ref.fileSystemPath).empty();
    ref.isVirtual = !ref.isFileSystem;
    return ref;
}

bool ExecutePidl(HWND owner, PCIDLIST_ABSOLUTE pidl, int showCmd, std::wstring& errorMessage) {
    if (!pidl) {
        errorMessage = L"Shell 对象为空。";
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpIDList = const_cast<ITEMIDLIST*>(pidl);
    info.nShow = NormalizeShowCmd(showCmd);
    if (!ShellExecuteExW(&info)) {
        errorMessage = FormatLastError(GetLastError());
        return false;
    }
    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    return true;
}

bool ExecuteFile(
    HWND owner,
    const std::wstring& file,
    const std::wstring& parameters,
    int showCmd,
    std::wstring& errorMessage) {
    if (Trim(file).empty()) {
        errorMessage = L"目标路径为空。";
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = NormalizeShowCmd(showCmd);
    if (!ShellExecuteExW(&info)) {
        errorMessage = FormatLastError(GetLastError());
        return false;
    }
    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    return true;
}

bool ExecutePath(HWND owner, const std::wstring& path, int showCmd, std::wstring& errorMessage) {
    return ExecuteFile(owner, path, L"", showCmd, errorMessage);
}

bool OpenParentAndSelect(HWND owner, PCIDLIST_ABSOLUTE pidl, int showCmd, std::wstring& errorMessage) {
    if (!pidl) {
        errorMessage = L"Shell 对象为空。";
        return false;
    }

    PidlHolder parent(ILCloneFull(pidl));
    if (parent.value && ILRemoveLastID(parent.value)) {
        PCUITEMID_CHILD child = ILFindLastID(pidl);
        if (child && child->mkid.cb != 0) {
            PCUITEMID_CHILD children[] = {child};
            if (SUCCEEDED(SHOpenFolderAndSelectItems(parent.value, 1, children, 0))) {
                return true;
            }
        }
    }

    if (SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0))) {
        return true;
    }
    return ExecutePidl(owner, pidl, showCmd, errorMessage);
}

bool OpenPathLocation(HWND owner, const std::wstring& path, std::wstring& errorMessage) {
    std::filesystem::path fsPath(path);
    std::error_code ec;
    if (std::filesystem::is_regular_file(fsPath, ec)) {
        const std::wstring args = L"/select," + QuoteForCommandLine(fsPath.wstring());
        return ExecuteFile(owner, L"explorer.exe", args, SW_SHOWNORMAL, errorMessage);
    }
    ec.clear();
    if (std::filesystem::is_directory(fsPath, ec)) {
        return ExecutePath(owner, fsPath.wstring(), SW_SHOWNORMAL, errorMessage);
    }
    return false;
}

std::optional<std::vector<std::uint8_t>> ResolveLinkPidl(const Link& link) {
    if (!link.pidl.empty() && ShellItemService::IsPidlBlobPlausible(link.pidl)) {
        return link.pidl;
    }

    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(link.path));
    if (target.empty() || IsUrlLink(link)) {
        return std::nullopt;
    }
    if (auto ref = ShellItemService::FromPathOrParseName(target)) {
        return ref->pidl;
    }
    return std::nullopt;
}

bool ExecutePidlVerb(HWND owner, PCIDLIST_ABSOLUTE pidl, const wchar_t* verb, int showCmd) {
    if (!pidl || !verb || !*verb) {
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_INVOKEIDLIST;
    info.hwnd = owner;
    info.lpVerb = verb;
    info.lpIDList = const_cast<ITEMIDLIST*>(pidl);
    info.nShow = NormalizeShowCmd(showCmd);
    return ShellExecuteExW(&info) != FALSE;
}

}

bool ShellItemService::IsShellParseName(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower.rfind(L"shell:", 0) == 0 || lower.rfind(L"::{", 0) == 0;
}

bool ShellItemService::IsPidlBlobPlausible(const std::vector<std::uint8_t>& blob) {
    if (blob.size() < 2 || blob.size() > kMaxPidlBytes) {
        return false;
    }

    std::size_t offset = 0;
    while (offset + sizeof(USHORT) <= blob.size()) {
        USHORT cb = 0;
        std::memcpy(&cb, blob.data() + offset, sizeof(cb));
        if (cb == 0) {
            return offset + sizeof(USHORT) == blob.size();
        }
        if (cb < sizeof(USHORT) || offset + cb > blob.size()) {
            return false;
        }
        offset += cb;
    }
    return false;
}

std::optional<ShellItemRef> ShellItemService::FromAbsolutePidl(PCIDLIST_ABSOLUTE pidl) {
    return BuildRefFromPidl(pidl);
}

std::optional<ShellItemRef> ShellItemService::FromPathOrParseName(const std::wstring& value) {
    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(value));
    if (target.empty()) {
        return std::nullopt;
    }

    PIDLIST_ABSOLUTE parsed = nullptr;
    HRESULT hr = SHParseDisplayName(target.c_str(), nullptr, &parsed, 0, nullptr);
    if (FAILED(hr)) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(target), ec)) {
            parsed = ILCreateFromPathW(target.c_str());
        }
    }

    PidlHolder holder(parsed);
    if (!holder.value) {
        return std::nullopt;
    }
    auto ref = BuildRefFromPidl(holder.value);
    if (ref && ref->parseName.empty()) {
        ref->parseName = target;
    }
    return ref;
}

std::optional<ShellItemRef> ShellItemService::FromPidlBlob(const std::vector<std::uint8_t>& blob) {
    if (!IsPidlBlobPlausible(blob)) {
        return std::nullopt;
    }
    auto pidl = reinterpret_cast<PCIDLIST_ABSOLUTE>(blob.data());
    return BuildRefFromPidl(pidl);
}

bool ShellItemService::RefreshLinkShellData(Link& link, bool clearOnFailure) {
    if (link.type == 2 || link.type == 4 || IsUrlLink(link)) {
        if (clearOnFailure) {
            link.pidl.clear();
        }
        return false;
    }

    auto ref = FromPathOrParseName(link.path);
    if (!ref) {
        if (clearOnFailure) {
            link.pidl.clear();
        }
        return false;
    }

    link.pidl = ref->pidl;
    if (ref->isVirtual || IsShellParseName(link.path)) {
        link.type = 3;
    }
    return true;
}

bool ShellItemService::OpenShellTarget(HWND owner, const Link& link, int showCmd, std::wstring& errorMessage) {
    errorMessage.clear();

    if (!link.pidl.empty()) {
        if (auto ref = FromPidlBlob(link.pidl)) {
            if (ExecutePidl(owner, reinterpret_cast<PCIDLIST_ABSOLUTE>(ref->pidl.data()), showCmd, errorMessage)) {
                return true;
            }
        }
    }

    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(link.path));
    if (!target.empty()) {
        if (auto ref = FromPathOrParseName(target)) {
            if (ExecutePidl(owner, reinterpret_cast<PCIDLIST_ABSOLUTE>(ref->pidl.data()), showCmd, errorMessage)) {
                return true;
            }
        }
        return ExecutePath(owner, target, showCmd, errorMessage);
    }

    errorMessage = L"Shell 目标为空。";
    return false;
}

bool ShellItemService::OpenContainingLocation(HWND owner, const Link& link, std::wstring& errorMessage) {
    errorMessage.clear();
    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(link.path));

    if (IsUrlLink(link)) {
        return ExecutePath(owner, NormalizeUrl(target), SW_SHOWNORMAL, errorMessage);
    }

    if (!target.empty() && OpenPathLocation(owner, target, errorMessage)) {
        return true;
    }

    if (!link.pidl.empty()) {
        if (auto ref = FromPidlBlob(link.pidl)) {
            if (OpenParentAndSelect(owner, reinterpret_cast<PCIDLIST_ABSOLUTE>(ref->pidl.data()), SW_SHOWNORMAL, errorMessage)) {
                return true;
            }
        }
    }

    if (!target.empty()) {
        if (auto ref = FromPathOrParseName(target)) {
            if (OpenParentAndSelect(owner, reinterpret_cast<PCIDLIST_ABSOLUTE>(ref->pidl.data()), SW_SHOWNORMAL, errorMessage)) {
                return true;
            }
        }
    }

    if (IsShellParseName(target)) {
        return OpenShellTarget(owner, link, SW_SHOWNORMAL, errorMessage);
    }

    if (errorMessage.empty()) {
        errorMessage = L"目标路径不存在或不是可打开的位置。";
    }
    return false;
}

bool ShellItemService::ShowNativeContextMenu(HWND owner, const Link& link, POINT screenPoint) {
    if (IsUrlLink(link)) {
        return false;
    }

    auto pidlBlob = ResolveLinkPidl(link);
    if (!pidlBlob || pidlBlob->empty()) {
        return false;
    }

    auto pidl = reinterpret_cast<PCIDLIST_ABSOLUTE>(pidlBlob->data());
    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(pidl, IID_PPV_ARGS(&parentFolder), &child)) || !parentFolder || !child) {
        SafeRelease(parentFolder);
        return false;
    }

    IContextMenu* contextMenu = nullptr;
    HRESULT hr = parentFolder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&contextMenu));
    SafeRelease(parentFolder);
    if (FAILED(hr) || !contextMenu) {
        return false;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        SafeRelease(contextMenu);
        return false;
    }

    bool handled = false;
    if (SUCCEEDED(contextMenu->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL))) {
        ActivateWindow(owner);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, owner, nullptr);
        if (command > 0) {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE;
            invoke.hwnd = owner;
            invoke.lpVerb = MAKEINTRESOURCEA(command - 1);
            invoke.lpVerbW = MAKEINTRESOURCEW(command - 1);
            invoke.nShow = SW_SHOWNORMAL;
            handled = SUCCEEDED(contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke)));
        } else {
            handled = true;
        }
    }

    DestroyMenu(menu);
    SafeRelease(contextMenu);
    return handled;
}

bool ShellItemService::OpenProperties(HWND owner, const Link& link) {
    if (IsUrlLink(link)) {
        return false;
    }

    if (auto pidlBlob = ResolveLinkPidl(link)) {
        auto pidl = reinterpret_cast<PCIDLIST_ABSOLUTE>(pidlBlob->data());
        if (ExecutePidlVerb(owner, pidl, L"properties", SW_SHOWNORMAL)) {
            return true;
        }
    }

    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(link.path));
    if (target.empty()) {
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST;
    info.hwnd = owner;
    info.lpVerb = L"properties";
    info.lpFile = target.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}
