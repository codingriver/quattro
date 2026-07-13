#include "ShellItemService.h"

#include "Utilities.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <utility>

namespace {
constexpr std::size_t kMaxPidlBytes = 256 * 1024;
constexpr UINT kContextMenuQueryFlags = CMF_NORMAL | CMF_SYNCCASCADEMENU;

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

struct NativeContextMenuSession {
    IContextMenu* contextMenu = nullptr;
    IContextMenu2* contextMenu2 = nullptr;
    IContextMenu3* contextMenu3 = nullptr;
    HMENU menu = nullptr;

    ~NativeContextMenuSession() {
        if (menu) {
            DestroyMenu(menu);
        }
        SafeRelease(contextMenu3);
        SafeRelease(contextMenu2);
        SafeRelease(contextMenu);
    }
};

bool CreateNativeContextMenuSession(HWND owner, const Link& link, NativeContextMenuSession& session) {
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
    const HRESULT hr = parentFolder->GetUIObjectOf(
        owner, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&session.contextMenu));
    SafeRelease(parentFolder);
    if (FAILED(hr) || !session.contextMenu) {
        return false;
    }
    session.contextMenu->QueryInterface(IID_PPV_ARGS(&session.contextMenu3));
    if (!session.contextMenu3) {
        session.contextMenu->QueryInterface(IID_PPV_ARGS(&session.contextMenu2));
    }
    session.menu = CreatePopupMenu();
    if (!session.menu) {
        return false;
    }
    // Tracked snapshots inspect cascade items immediately after QueryContextMenu.
    // Some handlers, notably TortoiseSVN/TortoiseGit, otherwise populate their
    // submenu image lists lazily and can return an empty HBMMENU_CALLBACK image
    // during the first refresh. Ask the shell extension to finish building the
    // cascade synchronously so menu state and native icons belong to one stable
    // snapshot.
    return SUCCEEDED(session.contextMenu->QueryContextMenu(
        session.menu,
        0,
        1,
        0x7FFF,
        kContextMenuQueryFlags));
}

std::wstring CleanMenuText(std::wstring text) {
    const auto tab = text.find(L'\t');
    if (tab != std::wstring::npos) {
        text.resize(tab);
    }
    std::wstring cleaned;
    cleaned.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != L'&') {
            cleaned.push_back(text[index]);
        } else if (index + 1 < text.size() && text[index + 1] == L'&') {
            cleaned.push_back(L'&');
            ++index;
        }
    }
    return Trim(cleaned);
}

std::wstring MenuText(HMENU menu, UINT position) {
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING;
    if (!GetMenuItemInfoW(menu, position, TRUE, &info)) {
        return {};
    }
    std::wstring text(info.cch + 1, L'\0');
    info.dwTypeData = text.data();
    info.cch = static_cast<UINT>(text.size());
    if (!GetMenuItemInfoW(menu, position, TRUE, &info)) {
        return {};
    }
    text.resize(info.cch);
    return CleanMenuText(std::move(text));
}

std::wstring CommandVerb(IContextMenu* contextMenu, UINT commandId) {
    if (!contextMenu || commandId < 1 || commandId > 0x7FFF) {
        return {};
    }
    std::array<wchar_t, 512> buffer{};
    if (FAILED(contextMenu->GetCommandString(
            commandId - 1,
            GCS_VERBW,
            nullptr,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<UINT>(buffer.size())))) {
        return {};
    }
    return buffer.data();
}

