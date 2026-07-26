#include "UpdateCheckDialog.h"

#include "ThemedFormLayout.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <memory>
#include <sstream>
#include <string>

namespace {
constexpr int ID_OPEN_RELEASE = IDNO;
constexpr int ID_UPDATE_DETAILS_TEXT = 2703;

std::wstring ReleaseNotesText(const std::wstring& notes) {
    if (notes.empty()) {
        return L"无";
    }
    return notes;
}

std::wstring UpdatePackageText(const UpdateReleaseInfo& info) {
    if (info.assetSizeBytes == 0) {
        return info.assetName;
    }
    return info.assetName + L"（" + FormatByteSizeForDisplay(info.assetSizeBytes) + L"）";
}

class UpdateCheckDialog {
public:
    UpdateCheckDialog(HWND owner, HINSTANCE instance, const Theme& theme, const UpdateReleaseInfo& info)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          info_(info) {}

    UpdateCheckDialogChoice Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_,
            owner_,
            L"QuattroUpdateCheckDialog",
            L"检查更新",
            UpdateCheckDialog::Proc,
            this,
            icon,
            icon);
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return UpdateCheckDialogChoice::Cancel;
        }
        if (windowUi_) {
            windowUi_->ShowModal();
        }
        UpdateWindow(hwnd_);

        MSG message{};
        bool quitRequested = false;
        int quitCode = 0;
        while (!done_) {
            const BOOL messageResult = GetMessageW(&message, nullptr, 0, 0);
            if (messageResult <= 0) {
                if (messageResult == 0) {
                    quitRequested = true;
                    quitCode = static_cast<int>(message.wParam);
                }
                if (hwnd_ && IsWindow(hwnd_)) {
                    DestroyWindow(hwnd_);
                }
                break;
            }
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        if (quitRequested) {
            PostQuitMessage(quitCode);
        }
        return choice_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        UpdateCheckDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<UpdateCheckDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<UpdateCheckDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Close(UpdateCheckDialogChoice choice) {
        choice_ = choice;
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

    std::wstring DetailsText() const {
        std::wostringstream text;
        text << L"发现新版本。\r\n\r\n";
        text << L"当前版本：" << FormatVersionForDisplay(info_.currentVersion) << L"\r\n";
        text << L"最新版本：" << FormatVersionForDisplay(info_.latestVersion) << L"\r\n";
        text << L"更新包：" << UpdatePackageText(info_) << L"\r\n\r\n";
        text << L"发布说明：\r\n" << ReleaseNotesText(info_.releaseNotes);
        return text.str();
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
            const int footerHeight = ui.footerButtonHeight();
            const RECT frame{
                ui.contentLeft(),
                ui.contentTop(),
                ui.contentLeft() + ui.contentWidth(),
                ui.footerButtonY(footerHeight) - ui.layout().footerGap};
            ui.ReadOnlyText(ID_UPDATE_DETAILS_TEXT, frame, DetailsText());

            ui.FooterButton(IDOK, L"下载更新", 0, 3, true, true);
            ui.FooterButton(ID_OPEN_RELEASE, L"发布页", 1, 3);
            ui.FooterButton(IDCANCEL, L"取消", 2, 3);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK:
                Close(UpdateCheckDialogChoice::Download);
                return 0;
            case ID_OPEN_RELEASE:
                Close(UpdateCheckDialogChoice::OpenRelease);
                return 0;
            case IDCANCEL:
                Close(UpdateCheckDialogChoice::Cancel);
                return 0;
            default:
                break;
            }
            break;
        case WM_CLOSE:
            Close(UpdateCheckDialogChoice::Cancel);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    const UpdateReleaseInfo& info_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    UpdateCheckDialogChoice choice_ = UpdateCheckDialogChoice::Cancel;
    bool done_ = false;
};
}

UpdateCheckDialogChoice ShowUpdateCheckDialog(HWND owner, HINSTANCE instance, const Theme& theme, const UpdateReleaseInfo& info) {
    UpdateCheckDialog dialog(owner, instance, theme, info);
    return dialog.Run();
}
