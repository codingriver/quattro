#include "QuickImportDialog.h"

#include "DialogLayout.h"
#include "ThemedControls.h"
#include "Utilities.h"
#include "../resources/resource.h"

#include <algorithm>
#include <commctrl.h>
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

enum class ImportViewMode {
    List,
    Icon,
};

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        const float clamped = std::max(0.0f, std::min(1.0f, value));
        return static_cast<BYTE>(clamped * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

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

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DialogWindow::WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QuattroQuickImportDialog";
        RegisterClassExW(&wc);

        const POINT position = CenterWindowOnOwnerMonitor(owner_, kDialogWidth, kDialogHeight);

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"快速导入",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
            position.x,
            position.y,
            kDialogWidth,
            kDialogHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return false;
        }

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
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
        switch (message) {
        case WM_CREATE:
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
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"comboBox", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"comboBox", L"normal", L"itemBg")));
            return reinterpret_cast<LRESULT>(comboListBrush_ ? comboListBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_NOTIFY: {
            LRESULT result = 0;
            if (HandleListNotify(reinterpret_cast<NMHDR*>(lParam), result)) {
                return result;
            }
            if (ThemedControls::HandleListViewCustomDraw(theme_, lParam, result)) {
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
        case WM_DESTROY:
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (font_ && ownsFont_) {
                DeleteObject(font_);
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
            }
            if (comboListBrush_) {
                DeleteObject(comboListBrush_);
            }
            DestroyImageLists();
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        comboListBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"comboBox", L"normal", L"itemBg")));
        font_ = ThemedControls::CreateDialogFont();
        ownsFont_ = font_ != nullptr;
        if (!font_) {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        layout_ = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Compact);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = client.right - client.left;
        const int clientHeight = client.bottom - client.top;
        const int labelHeight = ThemedControls::LabelHeight(theme_);
        const int fieldHeight = ThemedControls::ComboBoxHeight(theme_);
        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        const int tabHeight = ThemedControls::TabButtonHeight(theme_);
        const int topY = layout_.contentInsetY;
        const int labelOffset = std::max(0, (fieldHeight - labelHeight) / 2);
        const int sourceWidth = 150;
        const int directoryWidth = 250;
        const int pickDirectoryWidth = 86;
        const int scanWidth = 86;
        const int selectAllWidth = 64;
        const int selectNoneWidth = 64;
        const int viewButtonWidth = 52;
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

        sourceLabel_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"来源", toolbarX, topY + labelOffset, layout_.labelWidth, labelHeight, font_);
        sourceCombo_ = ThemedControls::CreateComboBox(instance_, hwnd_, IdSource, sourceX, topY, sourceWidth, ThemedControls::ComboBoxDropdownHeight(theme_), font_, theme_);
        ComboBox_AddString(sourceCombo_, SourceText(QuickImportService::Source::Directory).c_str());
        ComboBox_AddString(sourceCombo_, SourceText(QuickImportService::Source::Desktop).c_str());
        ComboBox_AddString(sourceCombo_, SourceText(QuickImportService::Source::StartMenu).c_str());
        ComboBox_SetCurSel(sourceCombo_, 0);

        directoryFrame_ = RECT{sourceX + sourceWidth + layout_.controlGapX, topY, sourceX + sourceWidth + layout_.controlGapX + directoryWidth, topY + fieldHeight};
        directoryText_ = ThemedControls::CreateFramedStatic(instance_, hwnd_, theme_, directoryFrame_, L"未选择目录", font_);
        pickDirectoryButton_ = ThemedControls::CreateButton(instance_, hwnd_, IdPickDirectory, L"选择目录", directoryFrame_.right + layout_.controlGapX, topY, pickDirectoryWidth, buttonHeight, font_);
        scanButton_ = ThemedControls::CreateButton(instance_, hwnd_, IdScan, L"扫描", directoryFrame_.right + layout_.controlGapX + pickDirectoryWidth + layout_.controlGapX, topY, scanWidth, buttonHeight, font_);

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
        status_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"请选择目录后扫描。", layout_.contentInsetX, statusY, actionX - layout_.contentInsetX - layout_.controlGapX, labelHeight, font_);

        const int viewY = statusRowY + std::max(0, (statusRowHeight - tabHeight) / 2);
        viewListButton_ = ThemedControls::CreateTabButton(instance_, hwnd_, IdViewList, L"列表", actionX, viewY, viewButtonWidth, tabHeight, font_, true);
        viewIconButton_ = ThemedControls::CreateTabButton(instance_, hwnd_, IdViewIcon, L"图标", actionX + viewButtonWidth, viewY, viewButtonWidth, tabHeight, font_, false);

        const int actionButtonY = statusRowY + std::max(0, (statusRowHeight - buttonHeight) / 2);
        selectAllButton_ = ThemedControls::CreateButton(instance_, hwnd_, IdSelectAll, L"全选", actionX + viewButtonWidth * 2 + layout_.controlGapX, actionButtonY, selectAllWidth, buttonHeight, font_);
        selectNoneButton_ = ThemedControls::CreateButton(instance_, hwnd_, IdSelectNone, L"清空", actionX + viewButtonWidth * 2 + layout_.controlGapX + selectAllWidth + layout_.controlGapX, actionButtonY, selectNoneWidth, buttonHeight, font_);

        listFrame_ = RECT{layout_.contentInsetX, statusRowY + statusRowHeight + layout_.rowGap, clientWidth - layout_.contentInsetX, clientHeight - layout_.footerInsetY - buttonHeight - layout_.footerGap};
        const RECT listRect = ThemedControls::ListFrameInnerRect(theme_, listFrame_);
        list_ = CreateWindowExW(
            0,
            WC_LISTVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
            listRect.left,
            listRect.top,
            listRect.right - listRect.left,
            listRect.bottom - listRect.top,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdList)),
            instance_,
            nullptr);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        ThemedControls::ApplyListViewTheme(list_, theme_);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        AddColumn(0, L"名称", 170);
        AddColumn(1, L"类型", 68);
        AddColumn(2, L"路径", 330);
        AddColumn(3, L"状态", 88);
        RebuildImageLists();

        const int footerY = layout_.FooterButtonY(clientHeight, buttonHeight);
        const int buttonGroupWidth = layout_.footerButtonWidth * 2 + layout_.footerButtonGap;
        const int buttonX = layout_.CenteredGroupX(clientWidth, buttonGroupWidth);
        ThemedControls::CreatePrimaryButton(instance_, hwnd_, IdImport, L"导入选中", buttonX, footerY, layout_.footerButtonWidth, buttonHeight, font_, true);
        ThemedControls::CreateButton(instance_, hwnd_, IdCancel, L"取消", buttonX + layout_.footerButtonWidth + layout_.footerButtonGap, footerY, layout_.footerButtonWidth, buttonHeight, font_);

        UpdateSourceControls();
        ApplyViewMode();
    }

    void AddColumn(int index, const wchar_t* text, int width) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(text);
        column.cx = width;
        column.iSubItem = index;
        ListView_InsertColumn(list_, index, &column);
    }

    void LayoutSourceRow(bool directorySource) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = client.right - client.left;
        const int labelHeight = ThemedControls::LabelHeight(theme_);
        const int fieldHeight = ThemedControls::ComboBoxHeight(theme_);
        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        const int topY = layout_.contentInsetY;
        const int labelOffset = std::max(0, (fieldHeight - labelHeight) / 2);
        const int sourceWidth = 150;
        const int directoryWidth = 250;
        const int pickDirectoryWidth = 86;
        const int scanWidth = 86;
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

        SetWindowPos(sourceLabel_, nullptr, toolbarX, topY + labelOffset, layout_.labelWidth, labelHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(sourceCombo_, nullptr, sourceX, topY, sourceWidth, ThemedControls::ComboBoxDropdownHeight(theme_), SWP_NOZORDER | SWP_NOACTIVATE);

        int nextX = sourceX + sourceWidth + layout_.controlGapX;
        if (directorySource) {
            directoryFrame_ = RECT{nextX, topY, nextX + directoryWidth, topY + fieldHeight};
            const RECT textRect = ThemedControls::FieldTextRect(theme_, directoryFrame_);
            SetWindowPos(directoryText_, nullptr, textRect.left, textRect.top, textRect.right - textRect.left, textRect.bottom - textRect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            nextX = directoryFrame_.right + layout_.controlGapX;
            SetWindowPos(pickDirectoryButton_, nullptr, nextX, topY, pickDirectoryWidth, buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            nextX += pickDirectoryWidth + layout_.controlGapX;
        } else {
            ShowWindow(directoryText_, SW_HIDE);
            ShowWindow(pickDirectoryButton_, SW_HIDE);
            directoryFrame_ = {};
        }

        SetWindowPos(scanButton_, nullptr, nextX, topY, scanWidth, buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);
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

    bool HandleListNotify(NMHDR* header, LRESULT& result) {
        if (!header || header->hwndFrom != list_) {
            return false;
        }

        switch (header->code) {
        case NM_CLICK:
            ToggleClickedListItem(reinterpret_cast<NMITEMACTIVATE*>(header));
            result = 0;
            return true;
        case LVN_ITEMCHANGED:
            RefreshSelectedStatusIfCheckChanged(reinterpret_cast<NMLISTVIEW*>(header));
            result = 0;
            return true;
        default:
            return false;
        }
    }

    void ToggleClickedListItem(const NMITEMACTIVATE* activate) {
        if (!activate) {
            return;
        }

        LVHITTESTINFO hit{};
        hit.pt = activate->ptAction;
        int index = viewMode_ == ImportViewMode::List
            ? ListView_SubItemHitTest(list_, &hit)
            : ListView_HitTest(list_, &hit);
        if (viewMode_ == ImportViewMode::List && index < 0 && (hit.flags & LVHT_ONITEMSTATEICON) == 0) {
            index = HitTestListRow(activate->ptAction);
        }
        if (index < 0 || index >= ListView_GetItemCount(list_)) {
            return;
        }
        if ((hit.flags & LVHT_ONITEMSTATEICON) != 0) {
            return;
        }

        ToggleItemCheck(index);
    }

    int HitTestListRow(POINT point) const {
        RECT client{};
        GetClientRect(list_, &client);
        if (!PtInRect(&client, point)) {
            return -1;
        }

        const int count = ListView_GetItemCount(list_);
        const int top = std::max(0, ListView_GetTopIndex(list_));
        const int bottom = std::min(count, top + ListView_GetCountPerPage(list_) + 1);
        for (int i = top; i < bottom; ++i) {
            RECT row{};
            if (!ListView_GetItemRect(list_, i, &row, LVIR_BOUNDS)) {
                continue;
            }
            row.left = client.left;
            row.right = client.right;
            if (PtInRect(&row, point)) {
                return i;
            }
        }
        return -1;
    }

    void ToggleItemCheck(int index) {
        if (index < 0 || index >= ListView_GetItemCount(list_)) {
            return;
        }

        const bool checked = ListView_GetCheckState(list_, index) != FALSE;
        SetItemCheck(index, !checked);
    }

    void SetItemCheck(int index, bool checked) {
        if (index < 0 || index >= ListView_GetItemCount(list_)) {
            return;
        }

        const auto itemIndex = static_cast<std::size_t>(index);
        if (itemIndex >= items_.size()) {
            return;
        }

        const bool canSelect = !items_[itemIndex].duplicate;
        ListView_SetCheckState(list_, index, checked && canSelect ? TRUE : FALSE);
    }

    void RefreshSelectedStatusIfCheckChanged(const NMLISTVIEW* changed) {
        if (!changed || changed->iItem < 0) {
            return;
        }
        if ((changed->uChanged & LVIF_STATE) == 0) {
            return;
        }
        if ((changed->uOldState & LVIS_STATEIMAGEMASK) == (changed->uNewState & LVIS_STATEIMAGEMASK)) {
            return;
        }

        RefreshSelectedStatus();
    }

    void RefreshSelectedStatus() {
        SetWindowTextW(status_, (L"已选中 " + std::to_wstring(SelectedCount()) + L" 项。").c_str());
    }

    void ApplyViewMode() {
        if (!list_) {
            return;
        }
        DWORD_PTR style = static_cast<DWORD_PTR>(GetWindowLongPtrW(list_, GWL_STYLE));
        style &= ~LVS_TYPEMASK;
        style |= viewMode_ == ImportViewMode::List ? LVS_REPORT : LVS_ICON;
        SetWindowLongPtrW(list_, GWL_STYLE, static_cast<LONG_PTR>(style));
        ListView_SetView(list_, viewMode_ == ImportViewMode::List ? LV_VIEW_DETAILS : LV_VIEW_ICON);
        ListView_SetIconSpacing(list_, 96, 72);
        ThemedControls::SetTabButtonSelected(viewListButton_, viewMode_ == ImportViewMode::List);
        ThemedControls::SetTabButtonSelected(viewIconButton_, viewMode_ == ImportViewMode::Icon);
        SetWindowPos(list_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(list_, nullptr, TRUE);
    }

    void DestroyImageLists() {
        if (list_ && IsWindow(list_)) {
            ListView_SetImageList(list_, nullptr, LVSIL_SMALL);
            ListView_SetImageList(list_, nullptr, LVSIL_NORMAL);
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
            ListView_SetImageList(list_, smallImages_, LVSIL_SMALL);
            ListView_SetImageList(list_, mediumImages_, LVSIL_NORMAL);
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
        ListView_DeleteAllItems(list_);
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const auto& item = items_[i];
            LVITEMW row{};
            row.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            row.iItem = static_cast<int>(i);
            row.pszText = const_cast<wchar_t*>(item.link.name.c_str());
            row.lParam = static_cast<LPARAM>(i);
            row.iImage = i < itemImageIndexes_.size() ? itemImageIndexes_[i] : -1;
            ListView_InsertItem(list_, &row);
            ListView_SetItemText(list_, static_cast<int>(i), 1, const_cast<wchar_t*>(TypeText(item.link.type).c_str()));
            ListView_SetItemText(list_, static_cast<int>(i), 2, const_cast<wchar_t*>(item.link.path.c_str()));
            ListView_SetItemText(list_, static_cast<int>(i), 3, const_cast<wchar_t*>(item.status.c_str()));
            ListView_SetCheckState(list_, static_cast<int>(i), item.selected ? TRUE : FALSE);
        }
    }

    void SetAllChecks(bool checked) {
        for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
            SetItemCheck(i, checked);
        }
        RefreshSelectedStatus();
    }

    int SelectedCount() const {
        int count = 0;
        for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
            if (!items_[static_cast<std::size_t>(i)].duplicate && ListView_GetCheckState(list_, i)) {
                ++count;
            }
        }
        return count;
    }

    void Accept() {
        selectedLinks_.clear();
        for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
            const auto& item = items_[static_cast<std::size_t>(i)];
            if (!item.duplicate && ListView_GetCheckState(list_, i)) {
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
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        if (IsWindowVisible(directoryText_)) {
            ThemedControls::DrawFieldFrame(theme_, dc, directoryFrame_, directoryText_, true);
        }
        ThemedControls::DrawListFrame(theme_, dc, listFrame_, list_);
    }

    void Close(bool accepted) {
        accepted_ = accepted;
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
    HWND viewListButton_ = nullptr;
    HWND viewIconButton_ = nullptr;
    HWND selectAllButton_ = nullptr;
    HWND selectNoneButton_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    RECT directoryFrame_{};
    RECT listFrame_{};
    HIMAGELIST smallImages_ = nullptr;
    HIMAGELIST mediumImages_ = nullptr;
    HFONT font_ = nullptr;
    bool ownsFont_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH comboListBrush_ = nullptr;
    bool done_ = false;
    bool accepted_ = false;
    bool ownerWasEnabled_ = true;
    bool ownerRestored_ = false;
};

}

bool QuickImportDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, const std::vector<Link>& existingLinks, std::vector<Link>& selectedLinks) {
    DialogWindow dialog(owner, instance, theme, existingLinks, selectedLinks);
    return dialog.Run();
}