void CaptureMenuBitmap(HBITMAP bitmap, ShellContextMenuItem& item, int quality) {
    if (!bitmap || reinterpret_cast<INT_PTR>(bitmap) <= 0) {
        return;
    }
    BITMAP source{};
    if (GetObjectW(bitmap, sizeof(source), &source) != sizeof(source)) {
        return;
    }
    const int width = std::abs(source.bmWidth);
    const int height = std::abs(source.bmHeight);
    if (width <= 0 || height <= 0 || width > 64 || height > 64) {
        return;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    HDC dc = GetDC(nullptr);
    const int rows = dc ? GetDIBits(
        dc,
        bitmap,
        0,
        static_cast<UINT>(height),
        pixels.data(),
        &info,
        DIB_RGB_COLORS) : 0;
    if (dc) {
        ReleaseDC(nullptr, dc);
    }
    if (rows != height) {
        return;
    }

    const bool hasAlpha = std::any_of(pixels.begin(), pixels.end(), [](std::uint32_t pixel) {
        return (pixel >> 24) != 0;
    });
    if (!hasAlpha) {
        const std::uint32_t transparentRgb = pixels.front() & 0x00FFFFFFu;
        bool hasVisiblePixel = false;
        for (auto& pixel : pixels) {
            const std::uint32_t rgb = pixel & 0x00FFFFFFu;
            if (rgb == transparentRgb) {
                pixel = 0;
            } else {
                pixel = 0xFF000000u | rgb;
                hasVisiblePixel = true;
            }
        }
        if (!hasVisiblePixel) {
            return;
        }
    }

    for (auto& pixel : pixels) {
        const std::uint32_t alpha = pixel >> 24;
        if (alpha == 0 || alpha == 255) {
            continue;
        }
        const std::uint32_t blue = pixel & 0xFFu;
        const std::uint32_t green = (pixel >> 8) & 0xFFu;
        const std::uint32_t red = (pixel >> 16) & 0xFFu;
        pixel = (alpha << 24) |
                ((red * alpha / 255) << 16) |
                ((green * alpha / 255) << 8) |
                (blue * alpha / 255);
    }
    item.iconWidth = width;
    item.iconHeight = height;
    item.iconQuality = quality;
    item.iconPixels = std::move(pixels);
}

std::wstring RegistryString(HKEY root, const std::wstring& subkey, const wchar_t* valueName) {
    DWORD size = 0;
    if (RegGetValueW(
            root,
            subkey.c_str(),
            valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            nullptr,
            &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(
            root,
            subkey.c_str(),
            valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            value.data(),
            &size) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    return ExpandEnvironmentStringsSafe(Trim(value));
}

std::wstring ExecutableFromCommand(const std::wstring& command) {
    if (command.empty()) {
        return {};
    }
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &count);
    if (!arguments || count <= 0) {
        if (arguments) LocalFree(arguments);
        return {};
    }
    std::wstring executable = arguments[0];
    LocalFree(arguments);
    return ExpandEnvironmentStringsSafe(Trim(executable));
}

bool ParseIconLocation(std::wstring value, std::wstring& path, int& index) {
    value = Trim(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() == L'@') {
        value.erase(value.begin());
    }
    index = 0;
    const auto comma = value.rfind(L',');
    if (comma != std::wstring::npos) {
        if (const auto parsed = ParseInt(Trim(value.substr(comma + 1)))) {
            index = *parsed;
            value.resize(comma);
        }
    }
    value = Trim(value);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    path = ExpandEnvironmentStringsSafe(Trim(value));
    return !path.empty();
}

void CaptureIconHandle(HICON icon, ShellContextMenuItem& item, int quality) {
    if (!icon) {
        return;
    }
    constexpr int size = 32;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !dc || !pixels) {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    std::fill_n(static_cast<std::uint32_t*>(pixels), size * size, 0);
    if (DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL)) {
        CaptureMenuBitmap(bitmap, item, quality);
    }
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
}

void CaptureRegisteredMenuIcon(const std::wstring& verb, ShellContextMenuItem& item) {
    if (verb.empty() || !item.iconPixels.empty()) {
        return;
    }
    const std::array<const wchar_t*, 5> scopes{{
        L"*\\shell\\",
        L"AllFilesystemObjects\\shell\\",
        L"Directory\\shell\\",
        L"Directory\\Background\\shell\\",
        L"Folder\\shell\\",
    }};
    std::wstring iconValue;
    std::wstring commandValue;
    for (const wchar_t* scope : scopes) {
        const std::wstring key = std::wstring(scope) + verb;
        iconValue = RegistryString(HKEY_CLASSES_ROOT, key, L"Icon");
        if (iconValue.empty()) {
            commandValue = RegistryString(HKEY_CLASSES_ROOT, key + L"\\command", nullptr);
        }
        if (!iconValue.empty() || !commandValue.empty()) {
            break;
        }
    }
    if (iconValue.empty() && !commandValue.empty()) {
        iconValue = ExecutableFromCommand(commandValue);
    }
    std::wstring iconPath;
    int iconIndex = 0;
    if (!ParseIconLocation(iconValue, iconPath, iconIndex)) {
        return;
    }
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    if (ExtractIconExW(iconPath.c_str(), iconIndex, &largeIcon, &smallIcon, 1) == 0) {
        return;
    }
    CaptureIconHandle(smallIcon ? smallIcon : largeIcon, item, 1);
    if (smallIcon) DestroyIcon(smallIcon);
    if (largeIcon) DestroyIcon(largeIcon);
}

std::wstring DetectProviderId(const std::wstring& text, const std::wstring& verb) {
    const std::wstring lowerText = ToLower(text);
    const std::wstring lowerVerb = ToLower(verb);
    if (lowerText.find(L"tortoisesvn") != std::wstring::npos ||
        lowerText.rfind(L"svn ", 0) == 0 || lowerText == L"svn" ||
        lowerVerb.find(L"svn") != std::wstring::npos) {
        return ShellContextMenuProviderId::Svn;
    }
    if (lowerText.find(L"tortoisegit") != std::wstring::npos ||
        lowerText.rfind(L"git ", 0) == 0 || lowerText == L"git" ||
        lowerVerb.find(L"git") != std::wstring::npos) {
        return ShellContextMenuProviderId::Git;
    }
    if (lowerText.find(L"visual studio code") != std::wstring::npos ||
        lowerText.find(L"open with code") != std::wstring::npos ||
        lowerText.find(L"通过 code 打开") != std::wstring::npos ||
        lowerText.find(L"使用 code 打开") != std::wstring::npos ||
        lowerText.find(L"用 code 打开") != std::wstring::npos ||
        lowerText.find(L"在 code 中打开") != std::wstring::npos ||
        lowerVerb.find(L"vscode") != std::wstring::npos ||
        lowerVerb.find(L"openwithcode") != std::wstring::npos) {
        return ShellContextMenuProviderId::VsCode;
    }
    if (lowerText.find(L"7-zip") != std::wstring::npos ||
        lowerText.find(L"7zip") != std::wstring::npos ||
        lowerText.find(L"nanazip") != std::wstring::npos ||
        lowerText.find(L"winrar") != std::wstring::npos ||
        lowerText.find(L"bandizip") != std::wstring::npos ||
        lowerText.find(L"peazip") != std::wstring::npos ||
        lowerVerb.find(L"7-zip") != std::wstring::npos ||
        lowerVerb.find(L"7zip") != std::wstring::npos ||
        lowerVerb.find(L"nanazip") != std::wstring::npos ||
        lowerVerb.find(L"winrar") != std::wstring::npos ||
        lowerVerb.find(L"bandizip") != std::wstring::npos ||
        lowerVerb.find(L"peazip") != std::wstring::npos) {
        return ShellContextMenuProviderId::Archive;
    }
    if (lowerText.find(L"windows terminal") != std::wstring::npos ||
        lowerText.find(L"open in terminal") != std::wstring::npos ||
        lowerText.find(L"在终端中打开") != std::wstring::npos ||
        lowerText.find(L"powershell") != std::wstring::npos ||
        lowerText.find(L"command window here") != std::wstring::npos ||
        lowerText.find(L"命令窗口") != std::wstring::npos ||
        lowerVerb.find(L"windowsterminal") != std::wstring::npos ||
        lowerVerb.find(L"openinterminal") != std::wstring::npos ||
        lowerVerb.find(L"powershell") != std::wstring::npos) {
        return ShellContextMenuProviderId::Terminal;
    }
    return {};
}

void InitializeSubmenu(NativeContextMenuSession& session, HMENU submenu, int position) {
    if (!submenu) {
        return;
    }
    if (session.contextMenu3) {
        LRESULT result = 0;
        session.contextMenu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), MAKELPARAM(position, FALSE), &result);
    } else if (session.contextMenu2) {
        session.contextMenu2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), MAKELPARAM(position, FALSE));
    }
}

