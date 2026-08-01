#include "WebDavFileDetailsDialog.h"

#include "../../resources/resource.h"

#include "AppLog.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"

#include <cstdint>
#include <sstream>
#include <utility>

namespace {
constexpr int kDetailsTextId = 501;
constexpr int kCopyDetailsId = 502;

std::wstring FormatDetailsFileSize(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::to_wstring((bytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull)) + L" MB";
    }
    if (bytes >= 1024ull) {
        return std::to_wstring((bytes + 1023ull) / 1024ull) + L" KB";
    }
    return std::to_wstring(bytes) + L" B";
}
}

WebDavFileDetailsDialog::WebDavFileDetailsDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    WebDavFileRecord record,
    std::wstring remoteRecordPath)
    : owner_(owner),
      instance_(instance),
      theme_(theme),
      record_(std::move(record)),
      remoteRecordPath_(std::move(remoteRecordPath)) {}

bool WebDavFileDetailsDialog::Run() {
    const std::wstring className = L"QuattroWebDavFileDetailsDialog_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    auto options = ThemedWindowUi::DialogOptions(
        instance_,
        owner_,
        className.c_str(),
        L"WebDAV 文件详情",
        Proc,
        this,
        LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)),
        LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)));
    options.clientWidth = kThemedDetailsClientWidth + 80;
    options.clientHeight = kThemedDetailsClientHeight + 100;
    options.placement = ThemedWindowPlacement::OffsetOwner;
    options.offsetX = 48;
    options.offsetY = 48;
    std::wstring error;
    hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
    if (!hwnd_) {
        WriteAppLog(L"WebDAV 文件详情窗口创建失败: " + error);
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
    return true;
}

std::wstring WebDavFileDetailsDialog::DetailsText() const {
    const bool healthy = record_.health == WebDavFileRecordHealth::Healthy;
    std::wstring healthText = L"正常";
    if (record_.health == WebDavFileRecordHealth::MissingMetadata) {
        healthText = L"Meta 缺失";
    } else if (record_.health == WebDavFileRecordHealth::InvalidMetadata) {
        healthText = L"Meta 无效";
    } else if (record_.health == WebDavFileRecordHealth::MetadataReadFailed) {
        healthText = L"Meta 读取失败";
    }

    std::wostringstream text;
    text << L"文件名：" << record_.displayName << L"\r\n";
    text << L"文件大小：" << (healthy ? FormatDetailsFileSize(record_.size) : std::wstring(L"—")) << L"\r\n";
    const std::wstring uploadedAtLocal = WebDavFileService::FormatUploadedAtLocal(record_.uploadedAtUtc);
    text << L"远程更新时间：" << (healthy && !uploadedAtLocal.empty() ? uploadedAtLocal : std::wstring(L"获取失败")) << L"\r\n";
    const std::wstring sourceModifiedAt = WebDavFileService::FormatSourceModifiedAtLocal(record_);
    text << L"远端记录时间：" << (healthy && !sourceModifiedAt.empty() ? sourceModifiedAt : std::wstring(L"-")) << L"\r\n";
    const std::wstring localModifiedAt = WebDavFileService::FormatLocalModifiedAt(record_.absolutePath);
    text << L"本地修改时间：" << (localModifiedAt.empty() ? std::wstring(L"-") : localModifiedAt) << L"\r\n";
    text << L"本地状态：" << WebDavFileService::LocalSyncStatusText(record_) << L"\r\n";
    text << L"上传状态：";
    if (healthy) {
        text << record_.uploadState << (record_.contentReady ? L" · 内容可用" : L" · 内容不可用");
    } else {
        text << healthText;
    }
    text << L"\r\n\r\n";
    text << (healthy ? L"系统绝对路径：" : L"错误信息：")
         << (healthy ? record_.absolutePath : record_.recordError) << L"\r\n\r\n";
    text << L"远端记录路径：" << remoteRecordPath_ << L"\r\n\r\n";
    text << (healthy ? L"SHA-256：" : L"记录 ID：") << (healthy ? record_.sha256 : record_.id);
    return text.str();
}

LRESULT CALLBACK WebDavFileDetailsDialog::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<WebDavFileDetailsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        dialog = static_cast<WebDavFileDetailsDialog*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
    }
    return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT WebDavFileDetailsDialog::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
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
        ui.FooterButton(kCopyDetailsId, L"复制全部", 0, 2);
        ui.FooterButton(IDOK, L"确定", 1, 2, true, true);
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
                windowUi_->ui().ShowToast(L"详情已复制。");
            }
            return 0;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        break;
    case WM_CLOSE:
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
