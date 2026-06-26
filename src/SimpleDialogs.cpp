#include "SimpleDialogs.h"

#include "AppLog.h"
#include "HotKeyEditor.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace {
constexpr int ID_SETTINGS_TAB_BASE = 280;
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
constexpr int ID_TAG_ALIGN_LEFT = 407;
constexpr int ID_TAG_ALIGN_CENTER = 408;
constexpr int ID_TAG_ALIGN_RIGHT = 409;

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

void FillRoundRect(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int borderWidth) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

class TextDialog {
public:
    TextDialog(HWND owner, HINSTANCE instance, const Theme& theme, std::wstring title, std::wstring label, std::wstring& value)
        : owner_(owner), instance_(instance), theme_(theme), title_(std::move(title)), label_(std::move(label)), value_(value) {}

    bool Run() {
        const std::wstring className = L"QuattroTextInputDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TextDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
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
            390,
            162,
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
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
        ActivateWindow(hwnd_);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner_, TRUE);
        ActivateWindow(owner_);
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
            editFont_ = ThemedControls::CreateEditFont(theme_);
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            fieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            ThemedControls::CreateLabelText(instance_, hwnd_, label_.c_str(), 24, 18, 320, theme_, font);
            const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
            editFrame_ = RECT{24, 42, 346, 42 + fieldHeight};
            edit_ = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, 100, theme_, editFrame_, value_, editFont_ ? editFont_ : font);
            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreateButton(instance_, hwnd_, IDOK, L"确定", 198, 88, 72, buttonHeight, font, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 286, 88, 72, buttonHeight, font);
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            ThemedControls::DrawFieldFrame(theme_, dc, editFrame_, edit_);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateRect(hwnd_, &editFrame_, TRUE);
            }
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
            if (editFont_) {
                DeleteObject(editFont_);
                editFont_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
                fieldBrush_ = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    const Theme& theme_;
    std::wstring title_;
    std::wstring label_;
    std::wstring& value_;
    RECT editFrame_{};
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HFONT editFont_ = nullptr;
    bool accepted_ = false;
    bool done_ = false;
};

class SettingsDialog {
public:
    SettingsDialog(HWND owner, HINSTANCE instance, AppConfig& config, const Theme& theme)
        : owner_(owner), instance_(instance), config_(config), draft_(config), theme_(theme) {}

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
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
            560,
            520,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(L"设置窗口创建失败: " + FormatLastError(GetLastError()));
            return false;
        }
        EnableWindow(owner_, FALSE);
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner_, TRUE);
        ActivateWindow(owner_);
        return accepted_;
    }