bool ForwardContextMenuMessage(
    NativeContextMenuSession& session,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT* result = nullptr) {
    LRESULT localResult = 0;
    if (session.contextMenu3 && SUCCEEDED(session.contextMenu3->HandleMenuMsg2(
            message, wParam, lParam, result ? result : &localResult))) {
        return true;
    }
    return session.contextMenu2 && SUCCEEDED(session.contextMenu2->HandleMenuMsg(message, wParam, lParam));
}

void CaptureOwnerDrawMenuIcon(
    NativeContextMenuSession& session,
    HMENU menu,
    const MENUITEMINFOW& info,
    ShellContextMenuItem& item) {
    if ((!session.contextMenu2 && !session.contextMenu3) || item.separator) {
        return;
    }

    MEASUREITEMSTRUCT measure{};
    measure.CtlType = ODT_MENU;
    measure.itemID = info.wID;
    measure.itemWidth = 240;
    measure.itemHeight = static_cast<UINT>(std::max(20, GetSystemMetrics(SM_CYMENU)));
    measure.itemData = info.dwItemData;
    ForwardContextMenuMessage(session, WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure));

    const int width = std::clamp(static_cast<int>(measure.itemWidth), 80, 512);
    const int height = std::clamp(static_cast<int>(measure.itemHeight), 16, 64);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !dc || !rawPixels) {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const COLORREF menuColor = GetSysColor(COLOR_MENU);
    const std::uint32_t background = 0xFF000000u |
        (static_cast<std::uint32_t>(GetRValue(menuColor)) << 16) |
        (static_cast<std::uint32_t>(GetGValue(menuColor)) << 8) |
        static_cast<std::uint32_t>(GetBValue(menuColor));
    auto* pixels = static_cast<std::uint32_t*>(rawPixels);

    DRAWITEMSTRUCT draw{};
    draw.CtlType = ODT_MENU;
    draw.itemID = info.wID;
    draw.itemAction = ODA_DRAWENTIRE;
    draw.itemState = item.enabled ? 0 : ODS_DISABLED;
    if (item.checked) draw.itemState |= ODS_CHECKED;
    draw.hwndItem = reinterpret_cast<HWND>(menu);
    draw.hDC = dc;
    draw.rcItem = RECT{0, 0, width, height};
    draw.itemData = info.dwItemData;
    // A few shell extensions initialize their callback image list on the first
    // WM_DRAWITEM and only paint it on the next request. Both requests stay in
    // the same native-menu session, so this is deterministic and does not alter
    // the cached menu state.
    for (int attempt = 0; attempt < 2 && item.iconPixels.empty(); ++attempt) {
        std::fill_n(pixels, static_cast<std::size_t>(width) * height, background);
        const bool rendered = ForwardContextMenuMessage(
            session, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
        if (!rendered) {
            continue;
        }
        const int iconZoneWidth = std::min(width, std::max(24, GetSystemMetrics(SM_CXMENUCHECK) + 10));
        int minX = iconZoneWidth;
        int minY = height;
        int maxX = -1;
        int maxY = -1;
        const std::uint32_t backgroundRgb = background & 0x00FFFFFFu;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < iconZoneWidth; ++x) {
                const std::uint32_t rgb = pixels[static_cast<std::size_t>(y) * width + x] & 0x00FFFFFFu;
                if (rgb == backgroundRgb) {
                    continue;
                }
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
        if (maxX >= minX && maxY >= minY) {
            const int capturedWidth = maxX - minX + 1;
            const int capturedHeight = maxY - minY + 1;
            std::vector<std::uint32_t> captured(
                static_cast<std::size_t>(capturedWidth) * capturedHeight);
            for (int y = 0; y < capturedHeight; ++y) {
                for (int x = 0; x < capturedWidth; ++x) {
                    const std::uint32_t pixel = pixels[
                        static_cast<std::size_t>(minY + y) * width + minX + x];
                    captured[static_cast<std::size_t>(y) * capturedWidth + x] =
                        (pixel & 0x00FFFFFFu) == backgroundRgb ? 0 : (0xFF000000u | (pixel & 0x00FFFFFFu));
                }
            }
            item.iconWidth = capturedWidth;
            item.iconHeight = capturedHeight;
            item.iconQuality = 2;
            item.iconPixels = std::move(captured);
        }
    }

    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
}

std::vector<ShellContextMenuItem> CollectTrackedItems(
    NativeContextMenuSession& session,
    HMENU menu,
    const std::wstring& inheritedProviderId,
    const ShellContextMenuTrackingOptions& tracking,
    bool initializeTrackedSubmenus,
    int depth) {
    std::vector<ShellContextMenuItem> result;
    if (!menu || depth > 3) {
        return result;
    }
    const int count = GetMenuItemCount(menu);
    for (int position = 0; position < count && result.size() < 100; ++position) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_BITMAP | MIIM_DATA;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info)) {
            continue;
        }
        if ((info.fType & MFT_SEPARATOR) != 0) {
            if (tracking.Includes(inheritedProviderId) && !result.empty() && !result.back().separator) {
                ShellContextMenuItem separator;
                separator.providerId = inheritedProviderId;
                separator.separator = true;
                result.push_back(std::move(separator));
            }
            continue;
        }

        ShellContextMenuItem item;
        item.text = MenuText(menu, static_cast<UINT>(position));
        item.verb = CommandVerb(session.contextMenu, info.wID);
        item.providerId = DetectProviderId(item.text, item.verb);
        if (item.providerId.empty()) {
            item.providerId = inheritedProviderId;
        }
        item.enabled = (info.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
        item.checked = (info.fState & MFS_CHECKED) != 0;
        CaptureMenuBitmap(info.hbmpItem, item, 3);
        if (item.iconPixels.empty() &&
            (info.hbmpItem == HBMMENU_CALLBACK || (info.fType & MFT_OWNERDRAW) != 0)) {
            CaptureOwnerDrawMenuIcon(session, menu, info, item);
        }
        CaptureRegisteredMenuIcon(item.verb, item);

        if (info.hSubMenu) {
            if (initializeTrackedSubmenus && tracking.Includes(item.providerId)) {
                InitializeSubmenu(session, info.hSubMenu, position);
            }
            item.children = CollectTrackedItems(
                session,
                info.hSubMenu,
                item.providerId,
                tracking,
                initializeTrackedSubmenus,
                depth + 1);
            if (item.providerId.empty() && !item.children.empty()) {
                item.providerId = item.children.front().providerId;
            }
        }

        if (tracking.Includes(item.providerId) && (!item.text.empty() || !item.children.empty())) {
            while (!item.children.empty() && item.children.back().separator) {
                item.children.pop_back();
            }
            result.push_back(std::move(item));
        }
    }
    while (!result.empty() && result.back().separator) {
        result.pop_back();
    }
    return result;
}

