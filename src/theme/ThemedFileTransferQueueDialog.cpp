#include "ThemedFileTransferQueueDialog.h"

#include "ThemedWindowUi.h"
#include "Utilities.h"

#include <algorithm>

namespace {
constexpr UINT WM_TRANSFER_QUEUE_CHANGED = WM_APP + 0x2D1;
constexpr int ID_STOP_ALL = 6101;
constexpr int ID_CLOSE = 6102;
constexpr int ID_PROGRESS = 6103;
constexpr int ID_TABLE = 6104;
constexpr int ID_CURRENT_PROGRESS = 6105;

std::wstring StatusText(ThemedFileTransferStatus status) {
    switch (status) {
    case ThemedFileTransferStatus::Waiting: return L"等待中";
    case ThemedFileTransferStatus::Preparing: return L"准备中";
    case ThemedFileTransferStatus::UploadingMeta: return L"上传 META";
    case ThemedFileTransferStatus::Uploading: return L"上传中";
    case ThemedFileTransferStatus::Confirming: return L"确认中";
    case ThemedFileTransferStatus::Downloading: return L"下载中";
    case ThemedFileTransferStatus::Verifying: return L"校验中";
    case ThemedFileTransferStatus::UploadCompleted: return L"上传完成";
    case ThemedFileTransferStatus::DownloadCompleted: return L"下载完成";
    case ThemedFileTransferStatus::UploadFailed: return L"上传失败";
    case ThemedFileTransferStatus::DownloadFailed: return L"下载失败";
    case ThemedFileTransferStatus::Stopped: return L"已停止";
    }
    return L"等待中";
}

bool SameRow(const ThemedFileTransferRow& a, const ThemedFileTransferRow& b) {
    return a.id == b.id && a.fileName == b.fileName && a.absolutePath == b.absolutePath &&
        a.size == b.size && a.status == b.status && a.direction == b.direction &&
        a.phaseIndex == b.phaseIndex && a.phaseCount == b.phaseCount &&
        a.phaseTransferred == b.phaseTransferred && a.phaseTotal == b.phaseTotal &&
        a.contentTransferred == b.contentTransferred && a.contentTotal == b.contentTotal &&
        a.error == b.error && a.active == b.active;
}

int Percent(std::uint64_t value, std::uint64_t total) {
    if (total == 0) return 0;
    return static_cast<int>(std::clamp(
        static_cast<double>(value) * 100.0 / static_cast<double>(total), 0.0, 100.0));
}

std::wstring RowStatusText(const ThemedFileTransferRow& row) {
    if (row.active && row.phaseTotal > 0) {
        const std::wstring phase = StatusText(row.status);
        return phase + L" " + std::to_wstring(Percent(row.phaseTransferred, row.phaseTotal)) + L"%";
    }
    return StatusText(row.status);
}

std::wstring RowProgressText(const ThemedFileTransferRow& row) {
    std::wstring text = StatusText(row.status);
    if (row.status == ThemedFileTransferStatus::UploadCompleted ||
        row.status == ThemedFileTransferStatus::DownloadCompleted) {
        return text + L" · " + FormatByteSizeForDisplay(row.size) + L" / " +
            FormatByteSizeForDisplay(row.size);
    }
    if (row.status == ThemedFileTransferStatus::UploadFailed ||
        row.status == ThemedFileTransferStatus::DownloadFailed) {
        if (!row.error.empty()) text += L"：" + row.error;
        return text;
    }
    if (row.phaseIndex > 0 && row.phaseCount > 0) {
        text += L"（" + std::to_wstring(row.phaseIndex) + L" / " + std::to_wstring(row.phaseCount) + L"）";
    }
    if (row.phaseTotal > 0 && row.status != ThemedFileTransferStatus::Preparing) {
        text += L" · " + FormatByteSizeForDisplay(row.phaseTransferred) + L" / " +
            FormatByteSizeForDisplay(row.phaseTotal) + L"（" +
            std::to_wstring(Percent(row.phaseTransferred, row.phaseTotal)) + L"%）";
    } else if (row.contentTotal > 0 && row.status == ThemedFileTransferStatus::Stopped) {
        text += L" · " + FormatByteSizeForDisplay(row.contentTransferred) + L" / " +
            FormatByteSizeForDisplay(row.contentTotal);
    }
    return text;
}

std::wstring RowTooltipText(const ThemedFileTransferRow& row) {
    return row.fileName + L"  ·  " + FormatByteSizeForDisplay(row.size) + L"\n" +
        row.absolutePath + L"\n" + RowProgressText(row);
}

ThemedTableRow TableRow(const ThemedFileTransferRow& row) {
    return ThemedTableRow{static_cast<std::intptr_t>(row.id), {
        ThemedTableCell{row.fileName}, ThemedTableCell{row.absolutePath},
        ThemedTableCell{FormatByteSizeForDisplay(row.size)}, ThemedTableCell{RowStatusText(row)}},
        false, true, row.active};
}
}

