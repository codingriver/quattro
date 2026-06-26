#include "SystemFunctionDialog.h"

#include "MenuCatalog.h"
#include "ShellItemService.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <array>
#include <algorithm>
#include <vector>

namespace {
constexpr int kDialogWidth = 520;
constexpr int kDialogHeight = 430;

enum ControlId {
    IdList = 1001,
    IdOk,
    IdCancel,
};

const std::array<SystemFunctionDefinition, 36> kSystemFunctions{{
    {L"我的电脑", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"", 3, SW_SHOWNORMAL, L"Windows Shell 系统位置"},
    {L"我的文档", L"shell:Personal", L"", 3, SW_SHOWNORMAL, L"当前用户文档目录"},
    {L"网络", L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", L"", 3, SW_SHOWNORMAL, L"网络位置"},
    {L"回收站", L"::{645FF040-5081-101B-9F08-00AA002F954E}", L"", 3, SW_SHOWNORMAL, L"回收站"},
    {L"控制面板", L"shell:ControlPanelFolder", L"", 3, SW_SHOWNORMAL, L"控制面板"},
    {L"注册表", L"%windir%\\regedit.exe", L"", 0, SW_SHOWNORMAL, L"注册表编辑器", -1, MenuIconWindows},
    {L"计算器", L"calc.exe", L"", 0, SW_SHOWNORMAL, L"Windows 计算器", -1, MenuIconCalculator},
    {L"命令行", L"%ComSpec%", L"", 0, SW_SHOWNORMAL, L"命令提示符", -1, MenuIconTerminal},
    {L"记事本", L"notepad.exe", L"", 0, SW_SHOWNORMAL, L"记事本", -1, MenuIconNotebook},
    {L"画图", L"mspaint.exe", L"", 0, SW_SHOWNORMAL, L"画图", -1, MenuIconTheme},
    {L"组策略", L"gpedit.msc", L"", 0, SW_SHOWNORMAL, L"本地组策略编辑器", -1, MenuIconTools},
    {L"任务管理器", L"taskmgr.exe", L"", 0, SW_SHOWNORMAL, L"任务管理器", -1, MenuIconSystem},
    {L"完全控制面板", L"shell:::{ED7BA470-8E54-465E-825C-99712043E01C}", L"", 3, SW_SHOWNORMAL, L"控制面板全部任务", -1, MenuIconSettings},
    {L"系统盘根目录", L"%SystemDrive%\\", L"", 1, SW_SHOWNORMAL, L"系统盘根目录"},
    {L"用户目录", L"%USERPROFILE%", L"", 1, SW_SHOWNORMAL, L"当前用户目录"},
    {L"AppData", L"%APPDATA%", L"", 1, SW_SHOWNORMAL, L"Roaming AppData"},
    {L"系统自启目录", L"shell:Common Startup", L"", 3, SW_SHOWNORMAL, L"所有用户启动目录"},
    {L"用户自启目录", L"shell:Startup", L"", 3, SW_SHOWNORMAL, L"当前用户启动目录"},
    {L"最近使用项目", L"shell:Recent", L"", 3, SW_SHOWNORMAL, L"最近使用项目"},
    {L"Win管理工具", L"shell:Administrative Tools", L"", 3, SW_SHOWNORMAL, L"Windows 管理工具"},
    {L"服务", L"services.msc", L"", 0, SW_SHOWNORMAL, L"服务管理器"},
    {L"计算机管理", L"compmgmt.msc", L"", 0, SW_SHOWNORMAL, L"计算机管理"},
    {L"远程桌面连接", L"mstsc.exe", L"", 0, SW_SHOWNORMAL, L"远程桌面连接"},
    {L"用户证书", L"certmgr.msc", L"", 0, SW_SHOWNORMAL, L"当前用户证书管理"},
    {L"关闭显示器", L"powershell.exe", L"-NoProfile -WindowStyle Hidden -Command \"Add-Type -MemberDefinition '[DllImport(\\\"user32.dll\\\")]public static extern int SendMessage(int hWnd,int hMsg,int wParam,int lParam);' -Name Native -Namespace Win32; [Win32.Native]::SendMessage(0xffff,0x0112,0xf170,2)\"", 0, SW_HIDE, L"关闭显示器", -1, MenuIconMonitor},
    {L"关机", L"shutdown.exe", L"/s /t 0", 0, SW_HIDE, L"关闭计算机", -1, MenuIconPower},
    {L"重启", L"shutdown.exe", L"/r /t 0", 0, SW_HIDE, L"重启计算机", -1, MenuIconRestart},
    {L"注销", L"shutdown.exe", L"/l", 0, SW_HIDE, L"注销当前用户", -1, MenuIconLogout},
    {L"锁定", L"rundll32.exe", L"user32.dll,LockWorkStation", 0, SW_HIDE, L"锁定当前会话", -1, MenuIconLock},
    {L"休眠", L"shutdown.exe", L"/h", 0, SW_HIDE, L"休眠计算机", -1, MenuIconSleep},
    {L"睡眠", L"rundll32.exe", L"powrprof.dll,SetSuspendState 0,1,0", 0, SW_HIDE, L"睡眠计算机", -1, MenuIconSleep},
    {L"hosts", L"notepad.exe", L"%windir%\\System32\\drivers\\etc\\hosts", 0, SW_SHOWNORMAL, L"编辑 hosts 文件", -1, MenuIconNotebook},
    {L"环境变量", L"rundll32.exe", L"sysdm.cpl,EditEnvironmentVariables", 0, SW_SHOWNORMAL, L"编辑环境变量", -1, MenuIconEnvironment},
    {L"音量+", L"powershell.exe", L"-NoProfile -WindowStyle Hidden -Command \"(New-Object -ComObject WScript.Shell).SendKeys([char]175)\"", 0, SW_HIDE, L"提高系统音量", -1, MenuIconVolumeUp},
    {L"音量-", L"powershell.exe", L"-NoProfile -WindowStyle Hidden -Command \"(New-Object -ComObject WScript.Shell).SendKeys([char]174)\"", 0, SW_HIDE, L"降低系统音量", -1, MenuIconVolumeDown},
    {L"静音", L"powershell.exe", L"-NoProfile -WindowStyle Hidden -Command \"(New-Object -ComObject WScript.Shell).SendKeys([char]173)\"", 0, SW_HIDE, L"切换静音", -1, MenuIconVolumeMute},
}};

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
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

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme, Link& link)
        : owner_(owner), instance_(instance), theme_(theme), link_(link) {}

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
        wc.hbrBackground = nullptr;
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
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
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

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
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
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            PaintBackground(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintBackground(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_ERASEBKGND: {
            return 1;
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
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
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (font_ && ownsFont_) {
                DeleteObject(font_);
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        font_ = ThemedControls::CreateDialogFont();
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
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT);
        ListView_SetBkColor(list_, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
        ListView_SetTextBkColor(list_, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
        ListView_SetTextColor(list_, ToColorRef(theme_.color(L"list", L"normal", L"text")));

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
            const int imageIndex = SystemFunctionImageIndex(item);

            LVITEMW row{};
            row.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
            row.iItem = i;
            row.pszText = const_cast<wchar_t*>(item.name);
            row.iImage = imageIndex;
            row.lParam = i;
            ListView_InsertItem(list_, &row);
        }
        ListView_SetItemState(list_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        ThemedControls::CreateButton(instance_, hwnd_, IdOk, L"确定", 336, 360, 76, buttonHeight, font_, true);
        ThemedControls::CreateButton(instance_, hwnd_, IdCancel, L"取消", 426, 360, 76, buttonHeight, font_);
    }

    void PaintBackground(HDC dc) {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    void Accept() {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (!ConfigureSystemFunctionLink(static_cast<std::size_t>(selected), link_)) {
            return;
        }
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
    const Theme& theme_;
    Link& link_;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
    bool accepted_ = false;
    HFONT font_ = nullptr;
    bool ownsFont_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    HWND list_ = nullptr;
};
}

std::span<const SystemFunctionDefinition> SystemFunctions() {
    return kSystemFunctions;
}

int SystemFunctionImageIndex(const SystemFunctionDefinition& item) {
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

bool ConfigureSystemFunctionLink(std::size_t index, Link& link) {
    if (index >= kSystemFunctions.size()) {
        return false;
    }

    const auto& item = kSystemFunctions[index];
    Link next = link;
    next.name = item.name;
    next.path = item.target;
    next.type = item.type;
    next.parameter = item.parameter;
    next.workDir.clear();
    next.icon.clear();
    next.remark = item.remark;
    next.showCmd = item.showCmd;
    next.isAdmin = false;
    ShellItemService::RefreshLinkShellData(next, true);
    link = std::move(next);
    return true;
}

bool SystemFunctionDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, Link& link) {
    DialogWindow dialog(owner, instance, theme, link);
    return dialog.Run();
}

