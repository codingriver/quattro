#include "ConfirmDialog.h"

#include "ThemedFormLayout.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "../../resources/resource.h"

#include <memory>

namespace {
constexpr int ID_SECONDARY = IDNO;

class ConfirmDialog {
public:
    ConfirmDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        std::wstring title,
        std::wstring message,
        std::wstring primaryText,
        std::wstring secondaryText)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          title_(std::move(title)),
          message_(std::move(message)),
          primaryText_(std::move(primaryText)),
          secondaryText_(std::move(secondaryText)) {}

    ConfirmDialogResult Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_,
            owner_,
            L"QuattroConfirmDialog",
            title_.c_str(),
            ConfirmDialog::Proc,
            this,
            icon,
            icon);
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return ConfirmDialogResult::Cancel;
        }
        if (windowUi_) {
            windowUi_->ShowModal();
        }
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        return result_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ConfirmDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ConfirmDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ConfirmDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Close(ConfirmDialogResult result) {
        result_ = result;
        done_ = true;
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        DestroyWindow(hwnd_);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }

        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_,
                owner_,
                hwnd_,
                theme_,
                kThemedDialogLayoutKind,
                kThemedDialogClientWidth,
                kThemedDialogClientHeight);
            const ThemedUi ui = windowUi_->ui();
            const ThemedFormLayout form(ui);
            int y = ui.contentTop();

            auto messageRow = form.row(y, ThemedRowAlign::Left, {form.text(ui.contentWidth())});
            ui.Label(message_, messageRow[0].left, messageRow[0].top, messageRow[0].right - messageRow[0].left);

            ui.FooterButton(IDOK, primaryText_, 0, 2, true, true);
            ui.FooterButton(ID_SECONDARY, secondaryText_, 1, 2);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK:
                Close(ConfirmDialogResult::Primary);
                return 0;
            case ID_SECONDARY:
                Close(ConfirmDialogResult::Secondary);
                return 0;
            case IDCANCEL:
                Close(ConfirmDialogResult::Cancel);
                return 0;
            default:
                break;
            }
            break;
        case WM_CLOSE:
            Close(ConfirmDialogResult::Cancel);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::wstring title_;
    std::wstring message_;
    std::wstring primaryText_;
    std::wstring secondaryText_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    ConfirmDialogResult result_ = ConfirmDialogResult::Cancel;
    bool done_ = false;
};
}

ConfirmDialogResult ShowConfirmDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::wstring& title,
    const std::wstring& message,
    const std::wstring& primaryText,
    const std::wstring& secondaryText) {
    ConfirmDialog dialog(owner, instance, theme, title, message, primaryText, secondaryText);
    return dialog.Run();
}
