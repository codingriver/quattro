#include "UpdateDownloadDialog.h"

#include "ConfirmDialog.h"
#include "ThemedFormLayout.h"
#include "ThemedControls.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <atomic>
#include <commctrl.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
constexpr int ID_PROGRESS = 2701;
constexpr int ID_CANCEL = 2702;
constexpr UINT_PTR ID_REFRESH_TIMER = 21;
constexpr UINT WM_DOWNLOAD_DONE = WM_APP + 0x90;

std::wstring FormatBytes(std::uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::wstringstream stream;
    if (unit == 0) {
        stream << bytes << L" " << units[unit];
    } else {
        stream.setf(std::ios::fixed);
        stream.precision(value >= 10.0 ? 1 : 2);
        stream << value << L" " << units[unit];
    }
    return stream.str();
}

void SetText(HWND hwnd, const std::wstring& value) {
    if (hwnd) {
        SetWindowTextW(hwnd, value.c_str());
    }
}

class UpdateDownloadDialog {
public:
    UpdateDownloadDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        std::filesystem::path appDirectory,
        UpdateReleaseInfo info,
        UpdateDownloadResult& result,
        std::wstring& error)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          appDirectory_(std::move(appDirectory)),
          info_(std::move(info)),
          result_(result),
          error_(error) {}

    bool Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_,
            owner_,
            L"QuattroUpdateDownloadDialog",
            L"下载更新",
            UpdateDownloadDialog::Proc,
            this,
            icon,
            icon);
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error_);
        if (!hwnd_) {
            return false;
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
        JoinWorker();
        return succeeded_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        UpdateDownloadDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<UpdateDownloadDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<UpdateDownloadDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void StartWorker() {
        worker_ = std::thread([this]() {
            UpdateCheckService service(appDirectory_);
            UpdateDownloadResult result;
            std::wstring error;
            const bool ok = service.DownloadUpdate(
                info_,
                result,
                error,
                [this](const UpdateDownloadProgress& progress) {
                    downloadedBytes_.store(progress.downloadedBytes);
                    if (progress.totalBytes != 0) {
                        totalBytes_.store(progress.totalBytes);
                    }
                },
                [this]() {
                    return cancelRequested_.load();
                });

            {
                std::lock_guard<std::mutex> lock(resultMutex_);
                result_ = result;
                error_ = error;
                succeeded_ = ok;
            }
            PostMessageW(hwnd_, WM_DOWNLOAD_DONE, 0, 0);
        });
    }

    void JoinWorker() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void Cancel() {
        if (workerDone_) {
            Close();
            return;
        }
        if (cancelRequested_.load()) {
            return;
        }
        const ConfirmDialogResult result = ShowConfirmDialog(
            hwnd_,
            instance_,
            theme_,
            L"取消下载",
            L"更新包正在下载，确定要取消下载吗？",
            L"取消下载",
            L"继续下载");
        if (result != ConfirmDialogResult::Primary) {
            return;
        }
        cancelRequested_.store(true);
        SetText(statusLabel_, L"正在取消下载...");
        EnableWindow(cancelButton_, FALSE);
    }

    void Close() {
        done_ = true;
        KillTimer(hwnd_, ID_REFRESH_TIMER);
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        DestroyWindow(hwnd_);
    }

    void RefreshUi() {
        const std::uint64_t downloaded = downloadedBytes_.load();
        const std::uint64_t total = totalBytes_.load();
        SetText(sizeLabel_, total == 0 ? L"未知" : FormatBytes(total));
        SetText(downloadedLabel_, FormatBytes(downloaded));
        if (!cancelRequested_.load()) {
            SetText(statusLabel_, total != 0 && downloaded >= total ? L"正在校验更新包..." : L"正在下载更新包...");
        }
        const bool indeterminate = total == 0;
        const double value = total == 0 ? 0.0 : static_cast<double>(downloaded) / static_cast<double>(total);
        ThemedControls::SetProgressBarValue(progressBar_, value, indeterminate);
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
            const int contentWidth = ui.contentWidth();
            int y = ui.contentTop();
            auto titleRow = form.row(y, ThemedRowAlign::Left, {form.text(contentWidth)});
            titleLabel_ = ui.Label(L"正在下载更新", titleRow[0].left, titleRow[0].top, titleRow[0].right - titleRow[0].left);
            y = form.nextRowY(y, {form.text(contentWidth)});
            const int labelWidth = form.labelWidthForTexts({L"版本：", L"文件：", L"大小：", L"已下载："});
            const int valueWidth = contentWidth - labelWidth - ui.layout().labelGap;
            auto versionGroup = form.labelText(labelWidth, valueWidth);
            auto versionRows = form.rowGroups(y, ThemedRowAlign::Left, {versionGroup});
            ui.Label(L"版本：", versionRows[0][0].left, versionRows[0][0].top, versionRows[0][0].right - versionRows[0][0].left);
            versionLabel_ = ui.Label(info_.latestVersion, versionRows[0][1].left, versionRows[0][1].top, versionRows[0][1].right - versionRows[0][1].left);
            y = form.nextRowY(y, {versionGroup});
            auto fileGroup = form.labelText(labelWidth, valueWidth);
            auto fileRows = form.rowGroups(y, ThemedRowAlign::Left, {fileGroup});
            ui.Label(L"文件：", fileRows[0][0].left, fileRows[0][0].top, fileRows[0][0].right - fileRows[0][0].left);
            fileLabel_ = ui.Label(info_.assetName, fileRows[0][1].left, fileRows[0][1].top, fileRows[0][1].right - fileRows[0][1].left);
            y = form.nextRowY(y, {fileGroup});
            auto sizeGroup = form.labelText(labelWidth, valueWidth);
            auto sizeRows = form.rowGroups(y, ThemedRowAlign::Left, {sizeGroup});
            ui.Label(L"大小：", sizeRows[0][0].left, sizeRows[0][0].top, sizeRows[0][0].right - sizeRows[0][0].left);
            sizeLabel_ = ui.Label(L"未知", sizeRows[0][1].left, sizeRows[0][1].top, sizeRows[0][1].right - sizeRows[0][1].left);
            y = form.nextRowY(y, {sizeGroup});
            auto downloadedGroup = form.labelText(labelWidth, valueWidth);
            auto downloadedRows = form.rowGroups(y, ThemedRowAlign::Left, {downloadedGroup});
            ui.Label(L"已下载：", downloadedRows[0][0].left, downloadedRows[0][0].top, downloadedRows[0][0].right - downloadedRows[0][0].left);
            downloadedLabel_ = ui.Label(L"0 B", downloadedRows[0][1].left, downloadedRows[0][1].top, downloadedRows[0][1].right - downloadedRows[0][1].left);
            y = form.nextRowY(y, {downloadedGroup});
            auto progressRow = form.row(y, ThemedRowAlign::Left, {form.progress(contentWidth)});
            progressBar_ = ui.ProgressBar(ID_PROGRESS, progressRow[0].left, progressRow[0].top, progressRow[0].right - progressRow[0].left);
            y = form.nextRowY(y, {form.progress(contentWidth)});
            auto statusRow = form.row(y, ThemedRowAlign::Left, {form.text(contentWidth)});
            statusLabel_ = ui.Label(L"正在连接下载服务器...", statusRow[0].left, statusRow[0].top, statusRow[0].right - statusRow[0].left);
            cancelButton_ = ui.FooterButton(ID_CANCEL, L"取消", 0, 1);
            totalBytes_.store(info_.assetSizeBytes);
            RefreshUi();
            SetTimer(hwnd_, ID_REFRESH_TIMER, 1000, nullptr);
            StartWorker();
            return 0;
        }
        case WM_TIMER:
            if (wParam == ID_REFRESH_TIMER) {
                RefreshUi();
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_CANCEL) {
                Cancel();
                return 0;
            }
            break;
        case WM_DOWNLOAD_DONE:
            workerDone_ = true;
            KillTimer(hwnd_, ID_REFRESH_TIMER);
            RefreshUi();
            Close();
            return 0;
        case WM_CLOSE:
            Cancel();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::filesystem::path appDirectory_;
    UpdateReleaseInfo info_;
    UpdateDownloadResult& result_;
    std::wstring& error_;
    HWND hwnd_ = nullptr;
    HWND titleLabel_ = nullptr;
    HWND versionLabel_ = nullptr;
    HWND fileLabel_ = nullptr;
    HWND sizeLabel_ = nullptr;
    HWND downloadedLabel_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND progressBar_ = nullptr;
    HWND cancelButton_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool done_ = false;
    bool workerDone_ = false;
    bool succeeded_ = false;
    std::atomic_bool cancelRequested_{false};
    std::atomic<std::uint64_t> downloadedBytes_{0};
    std::atomic<std::uint64_t> totalBytes_{0};
    std::thread worker_;
    std::mutex resultMutex_;
};
}

bool ShowUpdateDownloadDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    const UpdateReleaseInfo& info,
    UpdateDownloadResult& result,
    std::wstring& error) {
    UpdateDownloadDialog dialog(owner, instance, theme, appDirectory, info, result, error);
    return dialog.Run();
}
