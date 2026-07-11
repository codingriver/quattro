#include "AboutDialog.h"

#include "ThemedFormLayout.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Version.h"
#include "../../resources/resource.h"

#include <memory>
#include <string>

namespace {
constexpr const wchar_t* kAppDisplayName = L"Quattro快速启动器";

class AboutDialog {
public:
    AboutDialog(HWND owner, HINSTANCE instance, const Theme& theme, bool runningAsAdmin)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          runningAsAdmin_(runningAsAdmin) {}

    void Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_,
            owner_,
            L"QuattroAboutDialog",
            L"关于",
            AboutDialog::Proc,
            this,
            icon,
            icon);
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return;
        }
        if (windowUi_) {
            windowUi_->ShowModal();
        }
        UpdateWindow(hwnd_);

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
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        AboutDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<AboutDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<AboutDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Close() {
        done_ = true;
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        DestroyWindow(hwnd_);
    }

    void CreateLabelValueRow(const ThemedUi& ui, const ThemedFormLayout& form, int& y, int labelWidth, const std::wstring& label, const std::wstring& value) {
        const int valueWidth = ui.contentWidth() - labelWidth - ui.layout().labelGap;
        auto group = form.labelText(labelWidth, valueWidth);
        auto rows = form.rowGroups(y, ThemedRowAlign::Left, {group});
        ui.Label(label, rows[0][0].left, rows[0][0].top, rows[0][0].right - rows[0][0].left);
        ui.Label(value, rows[0][1].left, rows[0][1].top, rows[0][1].right - rows[0][1].left);
        y = form.nextRowY(y, {group});
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

            auto titleRow = form.row(y, ThemedRowAlign::Left, {form.text(ui.contentWidth())});
            ui.Label(kAppDisplayName, titleRow[0].left, titleRow[0].top, titleRow[0].right - titleRow[0].left);
            y = form.nextRowY(y, {form.text(ui.contentWidth())});

            const int labelWidth = form.labelWidthForTexts({L"版本：", L"开源仓库：", L"当前权限："});
            CreateLabelValueRow(ui, form, y, labelWidth, L"版本：", QuattroVersionText());

            auto descriptionRow = form.row(y, ThemedRowAlign::Left, {form.text(ui.contentWidth())});
            ui.Label(L"轻量级 Windows 快速启动工具", descriptionRow[0].left, descriptionRow[0].top, descriptionRow[0].right - descriptionRow[0].left);
            y = form.nextRowY(y, {form.text(ui.contentWidth())});

            auto stackRow = form.row(y, ThemedRowAlign::Left, {form.text(ui.contentWidth())});
            ui.Label(L"C++ / Win32 / Direct2D / DirectWrite", stackRow[0].left, stackRow[0].top, stackRow[0].right - stackRow[0].left);
            y = form.nextRowY(y, {form.text(ui.contentWidth())});

            CreateLabelValueRow(ui, form, y, labelWidth, L"开源仓库：", L"https://github.com/codingriver/quattro");
            CreateLabelValueRow(ui, form, y, labelWidth, L"当前权限：", runningAsAdmin_ ? L"管理员" : L"普通用户");
            ui.FooterButton(IDOK, L"确定", 0, 1, true, true);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                Close();
                return 0;
            }
            break;
        case WM_CLOSE:
            Close();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    bool runningAsAdmin_ = false;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool done_ = false;
};
}

void ShowAboutDialog(HWND owner, HINSTANCE instance, const Theme& theme, bool runningAsAdmin) {
    AboutDialog dialog(owner, instance, theme, runningAsAdmin);
    dialog.Run();
}
