#include "SimpleDialogs.h"

#include "AppLog.h"
#include "HotKeyEditor.h"
#include "Utilities.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace {
constexpr int ID_MAIN_HOTKEY_CAPTURE = 301;
constexpr int ID_MAIN_HOTKEY_CLEAR = 302;
constexpr int ID_SEARCH_HOTKEY_CAPTURE = 303;
constexpr int ID_SEARCH_HOTKEY_CLEAR = 304;
constexpr int ID_GROUP_WIDTH = 401;
constexpr int ID_TAG_WIDTH = 402;
constexpr int ID_DOCK_DELAY = 403;
constexpr int ID_GROUP_DELAY = 404;
constexpr int ID_TAG_DELAY = 405;
constexpr int ID_SEARCH_COUNT = 406;

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void SetFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

class TextDialog {
public:
    TextDialog(HWND owner, HINSTANCE instance, std::wstring title, std::wstring label, std::wstring& value)
        : owner_(owner), instance_(instance), title_(std::move(title)), label_(std::move(label)), value_(value) {}

    bool Run() {
        const std::wstring className = L"QuattroTextInputDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TextDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = className.c_str();
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(L"文本输入窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        SetLastError(ERROR_SUCCESS);
        hwnd_ = nullptr;
        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            title_.c_str(),
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 80,
            ownerRect.top + 100,
            420,
            170,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            const DWORD error = GetLastError();
            WriteAppLog(L"文本输入窗口创建失败: " + FormatLastError(error));
            return false;
        }
        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        TextDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<TextDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<TextDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HWND label = CreateWindowExW(0, L"STATIC", label_.c_str(), WS_CHILD | WS_VISIBLE, 22, 24, 360, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font);
            edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value_.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    22, 54, 360, 24, hwnd_, reinterpret_cast<HMENU>(100), instance_, nullptr);
            SetFont(edit_, font);
            HWND ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 214, 96, 76, 28, hwnd_, reinterpret_cast<HMENU>(IDOK), instance_, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 306, 96, 76, 28, hwnd_, reinterpret_cast<HMENU>(IDCANCEL), instance_, nullptr);
            SetFont(ok, font);
            SetFont(cancel, font);
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                std::wstring next = Trim(GetText(edit_));
                if (next.empty()) {
                    MessageBoxW(hwnd_, L"名称不能为空。", title_.c_str(), MB_OK | MB_ICONWARNING);
                    return 0;
                }
                value_ = next;
                accepted_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    std::wstring title_;
    std::wstring label_;
    std::wstring& value_;
    bool accepted_ = false;
    bool done_ = false;
};

