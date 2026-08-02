#include "WebDavFileBatchConfirmDialog.h"

#include "../../resources/resource.h"

#include "AppLog.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"

#include <sstream>
#include <utility>

namespace {
constexpr int kDetailsTextId = 511;
constexpr int kCopyDetailsId = 512;
}

WebDavFileBatchConfirmDialog::WebDavFileBatchConfirmDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    std::wstring title,
    std::wstring intro,
    std::wstring confirmText,
    std::vector<WebDavFileBatchConfirmItem> items,
    bool danger)
    : owner_(owner),
      instance_(instance),
      theme_(theme),
      title_(std::move(title)),
      intro_(std::move(intro)),
      confirmText_(std::move(confirmText)),
      items_(std::move(items)),
      danger_(danger) {}

bool WebDavFileBatchConfirmDialog::Run() {
    const std::wstring className = L"QuattroWebDavFileBatchConfirmDialog_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    auto options = ThemedWindowUi::DialogOptions(
        instance_,
        owner_,
        className.c_str(),
        title_.c_str(),
        Proc,
        this,
        LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)),
        LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)));
    options.clientWidth = kThemedDetailsClientWidth + 160;
    options.clientHeight = kThemedDetailsClientHeight + 140;
    options.resizable = true;
    options.placement = ThemedWindowPlacement::OffsetOwner;
    options.offsetX = 56;
    options.offsetY = 56;
    std::wstring error;
    hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
    if (!hwnd_) {
        WriteAppLog(L"WebDAV 批量确认窗口创建失败: " + error);
        return false;
    }
    windowUi_->ShowModal();
    UpdateWindow(hwnd_);
    MSG message{};
    while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return accepted_;
}

std::wstring WebDavFileBatchConfirmDialog::DetailsText() const {
    std::size_t actionable = 0;
    for (const auto& item : items_) {
        if (item.actionable) ++actionable;
    }

    std::wostringstream text;
    if (!intro_.empty()) {
        text << intro_ << L"\r\n\r\n";
    }
    text << L"可执行：" << actionable << L" 项";
    if (items_.size() > actionable) {
        text << L"，将跳过：" << (items_.size() - actionable) << L" 项";
    }
    text << L"\r\n\r\n";
    for (std::size_t index = 0; index < items_.size(); ++index) {
        const auto& item = items_[index];
        text << L"#" << (index + 1) << L"  " << (item.actionable ? L"[执行] " : L"[跳过] ")
             << (item.name.empty() ? L"(未命名文件)" : item.name) << L"\r\n";
        if (!item.sizeText.empty()) {
            text << L"大小：" << item.sizeText << L"\r\n";
        }
        if (!item.status.empty()) {
            text << L"状态：" << item.status << L"\r\n";
        }
        if (!item.localPath.empty()) {
            text << L"本地路径：" << item.localPath << L"\r\n";
        }
        text << L"\r\n";
    }
    if (danger_) {
        text << L"注意：删除操作会移除远端记录和文件内容，不会删除本地文件。\r\n";
    }
    return text.str();
}

LRESULT CALLBACK WebDavFileBatchConfirmDialog::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<WebDavFileBatchConfirmDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        dialog = static_cast<WebDavFileBatchConfirmDialog*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
    }
    return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT WebDavFileBatchConfirmDialog::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT common = 0;
    if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, common)) {
        return common;
    }
    switch (message) {
    case WM_CREATE: {
        RECT client{};
        GetClientRect(hwnd_, &client);
        windowUi_ = std::make_unique<ThemedWindowUi>(
            instance_,
            owner_,
            hwnd_,
            theme_,
            DialogLayoutKind::Standard,
            client.right,
            client.bottom);
        const ThemedUi ui = windowUi_->ui();
        const auto& layout = ui.layout();
        const int footerHeight = ui.footerButtonHeight();
        const RECT frame{
            ui.contentLeft(),
            ui.contentTop(),
            ui.contentLeft() + ui.contentWidth(),
            ui.footerButtonY(footerHeight) - layout.footerGap};
        ui.DetailText(kDetailsTextId, frame, DetailsText());
        ui.FooterButton(kCopyDetailsId, L"复制清单", 0, 3);
        ui.FooterButton(IDOK, confirmText_.empty() ? L"确认" : confirmText_, 1, 3, true, true);
        ui.FooterButton(IDCANCEL, L"取消", 2, 3);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        windowUi_->FillBackground(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kCopyDetailsId) {
            if (ThemedUi::CopyTextToClipboard(hwnd_, DetailsText()) && windowUi_) {
                windowUi_->ui().ShowToast(L"清单已复制。");
            }
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            accepted_ = true;
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            accepted_ = false;
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        break;
    case WM_CLOSE:
        accepted_ = false;
        done_ = true;
        DestroyWindow(hwnd_);
        return 0;
    case WM_NCDESTROY:
        done_ = true;
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
