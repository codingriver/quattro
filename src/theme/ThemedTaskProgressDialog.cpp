#include "ThemedTaskProgressDialog.h"

#include "ThemedWindowUi.h"
#include "Utilities.h"

#include <algorithm>
#include <utility>

namespace {
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kStatusTextId = -1001;
constexpr int kDetailTextId = -1002;
}

ThemedTaskProgressSnapshot ToThemedTaskProgressSnapshot(const TaskProgressSnapshot& snapshot) {
    ThemedTaskProgressSnapshot output;
    output.title = snapshot.title;
    output.status = snapshot.status;
    output.detail = snapshot.detail;
    if (snapshot.workerCount > 0) {
        std::wstring workerDetail = L"使用 " + std::to_wstring(snapshot.workerCount) + L" 个工作线程";
        if (!output.detail.empty()) workerDetail += L"，" + output.detail;
        output.detail = std::move(workerDetail);
    }
    output.finished = snapshot.taskStatus == TaskStatus::Completed ||
        snapshot.taskStatus == TaskStatus::Stopped ||
        snapshot.taskStatus == TaskStatus::Failed;
    output.completed = snapshot.taskStatus == TaskStatus::Completed;
    output.stopRequested = snapshot.stopRequested;
    output.indeterminate = snapshot.indeterminate || snapshot.total == 0;
    if (!output.indeterminate) {
        const double rawValue = std::clamp(
            static_cast<double>(snapshot.current) / static_cast<double>(snapshot.total),
            0.0,
            1.0);
        const bool running = snapshot.taskStatus == TaskStatus::Pending ||
            snapshot.taskStatus == TaskStatus::Running;
        if (output.completed) {
            output.value = 1.0;
        } else if (running && rawValue <= 0.0) {
            output.value = 0.01;
        } else {
            output.value = rawValue;
        }
    }
    output.activity = !output.indeterminate &&
        (snapshot.taskStatus == TaskStatus::Pending || snapshot.taskStatus == TaskStatus::Running) &&
        !output.stopRequested;
    output.showPercent = !output.indeterminate;
    if (snapshot.taskStatus == TaskStatus::Failed) {
        output.role = ThemedStatusRole::Danger;
        if (output.status.empty()) output.status = L"任务失败";
        if (output.detail.empty()) output.detail = snapshot.error;
    } else if (snapshot.taskStatus == TaskStatus::Stopped) {
        output.role = ThemedStatusRole::Warning;
        if (output.status.empty()) output.status = L"任务已停止";
    } else if (snapshot.taskStatus == TaskStatus::Completed) {
        output.role = ThemedStatusRole::Success;
        if (output.status.empty()) output.status = L"任务完成";
    } else {
        output.role = ThemedStatusRole::Info;
    }
    return output;
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
    closePosted_ = false;
    hasSnapshot_ = false;
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
    ThemedStatusTextOptions statusOptions{};
    statusOptions.align = ThemedTextAlign::Start;
    status_ = ui.SelectableStatusText(options_.initialStatus, left, y, ui.contentWidth(), statusOptions);
    y = ui.nextRowY(y, ui.labelHeight());
    detail_ = ui.SelectableFieldText(
        kDetailTextId,
        ui.rect(left, y, ui.contentWidth(), ui.editHeight()),
        options_.initialDetail);
    y += ui.editHeight() + layout.sectionGap;
    ThemedProgressBarOptions progressOptions{};
    progressOptions.value = 0.0;
    progressOptions.indeterminate = true;
    progressOptions.showPercent = false;
    progress_ = ui.ProgressBar(options_.progressBarId, left, y, ui.contentWidth(), progressOptions);
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
    if (!snapshot.title.empty() && (!hasSnapshot_ || snapshot.title != lastSnapshot_.title)) SetWindowTextW(hwnd_, snapshot.title.c_str());
    if (!hasSnapshot_ || snapshot.status != lastSnapshot_.status) ThemedUi::SetText(status_, snapshot.status);
    if (!hasSnapshot_ || snapshot.detail != lastSnapshot_.detail) ThemedUi::SetText(detail_, snapshot.detail);
    if (!hasSnapshot_ || snapshot.value != lastSnapshot_.value ||
        snapshot.indeterminate != lastSnapshot_.indeterminate ||
        snapshot.activity != lastSnapshot_.activity ||
        snapshot.showPercent != lastSnapshot_.showPercent ||
        snapshot.text != lastSnapshot_.text) {
        ThemedProgressBarOptions progressOptions{};
        progressOptions.value = std::clamp(snapshot.value, 0.0, 1.0);
        progressOptions.indeterminate = snapshot.indeterminate;
        progressOptions.activity = snapshot.activity;
        progressOptions.showPercent = snapshot.showPercent;
        progressOptions.text = snapshot.text;
        ThemedUi::SetProgress(progress_, progressOptions);
    }
    const bool stopEnabled = !snapshot.finished && !snapshot.stopRequested;
    if (stopEnabled_ != stopEnabled) {
        stopEnabled_ = stopEnabled;
        if (!stopEnabled && GetFocus() == stop_) SetFocus(close_);
        ui.SetEnabled(stop_, stopEnabled);
    }
    lastSnapshot_ = std::move(snapshot);
    hasSnapshot_ = true;
    if (options_.closeOnCompleted && lastSnapshot_.completed && !closePosted_) {
        closePosted_ = true;
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}
