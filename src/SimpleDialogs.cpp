#include "SimpleDialogs.h"

#include "../resources/resource.h"

#include "AppLog.h"
#include "HotKeyEditor.h"
#include "ThemedControls.h"
#include "Utilities.h"
#include "WebDavBackupService.h"
#include "WebDavCredentialService.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
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
constexpr int ID_WEBDAV_TEST = 410;
constexpr int ID_WEBDAV_CLEAR_PASSWORD = 411;
constexpr int ID_WEBDAV_UPLOAD = 412;
constexpr int ID_WEBDAV_DOWNLOAD = 413;
constexpr int ID_WEBDAV_BACKUP_LIST = 414;

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

std::wstring FormatConfigPackageReportText(const ConfigPackageReport& report) {
    std::wstring text = report.message.empty() ? (report.ok ? L"操作完成。" : L"操作失败。") : report.message;
    if (report.groupsAdded > 0 || report.tagsAdded > 0 || report.linksAdded > 0 ||
        report.notesAdded > 0 || report.todosAdded > 0 || report.pluginSettingsAdded > 0 ||
        report.urlIconsAdded > 0) {
        text += L"\n\n新增分组: " + std::to_wstring(report.groupsAdded);
        text += L"\n新增标签: " + std::to_wstring(report.tagsAdded);
        text += L"\n新增启动项: " + std::to_wstring(report.linksAdded);
        text += L"\n新增便签: " + std::to_wstring(report.notesAdded);
        text += L"\n新增待办: " + std::to_wstring(report.todosAdded);
        text += L"\n新增工具设置: " + std::to_wstring(report.pluginSettingsAdded);
        text += L"\n新增 URL 图标: " + std::to_wstring(report.urlIconsAdded);
    }
    if (!report.warnings.empty()) {
        text += L"\n\n警告:";
        for (const auto& warning : report.warnings) {
            text += L"\n- " + warning;
        }
    }
    return text;
}

std::wstring FormatFileSize(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::to_wstring((bytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull)) + L" MB";
    }
    if (bytes >= 1024ull) {
        return std::to_wstring((bytes + 1023ull) / 1024ull) + L" KB";
    }
    return std::to_wstring(bytes) + L" B";
}

std::wstring FormatBackupListItem(const WebDavRemoteFile& backup) {
    std::wstring text = backup.name;
    if (backup.size > 0) {
        text += L"    " + FormatFileSize(backup.size);
    }
    if (!backup.lastModified.empty()) {
        text += L"    " + backup.lastModified;
    }
    return text;
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
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
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
        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);

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
            ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"确定", 198, 88, 72, buttonHeight, font, true);
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
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
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
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool accepted_ = false;
    bool done_ = false;
};

