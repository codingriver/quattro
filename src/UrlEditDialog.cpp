#include "UrlEditDialog.h"

#include "Utilities.h"

#include <commctrl.h>
#include <wininet.h>

#include <utility>

namespace {
constexpr int kDialogWidth = 500;
constexpr int kDialogHeight = 250;

enum ControlId {
    IdName = 1001,
    IdUrl,
    IdRemark,
    IdOk,
    IdCancel,
};

std::wstring GetWindowTextString(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void SetControlFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::wstring UrlHostOrText(const std::wstring& value) {
    const std::wstring url = NormalizeUrl(value);
    URL_COMPONENTSW parts{};
    wchar_t host[260]{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
    if (InternetCrackUrlW(url.c_str(), 0, 0, &parts) && parts.dwHostNameLength > 0) {
        return std::wstring(host, parts.dwHostNameLength);
    }
    return Trim(value);
}

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, Link& link, bool isNew)
        : owner_(owner), instance_(instance), link_(link), isNew_(isNew) {}

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DialogWindow::WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"QuattroUrlEditDialog";
        RegisterClassExW(&wc);

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - kDialogWidth) / 2;
        const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - kDialogHeight) / 2;

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            isNew_ ? L"添加网址" : L"编辑<超链接>",
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
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            return 1;
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
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
            }
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND Label(const wchar_t* text, int x, int y) {
        HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                    x, y + 4, 58, 22, hwnd_, nullptr, instance_, nullptr);
        SetControlFont(hwnd, font_);
        return hwnd;
    }

    HWND Edit(int id, int x, int y, int width, const std::wstring& value, DWORD extraStyle = ES_AUTOHSCROLL) {
        HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value.c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
                                    x, y, width, extraStyle & ES_MULTILINE ? 74 : 26,
                                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SetControlFont(hwnd, font_);
        return hwnd;
    }

    void CreateControls() {
        backgroundBrush_ = CreateSolidBrush(RGB(246, 246, 246));
        font_ = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        ownsFont_ = font_ != nullptr;
        if (!font_) {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        Label(L"名称", 18, 24);
        nameEdit_ = Edit(IdName, 84, 22, 384, link_.name);

        Label(L"URL链接", 18, 60);
        urlEdit_ = Edit(IdUrl, 84, 58, 384, link_.path);

        Label(L"备注", 18, 96);
        remarkEdit_ = Edit(IdRemark, 84, 94, 384, link_.remark, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);

        HWND ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  306, 188, 76, 28, hwnd_, reinterpret_cast<HMENU>(IdOk), instance_, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      392, 188, 76, 28, hwnd_, reinterpret_cast<HMENU>(IdCancel), instance_, nullptr);
        SetControlFont(ok, font_);
        SetControlFont(cancel, font_);
    }

    void Accept() {
        Link next = link_;
        next.name = Trim(GetWindowTextString(nameEdit_));
        next.path = NormalizeUrl(GetWindowTextString(urlEdit_));
        next.remark = Trim(GetWindowTextString(remarkEdit_));
        next.type = 2;
        next.parameter.clear();
        next.workDir.clear();
        next.pidl.clear();
        next.showCmd = SW_SHOWNORMAL;
        if (Trim(next.icon).empty()) {
            next.icon = L"#url";
        }
        if (next.path.empty()) {
            MessageBoxW(hwnd_, L"请输入 URL 链接。", L"网址", MB_OK | MB_ICONWARNING);
            SetFocus(urlEdit_);
            return;
        }
        if (next.name.empty()) {
            next.name = UrlHostOrText(next.path);
        }
        if (next.name.empty()) {
            MessageBoxW(hwnd_, L"请输入名称。", L"网址", MB_OK | MB_ICONWARNING);
            SetFocus(nameEdit_);
            return;
        }
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
    bool isNew_ = false;
    bool done_ = false;
    bool accepted_ = false;
    HFONT font_ = nullptr;
    bool ownsFont_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND urlEdit_ = nullptr;
    HWND remarkEdit_ = nullptr;
};
}

bool UrlEditDialog::Show(HWND owner, HINSTANCE instance, Link& link, bool isNew) {
    DialogWindow dialog(owner, instance, link, isNew);
    return dialog.Run();
}