ThemedFileTransferQueueDialog::ThemedFileTransferQueueDialog(ThemedFileTransferQueueDialogOptions options)
    : options_(std::move(options)) {}

ThemedFileTransferQueueDialog::~ThemedFileTransferQueueDialog() { Close(); }

bool ThemedFileTransferQueueDialog::Show() {
    if (IsWindow(hwnd_)) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
        ActivateWindow(hwnd_);
        NotifyChanged();
        return true;
    }
    if (!options_.instance || options_.className.empty()) return false;
    auto create = ThemedWindowUi::DialogOptions(options_.instance, options_.owner,
        options_.className.c_str(), options_.title.c_str(), Proc, this, options_.icon, options_.icon);
    create.clientWidth = 900;
    create.clientHeight = 480;
    create.resizable = true;
    create.maximizable = true;
    create.minimizable = true;
    hwnd_ = ThemedWindowUi::CreateWindowHandle(create, nullptr);
    if (!hwnd_) return false;
    ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    return true;
}

void ThemedFileTransferQueueDialog::Hide() { if (IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE); }
void ThemedFileTransferQueueDialog::Close() { if (IsWindow(hwnd_)) DestroyWindow(hwnd_); hwnd_ = nullptr; }
void ThemedFileTransferQueueDialog::NotifyChanged() {
    if (IsWindow(hwnd_) && !changePosted_.exchange(true)) PostMessageW(hwnd_, WM_TRANSFER_QUEUE_CHANGED, 0, 0);
}
bool ThemedFileTransferQueueDialog::IsOpen() const { return IsWindow(hwnd_) != FALSE; }
bool ThemedFileTransferQueueDialog::IsVisible() const { return IsWindow(hwnd_) && IsWindowVisible(hwnd_); }