LRESULT CALLBACK NativeContextMenuSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR referenceData) {
    auto* session = reinterpret_cast<NativeContextMenuSession*>(referenceData);
    if (session && (message == WM_INITMENUPOPUP || message == WM_DRAWITEM ||
                    message == WM_MEASUREITEM || message == WM_MENUCHAR)) {
        if (session->contextMenu3) {
            LRESULT result = 0;
            if (SUCCEEDED(session->contextMenu3->HandleMenuMsg2(message, wParam, lParam, &result))) {
                return result;
            }
        } else if (session->contextMenu2 && SUCCEEDED(session->contextMenu2->HandleMenuMsg(message, wParam, lParam))) {
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, NativeContextMenuSubclassProc, subclassId);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

bool LocatorMatches(
    const ShellContextMenuLocator& locator,
    const std::wstring& providerId,
    const std::vector<std::wstring>& path,
    const std::wstring& verb) {
    if (providerId != locator.providerId) {
        return false;
    }
    if (!locator.verb.empty() && !verb.empty() && ToLower(locator.verb) == ToLower(verb)) {
        return true;
    }
    if (path.size() != locator.path.size()) {
        return false;
    }
    for (std::size_t index = 0; index < path.size(); ++index) {
        if (ToLower(path[index]) != ToLower(locator.path[index])) {
            return false;
        }
    }
    return true;
}

std::optional<UINT> FindTrackedCommand(
    NativeContextMenuSession& session,
    HMENU menu,
    const ShellContextMenuLocator& locator,
    const std::wstring& inheritedProviderId,
    std::vector<std::wstring>& path,
    int depth) {
    if (!menu || depth > 3) {
        return std::nullopt;
    }
    const int count = GetMenuItemCount(menu);
    for (int position = 0; position < count; ++position) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info) || (info.fType & MFT_SEPARATOR) != 0) {
            continue;
        }
        const std::wstring text = MenuText(menu, static_cast<UINT>(position));
        const std::wstring verb = CommandVerb(session.contextMenu, info.wID);
        std::wstring providerId = DetectProviderId(text, verb);
        if (providerId.empty()) {
            providerId = inheritedProviderId;
        }
        path.push_back(text);
        if (info.hSubMenu) {
            if (providerId == locator.providerId || providerId.empty()) {
                InitializeSubmenu(session, info.hSubMenu, position);
                if (auto found = FindTrackedCommand(session, info.hSubMenu, locator, providerId, path, depth + 1)) {
                    return found;
                }
            }
        } else if ((info.fState & (MFS_DISABLED | MFS_GRAYED)) == 0 &&
                   LocatorMatches(locator, providerId, path, verb)) {
            return info.wID;
        }
        path.pop_back();
    }
    return std::nullopt;
}

}

