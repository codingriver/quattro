#include "QuickImportDialog.h"

#include "DialogLayout.h"
#include "ThemedControls.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <algorithm>
#include <commctrl.h>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <system_error>
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
constexpr int IdViewList = 1007;
constexpr int IdViewIcon = 1008;
constexpr int IdActionToolbar = 1009;

enum class ImportViewMode {
    List,
    Icon,
};

std::wstring SourceText(QuickImportService::Source source) {
    switch (source) {
    case QuickImportService::Source::Directory:
        return L"选择目录";
    case QuickImportService::Source::Desktop:
        return L"桌面";
    case QuickImportService::Source::StartMenu:
        return L"开始菜单";
    default:
        return L"选择目录";
    }
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

HICON StockIcon(SHSTOCKICONID id, bool useSmallIcon) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    const UINT sizeFlag = useSmallIcon ? SHGSI_SMALLICON : SHGSI_LARGEICON;
    if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | sizeFlag, &info))) {
        return info.hIcon;
    }
    return nullptr;
}

HICON ExtractDisplayIcon(const Link& link, bool useSmallIcon) {
    if (link.icon == L"#url" || link.type == 2) {
        if (HICON icon = StockIcon(SIID_WORLD, useSmallIcon)) {
            return icon;
        }
    }

    const std::wstring iconPath = Trim(link.icon);
    if (!iconPath.empty() && iconPath != L"#url" && iconPath != L"默认系统缓存图标") {
        SHFILEINFOW info{};
        const std::wstring path = ExpandEnvironmentStringsSafe(iconPath);
        const UINT sizeFlag = useSmallIcon ? SHGFI_SMALLICON : SHGFI_LARGEICON;
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | sizeFlag)) {
            return info.hIcon;
        }
    }

    const std::wstring path = ExpandEnvironmentStringsSafe(Trim(link.path));
    SHFILEINFOW info{};
    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) || link.type == 1) {
        attrs = FILE_ATTRIBUTE_DIRECTORY;
    }

    UINT flags = SHGFI_ICON | (useSmallIcon ? SHGFI_SMALLICON : SHGFI_LARGEICON);
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        return info.hIcon;
    }
    flags |= SHGFI_USEFILEATTRIBUTES;
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        return info.hIcon;
    }

    if (link.type == 1) {
        if (HICON icon = StockIcon(SIID_FOLDER, useSmallIcon)) {
            return icon;
        }
    }
    if (HICON icon = StockIcon(SIID_APPLICATION, useSmallIcon)) {
        return icon;
    }
    return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
}

