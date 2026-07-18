#include "ThemedTaskProgressDialog.h"

#include "ThemedWindowUi.h"
#include "Utilities.h"

#include <algorithm>
#include <utility>

namespace {
constexpr UINT_PTR kRefreshTimer = 1;
}

ThemedTaskProgressDialog::ThemedTaskProgressDialog(ThemedTaskProgressDialogOptions options)
    : options_(std::move(options)) {}

ThemedTaskProgressDialog::~ThemedTaskProgressDialog() {
    Close();
}

bool ThemedTaskProgressDialog::Show() {
    if (IsWindow(hwnd_)) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
        ActivateWindow(hwnd_);
        return true;
    }
    if (!options_.instance || options_.className.empty() || options_.title.empty()) return false;
    ThemedWindowCreateOptions create = ThemedWindowUi::DialogOptions(
        options_.instance,
        options_.owner,
        options_.className.c_str(),
        options_.title.c_str(),
        Proc,
        this,
        options_.icon,
        options_.smallIcon ? options_.smallIcon : options_.icon);
    create.clientWidth = options_.clientWidth;
    create.clientHeight = options_.clientHeight;
    hwnd_ = ThemedWindowUi::CreateWindowHandle(create, nullptr);
    if (!hwnd_) return false;
    ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    return true;
}

void ThemedTaskProgressDialog::Close() {
    if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

bool ThemedTaskProgressDialog::IsOpen() const {
    return IsWindow(hwnd_) != FALSE;
}

LRESULT CALLBACK ThemedTaskProgressDialog::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ThemedTaskProgressDialog* dialog = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        dialog = static_cast<ThemedTaskProgressDialog*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
    } else {
        dialog = reinterpret_cast<ThemedTaskProgressDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT ThemedTaskProgressDialog::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT commonResult = 0;
    if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
        if (message == WM_DESTROY) {
            KillTimer(hwnd_, kRefreshTimer);
            hwnd_ = nullptr;
        }
        return commonResult;
    }
    switch (message) {
    case WM_CREATE:
        windowUi_ = std::make_unique<ThemedWindowUi>(
            options_.instance, options_.owner, hwnd_, options_.theme, DialogLayoutKind::Compact,
            options_.clientWidth, options_.clientHeight);
        CreateControls();
        SetTimer(hwnd_, kRefreshTimer, 80, nullptr);
        Refresh();
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            Refresh();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == options_.stopButtonId) {
            if (options_.requestStop) options_.requestStop();
            Refresh();
            return 0;
        }
        if (LOWORD(wParam) == options_.closeButtonId || LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hwnd_);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd_, &paint);
        windowUi_->FillBackground(dc);
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kRefreshTimer);
        hwnd_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ThemedTaskProgressDialog::CreateControls() {
    const ThemedUi ui = windowUi_->ui();
    stopEnabled_ = true;
    const DialogLayoutMetrics& layout = ui.layout();
    const int left = ui.contentLeft();
    int y = layout.contentInsetY;
    status_ = ui.StatusText(options_.initialStatus, left, y, ui.contentWidth(),
        ThemedStatusTextOptions{ThemedStatusRole::Info, ThemedTextAlign::Center});
    y = ui.nextRowY(y, ui.labelHeight());
    detail_ = ui.Label(options_.initialDetail, left, y, ui.contentWidth());
    y += ui.labelHeight() + layout.sectionGap;
    progress_ = ui.ProgressBar(options_.progressBarId, left, y, ui.contentWidth(),
        ThemedProgressBarOptions{0.0, true, true});
    stop_ = ui.FooterButton(options_.stopButtonId, options_.stopText, 0, 2, false, false);
    close_ = ui.FooterButton(options_.closeButtonId, options_.closeText, 1, 2, true, true);
}

void ThemedTaskProgressDialog::Refresh() {
    if (!windowUi_) return;
    ThemedTaskProgressSnapshot snapshot;
    snapshot.status = options_.initialStatus;
    snapshot.detail = options_.initialDetail;
    if (options_.readSnapshot) snapshot = options_.readSnapshot();
    const ThemedUi ui = windowUi_->ui();
    ui.SetStatusTextRole(status_, snapshot.role);
    ThemedUi::SetText(status_, snapshot.status);
    ThemedUi::SetText(detail_, snapshot.detail);
    ThemedUi::SetProgress(progress_, std::clamp(snapshot.value, 0.0, 1.0), snapshot.indeterminate);
    const bool stopEnabled = !snapshot.finished && !snapshot.stopRequested;
    if (stopEnabled_ != stopEnabled) {
        stopEnabled_ = stopEnabled;
        if (!stopEnabled && GetFocus() == stop_) SetFocus(close_);
        ui.SetEnabled(stop_, stopEnabled);
    }
}
