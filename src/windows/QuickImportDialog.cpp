#include "QuickImportDialog.h"

#include "DialogLayout.h"
#include "FileDialog.h"
#include "IconResolverService.h"
#include "TaskExecutionService.h"
#include "ThemedTaskProgressDialog.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <algorithm>
#include <commctrl.h>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <system_error>
#include <unordered_map>
#include <windowsx.h>

namespace {

constexpr int kDialogWidth = 760;
constexpr int kDialogHeight = 520;
constexpr int IdSource = 1001;
constexpr int IdScan = 1002;
constexpr int IdList = 1003;
constexpr int IdImport = IDOK;
constexpr int IdCancel = IDCANCEL;
constexpr int IdSelectAll = 1004;
constexpr int IdSelectNone = 1005;
constexpr int IdPickDirectory = 1006;
constexpr int IdViewMode = 1009;
constexpr int IdDirectory = 1010;
constexpr int IdSourceDesktop = 1011;
constexpr int IdSourceStartMenu = 1012;
constexpr int IdViewListTab = 1013;
constexpr int IdSourceStoreApps = 1014;
constexpr UINT_PTR IdScanPollTimer = 1015;
constexpr UINT WM_QUICK_IMPORT_ICONS_DONE = WM_APP + 0x8D;
constexpr int kQuickImportIconCaptureSize = 48;

struct QuickImportIconResult {
    std::wstring stableKey;
    ResolvedIcon icon;
};

struct QuickImportIconLoadResult {
    std::uint64_t generation = 0;
    std::vector<QuickImportIconResult> icons;
};

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::wstring TypeText(int type) {
    switch (type) {
    case 1:
        return L"文件夹";
    case 2:
        return L"网址";
    case 3:
        return L"系统项";
    default:
        return L"程序";
    }
}

DWORD QuickImportTestIconDelayMs() {
    wchar_t testMode[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_TEST_MODE", testMode, static_cast<DWORD>(std::size(testMode))) == 0) {
        return 0;
    }
    wchar_t delayText[16]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_TEST_QUICK_IMPORT_ICON_DELAY_MS",
            delayText,
            static_cast<DWORD>(std::size(delayText))) == 0) {
        return 0;
    }
    return (std::min<DWORD>)(wcstoul(delayText, nullptr, 10), 100);
}

bool PickFolder(HWND owner, std::filesystem::path& directory) {
    CommonFileDialogOptions options{};
    options.owner = owner;
    options.mode = CommonFileDialogMode::FolderOnly;
    options.context = L"快速导入目录";
    options.title = L"选择快速导入目录";
    options.defaultPath = directory.wstring();
    CommonFileDialogResult result{};
    if (!ShowCommonFileDialog(options, result)) {
        return false;
    }
    directory = result.path;
    return true;
}

