#include "SystemFunctionDialog.h"

#include "ShellItemService.h"
#include "Utilities.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <array>
#include <vector>

namespace {
constexpr int kDialogWidth = 520;
constexpr int kDialogHeight = 430;

enum ControlId {
    IdList = 1001,
    IdOk,
    IdCancel,
};

struct SystemFunctionItem {
    const wchar_t* name;
    const wchar_t* target;
    int type;
    const wchar_t* remark;
};

const std::array<SystemFunctionItem, 20> kSystemFunctions{{
    {L"我的电脑", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", 3, L"Windows Shell 系统位置"},
    {L"我的文档", L"shell:Personal", 3, L"当前用户文档目录"},
    {L"网络", L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", 3, L"网络位置"},
    {L"回收站", L"::{645FF040-5081-101B-9F08-00AA002F954E}", 3, L"回收站"},
    {L"控制面板", L"shell:ControlPanelFolder", 3, L"控制面板"},
    {L"注册表", L"%windir%\\regedit.exe", 0, L"注册表编辑器"},
    {L"计算器", L"calc.exe", 0, L"Windows 计算器"},
    {L"命令行", L"%ComSpec%", 0, L"命令提示符"},
    {L"记事本", L"notepad.exe", 0, L"记事本"},
    {L"画图", L"mspaint.exe", 0, L"画图"},
    {L"组策略", L"gpedit.msc", 0, L"本地组策略编辑器"},
    {L"任务管理器", L"taskmgr.exe", 0, L"任务管理器"},
    {L"系统盘根目录", L"%SystemDrive%\\", 1, L"系统盘根目录"},
    {L"用户目录", L"%USERPROFILE%", 1, L"当前用户目录"},
    {L"AppData", L"%APPDATA%", 1, L"Roaming AppData"},
    {L"最近使用项目", L"shell:Recent", 3, L"最近使用项目"},
    {L"Win管理工具", L"shell:Administrative Tools", 3, L"Windows 管理工具"},
    {L"服务", L"services.msc", 0, L"服务管理器"},
    {L"计算机管理", L"compmgmt.msc", 0, L"计算机管理"},
    {L"远程桌面连接", L"mstsc.exe", 0, L"远程桌面连接"},
}};

void SetControlFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::wstring ResolveSystemIconTarget(const std::wstring& value) {
    const std::wstring expanded = ExpandEnvironmentStringsSafe(value);
    if (expanded.empty()) {
        return {};
    }
    if (PathIsRelativeW(expanded.c_str()) && expanded.find(L'\\') == std::wstring::npos && expanded.find(L'/') == std::wstring::npos) {
        wchar_t resolved[MAX_PATH]{};
        if (SearchPathW(nullptr, expanded.c_str(), nullptr, static_cast<DWORD>(sizeof(resolved) / sizeof(resolved[0])), resolved, nullptr) > 0) {
            return resolved;
        }
    }
    return expanded;
}

int SystemImageIndex(const SystemFunctionItem& item) {
    const std::wstring target = ResolveSystemIconTarget(item.target);
    SHFILEINFOW iconInfo{};
    if (ShellItemService::IsShellParseName(target)) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            const DWORD_PTR ok = SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(pidl),
                0,
                &iconInfo,
                sizeof(iconInfo),
                SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
            CoTaskMemFree(pidl);
            if (ok) {
                return iconInfo.iIcon;
            }
        }
    }

    if (SHGetFileInfoW(target.c_str(), 0, &iconInfo, sizeof(iconInfo), SHGFI_SYSICONINDEX | SHGFI_LARGEICON)) {
        return iconInfo.iIcon;
    }
    if (SHGetFileInfoW(
            target.c_str(),
            item.type == 1 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
            &iconInfo,
            sizeof(iconInfo),
            SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
        return iconInfo.iIcon;
    }
    return 0;
}

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, Link& link)
        : owner_(owner), instance_(instance), link_(link) {}

    bool Run() {
        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&controls);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DialogWindow::WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"QuattroSystemFunctionDialog";
        RegisterClassExW(&wc);

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - kDialogWidth) / 2;
        const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - kDialogHeight) / 2;

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"系统功能",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            x,
            y,
            kDialogWidth,
            kDialogHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return false;
        }

        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }
        return accepted_;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        DialogWindow* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<DialogWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            return 1;
        }
        case WM_NOTIFY: {
            const auto* notify = reinterpret_cast<NMHDR*>(lParam);
            if (notify && notify->idFrom == IdList && notify->code == NM_DBLCLK) {
                Accept();
                return 0;
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IdOk) {
                Accept();
                return 0;
            }
            if (LOWORD(wParam) == IdCancel) {
                Close(false);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            Close(false);
            return 0;
        case WM_DESTROY:
            if (font_ && ownsFont_) {
                DeleteObject(font_);
            }
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        font_ = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        ownsFont_ = font_ != nullptr;
        if (!font_) {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        list_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            14,
            14,
            488,
            330,
            hwnd_,
            reinterpret_cast<HMENU>(IdList),
            instance_,
            nullptr);
        SetControlFont(list_, font_);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT);

        SHFILEINFOW info{};
        HIMAGELIST imageList = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
            L"C:\\",
            FILE_ATTRIBUTE_DIRECTORY,
            &info,
            sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES));
        if (imageList) {
            ListView_SetImageList(list_, imageList, LVSIL_NORMAL);
        }

        for (int i = 0; i < static_cast<int>(kSystemFunctions.size()); ++i) {
            const auto& item = kSystemFunctions[static_cast<std::size_t>(i)];
            const int imageIndex = SystemImageIndex(item);

            LVITEMW row{};
            row.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
            row.iItem = i;
            row.pszText = const_cast<wchar_t*>(item.name);
            row.iImage = imageIndex;
            row.lParam = i;
            ListView_InsertItem(list_, &row);
        }
        ListView_SetItemState(list_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

        HWND ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  336, 360, 76, 28, hwnd_, reinterpret_cast<HMENU>(IdOk), instance_, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      426, 360, 76, 28, hwnd_, reinterpret_cast<HMENU>(IdCancel), instance_, nullptr);
        SetControlFont(ok, font_);
        SetControlFont(cancel, font_);
    }

    void Accept() {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0 || selected >= static_cast<int>(kSystemFunctions.size())) {
            return;
        }

        const auto& item = kSystemFunctions[static_cast<std::size_t>(selected)];
        Link next = link_;
        next.name = item.name;
        next.path = item.target;
        next.type = item.type;
        next.parameter.clear();
        next.workDir.clear();
        next.icon.clear();
        next.remark = item.remark;
        next.showCmd = SW_SHOWNORMAL;
        next.isAdmin = false;
        ShellItemService::RefreshLinkShellData(next, true);
        link_ = std::move(next);
        Close(true);
    }

    void Close(bool accepted) {
        accepted_ = accepted;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    Link& link_;
    bool done_ = false;
    bool accepted_ = false;
    HFONT font_ = nullptr;
    bool ownsFont_ = false;
    HWND list_ = nullptr;
};
}

bool SystemFunctionDialog::Show(HWND owner, HINSTANCE instance, Link& link) {
    DialogWindow dialog(owner, instance, link);
    return dialog.Run();
}