class WebDavBackupSelectionDialog {
public:
    WebDavBackupSelectionDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        const std::vector<WebDavRemoteFile>& backups,
        std::wstring& selectedName)
        : owner_(owner), instance_(instance), theme_(theme), backups_(backups), selectedName_(selectedName) {}

    bool Run() {
        const std::wstring className = L"QuattroWebDavBackupSelectionDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WebDavBackupSelectionDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = className.c_str();
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(L"WebDAV 备份选择窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            L"选择 WebDAV 备份",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 60,
            ownerRect.top + 80,
            560,
            390,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(L"WebDAV 备份选择窗口创建失败: " + FormatLastError(GetLastError()));
            return false;
        }

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);

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
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        WebDavBackupSelectionDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<WebDavBackupSelectionDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<WebDavBackupSelectionDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void AcceptSelection() {
        const int selected = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (selected < 0 || selected >= static_cast<int>(backups_.size())) {
            MessageBoxW(hwnd_, L"请选择一个备份文件。", L"选择 WebDAV 备份", MB_OK | MB_ICONWARNING);
            return;
        }
        selectedName_ = backups_[static_cast<std::size_t>(selected)].name;
        accepted_ = true;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            listBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"list", L"normal", L"bg")));
            ThemedControls::CreateLabelText(instance_, hwnd_, L"云端备份记录", 24, 20, 180, theme_, font_);
            list_ = CreateWindowExW(
                0,
                L"LISTBOX",
                nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
                24,
                48,
                500,
                238,
                hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WEBDAV_BACKUP_LIST)),
                instance_,
                nullptr);
            SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            for (const auto& backup : backups_) {
                SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(FormatBackupListItem(backup).c_str()));
            }
            if (!backups_.empty()) {
                SendMessageW(list_, LB_SETCURSEL, 0, 0);
            }

            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"下载", 360, 310, 76, buttonHeight, font_, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 452, 310, 76, buttonHeight, font_);
            SetFocus(list_);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            const RECT listFrame{22, 46, 526, 288};
            ThemedControls::DrawListFrame(theme_, dc, listFrame, list_);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"list", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(listBrush_ ? listBrush_ : GetStockObject(WHITE_BRUSH));
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
            if (LOWORD(wParam) == ID_WEBDAV_BACKUP_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                AcceptSelection();
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                AcceptSelection();
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
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (listBrush_) {
                DeleteObject(listBrush_);
                listBrush_ = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HFONT font_ = nullptr;
    const Theme& theme_;
    const std::vector<WebDavRemoteFile>& backups_;
    std::wstring& selectedName_;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH listBrush_ = nullptr;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool accepted_ = false;
    bool done_ = false;
};

class SettingsDialog {
public:
    SettingsDialog(HWND owner, HINSTANCE instance, AppConfig& config, const Theme& theme, std::filesystem::path appDirectory)
        : owner_(owner), instance_(instance), config_(config), draft_(config), theme_(theme), appDirectory_(std::move(appDirectory)) {}

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
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
        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN &&
                message.wParam == VK_TAB &&
                (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                ShowTab((currentTab_ + (reverse ? 5 : 1)) % 6);
                continue;
            }
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return accepted_;
    }

    bool webDavDataImported() const {
        return webDavDataImported_;
    }

private:
    enum TabIndex {
        TabDisplay = 0,
        TabBehavior = 1,
        TabInteraction = 2,
        TabHotKeys = 3,
        TabLinks = 4,
        TabWebDav = 5,
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
        const wchar_t* titles[] = {L"显示", L"行为", L"交互", L"热键", L"链接", L"WebDAV"};
        const int startX = 30;
        const int startY = 18;
        const int itemWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupItemWidth", 58.0f));
        const std::array<int, 6> itemWidths{
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 58),
            std::max(itemWidth, 76),
        };
        const int itemGap = static_cast<int>(theme_.metric(L"tabButton", L"groupGap", 0.0f));
        const int separatorWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupBorderWidth", 1.0f));
        const int itemSpacing = std::max(itemGap, separatorWidth > 0 ? separatorWidth : 0);
        const int itemHeight = ThemedControls::TabButtonHeight(theme_);
        const int stripPadding = static_cast<int>(theme_.metric(L"tabButton", L"groupPadding", 3.0f));
        int stripWidth = 0;
        for (int width : itemWidths) {
            stripWidth += width;
        }
        stripWidth += 5 * itemSpacing;
        tabStripRect_ = RECT{
            startX - stripPadding,
            startY - stripPadding,
            startX + stripWidth + stripPadding,
            startY + itemHeight + stripPadding};
        int x = startX;
        tabSeparatorXs_.clear();
        for (int i = 0; i < 6; ++i) {
            HWND button = ThemedControls::CreateTabButton(
                instance_,
                hwnd_,
                ID_SETTINGS_TAB_BASE + i,
                titles[i],
                x,
                startY,
                itemWidths[static_cast<std::size_t>(i)],
                itemHeight,
                font_,
                i == TabDisplay);
            tabButtons_.push_back(button);
            x += itemWidths[static_cast<std::size_t>(i)];
            if (i < 5) {
                tabSeparatorXs_.push_back(x);
                x += itemSpacing;
            }
        }
    }

    void PaintTabs(HDC dc) {
        if (tabStripRect_.right <= tabStripRect_.left || tabStripRect_.bottom <= tabStripRect_.top) {
            return;
        }
        ThemedControls::DrawTabGroupFrame(theme_, dc, tabStripRect_);
        const int separatorWidth = static_cast<int>(theme_.metric(L"tabButton", L"groupBorderWidth", 1.0f));
        if (separatorWidth <= 0) {
            return;
        }
        HBRUSH separator = CreateSolidBrush(ToColorRef(theme_.color(L"tabButton", L"normal", L"groupBorder")));
        for (int x : tabSeparatorXs_) {
            RECT line{
                x,
                tabStripRect_.top,
                x + separatorWidth,
                tabStripRect_.bottom};
            FillRect(dc, &line, separator);
        }
        DeleteObject(separator);
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
        draft_.webDavEnabled = SendMessageW(webDavEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        draft_.webDavUrl = GetText(webDavUrlEdit_);
        draft_.webDavRemotePath = GetText(webDavRemotePathEdit_);
        draft_.webDavUserName = GetText(webDavUserNameEdit_);
        draft_.webDavKeepCount = ClampNumber(webDavKeepCountEdit_, 1, 100, draft_.webDavKeepCount);
        if (Trim(draft_.webDavRemotePath).empty()) {
            draft_.webDavRemotePath = L"/Quattro/backups/";
        }
    }

    AppConfig ReadWebDavDraftFromControls() {
        AppConfig value = draft_;
        value.webDavEnabled = SendMessageW(webDavEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        value.webDavUrl = GetText(webDavUrlEdit_);
        value.webDavRemotePath = GetText(webDavRemotePathEdit_);
        value.webDavUserName = GetText(webDavUserNameEdit_);
        value.webDavKeepCount = ClampNumber(webDavKeepCountEdit_, 1, 100, value.webDavKeepCount);
        if (Trim(value.webDavRemotePath).empty()) {
            value.webDavRemotePath = L"/Quattro/backups/";
        }
        return value;
    }

    bool SaveWebDavPasswordIfNeeded() {
        const std::wstring password = GetText(webDavPasswordEdit_);
        if (password.empty()) {
            return true;
        }
        std::wstring error;
        if (!WebDavCredentialService::SavePassword(draft_, password, error)) {
            MessageBoxW(hwnd_, error.c_str(), L"WebDAV 备份", MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    void TestWebDavConnection() {
        AppConfig value = ReadWebDavDraftFromControls();
        value.webDavEnabled = true;
        std::wstring password = GetText(webDavPasswordEdit_);
        std::wstring error;
        if (password.empty() && !WebDavCredentialService::LoadPassword(value, password, error)) {
            MessageBoxW(hwnd_, error.c_str(), L"WebDAV 备份", MB_OK | MB_ICONWARNING);
            return;
        }
        WebDavClient client(value, password);
        if (client.TestConnection()) {
            MessageBoxW(hwnd_, L"WebDAV 连接成功。", L"WebDAV 备份", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd_, client.lastError().c_str(), L"WebDAV 备份", MB_OK | MB_ICONWARNING);
        }
    }

    void ClearWebDavPassword() {
        AppConfig value = ReadWebDavDraftFromControls();
        std::wstring error;
        if (WebDavCredentialService::DeletePassword(value, error)) {
            SetWindowTextW(webDavPasswordEdit_, L"");
            MessageBoxW(hwnd_, L"WebDAV 密码已清除。", L"WebDAV 备份", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd_, error.c_str(), L"WebDAV 备份", MB_OK | MB_ICONWARNING);
        }
    }

    bool PrepareWebDavOperation() {
        ReadDraft();
        if (!SaveWebDavPasswordIfNeeded()) {
            return false;
        }
        return true;
    }

    void UploadWebDavBackup() {
        if (!PrepareWebDavOperation()) {
            return;
        }
        WebDavBackupService service(appDirectory_, draft_);
        const WebDavBackupReport report = service.UploadBackup();
        MessageBoxW(hwnd_, report.message.c_str(), L"上传到云端", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    void DownloadWebDavBackup() {
        if (!PrepareWebDavOperation()) {
            return;
        }
        WebDavBackupService service(appDirectory_, draft_);
        std::vector<WebDavRemoteFile> backups;
        std::wstring error;
        if (!service.ListBackups(backups, error)) {
            MessageBoxW(hwnd_, error.c_str(), L"从云端下载", MB_OK | MB_ICONWARNING);
            return;
        }
        if (backups.empty()) {
            MessageBoxW(hwnd_, L"远端目录中没有可用的 .q4cfg 备份。", L"从云端下载", MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring fileName = backups.front().name;
        if (!ShowWebDavBackupSelectionDialog(hwnd_, instance_, theme_, backups, fileName)) {
            return;
        }

        const int confirm = MessageBoxW(
            hwnd_,
            L"将下载所选 WebDAV 备份，并把其中的分组、标签、启动项、便签、待办和工具设置合并到当前数据。\n\n当前数据不会被覆盖，导入前会自动备份。",
            L"从云端下载",
            MB_OKCANCEL | MB_ICONINFORMATION);
        if (confirm != IDOK) {
            return;
        }

        const WebDavBackupReport report = service.DownloadAndImportMerge(fileName);
        if (report.ok) {
            webDavDataImported_ = true;
        }
        const std::wstring text = report.importReport.message.empty()
            ? report.message
            : FormatConfigPackageReportText(report.importReport);
        MessageBoxW(hwnd_, text.c_str(), L"从云端下载", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
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
            showDate_ = CheckBox(TabDisplay, 113, L"显示日期", 282, 94, draft_.showDate);
            showSearchButton_ = CheckBox(TabDisplay, 114, L"显示搜索按钮", 34, 124, draft_.showSearchButton);
            showMenuButton_ = CheckBox(TabDisplay, 115, L"显示菜单按钮", 282, 124, draft_.showMenuButton);
            showSkinButton_ = CheckBox(TabDisplay, 121, L"显示主题按钮", 34, 154, draft_.showSkinButton);
            linkNameSingleLine_ = CheckBox(TabDisplay, 118, L"启动项名称单行", 282, 154, draft_.linkNameSingleLine);
            showTooltip_ = CheckBox(TabDisplay, 119, L"显示提示", 34, 184, draft_.showTooltip);
            groupRight_ = CheckBox(TabDisplay, 120, L"分组栏在右侧", 282, 184, draft_.groupRight);
            tagRight_ = CheckBox(TabDisplay, 122, L"标签栏在右侧", 34, 214, draft_.tagRight);

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
            Label(TabBehavior, L"ms", 214, 238, 32);

            doubleClick_ = CheckBox(TabInteraction, 109, L"双击运行", 34, 64, draft_.doubleClickToRun);
            focusSearch_ = CheckBox(TabInteraction, 123, L"打开搜索时聚焦输入框", 282, 64, draft_.focusSearch);
            enterActiveGroup_ = CheckBox(TabInteraction, 124, L"鼠标进入激活分组", 34, 94, draft_.mouseEnterActiveGroup);
            enterActiveTag_ = CheckBox(TabInteraction, 125, L"鼠标进入激活标签", 282, 94, draft_.mouseEnterActiveTag);
            Label(TabInteraction, L"分组激活延迟", 34, 154, 100);
            groupDelayEdit_ = NumberEdit(TabInteraction, ID_GROUP_DELAY, 144, 148, 88, draft_.activeGroupDelay);
            Label(TabInteraction, L"ms", 240, 154, 32);
            Label(TabInteraction, L"标签激活延迟", 282, 154, 100);
            tagDelayEdit_ = NumberEdit(TabInteraction, ID_TAG_DELAY, 392, 148, 88, draft_.activeTagDelay);
            Label(TabInteraction, L"ms", 488, 154, 32);
            Label(TabInteraction, L"搜索计数", 34, 208, 88);
            searchCountEdit_ = FramedStatic(TabInteraction, 144, 202, 88, std::to_wstring(draft_.searchCount));

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
            Label(TabLinks, L"赞助链接", 282, 272, 110);
            rewardUrlEdit_ = FramedEdit(TabLinks, 206, 282, 296, 198, draft_.rewardUrl);

            webDavEnabled_ = CheckBox(TabWebDav, 208, L"启用 WebDAV 备份", 34, 64, draft_.webDavEnabled, 220);
            Label(TabWebDav, L"服务器地址", 34, 112, 110);
            webDavUrlEdit_ = FramedEdit(TabWebDav, 209, 34, 136, 446, draft_.webDavUrl);
            Label(TabWebDav, L"远端目录", 34, 184, 110);
            webDavRemotePathEdit_ = FramedEdit(TabWebDav, 210, 34, 208, 206, draft_.webDavRemotePath);
            Label(TabWebDav, L"保留数量", 282, 184, 110);
            webDavKeepCountEdit_ = NumberEdit(TabWebDav, 211, 282, 208, 90, draft_.webDavKeepCount);
            Label(TabWebDav, L"用户名", 34, 256, 110);
            webDavUserNameEdit_ = FramedEdit(TabWebDav, 212, 34, 280, 206, draft_.webDavUserName);
            Label(TabWebDav, L"密码/应用密码", 282, 256, 130);
            webDavPasswordEdit_ = FramedEdit(TabWebDav, 213, 282, 280, 198, L"", ES_AUTOHSCROLL | ES_PASSWORD);
            Button(TabWebDav, ID_WEBDAV_UPLOAD, L"上传到云端", 34, 340, 104);
            Button(TabWebDav, ID_WEBDAV_DOWNLOAD, L"从云端下载", 150, 340, 104);
            Button(TabWebDav, ID_WEBDAV_TEST, L"测试连接", 286, 340, 92);
            Button(TabWebDav, ID_WEBDAV_CLEAR_PASSWORD, L"清除密码", 390, 340, 90);

            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreatePrimaryButton(instance_, hwnd_, IDOK, L"确定", 350, 428, 76, buttonHeight, font_, true);
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
            if (LOWORD(wParam) >= ID_SETTINGS_TAB_BASE && LOWORD(wParam) < ID_SETTINGS_TAB_BASE + 6) {
                ShowTab(static_cast<int>(LOWORD(wParam) - ID_SETTINGS_TAB_BASE));
                return 0;
            }
            if (LOWORD(wParam) >= ID_TAG_ALIGN_LEFT && LOWORD(wParam) <= ID_TAG_ALIGN_RIGHT) {
                tagAlignIndex_ = static_cast<int>(LOWORD(wParam) - ID_TAG_ALIGN_LEFT);
                UpdateTagAlignButtons();
                return 0;
            }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CAPTURE) {
                draft_.mainHotKey = ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.mainHotKey);
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CLEAR) {
                draft_.mainHotKey = 0;
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_SEARCH_HOTKEY_CAPTURE) {
                draft_.searchHotKey = ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.searchHotKey);
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_SEARCH_HOTKEY_CLEAR) {
                draft_.searchHotKey = 0;
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_TEST) {
                TestWebDavConnection();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_CLEAR_PASSWORD) {
                ClearWebDavPassword();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_UPLOAD) {
                UploadWebDavBackup();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_DOWNLOAD) {
                DownloadWebDavBackup();
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                ReadDraft();
                if (!SaveWebDavPasswordIfNeeded()) {
                    return 0;
                }
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
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
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
    std::filesystem::path appDirectory_;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HBRUSH readOnlyFieldBrush_ = nullptr;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    int currentTab_ = TabDisplay;
    RECT tabStripRect_{};
    std::vector<HWND> tabButtons_;
    std::vector<int> tabSeparatorXs_;
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
    HWND webDavEnabled_ = nullptr;
    HWND webDavUrlEdit_ = nullptr;
    HWND webDavRemotePathEdit_ = nullptr;
    HWND webDavKeepCountEdit_ = nullptr;
    HWND webDavUserNameEdit_ = nullptr;
    HWND webDavPasswordEdit_ = nullptr;
    bool webDavDataImported_ = false;
    bool accepted_ = false;
    bool done_ = false;
};
}

bool ShowTextInputDialog(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& title, const std::wstring& label, std::wstring& value) {
    TextDialog dialog(owner, instance, theme, title, label, value);
    return dialog.Run();
}

bool ShowWebDavBackupSelectionDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::vector<WebDavRemoteFile>& backups,
    std::wstring& selectedName) {
    WebDavBackupSelectionDialog dialog(owner, instance, theme, backups, selectedName);
    return dialog.Run();
}

bool ShowSettingsDialog(
    HWND owner,
    HINSTANCE instance,
    AppConfig& config,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    bool* webDavDataImported) {
    SettingsDialog dialog(owner, instance, config, theme, appDirectory);
    const bool accepted = dialog.Run();
    if (webDavDataImported) {
        *webDavDataImported = dialog.webDavDataImported();
    }
    return accepted;
}
