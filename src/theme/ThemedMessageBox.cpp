#include "ThemedUi.h"
#include "ThemedWindowUi.h"

#include <memory>
#include <utility>

namespace {
class CommonThemedMessageDialog {
public:
    CommonThemedMessageDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        std::wstring message,
        std::wstring title,
        UINT flags)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          message_(std::move(message)),
          title_(std::move(title)),
          flags_(flags) {}

    int Run() {
        auto options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroCommonThemedMessageBox",
            title_.empty() ? L"提示" : title_.c_str(), Proc, this);
        options.clientWidth = kThemedMessageClientWidth;
        options.clientHeight = kThemedMessageClientHeight;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) return MessageBoxW(owner_, message_.c_str(), title_.c_str(), flags_);
        windowUi_->ShowModal();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return result_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        CommonThemedMessageDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<CommonThemedMessageDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<CommonThemedMessageDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool IsYesNo() const { return (flags_ & MB_TYPEMASK) == MB_YESNO; }
    bool IsYesNoCancel() const { return (flags_ & MB_TYPEMASK) == MB_YESNOCANCEL; }
    bool IsOkCancel() const { return (flags_ & MB_TYPEMASK) == MB_OKCANCEL; }
    int DefaultButtonIndex(int buttonCount) const {
        int index = 0;
        switch (flags_ & MB_DEFMASK) {
        case MB_DEFBUTTON2: index = 1; break;
        case MB_DEFBUTTON3: index = 2; break;
        case MB_DEFBUTTON4: index = 3; break;
        default: break;
        }
        return index < buttonCount ? index : 0;
    }

    void Close(int result) {
        result_ = result;
        DestroyWindow(hwnd_);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT result = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
            if (message == WM_DESTROY) done_ = true;
            return result;
        }
        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact,
                kThemedMessageClientWidth, kThemedMessageClientHeight);
            const ThemedUi ui = windowUi_->ui();
            const RECT content = ui.contentRect();
            ui.Label(message_, content.left, content.top, content.right - content.left,
                {ThemedTextAlign::Start, ThemedLabelLines::Three});
            if (IsYesNoCancel()) {
                const int defaultIndex = DefaultButtonIndex(3);
                ui.FooterButton(IDYES, L"是", 0, 3, defaultIndex == 0, defaultIndex == 0);
                ui.FooterButton(IDNO, L"否", 1, 3, defaultIndex == 1, defaultIndex == 1);
                ui.FooterButton(IDCANCEL, L"取消", 2, 3, defaultIndex == 2, defaultIndex == 2);
            } else if (IsYesNo()) {
                const int defaultIndex = DefaultButtonIndex(2);
                ui.FooterButton(IDYES, L"是", 0, 2, defaultIndex == 0, defaultIndex == 0);
                ui.FooterButton(IDNO, L"否", 1, 2, defaultIndex == 1, defaultIndex == 1);
            } else if (IsOkCancel()) {
                const int defaultIndex = DefaultButtonIndex(2);
                ui.FooterButton(IDOK, L"确定", 0, 2, defaultIndex == 0, defaultIndex == 0);
                ui.FooterButton(IDCANCEL, L"取消", 1, 2, defaultIndex == 1, defaultIndex == 1);
            } else {
                ui.FooterButton(IDOK, L"确定", 0, 1, true, true);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDYES: Close(IDYES); return 0;
            case IDNO: Close(IDNO); return 0;
            case IDOK: Close(IDOK); return 0;
            case IDCANCEL: Close(IDCANCEL); return 0;
            default: return 0;
            }
        case WM_CLOSE:
            Close(IsYesNo() ? IDNO : IDCANCEL);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::wstring message_;
    std::wstring title_;
    UINT flags_ = MB_OK;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    int result_ = IDOK;
    bool done_ = false;
};
}

int ThemedWindowUi::ShowMessageBox(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::wstring& message,
    const std::wstring& title,
    UINT flags) {
    return CommonThemedMessageDialog(owner, instance, theme, message, title, flags).Run();
}