bool ShellContextMenuTrackingOptions::Includes(const std::wstring& providerId) const {
    return (git && providerId == ShellContextMenuProviderId::Git) ||
           (svn && providerId == ShellContextMenuProviderId::Svn) ||
           (vsCode && providerId == ShellContextMenuProviderId::VsCode) ||
           (terminal && providerId == ShellContextMenuProviderId::Terminal) ||
           (archive && providerId == ShellContextMenuProviderId::Archive);
}

bool ShellContextMenuTrackingOptions::Any() const {
    return git || svn || vsCode || terminal || archive;
}

std::wstring ShellItemService::DetectTrackedContextMenuProvider(
    const std::wstring& text,
    const std::wstring& verb) {
    return DetectProviderId(text, verb);
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

bool ShellItemService::QueryTrackedContextMenu(
    HWND owner,
    const Link& link,
    const ShellContextMenuTrackingOptions& tracking,
    ShellContextMenuSnapshot& snapshot) {
    snapshot = {};
    NativeContextMenuSession session;
    if (!CreateNativeContextMenuSession(owner, link, session)) {
        return false;
    }
    snapshot.items = CollectTrackedItems(
        session, session.menu, {}, tracking, true, 0);
    snapshot.complete = true;
    return true;
}

bool ShellItemService::ShowNativeContextMenu(
    HWND owner,
    const Link& link,
    POINT screenPoint,
    const ShellContextMenuTrackingOptions& tracking,
    ShellContextMenuSnapshot* snapshot) {
    if (snapshot) {
        *snapshot = {};
    }
    NativeContextMenuSession session;
    if (!CreateNativeContextMenuSession(owner, link, session)) {
        return false;
    }

    constexpr UINT_PTR kContextMenuSubclassId = 0x51434D;
    SetWindowSubclass(
        owner,
        NativeContextMenuSubclassProc,
        kContextMenuSubclassId,
        reinterpret_cast<DWORD_PTR>(&session));
    ActivateWindow(owner);
    const UINT command = TrackPopupMenu(
        session.menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        owner,
        nullptr);
    RemoveWindowSubclass(owner, NativeContextMenuSubclassProc, kContextMenuSubclassId);

    if (snapshot) {
        snapshot->items = CollectTrackedItems(
            session, session.menu, {}, tracking, false, 0);
        snapshot->complete = true;
    }
    if (command == 0) {
        return true;
    }

    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    invoke.hwnd = owner;
    invoke.lpVerb = MAKEINTRESOURCEA(command - 1);
    invoke.lpVerbW = MAKEINTRESOURCEW(command - 1);
    invoke.nShow = SW_SHOWNORMAL;
    return SUCCEEDED(session.contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke)));
}

bool ShellItemService::InvokeTrackedContextMenuItem(
    HWND owner,
    const Link& link,
    const ShellContextMenuLocator& locator,
    std::wstring& errorMessage) {
    errorMessage.clear();
    NativeContextMenuSession session;
    if (!CreateNativeContextMenuSession(owner, link, session)) {
        errorMessage = L"无法读取当前 Windows 原生菜单。";
        return false;
    }
    std::vector<std::wstring> path;
    const auto command = FindTrackedCommand(
        session, session.menu, locator, {}, path, 0);
    if (!command || *command < 1) {
        errorMessage = L"该原生菜单已经变化，请点击刷新或重新打开 Windows 原生菜单。";
        return false;
    }
    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    invoke.hwnd = owner;
    invoke.lpVerb = MAKEINTRESOURCEA(*command - 1);
    invoke.lpVerbW = MAKEINTRESOURCEW(*command - 1);
    invoke.nShow = SW_SHOWNORMAL;
    if (FAILED(session.contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke)))) {
        errorMessage = L"执行原生菜单命令失败。";
        return false;
    }
    return true;
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