bool PickFolder(HWND owner, std::filesystem::path& directory) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
        return false;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择快速导入目录");

    bool accepted = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                directory = path;
                CoTaskMemFree(path);
                accepted = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return accepted;
}

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme, const std::vector<Link>& existingLinks, std::vector<Link>& selectedLinks)
        : owner_(owner), instance_(instance), theme_(theme), existingLinks_(existingLinks), selectedLinks_(selectedLinks) {}

    ~DialogWindow() {
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
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IdSource:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    UpdateSourceControls();
                }
                return 0;
            case IdPickDirectory:
                PickScanDirectory();
                return 0;
            case IdScan:
                Scan();
                return 0;
            case IdViewList:
                SwitchView(ImportViewMode::List);
                return 0;
            case IdViewIcon:
                SwitchView(ImportViewMode::Icon);
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
    }

    void CreateControls() {
        layout_ = windowUi_->ui().layout();
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = client.right - client.left;
        const int clientHeight = client.bottom - client.top;
        const ThemedUi ui = windowUi_->ui();
        const int labelHeight = ui.labelHeight();
        const int fieldHeight = ui.comboBoxHeight();
        const int buttonHeight = ui.footerButtonHeight();
        const int tabHeight = ui.tabButtonHeight();
        const int topY = layout_.contentInsetY;
        const int labelOffset = std::max(0, (fieldHeight - labelHeight) / 2);
        const int sourceWidth = ui.scale(150);
        const int directoryWidth = ui.scale(250);
        const int pickDirectoryWidth = ui.scale(86);
        const int scanWidth = ui.scale(86);
        const int selectAllWidth = ui.scale(64);
        const int selectNoneWidth = ui.scale(64);
        const int viewButtonWidth = ui.scale(52);
        const int toolbarWidth =
            layout_.labelWidth +
            layout_.labelGap +
            sourceWidth +
            layout_.controlGapX +
            directoryWidth +
            layout_.controlGapX +
            pickDirectoryWidth +
            layout_.controlGapX +
            scanWidth;
        const int toolbarX = layout_.CenteredGroupX(clientWidth, toolbarWidth);
        const int sourceX = toolbarX + layout_.labelWidth + layout_.labelGap;

        sourceLabel_ = ui.Label(L"来源", toolbarX, topY + labelOffset, layout_.labelWidth);
        sourceCombo_ = ui.ComboBox(IdSource, sourceX, topY, sourceWidth);
        ComboBox_AddString(sourceCombo_, SourceText(QuickImportService::Source::Directory).c_str());
        ComboBox_AddString(sourceCombo_, SourceText(QuickImportService::Source::Desktop).c_str());
        ComboBox_AddString(sourceCombo_, SourceText(QuickImportService::Source::StartMenu).c_str());
        ComboBox_SetCurSel(sourceCombo_, 0);

        directoryFrame_ = RECT{sourceX + sourceWidth + layout_.controlGapX, topY, sourceX + sourceWidth + layout_.controlGapX + directoryWidth, topY + fieldHeight};
        ThemedEditOptions directoryOptions{};
        directoryOptions.readOnly = true;
        directoryText_ = ui.Edit(IdPickDirectory + 100, directoryFrame_, L"未选择目录", directoryOptions);
        pickDirectoryButton_ = ui.Button(IdPickDirectory, L"选择目录", directoryFrame_.right + layout_.controlGapX, topY, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, pickDirectoryWidth);
        scanButton_ = ui.Button(IdScan, L"扫描", directoryFrame_.right + layout_.controlGapX + pickDirectoryWidth + layout_.controlGapX, topY, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, scanWidth);

        const int statusRowY = topY + fieldHeight + layout_.rowGap;
        const int statusRowHeight = std::max({buttonHeight, tabHeight, labelHeight});
        const int actionWidth =
            viewButtonWidth * 2 +
            layout_.controlGapX +
            selectAllWidth +
            layout_.controlGapX +
            selectNoneWidth;
        const int actionX = clientWidth - layout_.contentInsetX - actionWidth;
        const int statusY = statusRowY + std::max(0, (statusRowHeight - labelHeight) / 2);
        status_ = ui.StatusText(
            L"请选择目录后扫描。",
            layout_.contentInsetX,
            statusY,
            actionX - layout_.contentInsetX - layout_.controlGapX,
            ThemedStatusTextOptions{ThemedStatusRole::Normal, ThemedTextAlign::Start});

        actionToolbar_ = ui.ToolBar(
            IdActionToolbar,
            RECT{actionX, statusRowY, actionX + actionWidth, statusRowY + statusRowHeight},
            {
                ThemedToolItem{IdViewList, L"列表", ThemedToolItemKind::Toggle, ThemedToolItemAlignment::Leading, true, true},
                ThemedToolItem{IdViewIcon, L"图标", ThemedToolItemKind::Toggle},
                ThemedToolItem{0, L"", ThemedToolItemKind::Separator},
                ThemedToolItem{IdSelectAll, L"全选"},
                ThemedToolItem{IdSelectNone, L"清空"},
            });

        listFrame_ = RECT{layout_.contentInsetX, statusRowY + statusRowHeight + layout_.rowGap, clientWidth - layout_.contentInsetX, clientHeight - layout_.footerInsetY - buttonHeight - layout_.footerGap};
        ThemedTableOptions tableOptions{};
        tableOptions.checkable = true;
        list_ = ui.Table(
            IdList,
            listFrame_,
            {
                ThemedTableColumn{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 170},
                ThemedTableColumn{L"type", L"类型", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 68},
                ThemedTableColumn{L"path", L"路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                ThemedTableColumn{L"status", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 88},
            },
            tableOptions);
        RebuildImageLists();

        const int footerY = layout_.FooterButtonY(clientHeight, buttonHeight);
        const int buttonGroupWidth = layout_.footerButtonWidth * 2 + layout_.footerButtonGap;
        const int buttonX = layout_.CenteredGroupX(clientWidth, buttonGroupWidth);
        ui.Button(IdImport, L"导入选中", buttonX, footerY, ThemedButtonRole::Primary, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, layout_.footerButtonWidth, true);
        ui.Button(IdCancel, L"取消", buttonX + layout_.footerButtonWidth + layout_.footerButtonGap, footerY, ThemedButtonRole::Normal, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, layout_.footerButtonWidth);

        UpdateSourceControls();
        ApplyViewMode();
    }

    void LayoutSourceRow(bool directorySource) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = client.right - client.left;
        const ThemedUi ui = windowUi_->ui();
        const int labelHeight = ui.labelHeight();
        const int fieldHeight = ui.comboBoxHeight();
        const int topY = layout_.contentInsetY;
        const int labelOffset = std::max(0, (fieldHeight - labelHeight) / 2);
        const int sourceWidth = ui.scale(150);
        const int directoryWidth = ui.scale(250);
        const int pickDirectoryWidth = ui.scale(86);
        const int scanWidth = ui.scale(86);
        const int directoryPartWidth = directorySource
            ? directoryWidth + layout_.controlGapX + pickDirectoryWidth + layout_.controlGapX
            : 0;
        const int toolbarWidth =
            layout_.labelWidth +
            layout_.labelGap +
            sourceWidth +
            layout_.controlGapX +
            directoryPartWidth +
            scanWidth;
        const int toolbarX = layout_.CenteredGroupX(clientWidth, toolbarWidth);
        const int sourceX = toolbarX + layout_.labelWidth + layout_.labelGap;

        ui.MoveControl(sourceLabel_, toolbarX, topY + labelOffset, layout_.labelWidth);
        ui.MoveComboBox(sourceCombo_, sourceX, topY, sourceWidth);

        int nextX = sourceX + sourceWidth + layout_.controlGapX;
        if (directorySource) {
            directoryFrame_ = RECT{nextX, topY, nextX + directoryWidth, topY + fieldHeight};
            windowUi_->MoveEditFrame(directoryText_, directoryFrame_);
            ShowWindow(directoryText_, SW_SHOWNA);
            nextX = directoryFrame_.right + layout_.controlGapX;
            ui.MoveControl(pickDirectoryButton_, nextX, topY, pickDirectoryWidth);
            ShowWindow(pickDirectoryButton_, SW_SHOWNA);
            nextX += pickDirectoryWidth + layout_.controlGapX;
        } else {
            ShowWindow(directoryText_, SW_HIDE);
            ShowWindow(pickDirectoryButton_, SW_HIDE);
            directoryFrame_ = {};
        }

        ui.MoveControl(scanButton_, nextX, topY, scanWidth);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    std::wstring DirectoryDisplayText() const {
        return selectedDirectory_.empty() ? L"未选择目录" : selectedDirectory_.wstring();
    }

    void UpdateDirectoryText() {
        if (directoryText_) {
            SetWindowTextW(directoryText_, DirectoryDisplayText().c_str());
        }
    }

    void UpdateSourceControls() {
        const bool directorySource = SelectedSource() == QuickImportService::Source::Directory;
        LayoutSourceRow(directorySource);
        UpdateDirectoryText();
        if (directorySource) {
            const std::wstring status = selectedDirectory_.empty() ? L"请选择目录后扫描。" : L"将扫描：" + selectedDirectory_.wstring();
            SetWindowTextW(status_, status.c_str());
        } else {
            SetWindowTextW(status_, L"选择来源后扫描，最多递归 5 层目录。");
        }
    }

    bool PickScanDirectory() {
        std::filesystem::path directory = selectedDirectory_;
        if (!PickFolder(hwnd_, directory)) {
            return false;
        }
        selectedDirectory_ = std::move(directory);
        UpdateDirectoryText();
        SetWindowTextW(status_, (L"将扫描：" + selectedDirectory_.wstring()).c_str());
        return true;
    }

    void SwitchView(ImportViewMode mode) {
        if (viewMode_ == mode) {
            return;
        }
        viewMode_ = mode;
        ApplyViewMode();
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
            list_, point, viewMode_ == ImportViewMode::List, &stateIcon);
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

        const auto itemIndex = static_cast<std::size_t>(index);
        if (itemIndex >= items_.size()) {
            return;
        }

        const bool canSelect = !items_[itemIndex].duplicate;
        ThemedUi::SetTableChecked(list_, index, checked && canSelect);
    }

    void RefreshSelectedStatus() {
        SetWindowTextW(status_, (L"已选中 " + std::to_wstring(SelectedCount()) + L" 项。").c_str());
    }

    void ApplyViewMode() {
        if (!list_) {
            return;
        }
        ThemedUi::SetTableView(list_, viewMode_ == ImportViewMode::List ? ThemedTableView::Details : ThemedTableView::Icons);
        ThemedUi::SetTableIconSpacing(list_, 96, 72);
        ThemedUi::SetToolChecked(actionToolbar_, IdViewList, viewMode_ == ImportViewMode::List);
        ThemedUi::SetToolChecked(actionToolbar_, IdViewIcon, viewMode_ == ImportViewMode::Icon);
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

        HICON smallIcon = ExtractDisplayIcon(item.link, true);
        HICON mediumIcon = ExtractDisplayIcon(item.link, false);
        const int index = smallIcon ? ImageList_AddIcon(smallImages_, smallIcon) : -1;
        if (mediumIcon) {
            ImageList_AddIcon(mediumImages_, mediumIcon);
        } else if (smallIcon) {
            ImageList_AddIcon(mediumImages_, smallIcon);
        }
        if (smallIcon) {
            DestroyIcon(smallIcon);
        }
        if (mediumIcon) {
            DestroyIcon(mediumIcon);
        }
        return index;
    }

    void RebuildImageLists() {
        DestroyImageLists();
        const int smallSize = std::max(16, GetSystemMetrics(SM_CXSMICON));
        constexpr int mediumSize = 32;
        const int initialCount = std::max(1, static_cast<int>(items_.size()));
        smallImages_ = ImageList_Create(smallSize, smallSize, ILC_COLOR32 | ILC_MASK, initialCount, 8);
        mediumImages_ = ImageList_Create(mediumSize, mediumSize, ILC_COLOR32 | ILC_MASK, initialCount, 8);
        itemImageIndexes_.clear();
        itemImageIndexes_.reserve(items_.size());
        for (const auto& item : items_) {
            itemImageIndexes_.push_back(AddImageForItem(item));
        }
        if (list_) {
            ThemedUi::SetTableImageLists(list_, smallImages_, mediumImages_);
        }
    }

    QuickImportService::Source SelectedSource() const {
        switch (ComboBox_GetCurSel(sourceCombo_)) {
        case 1:
            return QuickImportService::Source::Desktop;
        case 2:
            return QuickImportService::Source::StartMenu;
        case 0:
        default:
            return QuickImportService::Source::Directory;
        }
    }

    void Scan() {
        std::filesystem::path directory;
        const auto source = SelectedSource();
        if (source == QuickImportService::Source::Directory) {
            if (selectedDirectory_.empty() && !PickScanDirectory()) {
                SetWindowTextW(status_, L"请选择要扫描的目录。");
                return;
            }
            directory = selectedDirectory_;
        }

        SetWindowTextW(status_, L"正在扫描，请稍候……");
        EnableWindow(scanButton_, FALSE);
        UpdateWindow(hwnd_);

        std::wstring error;
        items_ = scanner_.Scan(source, directory, existingLinks_, error);
        RebuildImageLists();
        PopulateList();

        EnableWindow(scanButton_, TRUE);
        const int selected = SelectedCount();
        std::wstring status = L"扫描到 " + std::to_wstring(items_.size()) + L" 项，可导入 " + std::to_wstring(selected) + L" 项。";
        if (!error.empty()) {
            status = error;
        }
        SetWindowTextW(status_, status.c_str());
    }

    void PopulateList() {
        std::vector<ThemedTableRow> rows;
        rows.reserve(items_.size());
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const auto& item = items_[i];
            ThemedTableRow row{};
            row.key = static_cast<std::intptr_t>(i);
            row.checked = item.selected;
            row.enabled = !item.duplicate;
            row.cells = {
                ThemedTableCell{item.link.name, i < itemImageIndexes_.size() ? itemImageIndexes_[i] : -1},
                ThemedTableCell{TypeText(item.link.type)},
                ThemedTableCell{item.link.path},
                ThemedTableCell{item.status},
            };
            rows.push_back(std::move(row));
        }
        ThemedUi::SetTableRows(list_, rows);
    }

    void SetAllChecks(bool checked) {
        for (int i = 0; i < ThemedUi::TableRowCount(list_); ++i) {
            SetItemCheck(i, checked);
        }
        RefreshSelectedStatus();
    }

    int SelectedCount() const {
        int count = 0;
        for (int i = 0; i < ThemedUi::TableRowCount(list_); ++i) {
            if (!items_[static_cast<std::size_t>(i)].duplicate && ThemedUi::IsTableChecked(list_, i)) {
                ++count;
            }
        }
        return count;
    }

    void Accept() {
        selectedLinks_.clear();
        for (int i = 0; i < ThemedUi::TableRowCount(list_); ++i) {
            const auto& item = items_[static_cast<std::size_t>(i)];
            if (!item.duplicate && ThemedUi::IsTableChecked(list_, i)) {
                selectedLinks_.push_back(item.link);
            }
        }
        if (selectedLinks_.empty()) {
            MessageBoxW(hwnd_, L"请先扫描并勾选要导入的启动项。", L"快速导入", MB_OK | MB_ICONINFORMATION);
            return;
        }
        Close(true);
    }

    void Paint(HDC dc) {
        windowUi_->DrawRegisteredEditFrames(dc);
        windowUi_->DrawRegisteredTableFrames(dc);
    }

    void Close(bool accepted) {
        accepted_ = accepted;
        done_ = true;
        DestroyImageLists();
        DestroyWindow(hwnd_);
    }

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    const std::vector<Link>& existingLinks_;
    std::vector<Link>& selectedLinks_;
    QuickImportService scanner_;
    DialogLayoutMetrics layout_{};
    std::vector<QuickImportService::Item> items_;
    std::vector<int> itemImageIndexes_;
    std::filesystem::path selectedDirectory_;
    ImportViewMode viewMode_ = ImportViewMode::List;
    HWND sourceLabel_ = nullptr;
    HWND sourceCombo_ = nullptr;
    HWND directoryText_ = nullptr;
    HWND pickDirectoryButton_ = nullptr;
    HWND scanButton_ = nullptr;
    HWND actionToolbar_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    RECT directoryFrame_{};
    RECT listFrame_{};
    HIMAGELIST smallImages_ = nullptr;
    HIMAGELIST mediumImages_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool done_ = false;
    bool accepted_ = false;
};

}

bool QuickImportDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, const std::vector<Link>& existingLinks, std::vector<Link>& selectedLinks) {
    DialogWindow dialog(owner, instance, theme, existingLinks, selectedLinks);
    return dialog.Run();
}