class SettingsDialog {
public:
    SettingsDialog(HWND owner, HINSTANCE instance, AppConfig& config)
        : owner_(owner), instance_(instance), config_(config), draft_(config) {}

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"QuattroSettingsDialog";
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(L"设置窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"设置",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 60,
            ownerRect.top + 70,
            520,
            900,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(L"设置窗口创建失败: " + FormatLastError(GetLastError()));
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
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        SettingsDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<SettingsDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND CheckBox(int id, const wchar_t* text, int x, int y, bool checked) {
        HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    x, y, 190, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SetFont(hwnd, font_);
        SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        return hwnd;
    }

    HWND NumberEdit(int id, int x, int y, int width, int value) {
        HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(value).c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                    x, y, width, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SetFont(hwnd, font_);
        return hwnd;
    }

    int ClampNumber(HWND edit, int minValue, int maxValue, int fallback) const {
        auto value = ParseInt(GetText(edit));
        if (!value) {
            return fallback;
        }
        return std::max(minValue, std::min(maxValue, *value));
    }

    void SelectTagAlign() {
        int index = 1;
        if (draft_.tagAlign == L"left") {
            index = 0;
        } else if (draft_.tagAlign == L"right") {
            index = 2;
        }
        SendMessageW(tagAlignCombo_, CB_SETCURSEL, index, 0);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            showTitle_ = CheckBox(101, L"显示标题栏", 26, 20, draft_.showTitle);
            showGroup_ = CheckBox(102, L"显示分组栏", 250, 20, draft_.showGroup);
            showTag_ = CheckBox(103, L"显示标签栏", 26, 50, draft_.showTag);
            topMost_ = CheckBox(104, L"窗口置顶", 250, 50, draft_.topMost);
            autoDock_ = CheckBox(105, L"自动停靠", 26, 80, draft_.autoDock);
            hideInactive_ = CheckBox(106, L"失焦隐藏", 250, 80, draft_.hideWhenInactive);
            hideAfterLink_ = CheckBox(107, L"运行后隐藏", 26, 110, draft_.hideAfterLink);
            hideOnStart_ = CheckBox(116, L"启动后隐藏", 250, 110, draft_.hideOnStart);
            showRunCount_ = CheckBox(108, L"显示运行次数", 26, 140, draft_.showRunCount);
            doubleClick_ = CheckBox(109, L"双击运行", 250, 140, draft_.doubleClickToRun);
            hideNotify_ = CheckBox(110, L"隐藏托盘图标", 26, 170, draft_.hideNotifyIcon);
            deleteConfirm_ = CheckBox(111, L"删除确认", 250, 170, draft_.deleteConfirm);
            saveRunCount_ = CheckBox(112, L"保存运行次数", 26, 200, draft_.saveRunCount);
            showDate_ = CheckBox(113, L"显示日期", 250, 200, draft_.showDate);
            showSearchButton_ = CheckBox(114, L"显示搜索按钮", 26, 230, draft_.showSearchButton);
            showMenuButton_ = CheckBox(115, L"显示菜单按钮", 250, 230, draft_.showMenuButton);
            showSkinButton_ = CheckBox(121, L"显示主题按钮", 26, 260, draft_.showSkinButton);
            autoRun_ = CheckBox(117, L"开机自启动", 250, 260, draft_.autoRun);
            linkNameSingleLine_ = CheckBox(118, L"启动项名称单行", 26, 290, draft_.linkNameSingleLine);
            showTooltip_ = CheckBox(119, L"显示提示", 250, 290, draft_.showTooltip);
            groupRight_ = CheckBox(120, L"分组栏在右侧", 26, 320, draft_.groupRight);
            tagRight_ = CheckBox(122, L"标签栏在右侧", 250, 320, draft_.tagRight);
            focusSearch_ = CheckBox(123, L"打开搜索时聚焦输入框", 26, 350, draft_.focusSearch);
            enterActiveGroup_ = CheckBox(124, L"鼠标进入激活分组", 250, 350, draft_.mouseEnterActiveGroup);
            enterActiveTag_ = CheckBox(125, L"鼠标进入激活标签", 26, 380, draft_.mouseEnterActiveTag);

            HWND label = CreateWindowExW(0, L"STATIC", L"透明度", WS_CHILD | WS_VISIBLE, 26, 416, 70, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            alphaEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(draft_.alpha).c_str(),
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                         130, 412, 72, 24, hwnd_, reinterpret_cast<HMENU>(201), instance_, nullptr);
            SetFont(alphaEdit_, font_);

            label = CreateWindowExW(0, L"STATIC", L"标签文字", WS_CHILD | WS_VISIBLE, 250, 416, 70, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            tagAlignCombo_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                             326, 412, 112, 100, hwnd_, nullptr, instance_, nullptr);
            SetFont(tagAlignCombo_, font_);
            SendMessageW(tagAlignCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"左对齐"));
            SendMessageW(tagAlignCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"居中"));
            SendMessageW(tagAlignCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"右对齐"));
            SelectTagAlign();

            label = CreateWindowExW(0, L"STATIC", L"分组宽度", WS_CHILD | WS_VISIBLE, 26, 452, 90, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            groupWidthEdit_ = NumberEdit(ID_GROUP_WIDTH, 130, 448, 72, draft_.groupWidth);
            label = CreateWindowExW(0, L"STATIC", L"标签宽度", WS_CHILD | WS_VISIBLE, 250, 452, 90, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            tagWidthEdit_ = NumberEdit(ID_TAG_WIDTH, 366, 448, 72, draft_.tagWidth);

            label = CreateWindowExW(0, L"STATIC", L"停靠延迟", WS_CHILD | WS_VISIBLE, 26, 488, 90, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            dockDelayEdit_ = NumberEdit(ID_DOCK_DELAY, 130, 484, 72, draft_.dockDelay);
            label = CreateWindowExW(0, L"STATIC", L"搜索计数", WS_CHILD | WS_VISIBLE, 250, 488, 90, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            searchCountEdit_ = NumberEdit(ID_SEARCH_COUNT, 366, 484, 72, draft_.searchCount);

            label = CreateWindowExW(0, L"STATIC", L"分组激活延迟", WS_CHILD | WS_VISIBLE, 26, 524, 100, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            groupDelayEdit_ = NumberEdit(ID_GROUP_DELAY, 130, 520, 72, draft_.activeGroupDelay);
            label = CreateWindowExW(0, L"STATIC", L"标签激活延迟", WS_CHILD | WS_VISIBLE, 250, 524, 100, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            tagDelayEdit_ = NumberEdit(ID_TAG_DELAY, 366, 520, 72, draft_.activeTagDelay);

            label = CreateWindowExW(0, L"STATIC", L"主窗口热键", WS_CHILD | WS_VISIBLE, 26, 560, 90, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            mainHotKeyText_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", FormatHotKeyText(draft_.mainHotKey).c_str(),
                                              WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                              130, 556, 180, 24, hwnd_, nullptr, instance_, nullptr);
            SetFont(mainHotKeyText_, font_);
            HWND button = CreateWindowExW(0, L"BUTTON", L"录入", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          318, 555, 52, 26, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MAIN_HOTKEY_CAPTURE)), instance_, nullptr);
            SetFont(button, font_);
            button = CreateWindowExW(0, L"BUTTON", L"清除", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     386, 555, 52, 26, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MAIN_HOTKEY_CLEAR)), instance_, nullptr);
            SetFont(button, font_);

            label = CreateWindowExW(0, L"STATIC", L"搜索热键", WS_CHILD | WS_VISIBLE, 26, 596, 70, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            searchHotKeyText_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", FormatHotKeyText(draft_.searchHotKey).c_str(),
                                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                                130, 592, 180, 24, hwnd_, nullptr, instance_, nullptr);
            SetFont(searchHotKeyText_, font_);
            button = CreateWindowExW(0, L"BUTTON", L"录入", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     318, 591, 52, 26, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_HOTKEY_CAPTURE)), instance_, nullptr);
            SetFont(button, font_);
            button = CreateWindowExW(0, L"BUTTON", L"清除", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     386, 591, 52, 26, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_HOTKEY_CLEAR)), instance_, nullptr);
            SetFont(button, font_);

            label = CreateWindowExW(0, L"STATIC", L"打开目录命令", WS_CHILD | WS_VISIBLE, 26, 632, 100, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            openDirEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft_.openDirCommand.c_str(),
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                           130, 628, 308, 24, hwnd_, reinterpret_cast<HMENU>(202), instance_, nullptr);
            SetFont(openDirEdit_, font_);

            label = CreateWindowExW(0, L"STATIC", L"帮助链接", WS_CHILD | WS_VISIBLE, 26, 668, 100, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            helpUrlEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft_.helpUrl.c_str(),
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                           130, 664, 308, 24, hwnd_, reinterpret_cast<HMENU>(203), instance_, nullptr);
            SetFont(helpUrlEdit_, font_);

            label = CreateWindowExW(0, L"STATIC", L"更新链接", WS_CHILD | WS_VISIBLE, 26, 704, 100, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            updateUrlEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft_.updateUrl.c_str(),
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                             130, 700, 308, 24, hwnd_, reinterpret_cast<HMENU>(204), instance_, nullptr);
            SetFont(updateUrlEdit_, font_);

            label = CreateWindowExW(0, L"STATIC", L"FAQ 链接", WS_CHILD | WS_VISIBLE, 26, 740, 100, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            faqUrlEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft_.faqUrl.c_str(),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                          130, 736, 308, 24, hwnd_, reinterpret_cast<HMENU>(205), instance_, nullptr);
            SetFont(faqUrlEdit_, font_);

            label = CreateWindowExW(0, L"STATIC", L"赞助链接", WS_CHILD | WS_VISIBLE, 26, 776, 100, 22, hwnd_, nullptr, instance_, nullptr);
            SetFont(label, font_);
            rewardUrlEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft_.rewardUrl.c_str(),
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                             130, 772, 308, 24, hwnd_, reinterpret_cast<HMENU>(206), instance_, nullptr);
            SetFont(rewardUrlEdit_, font_);

            HWND ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 290, 818, 76, 30, hwnd_, reinterpret_cast<HMENU>(IDOK), instance_, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 382, 818, 76, 30, hwnd_, reinterpret_cast<HMENU>(IDCANCEL), instance_, nullptr);
            SetFont(ok, font_);
            SetFont(cancel, font_);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CAPTURE) {
                draft_.mainHotKey = ShowHotKeyCaptureDialog(hwnd_, instance_, draft_.mainHotKey);
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CLEAR) {
                draft_.mainHotKey = 0;
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_SEARCH_HOTKEY_CAPTURE) {
                draft_.searchHotKey = ShowHotKeyCaptureDialog(hwnd_, instance_, draft_.searchHotKey);
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_SEARCH_HOTKEY_CLEAR) {
                draft_.searchHotKey = 0;
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                draft_.showTitle = SendMessageW(showTitle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showGroup = SendMessageW(showGroup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showTag = SendMessageW(showTag_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.topMost = SendMessageW(topMost_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.autoDock = SendMessageW(autoDock_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.hideWhenInactive = SendMessageW(hideInactive_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.hideAfterLink = SendMessageW(hideAfterLink_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.hideOnStart = SendMessageW(hideOnStart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showRunCount = SendMessageW(showRunCount_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.doubleClickToRun = SendMessageW(doubleClick_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.hideNotifyIcon = SendMessageW(hideNotify_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.deleteConfirm = SendMessageW(deleteConfirm_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.saveRunCount = SendMessageW(saveRunCount_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showDate = SendMessageW(showDate_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showSearchButton = SendMessageW(showSearchButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showMenuButton = SendMessageW(showMenuButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showSkinButton = SendMessageW(showSkinButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.autoRun = SendMessageW(autoRun_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.linkNameSingleLine = SendMessageW(linkNameSingleLine_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.showTooltip = SendMessageW(showTooltip_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.groupRight = SendMessageW(groupRight_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.tagRight = SendMessageW(tagRight_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.focusSearch = SendMessageW(focusSearch_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.mouseEnterActiveGroup = SendMessageW(enterActiveGroup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft_.mouseEnterActiveTag = SendMessageW(enterActiveTag_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                const int alignIndex = static_cast<int>(SendMessageW(tagAlignCombo_, CB_GETCURSEL, 0, 0));
                draft_.tagAlign = alignIndex == 0 ? L"left" : (alignIndex == 2 ? L"right" : L"center");
                auto alpha = ParseInt(GetText(alphaEdit_));
                draft_.alpha = alpha ? std::max(64, std::min(255, *alpha)) : 255;
                draft_.groupWidth = ClampNumber(groupWidthEdit_, 40, 240, draft_.groupWidth);
                draft_.tagWidth = ClampNumber(tagWidthEdit_, 40, 240, draft_.tagWidth);
                draft_.dockDelay = ClampNumber(dockDelayEdit_, 0, 5000, draft_.dockDelay);
                draft_.activeGroupDelay = ClampNumber(groupDelayEdit_, 0, 5000, draft_.activeGroupDelay);
                draft_.activeTagDelay = ClampNumber(tagDelayEdit_, 0, 5000, draft_.activeTagDelay);
                draft_.searchCount = ClampNumber(searchCountEdit_, 0, 10000, draft_.searchCount);
                draft_.openDirCommand = GetText(openDirEdit_);
                draft_.helpUrl = GetText(helpUrlEdit_);
                draft_.updateUrl = GetText(updateUrlEdit_);
                draft_.faqUrl = GetText(faqUrlEdit_);
                draft_.rewardUrl = GetText(rewardUrlEdit_);
                config_ = draft_;
                accepted_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void UpdateHotKeyLabels() {
        if (mainHotKeyText_) {
            SetWindowTextW(mainHotKeyText_, FormatHotKeyText(draft_.mainHotKey).c_str());
        }
        if (searchHotKeyText_) {
            SetWindowTextW(searchHotKeyText_, FormatHotKeyText(draft_.searchHotKey).c_str());
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    AppConfig& config_;
    AppConfig draft_;
    HWND showTitle_ = nullptr;
    HWND showGroup_ = nullptr;
    HWND showTag_ = nullptr;
    HWND topMost_ = nullptr;
    HWND autoDock_ = nullptr;
    HWND hideInactive_ = nullptr;
    HWND hideAfterLink_ = nullptr;
    HWND hideOnStart_ = nullptr;
    HWND showRunCount_ = nullptr;
    HWND doubleClick_ = nullptr;
    HWND hideNotify_ = nullptr;
    HWND deleteConfirm_ = nullptr;
    HWND saveRunCount_ = nullptr;
    HWND showDate_ = nullptr;
    HWND showSearchButton_ = nullptr;
    HWND showMenuButton_ = nullptr;
    HWND showSkinButton_ = nullptr;
    HWND autoRun_ = nullptr;
    HWND linkNameSingleLine_ = nullptr;
    HWND showTooltip_ = nullptr;
    HWND groupRight_ = nullptr;
    HWND tagRight_ = nullptr;
    HWND focusSearch_ = nullptr;
    HWND enterActiveGroup_ = nullptr;
    HWND enterActiveTag_ = nullptr;
    HWND alphaEdit_ = nullptr;
    HWND groupWidthEdit_ = nullptr;
    HWND tagWidthEdit_ = nullptr;
    HWND dockDelayEdit_ = nullptr;
    HWND groupDelayEdit_ = nullptr;
    HWND tagDelayEdit_ = nullptr;
    HWND searchCountEdit_ = nullptr;
    HWND tagAlignCombo_ = nullptr;
    HWND mainHotKeyText_ = nullptr;
    HWND searchHotKeyText_ = nullptr;
    HWND openDirEdit_ = nullptr;
    HWND helpUrlEdit_ = nullptr;
    HWND updateUrlEdit_ = nullptr;
    HWND faqUrlEdit_ = nullptr;
    HWND rewardUrlEdit_ = nullptr;
    bool accepted_ = false;
    bool done_ = false;
};
}

bool ShowTextInputDialog(HWND owner, HINSTANCE instance, const std::wstring& title, const std::wstring& label, std::wstring& value) {
    TextDialog dialog(owner, instance, title, label, value);
    return dialog.Run();
}

bool ShowSettingsDialog(HWND owner, HINSTANCE instance, AppConfig& config) {
    SettingsDialog dialog(owner, instance, config);
    return dialog.Run();
}