private:
    enum TabIndex {
        TabDisplay = 0,
        TabBehavior = 1,
        TabInteraction = 2,
        TabHotKeys = 3,
        TabLinks = 4,
    };

    struct TabChild {
        HWND hwnd = nullptr;
        int tab = 0;
    };

    struct FieldFrame {
        RECT rect{};
        HWND child = nullptr;
        int tab = 0;
        bool readOnly = false;
    };

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

    void AddTabChild(HWND hwnd, int tab) {
        if (!hwnd) {
            return;
        }
        tabChildren_.push_back(TabChild{hwnd, tab});
    }

    HWND Label(int tab, const wchar_t* text, int x, int y, int width = 110) {
        HWND hwnd = ThemedControls::CreateLabelText(instance_, hwnd_, text, x, y, width, theme_, font_);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND CheckBox(int tab, int id, const wchar_t* text, int x, int y, bool checked, int width = 210) {
        HWND hwnd = ThemedControls::CreateCheckBox(instance_, hwnd_, id, text, x, y, width, ThemedControls::CheckBoxHeight(theme_), font_, checked);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND Button(int tab, int id, const wchar_t* text, int x, int y, int width) {
        HWND hwnd = ThemedControls::CreateButton(instance_, hwnd_, id, text, x, y, width, ThemedControls::CompactButtonHeight(theme_), font_);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND FramedEdit(int tab, int id, int x, int y, int width, const std::wstring& text, DWORD extraStyle = ES_AUTOHSCROLL) {
        const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, id, theme_, frame, text, editFont_ ? editFont_ : font_, extraStyle);
        AddTabChild(hwnd, tab);
        fieldFrames_.push_back(FieldFrame{frame, hwnd, tab, false});
        return hwnd;
    }

    HWND FramedStatic(int tab, int x, int y, int width, const std::wstring& text) {
        const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ThemedControls::CreateFramedStatic(instance_, hwnd_, theme_, frame, text, font_);
        AddTabChild(hwnd, tab);
        fieldFrames_.push_back(FieldFrame{frame, hwnd, tab, true});
        return hwnd;
    }

    HWND NumberEdit(int tab, int id, int x, int y, int width, int value) {
        return FramedEdit(tab, id, x, y, width, std::to_wstring(value), ES_NUMBER);
    }

    int ClampNumber(HWND edit, int minValue, int maxValue, int fallback) const {
        auto value = ParseInt(GetText(edit));
        if (!value) {
            return fallback;
        }
        return std::max(minValue, std::min(maxValue, *value));
    }

    void SelectTagAlign() {
        tagAlignIndex_ = 1;
        if (draft_.tagAlign == L"left") {
            tagAlignIndex_ = 0;
        } else if (draft_.tagAlign == L"right") {
            tagAlignIndex_ = 2;
        }
        UpdateTagAlignButtons();
    }

    void UpdateTagAlignButtons() {
        const HWND buttons[] = {tagAlignLeft_, tagAlignCenter_, tagAlignRight_};
        for (int i = 0; i < 3; ++i) {
            if (buttons[i]) {
                SendMessageW(buttons[i], BM_SETCHECK, i == tagAlignIndex_ ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(buttons[i], nullptr, TRUE);
            }
        }
    }

    void CreateTabs() {
        const wchar_t* titles[] = {L"显示", L"行为", L"交互", L"热键", L"链接"};
        const int startX = 30;
        const int startY = 18;
        const int itemWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupItemWidth", 58.0f));
        const int itemGap = static_cast<int>(theme_.metric(L"tabButton", L"groupGap", 0.0f));
        const int itemHeight = ThemedControls::TabButtonHeight(theme_);
        const int stripPadding = static_cast<int>(theme_.metric(L"tabButton", L"groupPadding", 3.0f));
        tabStripRect_ = RECT{
            startX - stripPadding,
            startY - stripPadding,
            startX + 5 * itemWidth + 4 * itemGap + stripPadding,
            startY + itemHeight + stripPadding};
        for (int i = 0; i < 5; ++i) {
            HWND button = ThemedControls::CreateTabButton(
                instance_,
                hwnd_,
                ID_SETTINGS_TAB_BASE + i,
                titles[i],
                startX + i * (itemWidth + itemGap),
                startY,
                itemWidth,
                itemHeight,
                font_,
                i == TabDisplay);
            tabButtons_.push_back(button);
        }
    }

    void PaintTabs(HDC dc) {
        if (tabStripRect_.right <= tabStripRect_.left || tabStripRect_.bottom <= tabStripRect_.top) {
            return;
        }
        FillRoundRect(
            dc,
            tabStripRect_,
            static_cast<int>(theme_.metric(L"tabButton", L"groupRadius", 10.0f)),
            ToColorRef(theme_.color(L"tabButton", L"normal", L"groupBg")),
            ToColorRef(theme_.color(L"tabButton", L"normal", L"groupBorder")),
            static_cast<int>(theme_.metric(L"tabButton", L"groupBorderWidth", 1.0f)));
    }

    void ShowTab(int tab) {
        currentTab_ = tab;
        for (int i = 0; i < static_cast<int>(tabButtons_.size()); ++i) {
            SendMessageW(tabButtons_[i], BM_SETCHECK, i == currentTab_ ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(tabButtons_[i], nullptr, TRUE);
        }
        for (const auto& child : tabChildren_) {
            const bool visible = child.tab == currentTab_;
            ShowWindow(child.hwnd, visible ? SW_SHOW : SW_HIDE);
            EnableWindow(child.hwnd, visible ? TRUE : FALSE);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    bool IsFieldChild(HWND hwnd) const {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == hwnd) {
                return true;
            }
        }
        return false;
    }

    void InvalidateField(HWND hwnd) {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == hwnd) {
                InvalidateRect(hwnd_, &frame.rect, TRUE);
                return;
            }
        }
    }

    void PaintFields(HDC dc) {
        for (const auto& frame : fieldFrames_) {
            if (frame.tab != currentTab_) {
                continue;
            }
            ThemedControls::DrawFieldFrame(theme_, dc, frame.rect, frame.child, frame.readOnly);
        }
    }

    void ReadDraft() {
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
        draft_.tagAlign = tagAlignIndex_ == 0 ? L"left" : (tagAlignIndex_ == 2 ? L"right" : L"center");
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
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            editFont_ = ThemedControls::CreateEditFont(theme_);
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            fieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            readOnlyFieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"field", L"readonly", L"bg")));
            CreateTabs();

            showTitle_ = CheckBox(TabDisplay, 101, L"显示标题栏", 34, 64, draft_.showTitle);
            showGroup_ = CheckBox(TabDisplay, 102, L"显示分组栏", 282, 64, draft_.showGroup);
            showTag_ = CheckBox(TabDisplay, 103, L"显示标签栏", 34, 94, draft_.showTag);
            showRunCount_ = CheckBox(TabDisplay, 108, L"显示运行次数", 282, 94, draft_.showRunCount);
            showDate_ = CheckBox(TabDisplay, 113, L"显示日期", 34, 124, draft_.showDate);
            showSearchButton_ = CheckBox(TabDisplay, 114, L"显示搜索按钮", 282, 124, draft_.showSearchButton);
            showMenuButton_ = CheckBox(TabDisplay, 115, L"显示菜单按钮", 34, 154, draft_.showMenuButton);
            showSkinButton_ = CheckBox(TabDisplay, 121, L"显示主题按钮", 282, 154, draft_.showSkinButton);
            linkNameSingleLine_ = CheckBox(TabDisplay, 118, L"启动项名称单行", 34, 184, draft_.linkNameSingleLine);
            showTooltip_ = CheckBox(TabDisplay, 119, L"显示提示", 282, 184, draft_.showTooltip);
            groupRight_ = CheckBox(TabDisplay, 120, L"分组栏在右侧", 34, 214, draft_.groupRight);
            tagRight_ = CheckBox(TabDisplay, 122, L"标签栏在右侧", 282, 214, draft_.tagRight);

            Label(TabDisplay, L"透明度", 34, 260, 76);
            alphaEdit_ = NumberEdit(TabDisplay, 201, 118, 254, 78, draft_.alpha);
            Label(TabDisplay, L"标签文字", 282, 260, 72);
            const int tabButtonHeight = ThemedControls::TabButtonHeight(theme_);
            tagAlignLeft_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAG_ALIGN_LEFT, L"左", 364, 255, 36, tabButtonHeight, font_, false);
            tagAlignCenter_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAG_ALIGN_CENTER, L"中", 404, 255, 36, tabButtonHeight, font_, true);
            tagAlignRight_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAG_ALIGN_RIGHT, L"右", 444, 255, 36, tabButtonHeight, font_, false);
            AddTabChild(tagAlignLeft_, TabDisplay);
            AddTabChild(tagAlignCenter_, TabDisplay);
            AddTabChild(tagAlignRight_, TabDisplay);
            SelectTagAlign();

            Label(TabDisplay, L"分组宽度", 34, 314, 76);
            groupWidthEdit_ = NumberEdit(TabDisplay, ID_GROUP_WIDTH, 118, 308, 78, draft_.groupWidth);
            Label(TabDisplay, L"标签宽度", 282, 314, 72);
            tagWidthEdit_ = NumberEdit(TabDisplay, ID_TAG_WIDTH, 364, 308, 78, draft_.tagWidth);

            topMost_ = CheckBox(TabBehavior, 104, L"窗口置顶", 34, 64, draft_.topMost);
            autoDock_ = CheckBox(TabBehavior, 105, L"自动停靠", 282, 64, draft_.autoDock);
            hideInactive_ = CheckBox(TabBehavior, 106, L"失焦隐藏", 34, 94, draft_.hideWhenInactive);
            hideAfterLink_ = CheckBox(TabBehavior, 107, L"运行后隐藏", 282, 94, draft_.hideAfterLink);
            hideOnStart_ = CheckBox(TabBehavior, 116, L"启动后隐藏", 34, 124, draft_.hideOnStart);
            autoRun_ = CheckBox(TabBehavior, 117, L"开机自启动", 282, 124, draft_.autoRun);
            hideNotify_ = CheckBox(TabBehavior, 110, L"隐藏托盘图标", 34, 154, draft_.hideNotifyIcon);
            deleteConfirm_ = CheckBox(TabBehavior, 111, L"删除确认", 282, 154, draft_.deleteConfirm);
            saveRunCount_ = CheckBox(TabBehavior, 112, L"保存运行次数", 34, 184, draft_.saveRunCount);
            Label(TabBehavior, L"停靠延迟", 34, 238, 76);
            dockDelayEdit_ = NumberEdit(TabBehavior, ID_DOCK_DELAY, 118, 232, 88, draft_.dockDelay);

            doubleClick_ = CheckBox(TabInteraction, 109, L"双击运行", 34, 64, draft_.doubleClickToRun);
            focusSearch_ = CheckBox(TabInteraction, 123, L"打开搜索时聚焦输入框", 282, 64, draft_.focusSearch);
            enterActiveGroup_ = CheckBox(TabInteraction, 124, L"鼠标进入激活分组", 34, 94, draft_.mouseEnterActiveGroup);
            enterActiveTag_ = CheckBox(TabInteraction, 125, L"鼠标进入激活标签", 282, 94, draft_.mouseEnterActiveTag);
            Label(TabInteraction, L"分组激活延迟", 34, 154, 100);
            groupDelayEdit_ = NumberEdit(TabInteraction, ID_GROUP_DELAY, 144, 148, 88, draft_.activeGroupDelay);
            Label(TabInteraction, L"标签激活延迟", 282, 154, 100);
            tagDelayEdit_ = NumberEdit(TabInteraction, ID_TAG_DELAY, 392, 148, 88, draft_.activeTagDelay);
            Label(TabInteraction, L"搜索计数", 34, 208, 88);
            searchCountEdit_ = NumberEdit(TabInteraction, ID_SEARCH_COUNT, 144, 202, 88, draft_.searchCount);

            Label(TabHotKeys, L"主窗口热键", 34, 74, 84);
            mainHotKeyText_ = FramedStatic(TabHotKeys, 128, 66, 210, FormatHotKeyText(draft_.mainHotKey));
            Button(TabHotKeys, ID_MAIN_HOTKEY_CAPTURE, L"录入", 354, 68, 56);
            Button(TabHotKeys, ID_MAIN_HOTKEY_CLEAR, L"清除", 424, 68, 56);
            Label(TabHotKeys, L"搜索热键", 34, 128, 84);
            searchHotKeyText_ = FramedStatic(TabHotKeys, 128, 120, 210, FormatHotKeyText(draft_.searchHotKey));
            Button(TabHotKeys, ID_SEARCH_HOTKEY_CAPTURE, L"录入", 354, 122, 56);
            Button(TabHotKeys, ID_SEARCH_HOTKEY_CLEAR, L"清除", 424, 122, 56);

            Label(TabLinks, L"打开目录命令", 34, 68, 110);
            openDirEdit_ = FramedEdit(TabLinks, 202, 34, 92, 446, draft_.openDirCommand);
            Label(TabLinks, L"帮助链接", 34, 136, 110);
            helpUrlEdit_ = FramedEdit(TabLinks, 203, 34, 160, 446, draft_.helpUrl);
            Label(TabLinks, L"更新链接", 34, 204, 110);
            updateUrlEdit_ = FramedEdit(TabLinks, 204, 34, 228, 446, draft_.updateUrl);
            Label(TabLinks, L"FAQ 链接", 34, 272, 110);
            faqUrlEdit_ = FramedEdit(TabLinks, 205, 34, 296, 206, draft_.faqUrl);
            Label(TabLinks, L"赞助链接", 274, 272, 110);
            rewardUrlEdit_ = FramedEdit(TabLinks, 206, 274, 296, 206, draft_.rewardUrl);

            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreateButton(instance_, hwnd_, IDOK, L"确定", 350, 428, 76, buttonHeight, font_, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 442, 428, 76, buttonHeight, font_);
            ShowTab(TabDisplay);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            PaintTabs(dc);
            PaintFields(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: {
            return 1;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            HWND child = reinterpret_cast<HWND>(lParam);
            const bool fieldChild = IsFieldChild(child);
            SetTextColor(dc, ToColorRef(fieldChild ? theme_.color(L"field", L"readonly", L"text") : theme_.color(L"label", L"normal", L"text")));
            if (fieldChild && readOnlyFieldBrush_) {
                return reinterpret_cast<LRESULT>(readOnlyFieldBrush_);
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateField(reinterpret_cast<HWND>(lParam));
            }
            if (LOWORD(wParam) >= ID_SETTINGS_TAB_BASE && LOWORD(wParam) < ID_SETTINGS_TAB_BASE + 5) {
                ShowTab(static_cast<int>(LOWORD(wParam) - ID_SETTINGS_TAB_BASE));
                return 0;
            }
            if (LOWORD(wParam) >= ID_TAG_ALIGN_LEFT && LOWORD(wParam) <= ID_TAG_ALIGN_RIGHT) {
                tagAlignIndex_ = static_cast<int>(LOWORD(wParam) - ID_TAG_ALIGN_LEFT);
                UpdateTagAlignButtons();
                return 0;
            }
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
                ReadDraft();
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
            if (editFont_) {
                DeleteObject(editFont_);
                editFont_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
                fieldBrush_ = nullptr;
            }
            if (readOnlyFieldBrush_) {
                DeleteObject(readOnlyFieldBrush_);
                readOnlyFieldBrush_ = nullptr;
            }
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
    HFONT editFont_ = nullptr;
    AppConfig& config_;
    AppConfig draft_;
    const Theme& theme_;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HBRUSH readOnlyFieldBrush_ = nullptr;
    int currentTab_ = TabDisplay;
    RECT tabStripRect_{};
    std::vector<HWND> tabButtons_;
    std::vector<TabChild> tabChildren_;
    std::vector<FieldFrame> fieldFrames_;
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
    int tagAlignIndex_ = 1;
    HWND tagAlignLeft_ = nullptr;
    HWND tagAlignCenter_ = nullptr;
    HWND tagAlignRight_ = nullptr;
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

bool ShowTextInputDialog(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& title, const std::wstring& label, std::wstring& value) {
    TextDialog dialog(owner, instance, theme, title, label, value);
    return dialog.Run();
}

bool ShowSettingsDialog(HWND owner, HINSTANCE instance, AppConfig& config, const Theme& theme) {
    SettingsDialog dialog(owner, instance, config, theme);
    return dialog.Run();
}