LRESULT CALLBACK ThemedFileTransferQueueDialog::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ThemedFileTransferQueueDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ThemedFileTransferQueueDialog*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT ThemedFileTransferQueueDialog::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT common = 0;
    if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, common)) return common;
    switch (message) {
    case WM_CREATE:
        { RECT client{}; GetClientRect(hwnd_, &client);
        windowUi_ = std::make_unique<ThemedWindowUi>(options_.instance, options_.owner, hwnd_, options_.theme,
            DialogLayoutKind::Compact, client.right, client.bottom); }
        windowUi_->SetDpiChangedCallback([this](UINT) { LayoutControls(); });
        CreateControls(); Refresh(); return 0;
    case WM_SIZE:
        if (windowUi_) LayoutControls(); return 0;
    case WM_TRANSFER_QUEUE_CHANGED:
        changePosted_ = false;
        Refresh(); return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_STOP_ALL) { if (options_.requestStopAll) options_.requestStopAll(); return 0; }
        if (LOWORD(wParam) == ID_CLOSE || LOWORD(wParam) == IDCANCEL) { Hide(); return 0; }
        break;
    case WM_CLOSE:
        Hide(); return 0;
    case WM_PAINT:
        { PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd_, &ps); windowUi_->FillBackground(dc);
        windowUi_->DrawRegisteredTableFrames(dc); EndPaint(hwnd_, &ps); return 0; }
    case WM_NCDESTROY:
        hwnd_ = nullptr; windowUi_.reset(); return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ThemedFileTransferQueueDialog::CreateControls() {
    const ThemedUi ui = windowUi_->ui();
    status_ = ui.StatusText(L"等待传输", ui.contentLeft(), ui.contentTop(), ui.contentWidth(),
        ThemedStatusTextOptions{ThemedStatusRole::Info, ThemedTextAlign::Start});
    detail_ = ui.Label(L"尚未添加文件。", ui.contentLeft(), ui.contentTop(), ui.contentWidth());
    progress_ = ui.ProgressBar(ID_PROGRESS, ui.contentLeft(), ui.contentTop(), ui.contentWidth(),
        ThemedProgressBarOptions{0.0, false, true});
    currentProgress_ = ui.ProgressBar(ID_CURRENT_PROGRESS, ui.contentLeft(), ui.contentTop(), ui.contentWidth(),
        ThemedProgressBarOptions{0.0, false, true});
    table_ = ui.Table(ID_TABLE, RECT{0,0,1,1}, {
        {L"name", L"文件名", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.scale(180)},
        {L"path", L"绝对路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
        {L"size", L"大小", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Fixed, ui.scale(100)},
        {L"status", L"状态", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ui.scale(100)},
    }, ThemedTableOptions{ThemedTableSelection::Single, ThemedTableView::Details, false, true, true, true, true, false, false, false, true});
    ThemedTooltipOptions rowTooltipOptions{};
    rowTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
    ui.SetTableRowTooltip(table_, [this](int, std::intptr_t rowKey) {
        const auto found = std::find_if(lastSnapshot_.rows.begin(), lastSnapshot_.rows.end(),
            [rowKey](const auto& row) { return row.id == static_cast<std::uint64_t>(rowKey); });
        return found == lastSnapshot_.rows.end() ? std::wstring{} : RowTooltipText(*found);
    }, rowTooltipOptions);
    stop_ = ui.FooterButton(ID_STOP_ALL, L"停止", 0, 2);
    ui.SetTooltip(stop_, L"停止全部传输任务");
    close_ = ui.FooterButton(ID_CLOSE, L"关闭", 1, 2, true, true);
    LayoutControls();
}

void ThemedFileTransferQueueDialog::LayoutControls() {
    if (!windowUi_) return;
    RECT client{}; GetClientRect(hwnd_, &client);
    const ThemedUi ui(options_.instance, hwnd_, options_.theme, windowUi_->font(), DialogLayoutKind::Compact,
        client.right, client.bottom, windowUi_.get(), windowUi_.get(), windowUi_.get(), windowUi_.get());
    const auto& layout = ui.layout();
    int y = ui.contentTop();
    MoveWindow(status_, ui.contentLeft(), y, ui.contentWidth(), ui.labelHeight(), TRUE);
    y = ui.nextRowY(y, ui.labelHeight());
    MoveWindow(progress_, ui.contentLeft(), y, ui.contentWidth(), ui.progressBarHeight(), TRUE);
    y += ui.progressBarHeight() + layout.rowGap;
    MoveWindow(detail_, ui.contentLeft(), y, ui.contentWidth(), ui.labelHeight(), TRUE);
    y = ui.nextRowY(y, ui.labelHeight());
    if (currentProgressVisible_) {
        MoveWindow(currentProgress_, ui.contentLeft(), y, ui.contentWidth(), ui.progressBarHeight(), TRUE);
        y += ui.progressBarHeight() + layout.sectionGap;
    } else {
        y += layout.sectionGap - layout.rowGap;
    }
    const int footerY = layout.FooterButtonY(ui.clientHeight(), ui.footerButtonHeight());
    ui.MoveTable(table_, RECT{ui.contentLeft(), y, ui.contentLeft() + ui.contentWidth(), footerY - layout.footerGap});
    MoveWindow(stop_, layout.FooterButtonX(ui.clientWidth(), 0, 2), footerY,
        layout.footerButtonWidth, ui.footerButtonHeight(), TRUE);
    MoveWindow(close_, layout.FooterButtonX(ui.clientWidth(), 1, 2), footerY,
        layout.footerButtonWidth, ui.footerButtonHeight(), TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ThemedFileTransferQueueDialog::Refresh() {
    if (!windowUi_) return;
    ThemedFileTransferQueueSnapshot snapshot;
    if (options_.readSnapshot) snapshot = options_.readSnapshot();
    const ThemedUi ui = windowUi_->ui();
    if (!snapshot.title.empty() && (!hasSnapshot_ || snapshot.title != lastSnapshot_.title)) SetWindowTextW(hwnd_, snapshot.title.c_str());
    if (!hasSnapshot_ || snapshot.status != lastSnapshot_.status) ThemedUi::SetText(status_, snapshot.status);
    if (!hasSnapshot_ || snapshot.detail != lastSnapshot_.detail) ThemedUi::SetText(detail_, snapshot.detail);
    if (!hasSnapshot_ || snapshot.progress != lastSnapshot_.progress) ThemedUi::SetProgress(progress_, std::clamp(snapshot.progress, 0.0, 1.0), false);
    if (!hasSnapshot_ || snapshot.currentProgress != lastSnapshot_.currentProgress) {
        ThemedUi::SetProgress(currentProgress_, std::clamp(snapshot.currentProgress, 0.0, 1.0), false);
    }
    if (!hasSnapshot_) {
        std::vector<ThemedTableRow> rows;
        rows.reserve(snapshot.rows.size());
        for (const auto& row : snapshot.rows) rows.push_back(TableRow(row));
        ThemedUi::SetTableRows(table_, rows);
    } else {
        for (int index = ThemedUi::TableRowCount(table_) - 1; index >= 0; --index) {
            const auto key = static_cast<std::uint64_t>(ThemedUi::TableRowKey(table_, index));
            if (std::none_of(snapshot.rows.begin(), snapshot.rows.end(),
                    [key](const auto& row) { return row.id == key; })) {
                ThemedUi::RemoveTableRow(table_, index);
            }
        }
        for (const auto& row : snapshot.rows) {
            const int index = ThemedUi::FindTableRowByKey(table_, static_cast<std::intptr_t>(row.id));
            if (index < 0) {
                ThemedUi::AppendTableRow(table_, TableRow(row));
                continue;
            }
            const auto previous = std::find_if(lastSnapshot_.rows.begin(), lastSnapshot_.rows.end(),
                [&](const auto& item) { return item.id == row.id; });
            if (previous == lastSnapshot_.rows.end() || !SameRow(row, *previous)) {
                ThemedUi::UpdateTableRow(table_, index, TableRow(row));
            }
        }
    }
    if (!hasSnapshot_ || snapshot.currentProgressVisible != currentProgressVisible_) {
        currentProgressVisible_ = snapshot.currentProgressVisible;
        ui.SetVisible(currentProgress_, currentProgressVisible_);
        LayoutControls();
    }
    const bool enabled = snapshot.running && !snapshot.stopRequested;
    if (!hasSnapshot_ || enabled != stopEnabled_) { stopEnabled_ = enabled; ui.SetEnabled(stop_, enabled); }
    lastSnapshot_ = std::move(snapshot);
    hasSnapshot_ = true;
    ThemedUi::RefreshTableRowTooltip(table_);
}
