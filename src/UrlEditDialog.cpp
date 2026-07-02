#include "UrlEditDialog.h"

#include "../resources/resource.h"

#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <wininet.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace {
constexpr int kDialogWidth = 500;
constexpr int kDialogHeight = 250;
constexpr int kLabelX = 28;
constexpr int kFieldX = 108;
constexpr int kFieldWidth = 360;

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

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
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

bool IsValidUrlText(const std::wstring& value) {
    const std::wstring url = NormalizeUrl(value);
    if (!HasUrlScheme(url)) {
        return false;
    }
    URL_COMPONENTSW parts{};
    wchar_t host[260]{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
    return InternetCrackUrlW(url.c_str(), 0, 0, &parts) && parts.dwHostNameLength > 0;
}

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme, Link& link, bool isNew)
        : owner_(owner), instance_(instance), theme_(theme), link_(link), isNew_(isNew) {}

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DialogWindow::WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
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
            PaintFields(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintBackground(reinterpret_cast<HDC>(wParam));
            PaintFields(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wParam), ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            SetTextColor(reinterpret_cast<HDC>(wParam), ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(WHITE_BRUSH));
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_ERASEBKGND: {
            return 1;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateField(reinterpret_cast<HWND>(lParam));
            }
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
            if (editFont_) {
                DeleteObject(editFont_);
                editFont_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
            }
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    struct FieldFrame {
        RECT rect{};
        HWND child = nullptr;
    };

    HWND Label(const wchar_t* text, int x, int y) {
        return ThemedControls::CreateLabelText(instance_, hwnd_, text, x, y + 4, 58, theme_, font_, SS_RIGHT);
    }

    HWND Edit(int id, int x, int y, int width, const std::wstring& value, DWORD extraStyle = ES_AUTOHSCROLL) {
        const int height = extraStyle & ES_MULTILINE ? ThemedControls::EditFrameHeight(theme_) * 2 + 14 : FieldHeight();
        const RECT frame{x, y, x + width, y + height};
        HWND hwnd = (extraStyle & ES_MULTILINE)
            ? ThemedControls::CreateMultiLineEdit(instance_, hwnd_, id, theme_, frame, value, font_, extraStyle)
            : ThemedControls::CreateSingleLineEdit(instance_, hwnd_, id, theme_, frame, value, editFont_ ? editFont_ : font_, extraStyle);
        fieldFrames_.push_back(FieldFrame{frame, hwnd});
        return hwnd;
    }

    void CreateControls() {
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        fieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
        font_ = ThemedControls::CreateDialogFont();
        ownsFont_ = font_ != nullptr;
        if (!font_) {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
        editFont_ = ThemedControls::CreateEditFont(theme_);

        const int rowStep = FieldHeight() + static_cast<int>(theme_.metric(L"global", L"itemGap", 8.0f));
        int y = 24;
        Label(L"名称", kLabelX, y);
        nameEdit_ = Edit(IdName, kFieldX, y - 2, kFieldWidth, link_.name);

        y += rowStep;
        Label(L"URL链接 *", kLabelX, y);
        urlEdit_ = Edit(IdUrl, kFieldX, y - 2, kFieldWidth, link_.path);

        y += rowStep;
        Label(L"备注", kLabelX, y);
        remarkEdit_ = Edit(IdRemark, kFieldX, y - 2, kFieldWidth, link_.remark, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);

        const int footerY = y - 2 + FieldHeight() * 2 + 14 + static_cast<int>(theme_.metric(L"global", L"sectionGap", 16.0f));
        ThemedControls::CreatePrimaryButton(instance_, hwnd_, IdOk, L"确定", 306, footerY, 76, ButtonHeight(), font_, true);
        ThemedControls::CreateButton(instance_, hwnd_, IdCancel, L"取消", 392, footerY, 76, ButtonHeight(), font_);
    }

    int FieldHeight() const {
        return ThemedControls::EditFrameHeight(theme_);
    }

    int ButtonHeight() const {
        return ThemedControls::ButtonHeight(theme_);
    }

    void PaintBackground(HDC dc) {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    void PaintFields(HDC dc) {
        for (const auto& frame : fieldFrames_) {
            ThemedControls::DrawFieldFrame(theme_, dc, frame.rect, frame.child);
        }
    }

    void InvalidateField(HWND child) {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == child) {
                InvalidateRect(hwnd_, &frame.rect, TRUE);
                return;
            }
        }
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
        if (!IsValidUrlText(next.path)) {
            MessageBoxW(hwnd_, L"请输入有效的 URL 链接。", L"网址", MB_OK | MB_ICONWARNING);
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
    const Theme& theme_;
    Link& link_;
    bool isNew_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
    bool accepted_ = false;
    HFONT font_ = nullptr;
    HFONT editFont_ = nullptr;
    bool ownsFont_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    std::vector<FieldFrame> fieldFrames_;
    HWND nameEdit_ = nullptr;
    HWND urlEdit_ = nullptr;
    HWND remarkEdit_ = nullptr;
};
}

bool UrlEditDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, Link& link, bool isNew) {
    DialogWindow dialog(owner, instance, theme, link, isNew);
    return dialog.Run();
}

