#include "UrlEditDialog.h"

#include "../../resources/resource.h"

#include "DialogLayout.h"
#include "SimpleDialogs.h"
#include "ThemedControls.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"

#include <commctrl.h>
#include <wininet.h>

#include <algorithm>
#include <utility>
#include <memory>

namespace {
#define MessageBoxW(owner, message, title, flags) ShowThemedMessageBox((owner), instance_, theme_, (message), (title), (flags))

constexpr int kDialogWidth = 560;
constexpr int kDialogHeight = 286;

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
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroUrlEditDialog",
            isNew_ ? L"添加网址" : L"编辑<超链接>", DialogWindow::WindowProc, this, icon, icon);
        options.clientWidth = kDialogWidth;
        options.clientHeight = kDialogHeight;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return false;
        }

        if (windowUi_) {
            windowUi_->ShowModal();
        }
        UpdateWindow(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (windowUi_) {
            windowUi_->RestoreModalOwner();
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
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Standard, kDialogWidth, kDialogHeight);
            CreateControls();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->DrawRegisteredEditFrames(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            windowUi_->DrawRegisteredEditFrames(reinterpret_cast<HDC>(wParam));
            return 0;
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
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND Label(const wchar_t* text, int x, int y, int width) {
        return windowUi_->ui().Label(text, x, y + 7, width);
    }

    HWND Edit(int id, int x, int y, int width, const std::wstring& value, ThemedEditMode mode = ThemedEditMode::SingleLine) {
        const ThemedUi ui = windowUi_->ui();
        ThemedEditOptions options{};
        options.mode = mode;
        return ui.Edit(id, ui.editFrame(x, y, width, mode), value, options);
    }

    void CreateControls() {
        const ThemedUi ui = windowUi_->ui();
        const DialogLayoutMetrics& layout = ui.layout();
        const int rowStep = layout.RowStep(ui.editHeight());
        const int fieldWidth = ui.clientWidth() - layout.fieldX - layout.contentInsetX;
        int y = layout.contentInsetY;
        Label(L"名称", layout.contentInsetX, y, layout.labelWidth);
        nameEdit_ = Edit(IdName, layout.fieldX, y, fieldWidth, link_.name);

        y += rowStep;
        Label(L"URL链接 *", layout.contentInsetX, y, layout.labelWidth);
        urlEdit_ = Edit(IdUrl, layout.fieldX, y, fieldWidth, link_.path);

        y += rowStep;
        Label(L"备注", layout.contentInsetX, y, layout.labelWidth);
        remarkEdit_ = Edit(IdRemark, layout.fieldX, y, fieldWidth, link_.remark, ThemedEditMode::MultiLine);

        const int footerY = layout.FooterY(y + ui.editHeight(ThemedEditMode::MultiLine));
        ui.Button(IdOk, L"确定", layout.FooterButtonX(ui.clientWidth(), 0, 2), footerY,
                  ThemedButtonRole::Primary, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, layout.footerButtonWidth, true);
        ui.Button(IdCancel, L"取消", layout.FooterButtonX(ui.clientWidth(), 1, 2), footerY,
                  ThemedButtonRole::Normal, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, layout.footerButtonWidth);
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
    bool done_ = false;
    bool accepted_ = false;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND nameEdit_ = nullptr;
    HWND urlEdit_ = nullptr;
    HWND remarkEdit_ = nullptr;
};
}

bool UrlEditDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, Link& link, bool isNew) {
    DialogWindow dialog(owner, instance, theme, link, isNew);
    return dialog.Run();
}