std::filesystem::path KnownFolderPathOrEmpty(REFKNOWNFOLDERID folderId) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) {
        return {};
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme, const std::vector<Link>&, std::vector<Link>& selectedLinks)
        : owner_(owner), instance_(instance), theme_(theme), selectedLinks_(selectedLinks) {}

    ~DialogWindow() {
        if (scanTask_) {
            scanTask_->RequestStop();
        }
        StopIconLoadTask();
        DestroyImageLists();
    }

    bool Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroQuickImportDialog", L"快速导入", DialogWindow::WindowProc, this, icon, icon);
        options.clientWidth = kDialogWidth;
        options.clientHeight = kDialogHeight;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return false;
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
        return accepted_;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        DialogWindow* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<DialogWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact, kDialogWidth, kDialogHeight);
            windowUi_->SetDpiChangedCallback([this](UINT) {
                LayoutControls();
            });
            CreateControls();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            Paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            Paint(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_NOTIFY: {
            LRESULT result = 0;
            if (HandleListNotify(lParam, result)) {
                return result;
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == IdScanPollTimer) {
                FinishScanIfReady();
                return 0;
            }
            break;
        case WM_QUICK_IMPORT_ICONS_DONE:
            ApplyIconLoadResult(static_cast<std::uint64_t>(wParam));
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IdSource:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    ApplySelectedSource();
                }
                return 0;
            case IdViewMode:
                return 0;
            case IdDirectory:
                if (HIWORD(wParam) == EN_CHANGE && status_) {
                    if (!applyingDirectoryText_) {
                        ClearScanResults();
                    }
                    windowUi_->SetEditFrameState(directoryText_, false, false);
                    SetWindowTextW(status_, L"尚未扫描");
                }
                return 0;
            case IdPickDirectory:
                PickScanDirectory();
                return 0;
            case IdScan:
                Scan();
                return 0;
            case IdSelectAll:
                SetAllChecks(true);
                return 0;
            case IdSelectNone:
                SetAllChecks(false);
                return 0;
            case IdImport:
                Accept();
                return 0;
            case IdCancel:
                Close(false);
                return 0;
            default:
                break;
            }
            return 0;
        case WM_CLOSE:
            Close(false);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    void CreateControls() {
        layout_ = windowUi_->ui().layout();
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = client.right - client.left;
        const int clientHeight = client.bottom - client.top;
        const ThemedUi ui = windowUi_->ui();
        const int labelHeight = ui.labelHeight();
        const int fieldHeight = ui.editHeight();
        const int buttonHeight = ui.footerButtonHeight();
        const int tabHeight = ui.tabButtonHeight();
        const int topY = layout_.contentInsetY;
        const int sourceWidth = ui.scale(252);
        const int pickDirectoryWidth = ui.buttonWidth(L"选择目录", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int scanWidth = ui.buttonWidth(L"扫描", ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int selectAllWidth = ui.buttonWidth(L"全选", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        const int selectNoneWidth = ui.buttonWidth(L"清空", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        const int contentLeft = layout_.contentInsetX;
        const int contentRight = clientWidth - layout_.contentInsetX;

        ThemedTabControlOptions sourceOptions{};
        sourceOptions.activeIndex = 0;
        sourceOptions.equalWidth = true;
        sourceOptions.appearance = ThemedTabControlAppearance::SoftPill;
        sourceOptions.orientation = ThemedTabControlOrientation::Horizontal;
        sourceOptions.containerStyle = ThemedTabControlContainerStyle::Borderless;
        sourceTabs_ = ui.TabControl(
            IdSource,
            RECT{contentLeft, topY, contentLeft + sourceWidth, topY + tabHeight},
            {
                ThemedTabItem{IdSourceDesktop, L"桌面", true},
                ThemedTabItem{IdSourceStartMenu, L"开始菜单", true},
                ThemedTabItem{IdSourceStoreApps, L"商店应用", true},
            },
            sourceOptions);

        selectedDirectory_ = KnownFolderPathOrEmpty(FOLDERID_Desktop);
        const int directoryX = contentLeft + sourceWidth + layout_.controlGapX;
        const int directoryWidth = std::max(
            ui.scale(120),
            contentRight - directoryX - pickDirectoryWidth - scanWidth - layout_.controlGapX * 2);
        directoryFrame_ = RECT{directoryX, topY, directoryX + directoryWidth, topY + fieldHeight};
        ThemedEditOptions directoryOptions{};
        directoryOptions.placeholder = L"输入要扫描的绝对路径";
        directoryText_ = ui.Edit(IdDirectory, directoryFrame_, selectedDirectory_.wstring(), directoryOptions);
        pickDirectoryButton_ = ui.Button(IdPickDirectory, L"选择目录", directoryFrame_.right + layout_.controlGapX, topY, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, pickDirectoryWidth);
        scanButton_ = ui.Button(IdScan, L"扫描", directoryFrame_.right + layout_.controlGapX + pickDirectoryWidth + layout_.controlGapX, topY, ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, scanWidth);

        const int statusRowY = topY + fieldHeight + layout_.rowGap;
        const int statusRowHeight = std::max({ui.compactButtonHeight(), tabHeight, labelHeight});
        const int viewWidth = ui.scale(64);
        ThemedTabControlOptions viewOptions{};
        viewOptions.activeIndex = 0;
        viewOptions.equalWidth = true;
        viewOptions.appearance = ThemedTabControlAppearance::SoftPill;
        viewOptions.orientation = ThemedTabControlOrientation::Horizontal;
        viewOptions.containerStyle = ThemedTabControlContainerStyle::Borderless;
        viewTabs_ = ui.TabControl(
            IdViewMode,
            RECT{contentLeft, statusRowY, contentLeft + viewWidth, statusRowY + statusRowHeight},
            {
                ThemedTabItem{IdViewListTab, L"列表", true},
            },
            viewOptions);

        const int actionWidth = selectAllWidth + layout_.controlGapX + selectNoneWidth;
        const int actionX = contentRight - actionWidth;
        const int actionY = statusRowY + std::max(0, (statusRowHeight - ui.compactButtonHeight()) / 2);
        selectAllButton_ = ui.Button(IdSelectAll, L"全选", actionX, actionY, ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Fixed, selectAllWidth);
        selectNoneButton_ = ui.Button(IdSelectNone, L"清空", actionX + selectAllWidth + layout_.controlGapX, actionY, ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Fixed, selectNoneWidth);

        const int statusX = contentLeft + viewWidth + layout_.controlGapX;
        const int statusY = statusRowY + std::max(0, (statusRowHeight - labelHeight) / 2);
        status_ = ui.StatusText(
            L"尚未扫描",
            statusX,
            statusY,
            std::max(0, actionX - layout_.controlGapX - statusX),
            ThemedStatusTextOptions{ThemedStatusRole::Normal, ThemedTextAlign::Start});

        listFrame_ = RECT{layout_.contentInsetX, statusRowY + statusRowHeight + layout_.rowGap, clientWidth - layout_.contentInsetX, clientHeight - layout_.footerInsetY - buttonHeight - layout_.footerGap};
        ThemedTableOptions tableOptions{};
        tableOptions.checkable = true;
        tableOptions.allowHorizontalScroll = false;
        tableOptions.reserveScrollBarGutter = true;
        const int nameColumnWidth = ui.scale(170);
        const int typeColumnWidth = ui.tableColumnWidth({L"程序", L"文件夹", L"网址"});
        const int statusColumnWidth = ui.tableColumnWidth(L"可导入");
        list_ = ui.Table(
            IdList,
            listFrame_,
            {
                ThemedTableColumn{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, nameColumnWidth},
                ThemedTableColumn{L"type", L"类型", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, typeColumnWidth},
                ThemedTableColumn{L"path", L"路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                ThemedTableColumn{L"status", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, statusColumnWidth},
            },
            tableOptions);
        RebuildImageLists();

        const int footerY = layout_.FooterButtonY(clientHeight, buttonHeight);
        const int buttonGroupWidth = layout_.footerButtonWidth * 2 + layout_.footerButtonGap;
        const int buttonX = layout_.CenteredGroupX(clientWidth, buttonGroupWidth);
        importButton_ = ui.Button(IdImport, L"导入选中", buttonX, footerY, ThemedButtonRole::Primary, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, layout_.footerButtonWidth, true);
        cancelButton_ = ui.Button(IdCancel, L"取消", buttonX + layout_.footerButtonWidth + layout_.footerButtonGap, footerY, ThemedButtonRole::Normal, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, layout_.footerButtonWidth);

        LayoutControls();
        ApplyViewMode();
    }

    void LayoutControls() {
        if (!windowUi_ || !sourceTabs_) {
            return;
        }

        const ThemedUi ui = windowUi_->ui();
        layout_ = ui.layout();
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = client.right - client.left;
        const int clientHeight = client.bottom - client.top;
        const int labelHeight = ui.labelHeight();
        const int fieldHeight = ui.editHeight();
        const int buttonHeight = ui.footerButtonHeight();
        const int tabHeight = ui.tabButtonHeight();
        const int topY = layout_.contentInsetY;
        const int contentLeft = layout_.contentInsetX;
        const int contentRight = clientWidth - layout_.contentInsetX;
        const int sourceWidth = ui.scale(252);
        const int pickDirectoryWidth = ui.buttonWidth(L"选择目录", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int scanWidth = ui.buttonWidth(L"扫描", ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int selectAllWidth = ui.buttonWidth(L"全选", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        const int selectNoneWidth = ui.buttonWidth(L"清空", ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);

        ui.MoveControl(sourceTabs_, contentLeft, topY, sourceWidth);

        const int directoryX = contentLeft + sourceWidth + layout_.controlGapX;
        const int directoryWidth = std::max(
            ui.scale(120),
            contentRight - directoryX - pickDirectoryWidth - scanWidth - layout_.controlGapX * 2);
        directoryFrame_ = RECT{directoryX, topY, directoryX + directoryWidth, topY + fieldHeight};
        windowUi_->MoveEditFrame(directoryText_, directoryFrame_);
        ui.MoveControl(pickDirectoryButton_, directoryFrame_.right + layout_.controlGapX, topY, pickDirectoryWidth);
        ui.MoveControl(scanButton_, directoryFrame_.right + layout_.controlGapX + pickDirectoryWidth + layout_.controlGapX, topY, scanWidth);

        const int statusRowY = topY + fieldHeight + layout_.rowGap;
        const int statusRowHeight = std::max({ui.compactButtonHeight(), tabHeight, labelHeight});
        const int viewWidth = ui.scale(64);
        ui.MoveControl(viewTabs_, contentLeft, statusRowY, viewWidth);

        const int actionWidth = selectAllWidth + layout_.controlGapX + selectNoneWidth;
        const int actionX = contentRight - actionWidth;
        const int actionY = statusRowY + std::max(0, (statusRowHeight - ui.compactButtonHeight()) / 2);
        ui.MoveControl(selectAllButton_, actionX, actionY, selectAllWidth);
        ui.MoveControl(selectNoneButton_, actionX + selectAllWidth + layout_.controlGapX, actionY, selectNoneWidth);

        const int statusX = contentLeft + viewWidth + layout_.controlGapX;
        const int statusY = statusRowY + std::max(0, (statusRowHeight - labelHeight) / 2);
        ui.MoveControl(status_, statusX, statusY, std::max(0, actionX - layout_.controlGapX - statusX));

        listFrame_ = RECT{
            layout_.contentInsetX,
            statusRowY + statusRowHeight + layout_.rowGap,
            clientWidth - layout_.contentInsetX,
            clientHeight - layout_.footerInsetY - buttonHeight - layout_.footerGap};
        SetWindowPos(
            list_,
            nullptr,
            listFrame_.left,
            listFrame_.top,
            listFrame_.right - listFrame_.left,
            listFrame_.bottom - listFrame_.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        windowUi_->RegisterTableFrame(list_, listFrame_);

        const int footerY = layout_.FooterButtonY(clientHeight, buttonHeight);
        const int buttonGroupWidth = layout_.footerButtonWidth * 2 + layout_.footerButtonGap;
        const int buttonX = layout_.CenteredGroupX(clientWidth, buttonGroupWidth);
        ui.MoveControl(importButton_, buttonX, footerY, layout_.footerButtonWidth);
        ui.MoveControl(cancelButton_, buttonX + layout_.footerButtonWidth + layout_.footerButtonGap, footerY, layout_.footerButtonWidth);

        ApplyViewMode();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ApplySelectedSource() {
        const int active = ThemedUi::ActiveTab(sourceTabs_);
        const bool storeApps = active == 2;
        ClearScanResults();
        if (storeApps) {
            selectedDirectory_.clear();
            applyingDirectoryText_ = true;
            SetWindowTextW(directoryText_, L"Windows 已安装应用");
            applyingDirectoryText_ = false;
            windowUi_->SetEditEnabled(directoryText_, false);
            EnableWindow(pickDirectoryButton_, FALSE);
            windowUi_->SetEditFrameState(directoryText_, false, false);
            SetWindowTextW(status_, L"尚未扫描");
            return;
        }
        windowUi_->SetEditEnabled(directoryText_, true);
        EnableWindow(pickDirectoryButton_, TRUE);
        REFKNOWNFOLDERID folderId = active == 0 ? FOLDERID_Desktop : FOLDERID_StartMenu;
        selectedDirectory_ = KnownFolderPathOrEmpty(folderId);
        applyingDirectoryText_ = true;
        SetWindowTextW(directoryText_, selectedDirectory_.wstring().c_str());
        applyingDirectoryText_ = false;
        windowUi_->SetEditFrameState(directoryText_, false, selectedDirectory_.empty());
        SetWindowTextW(status_, selectedDirectory_.empty() ? L"无法定位所选目录" : L"尚未扫描");
    }

    bool PickScanDirectory() {
        std::filesystem::path directory = selectedDirectory_;
        if (!PickFolder(hwnd_, directory)) {
            return false;
        }
        selectedDirectory_ = std::move(directory);
        ClearScanResults();
        applyingDirectoryText_ = true;
        SetWindowTextW(directoryText_, selectedDirectory_.wstring().c_str());
        applyingDirectoryText_ = false;
        windowUi_->SetEditFrameState(directoryText_, false, false);
        SetWindowTextW(status_, L"尚未扫描");
        return true;
    }

    bool HandleListNotify(LPARAM lParam, LRESULT& result) {
        ThemedTableEvent event{};
        if (!ThemedUi::DecodeTableEvent(list_, lParam, event)) return false;
        switch (event.kind) {
        case ThemedTableEventKind::Click:
            ToggleClickedListItem(event.point);
            result = 0;
            return true;
        case ThemedTableEventKind::CheckChanged:
            SyncItemSelection(event.row, event.checked);
            RefreshSelectedStatus();
            result = 0;
            return true;
        default:
            return false;
        }
    }

    void ToggleClickedListItem(POINT point) {
        bool stateIcon = false;
        const int index = ThemedUi::TableHitTest(
            list_, point, true, &stateIcon);
        if (index < 0 || index >= ThemedUi::TableRowCount(list_)) {
            return;
        }
        if (stateIcon) {
            return;
        }

        ToggleItemCheck(index);
    }

    void ToggleItemCheck(int index) {
        if (index < 0 || index >= ThemedUi::TableRowCount(list_)) {
            return;
        }

        const bool checked = ThemedUi::IsTableChecked(list_, index);
        SetItemCheck(index, !checked);
    }

    void SetItemCheck(int index, bool checked) {
        if (index < 0 || index >= ThemedUi::TableRowCount(list_)) {
            return;
        }

        const std::intptr_t rowKey = ThemedUi::TableRowKey(list_, index);
        const auto itemIndex = ItemIndexForRowKey(rowKey);
        if (itemIndex >= items_.size()) {
            return;
        }

        ThemedUi::SetTableChecked(list_, index, checked);
        items_[itemIndex].selected = checked;
    }

    void RefreshSelectedStatus() {
        SetWindowTextW(status_, (L"已选中 " + std::to_wstring(SelectedCount()) + L" 项。").c_str());
    }

    void ApplyViewMode() {
        if (!list_) {
            return;
        }
        ThemedUi::SetTableView(list_, ThemedTableView::Details);
        const ThemedUi ui = windowUi_->ui();
        ThemedUi::SetTableIconSpacing(list_, ui.scale(128), ui.scale(112));
        ThemedUi::SetActiveTab(viewTabs_, 0, false);
        SetWindowPos(list_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(list_, nullptr, TRUE);
    }

    void DestroyImageLists() {
        if (list_ && IsWindow(list_)) {
            ThemedUi::SetTableImageLists(list_, nullptr, nullptr);
        }
        if (smallImages_) {
            ImageList_Destroy(smallImages_);
            smallImages_ = nullptr;
        }
        if (mediumImages_) {
            ImageList_Destroy(mediumImages_);
            mediumImages_ = nullptr;
        }
    }

    int AddImageForItem(const QuickImportService::Item& item) {
        if (!smallImages_ || !mediumImages_) {
            return -1;
        }

        IconResolverService resolver;
        const ResolvedIcon icon = resolver.Resolve(
            IconResolverService::ForLink(item.link, kQuickImportIconCaptureSize));
        HBITMAP smallBitmap = IconResolverService::CreateBitmapFromPixels(
            icon,
            smallSize_,
            ThemedUi::ListSurfaceColor(theme_),
            true);
        HBITMAP mediumBitmap = IconResolverService::CreateBitmapFromPixels(
            icon,
            mediumSize_,
            ThemedUi::ListSurfaceColor(theme_),
            true);
        const int index = smallBitmap ? ImageList_Add(smallImages_, smallBitmap, nullptr) : -1;
        if (mediumBitmap) {
            ImageList_Add(mediumImages_, mediumBitmap, nullptr);
        } else if (smallBitmap) {
            ImageList_Add(mediumImages_, smallBitmap, nullptr);
        }
        if (smallBitmap) {
            DeleteObject(smallBitmap);
        }
        if (mediumBitmap) {
            DeleteObject(mediumBitmap);
        }
        return index;
    }

    int AddResolvedImagePair(const ResolvedIcon& smallIcon, const ResolvedIcon& mediumIcon) {
        if (!smallImages_ || !mediumImages_) {
            return -1;
        }
        HBITMAP smallBitmap = IconResolverService::CreateBitmapFromPixels(
            smallIcon,
            smallSize_,
            ThemedUi::ListSurfaceColor(theme_),
            true);
        HBITMAP mediumBitmap = IconResolverService::CreateBitmapFromPixels(
            mediumIcon,
            mediumSize_,
            ThemedUi::ListSurfaceColor(theme_),
            true);
        const int index = smallBitmap ? ImageList_Add(smallImages_, smallBitmap, nullptr) : -1;
        if (mediumBitmap) {
            ImageList_Add(mediumImages_, mediumBitmap, nullptr);
        } else if (smallBitmap) {
            ImageList_Add(mediumImages_, smallBitmap, nullptr);
        }
        if (smallBitmap) {
            DeleteObject(smallBitmap);
        }
        if (mediumBitmap) {
            DeleteObject(mediumBitmap);
        }
        return index;
    }

    void RebuildImageLists(bool resolveIcons = true) {
        DestroyImageLists();
        smallSize_ = std::max(16, GetSystemMetrics(SM_CXSMICON));
        mediumSize_ = 32;
        const int initialCount = std::max(1, static_cast<int>(items_.size()));
        smallImages_ = ImageList_Create(smallSize_, smallSize_, ILC_COLOR32 | ILC_MASK, initialCount, 8);
        mediumImages_ = ImageList_Create(mediumSize_, mediumSize_, ILC_COLOR32 | ILC_MASK, initialCount, 8);
        itemImageIndexes_.clear();
        itemImageIndexes_.reserve(items_.size());
        for (const auto& item : items_) {
            itemImageIndexes_.push_back(resolveIcons ? AddImageForItem(item) : -1);
        }
        if (list_) {
            ThemedUi::SetTableImageLists(list_, smallImages_, mediumImages_);
        }
    }

    ThemedTableRow RowForItem(const QuickImportService::Item& item, std::size_t index) {
        ThemedTableRow row{};
        row.key = RowKeyForItem(item, index);
        row.checked = item.selected;
        row.enabled = true;
        row.cells = {
            ThemedTableCell{item.link.name, index < itemImageIndexes_.size() ? itemImageIndexes_[index] : -1},
            ThemedTableCell{item.sourceName.empty() ? TypeText(item.link.type) : item.sourceName},
            ThemedTableCell{item.link.path},
            ThemedTableCell{item.status},
        };
        return row;
    }

    void StopIconLoadTask() {
        ++iconGeneration_;
        if (iconTask_) {
            iconTask_->RequestStop();
            if (iconTask_->IsFinished()) {
                iconTask_.reset();
            }
        }
    }

    void ClearScanResults() {
        StopIconLoadTask();
        items_.clear();
        itemImageIndexes_.clear();
        rowKeys_.clear();
        nextRowKey_ = 1;
        RebuildImageLists();
        if (list_) {
            ThemedUi::ClearTable(list_);
        }
    }

    void StartStoreAppIconLoad() {
        if (!scanWasStoreApps_ || items_.empty() || !hwnd_) {
            return;
        }
        StopIconLoadTask();
        const std::uint64_t generation = iconGeneration_;
        const HWND target = hwnd_;
        const std::vector<QuickImportService::Item> items = items_;
        const DWORD testIconDelayMs = QuickImportTestIconDelayMs();

        TaskOptions options{};
        options.mode = TaskExecutionMode::BackgroundSingle;
        options.completionCallback = [target, generation] {
            if (IsWindow(target)) {
                PostMessageW(target, WM_QUICK_IMPORT_ICONS_DONE, static_cast<WPARAM>(generation), 0);
            }
        };
        iconTask_ = TaskExecutionService::StartTyped<QuickImportIconLoadResult>(
            std::move(options),
            [generation, items, testIconDelayMs](TaskContext& context) {
                QuickImportIconLoadResult result;
                result.generation = generation;
                TaskProgressUpdate progress{};
                progress.phase = L"store-app-icons";
                progress.title = L"快速导入扫描进度";
                progress.status = L"正在刷新应用图标";
                progress.detail = L"Windows 已安装应用";
                progress.total = items.size();
                progress.current = 0;
                progress.workerCount = 1;
                progress.indeterminate = false;
                context.Report(std::move(progress));
                const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                IconResolverService resolver;
                result.icons.reserve(items.size());
                std::uint64_t completed = 0;
                for (const auto& item : items) {
                    if (context.StopRequested()) {
                        break;
                    }
                    QuickImportIconResult icon;
                    icon.stableKey = item.stableKey;
                    icon.icon = resolver.Resolve(
                        IconResolverService::ForLink(item.link, kQuickImportIconCaptureSize),
                        context.StopToken());
                    result.icons.push_back(std::move(icon));
                    ++completed;
                    context.UpdateProgress([completed, total = static_cast<std::uint64_t>(items.size())](TaskProgressUpdate& value) {
                        value.current = completed;
                        value.completed = completed;
                        value.total = total;
                        value.status = L"正在刷新应用图标";
                        value.detail = L"已刷新 " + std::to_wstring(completed) + L" / " + std::to_wstring(total) + L" 个图标";
                        value.indeterminate = false;
                    });
                    if (testIconDelayMs > 0) {
                        Sleep(testIconDelayMs);
                    }
                }
                if (SUCCEEDED(comResult)) {
                    CoUninitialize();
                }
                context.UpdateProgress([completed, total = static_cast<std::uint64_t>(items.size())](TaskProgressUpdate& value) {
                    value.current = completed;
                    value.completed = completed;
                    value.total = total;
                    value.status = completed >= total ? L"应用图标刷新完成" : L"应用图标刷新已停止";
                    value.detail = L"已刷新 " + std::to_wstring(completed) + L" / " + std::to_wstring(total) + L" 个图标";
                    value.indeterminate = false;
                });
                return result;
            });
    }

    void ApplyIconLoadResult(std::uint64_t generation) {
        if (!iconTask_ || generation != iconGeneration_ || !iconTask_->IsFinished()) {
            return;
        }
        if (iconTask_->Status() != TaskStatus::Completed) {
            CompleteScanWorkflow(L"应用图标刷新已停止。", false, false);
            return;
        }

        QuickImportIconLoadResult result = iconTask_->ResultCopy<QuickImportIconLoadResult>();
        if (result.generation != iconGeneration_) {
            iconTask_.reset();
            return;
        }

        std::unordered_map<std::wstring, std::size_t> indexByStableKey;
        indexByStableKey.reserve(items_.size());
        for (std::size_t index = 0; index < items_.size(); ++index) {
            if (!items_[index].stableKey.empty()) {
                indexByStableKey.emplace(items_[index].stableKey, index);
            }
        }

        bool updated = false;
        for (const QuickImportIconResult& icon : result.icons) {
            const auto found = indexByStableKey.find(icon.stableKey);
            if (found == indexByStableKey.end()) {
                continue;
            }
            const std::size_t index = found->second;
            const int imageIndex = AddResolvedImagePair(icon.icon, icon.icon);
            if (imageIndex < 0 || index >= itemImageIndexes_.size()) {
                continue;
            }
            itemImageIndexes_[index] = imageIndex;
            const std::intptr_t rowKey = RowKeyForItem(items_[index], index);
            const int rowIndex = ThemedUi::FindTableRowByKey(list_, rowKey);
            if (rowIndex >= 0) {
                ThemedUi::UpdateTableRow(list_, rowIndex, RowForItem(items_[index], index));
                updated = true;
            }
        }
        if (updated) {
            InvalidateRect(list_, nullptr, FALSE);
        }
        iconTask_.reset();
        CompleteScanWorkflow(L"扫描到 " + std::to_wstring(items_.size()) +
            L" 项，可导入 " + std::to_wstring(SelectedCount()) + L" 项。", false, true);
    }

    void Scan() {
        if (scanTask_ && !scanTask_->IsFinished()) {
            if (scanProgressDialog_) scanProgressDialog_->Show();
            return;
        }
        const bool storeApps = ThemedUi::ActiveTab(sourceTabs_) == 2;
        const std::wstring directoryText = Trim(GetText(directoryText_));

        ClearScanResults();
        SetWindowTextW(status_, L"正在后台扫描，可在进度窗口中查看或停止。");
        windowUi_->SetEditFrameState(directoryText_, false, false);
        EnableWindow(scanButton_, FALSE);
        EnableWindow(sourceTabs_, FALSE);
        EnableWindow(pickDirectoryButton_, FALSE);
        windowUi_->SetEditEnabled(directoryText_, false);

        QuickImportService::ScanRequest request;
        request.source = storeApps
            ? QuickImportService::Source::StoreApps
            : QuickImportService::Source::Directory;
        request.directory = std::filesystem::path(directoryText);
        scanWasStoreApps_ = storeApps;
        scanTask_ = scanner_.StartScan(request);

        ThemedTaskProgressDialogOptions progressOptions{};
        progressOptions.owner = hwnd_;
        progressOptions.instance = instance_;
        progressOptions.theme = theme_;
        progressOptions.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        progressOptions.className = L"QuattroQuickImportProgress_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        progressOptions.title = L"快速导入扫描进度";
        progressOptions.initialStatus = L"正在准备扫描";
        progressOptions.initialDetail = directoryText;
        progressOptions.closeOnCompleted = false;
        progressOptions.readSnapshot = [this]() {
            return ReadProgressSnapshot();
        };
        progressOptions.requestStop = [this]() { RequestProgressStop(); };
        scanProgressDialog_ = std::make_unique<ThemedTaskProgressDialog>(std::move(progressOptions));
        scanProgressDialog_->Show();
        SetTimer(hwnd_, IdScanPollTimer, 80, nullptr);
    }

    void FinishScanIfReady() {
        if (!scanTask_ || !scanTask_->IsFinished()) {
            return;
        }
        KillTimer(hwnd_, IdScanPollTimer);
        scanTask_->Wait();
        const ScanProgressSnapshot snapshot = scanTask_->Snapshot();
        QuickImportService::ScanResult result;
        if (snapshot.taskStatus == ScanTaskStatus::Failed) {
            result.error = snapshot.error.empty() ? L"扫描失败，请稍后重试。" : snapshot.error;
        } else {
            result = scanTask_->ResultCopy<QuickImportService::ScanResult>();
        }
        items_ = std::move(result.items);
        StopIconLoadTask();
        rowKeys_.clear();
        nextRowKey_ = 1;
        RebuildImageLists(!scanWasStoreApps_);
        PopulateList();
        windowUi_->SetEditFrameState(directoryText_, false, !result.error.empty());
        const int selected = SelectedCount();
        std::wstring status = L"扫描到 " + std::to_wstring(items_.size()) + L" 项，可导入 " + std::to_wstring(selected) + L" 项。";
        if (result.cancelled) {
            status = L"扫描已停止，保留 " + std::to_wstring(items_.size()) + L" 项结果。";
        } else if (!result.error.empty()) {
            status = result.error;
        }
        SetWindowTextW(status_, status.c_str());
        if (scanWasStoreApps_ && result.error.empty() && !result.cancelled && !items_.empty()) {
            scanTask_.reset();
            StartStoreAppIconLoad();
            if (iconTask_) {
                SetWindowTextW(status_, L"正在刷新应用图标，请稍候。");
                return;
            }
        }
        const bool scanSucceeded = result.error.empty() && !result.cancelled;
        CompleteScanWorkflow(status, !result.error.empty(), scanSucceeded);
        if (scanSucceeded) {
            scanTask_.reset();
        }
    }

    ThemedTaskProgressSnapshot ReadProgressSnapshot() const {
        if (iconTask_) {
            return ToThemedTaskProgressSnapshot(iconTask_->Snapshot());
        }
        if (scanTask_) {
            return ToThemedTaskProgressSnapshot(scanTask_->Snapshot());
        }
        TaskProgressSnapshot snapshot{};
        snapshot.taskStatus = TaskStatus::Completed;
        snapshot.title = L"快速导入扫描进度";
        snapshot.status = L"扫描完成";
        snapshot.indeterminate = false;
        snapshot.current = 1;
        snapshot.total = 1;
        return ToThemedTaskProgressSnapshot(snapshot);
    }

    void RequestProgressStop() {
        if (iconTask_ && !iconTask_->IsFinished()) {
            iconTask_->RequestStop();
            return;
        }
        if (scanTask_ && !scanTask_->IsFinished()) {
            scanTask_->RequestStop();
        }
    }

    void CompleteScanWorkflow(const std::wstring& status, bool directoryError, bool closeProgress) {
        EnableWindow(scanButton_, TRUE);
        EnableWindow(sourceTabs_, TRUE);
        const bool storeApps = ThemedUi::ActiveTab(sourceTabs_) == 2;
        windowUi_->SetEditEnabled(directoryText_, !storeApps);
        EnableWindow(pickDirectoryButton_, storeApps ? FALSE : TRUE);
        windowUi_->SetEditFrameState(directoryText_, false, directoryError);
        SetWindowTextW(status_, status.c_str());
        if (closeProgress && scanProgressDialog_) {
            scanProgressDialog_->Close();
        }
    }

    void PopulateList() {
        std::vector<ThemedTableRow> rows;
        rows.reserve(items_.size());
        for (std::size_t i = 0; i < items_.size(); ++i) {
            rows.push_back(RowForItem(items_[i], i));
        }
        ThemedUi::SetTableRows(list_, rows);
    }

    std::intptr_t RowKeyForItem(const QuickImportService::Item& item, std::size_t fallbackIndex) {
        const std::wstring stableKey = item.stableKey.empty()
            ? L"quick-import-row:" + std::to_wstring(fallbackIndex + 1)
            : item.stableKey;
        const auto found = rowKeys_.find(stableKey);
        if (found != rowKeys_.end()) {
            return found->second;
        }
        const std::intptr_t rowKey = nextRowKey_++;
        rowKeys_.emplace(stableKey, rowKey);
        return rowKey;
    }

    void SetAllChecks(bool checked) {
        if (!list_) {
            return;
        }
        for (auto& item : items_) {
            item.selected = checked;
        }
        ThemedUi::SetTableCheckedAll(list_, checked);
        RefreshSelectedStatus();
    }

    void SyncItemSelection(int row, bool checked) {
        if (row < 0 || row >= ThemedUi::TableRowCount(list_)) {
            return;
        }
        const std::intptr_t rowKey = ThemedUi::TableRowKey(list_, row);
        const auto index = ItemIndexForRowKey(rowKey);
        if (index < items_.size()) {
            items_[index].selected = checked;
        }
    }

    std::size_t ItemIndexForRowKey(std::intptr_t rowKey) {
        for (std::size_t index = 0; index < items_.size(); ++index) {
            if (RowKeyForItem(items_[index], index) == rowKey) return index;
        }
        return items_.size();
    }

    int SelectedCount() const {
        return static_cast<int>(std::count_if(items_.begin(), items_.end(), [](const auto& item) {
            return item.selected;
        }));
    }

    void Accept() {
        selectedLinks_.clear();
        for (const auto& item : items_) {
            if (item.selected) {
                selectedLinks_.push_back(item.link);
            }
        }
        if (selectedLinks_.empty()) {
            if (windowUi_) {
                ThemedToastOptions options{};
                options.role = ThemedToastRole::Warning;
                windowUi_->ui().ShowToast(L"请先扫描并勾选要导入的启动项。", options);
            }
            return;
        }
        Close(true);
    }

    void Paint(HDC dc) {
        windowUi_->FillBackground(dc);
        windowUi_->DrawRegisteredEditFrames(dc);
        windowUi_->DrawRegisteredTableFrames(dc);
    }

    void Close(bool accepted) {
        KillTimer(hwnd_, IdScanPollTimer);
        if (scanTask_) {
            scanTask_->RequestStop();
            scanTask_.reset();
        }
        StopIconLoadTask();
        if (scanProgressDialog_) {
            scanProgressDialog_->Close();
        }
        accepted_ = accepted;
        done_ = true;
        DestroyImageLists();
        DestroyWindow(hwnd_);
    }

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::vector<Link>& selectedLinks_;
    QuickImportService scanner_;
    DialogLayoutMetrics layout_{};
    std::vector<QuickImportService::Item> items_;
    std::vector<int> itemImageIndexes_;
    std::unordered_map<std::wstring, std::intptr_t> rowKeys_;
    std::intptr_t nextRowKey_ = 1;
    std::filesystem::path selectedDirectory_;
    bool applyingDirectoryText_ = false;
    HWND sourceTabs_ = nullptr;
    HWND directoryText_ = nullptr;
    HWND pickDirectoryButton_ = nullptr;
    HWND scanButton_ = nullptr;
    HWND viewTabs_ = nullptr;
    HWND selectAllButton_ = nullptr;
    HWND selectNoneButton_ = nullptr;
    HWND importButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    RECT directoryFrame_{};
    RECT listFrame_{};
    HIMAGELIST smallImages_ = nullptr;
    HIMAGELIST mediumImages_ = nullptr;
    int smallSize_ = 16;
    int mediumSize_ = 32;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    std::shared_ptr<ScanTaskHandle> scanTask_;
    std::shared_ptr<TaskHandle> iconTask_;
    std::unique_ptr<ThemedTaskProgressDialog> scanProgressDialog_;
    std::uint64_t iconGeneration_ = 1;
    bool scanWasStoreApps_ = false;
    bool done_ = false;
    bool accepted_ = false;
};

}

bool QuickImportDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, const std::vector<Link>& existingLinks, std::vector<Link>& selectedLinks) {
    DialogWindow dialog(owner, instance, theme, existingLinks, selectedLinks);
    return dialog.Run();
}
