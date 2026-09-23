#include "FileHelperDialog.h"

#include "../common/AppLog.h"
#include "../common/FileDialog.h"
#include "../common/Utilities.h"
#include "../services/FileHelperService.h"
#include "../theme/ThemedUi.h"
#include "../theme/ThemedWindowUi.h"
#include "ToolWindowPosition.h"
#include "../../resources/resource.h"

#include <algorithm>
#include <atomic>
#include <memory>

namespace {
constexpr int kPickerLogicalWidth = 96;
constexpr int kPickFolderCommand = 7910;
constexpr wchar_t kFileHelperToolId[] = L"quattro.builtin.file-helper";
constexpr UINT WM_FILE_HELPER_ACTIVATE = WM_APP + 0x8C;

std::atomic<HWND> gFileHelperWindow{nullptr};

bool IsMessageForWindow(HWND root, const MSG& message) {
    return root && IsWindow(root) && (message.hwnd == root || IsChild(root, message.hwnd));
}
}

class FileHelperDialog final {
public:
    FileHelperDialog(HWND owner, HINSTANCE instance, const Theme& theme)
        : owner_(owner), instance_(instance), theme_(theme) {}

    static bool Open(HWND owner, HINSTANCE instance, const Theme& theme) {
        HWND existing = gFileHelperWindow.load();
        if (existing && IsWindow(existing)) {
            PostMessageW(existing, WM_FILE_HELPER_ACTIVATE, 0, 0);
            return true;
        }
        if (existing) {
            gFileHelperWindow.compare_exchange_strong(existing, nullptr);
        }

        auto dialog = std::make_unique<FileHelperDialog>(owner, instance, theme);
        if (!dialog->Create()) {
            return false;
        }
        dialog->deleteOnDestroy_ = true;
        dialog.release();
        return true;
    }

    static bool Toggle(HWND owner, HINSTANCE instance, const Theme& theme) {
        HWND existing = gFileHelperWindow.load();
        if (existing && IsWindow(existing)) {
            return PostMessageW(existing, WM_CLOSE, 0, 0) != FALSE;
        }
        if (existing) {
            gFileHelperWindow.compare_exchange_strong(existing, nullptr);
        }
        return Open(owner, instance, theme);
    }

private:
    bool Create() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, kFileHelperWindowClass, L"文件助手", Proc, this,
            icon, icon, ThemedWindowSizePreset::WideCompactTool);
        options = ThemedWindowUi::BorderlessToolOptions(options);
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(L"文件助手窗口创建失败: " + error);
            return false;
        }
        RECT bounds{};
        if (GetWindowRect(hwnd_, &bounds)) {
            if (const auto position = LoadToolWindowPosition(kFileHelperToolId,
                    bounds.right - bounds.left, bounds.bottom - bounds.top)) {
                SetWindowPos(hwnd_, nullptr, position->x, position->y, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }
        gFileHelperWindow.store(hwnd_);
        ActivateAndFocus();
        return true;
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        FileHelperDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<FileHelperDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<FileHelperDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!dialog) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        const LRESULT result = dialog->Handle(message, wParam, lParam);
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            HWND expected = hwnd;
            gFileHelperWindow.compare_exchange_strong(expected, nullptr);
            if (dialog->deleteOnDestroy_) {
                delete dialog;
            }
        }
        return result;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT common = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, common)) {
            if (message == WM_DESTROY) {
                SaveToolWindowPosition(kFileHelperToolId, hwnd_);
            }
            return common;
        }

        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact,
                kThemedWideCompactToolClientWidth, kThemedWideCompactToolClientHeight);
            windowUi_->SetDpiChangedCallback([this](UINT) {
                const ThemedUi ui = windowUi_->ui();
                windowUi_->ResizeClientArea(ui.clientWidth(),
                    ui.twoRowClientHeight(ui.editHeight(), ui.compactButtonHeight()), false);
                LayoutControls();
            });
            CreateControls();
            return 0;
        case WM_COMMAND:
            return HandleCommand(LOWORD(wParam), HIWORD(wParam));
        case WM_EXITSIZEMOVE:
            SaveToolWindowPosition(kFileHelperToolId, hwnd_);
            return 0;
        case WM_FILE_HELPER_ACTIVATE:
            ActivateAndFocus();
            return 0;
        case WM_QUATTRO_TEST_FILE_HELPER:
            return HandleTestCommand(static_cast<FileHelperTestCommand>(wParam), lParam);
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd_, &paint);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredEditFrames(dc);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_CLOSE:
            windowUi_->ui().HideToast();
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        const ThemedUi ui = windowUi_->ui();
        history_ = service_.LoadHistory();
        dragHandle_ = ui.Label(L"⋮⋮", 0, 0, ui.textWidth(L"移动"),
            ThemedLabelOptions{ThemedTextAlign::Center});
        windowUi_->SetDragHandle(dragHandle_);
        ThemedComboBoxOptions pathOptions{};
        pathOptions.mode = ThemedComboBoxMode::Editable;
        pathOptions.placeholder = L"输入本地绝对路径";
        pathOptions.openOnFocus = true;
        pathOptions.selectAllOnFocus = true;
        pathEdit_ = ui.ComboBox(ID_FILE_HELPER_PATH, 0, 0, ui.scale(120), pathOptions);
        ThemedUi::SetComboBoxItems(pathEdit_, HistoryItems(), -1);

        ThemedPathPickerSplitButtonOptions pickerOptions{};
        pickerOptions.primaryId = ID_FILE_HELPER_PICK;
        pickerOptions.menuId = ID_FILE_HELPER_PICK_MENU;
        pickerOptions.defaultKind = CommonPathPickerKind::File;
        pickerOptions.widthMode = ThemedButtonWidthMode::Fixed;
        pickerOptions.fixedWidth = ui.scale(kPickerLogicalWidth);
        pickerOptions.surface = ThemedControlSurface::Dialog;
        picker_ = ui.PathPickerSplitButton(pickerOptions);

        openFile_ = ui.Button(ID_FILE_HELPER_OPEN_FILE, L"打开文件", 0, 0,
            ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        openFolder_ = ui.Button(ID_FILE_HELPER_OPEN_FOLDER, L"打开文件夹", 0, 0,
            ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        createFile_ = ui.Button(ID_FILE_HELPER_CREATE_FILE, L"创建文件", 0, 0,
            ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        createFolder_ = ui.Button(ID_FILE_HELPER_CREATE_FOLDER, L"创建目录", 0, 0,
            ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);

        openLocation_ = ui.LinkText(ID_FILE_HELPER_OPEN_LOCATION, L"打开所在位置", 0, 0, ui.scale(96));
        ui.SetEnabled(openLocation_, false);
        windowUi_->ResizeClientArea(ui.clientWidth(),
            ui.twoRowClientHeight(ui.editHeight(), ui.compactButtonHeight()), false);
        LayoutControls();
    }

    void LayoutControls() {
        if (!windowUi_ || !pathEdit_) {
            return;
        }
        const ThemedUi ui = windowUi_->ui();
        const auto& layout = ui.layout();
        const int left = ui.contentLeft();
        const int contentWidth = ui.contentWidth();
        const int pickerWidth = ui.scale(kPickerLogicalWidth);
        const int editWidth = std::max(ui.textWidth(L"输入路径"),
            contentWidth - pickerWidth - layout.controlGapX);
        int y = ui.contentTop();
        ui.MoveComboBox(pathEdit_, left, y, editWidth);

        RECT menuRect{};
        GetWindowRect(picker_.split.menu, &menuRect);
        const int menuWidth = std::max(1, static_cast<int>(menuRect.right - menuRect.left));
        const int primaryWidth = std::max(1, pickerWidth - menuWidth);
        const int pickerX = left + editWidth + layout.controlGapX;
        ui.MoveControl(picker_.split.primary, pickerX, y, primaryWidth);
        ui.MoveControl(picker_.split.menu, pickerX + primaryWidth, y, menuWidth);

        y = ui.nextRowY(y, ui.editHeight());
        LayoutButtonRow(ui, y);
    }

    void LayoutButtonRow(const ThemedUi& ui, int y) {
        const int openFileWidth = ui.buttonWidth(L"打开文件", ThemedButtonRole::Normal,
            ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        const int openFolderWidth = ui.buttonWidth(L"打开文件夹", ThemedButtonRole::Normal,
            ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        const int createFileWidth = ui.buttonWidth(L"创建文件", ThemedButtonRole::Normal,
            ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        const int createFolderWidth = ui.buttonWidth(L"创建目录", ThemedButtonRole::Normal,
            ThemedButtonSize::Compact, ThemedButtonWidthMode::Text);
        const int gap = ui.layout().controlGapX;
        const int linkWidth = ui.textWidth(L"打开所在位置");
        const int groupWidth = openFileWidth + openFolderWidth + createFileWidth + createFolderWidth + linkWidth + gap * 4;
        const int gripWidth = ui.textWidth(L"移动");
        const int left = ui.contentLeft();
        ui.MoveControl(dragHandle_, left,
            y + (ui.compactButtonHeight() - ui.labelHeight()) / 2, gripWidth);
        const int x = std::max(left + gripWidth + gap, ui.centeredGroupX(groupWidth));
        ui.MoveControl(openFile_, x, y, openFileWidth);
        ui.MoveControl(openFolder_, x + openFileWidth + gap, y, openFolderWidth);
        ui.MoveControl(createFile_, x + openFileWidth + openFolderWidth + gap * 2, y, createFileWidth);
        ui.MoveControl(createFolder_, x + openFileWidth + openFolderWidth + createFileWidth + gap * 3,
            y, createFolderWidth);
        ui.MoveControl(openLocation_, x + groupWidth - linkWidth,
            y + (ui.compactButtonHeight() - ui.labelHeight()) / 2, linkWidth);
    }

    LRESULT HandleCommand(int id, int notification) {
        if (id == ID_FILE_HELPER_PATH && notification == CBN_SELCHANGE) {
            const int index = ThemedUi::ComboBoxSelectedIndex(pathEdit_);
            if (index >= 0) SelectHistory(static_cast<std::size_t>(index));
            return 0;
        }
        if (id == ID_FILE_HELPER_PATH && notification == CBN_EDITCHANGE) {
            UpdateOpenLocationEnabled();
            return 0;
        }
        switch (id) {
        case ID_FILE_HELPER_PICK:
            PickPath(picker_.primaryKind);
            return 0;
        case ID_FILE_HELPER_PICK_MENU: {
            const auto items = PickerMenuItems();
            const UINT command = windowUi_->ui().ShowSplitButtonMenu(
                hwnd_, picker_.split.menu, items);
            if (command == kPickFolderCommand) {
                PickPath(picker_.menuKind);
            }
            return 0;
        }
        case ID_FILE_HELPER_OPEN_FILE:
            RunAction(id, service_.OpenFile(hwnd_, CurrentPath()));
            return 0;
        case ID_FILE_HELPER_OPEN_FOLDER:
            RunAction(id, service_.OpenFolder(hwnd_, CurrentPath()));
            return 0;
        case ID_FILE_HELPER_CREATE_FILE:
            CreateFile(false);
            return 0;
        case ID_FILE_HELPER_CREATE_FOLDER:
            RunAction(id, service_.CreateFolder(CurrentPath()));
            return 0;
        case ID_FILE_HELPER_OPEN_LOCATION:
            RunAction(id, service_.OpenContainingLocation(hwnd_, CurrentPath()));
            return 0;
        case IDCANCEL:
            if (ThemedUi::IsComboBoxDropDownVisible(pathEdit_)) {
                ThemedUi::SetComboBoxDropDownVisible(pathEdit_, false);
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        default:
            return 0;
        }
    }

    void PickPath(CommonPathPickerKind kind) {
        CommonPathPickerDialogOptions options{};
        options.owner = hwnd_;
        options.defaultPath = CurrentPath();
        options.fileContext = L"file-helper-file";
        options.folderContext = L"file-helper-folder";
        options.fileTitle = L"选择文件";
        options.folderTitle = L"选择文件夹";
        CommonPathPickerResult result{};
        if (!ShowCommonPathPickerDialog(kind, options, result) || !result.dialog.accepted) {
            return;
        }
        ThemedUi::SetComboBoxText(pathEdit_, result.dialog.path);
        ShowResult(kind == CommonPathPickerKind::File ? L"已选择文件。" : L"已选择文件夹。",
            ThemedToastRole::Info);
        UpdateOpenLocationEnabled();
    }

    std::vector<ThemedSplitButtonMenuItem> PickerMenuItems() const {
        return windowUi_->ui().PathPickerSplitButtonMenuItems(picker_, kPickFolderCommand);
    }

    std::vector<std::wstring> HistoryItems() const {
        std::vector<std::wstring> items;
        items.reserve(history_.size());
        for (const auto& path : history_) items.push_back(path.wstring());
        return items;
    }

    void SelectHistory(std::size_t index) {
        if (index >= history_.size()) return;
        ThemedUi::SetComboBoxText(pathEdit_, history_[index].wstring());
        ShowResult(L"已选择历史路径。", ThemedToastRole::Info);
        UpdateOpenLocationEnabled();
    }

    void CreateFile(bool overwrite) {
        FileHelperResult result = service_.CreateFile(CurrentPath(), overwrite);
        if (result.status == FileHelperStatus::AlreadyExists && !overwrite) {
            const int answer = ThemedWindowUi::ShowMessageBox(
                hwnd_, instance_, theme_, L"文件已存在，覆盖会清空现有内容。", L"创建文件",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) {
                ShowResult(L"已取消覆盖。", ThemedToastRole::Warning);
                UpdateOpenLocationEnabled();
                return;
            }
            result = service_.CreateFile(CurrentPath(), true);
        }
        RunAction(ID_FILE_HELPER_CREATE_FILE, result);
    }

    void RunAction(int action, const FileHelperResult& result) {
        lastAction_ = action;
        if (!result.path.empty()) {
            ThemedUi::SetComboBoxText(pathEdit_, result.path.wstring());
        }
        if (!result.path.empty() && result.status != FileHelperStatus::Failed &&
            service_.RememberPath(result.path)) {
            history_ = service_.LoadHistory();
            ThemedUi::SetComboBoxItems(pathEdit_, HistoryItems(), -1);
        }
        ThemedToastRole role = ThemedToastRole::Danger;
        if (result.status == FileHelperStatus::Success) {
            role = ThemedToastRole::Success;
        } else if (result.status == FileHelperStatus::AlreadyExists) {
            role = ThemedToastRole::Info;
        }
        ShowResult(result.message, role);
        UpdateOpenLocationEnabled();
    }

    void ShowResult(const std::wstring& text, ThemedToastRole role) {
        ThemedToastOptions options{};
        options.anchor = ThemedToastAnchor::OwnerOutsideBottomRight;
        options.role = role;
        options.durationMs = (role == ThemedToastRole::Warning || role == ThemedToastRole::Danger) ? 6000 : 3000;
        windowUi_->ui().ShowToast(text, options);
    }

    std::wstring CurrentPath() const {
        return ThemedUi::ComboBoxText(pathEdit_);
    }

    void UpdateOpenLocationEnabled() {
        const bool enabled = service_.IsExistingPath(CurrentPath());
        windowUi_->ui().SetEnabled(openLocation_, enabled);
    }

    void ActivateAndFocus() {
        focusRequested_ = true;
        if (windowUi_) {
            windowUi_->ShowModeless();
        }
        if (!BackgroundAcceptanceMode() && pathEdit_) {
            ThemedUi::FocusComboBoxInput(pathEdit_);
        }
    }

    LRESULT HandleTestCommand(FileHelperTestCommand command, LPARAM value) {
        if (!QuattroTestMode() || !BackgroundAcceptanceMode()) {
            return FALSE;
        }
        auto* request = reinterpret_cast<FileHelperTestRequest*>(value);
        switch (command) {
        case FileHelperTestCommand::SetPath:
            if (!request) return FALSE;
            ThemedUi::SetComboBoxText(pathEdit_, request->path);
            UpdateOpenLocationEnabled();
            return TRUE;
        case FileHelperTestCommand::OpenFile:
            HandleCommand(ID_FILE_HELPER_OPEN_FILE, BN_CLICKED);
            return TRUE;
        case FileHelperTestCommand::OpenFolder:
            HandleCommand(ID_FILE_HELPER_OPEN_FOLDER, BN_CLICKED);
            return TRUE;
        case FileHelperTestCommand::CreateFileAction:
            CreateFile(false);
            return TRUE;
        case FileHelperTestCommand::CreateFileOverwriteAction:
            RunAction(ID_FILE_HELPER_CREATE_FILE, service_.CreateFile(CurrentPath(), true));
            return TRUE;
        case FileHelperTestCommand::CreateFolder:
            HandleCommand(ID_FILE_HELPER_CREATE_FOLDER, BN_CLICKED);
            return TRUE;
        case FileHelperTestCommand::OpenContainingLocation:
            HandleCommand(ID_FILE_HELPER_OPEN_LOCATION, BN_CLICKED);
            return TRUE;
        case FileHelperTestCommand::QueryFocusRequested:
            return focusRequested_ ? TRUE : FALSE;
        case FileHelperTestCommand::ResetFocusRequested:
            focusRequested_ = false;
            return TRUE;
        case FileHelperTestCommand::QueryContainingLocationEnabled:
            return IsWindowEnabled(openLocation_) ? TRUE : FALSE;
        case FileHelperTestCommand::QueryLastAction:
            return lastAction_;
        case FileHelperTestCommand::QueryHistoryCount:
            return static_cast<LRESULT>(history_.size());
        case FileHelperTestCommand::SelectHistory:
            if (value < 0 || static_cast<std::size_t>(value) >= history_.size()) return FALSE;
            SelectHistory(static_cast<std::size_t>(value));
            return TRUE;
        }
        return FALSE;
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    FileHelperService service_;
    std::vector<std::filesystem::path> history_;
    HWND pathEdit_ = nullptr;
    HWND dragHandle_ = nullptr;
    ThemedPathPickerSplitButton picker_{};
    HWND openFile_ = nullptr;
    HWND openFolder_ = nullptr;
    HWND createFile_ = nullptr;
    HWND createFolder_ = nullptr;
    HWND openLocation_ = nullptr;
    int lastAction_ = 0;
    bool focusRequested_ = false;
    bool deleteOnDestroy_ = false;
};

bool ShowFileHelperDialog(HWND owner, HINSTANCE instance, const Theme& theme) {
    return FileHelperDialog::Open(owner, instance, theme);
}

bool ToggleFileHelperDialog(HWND owner, HINSTANCE instance, const Theme& theme) {
    return FileHelperDialog::Toggle(owner, instance, theme);
}

bool PreTranslateFileHelperMessage(const MSG& message) {
    const HWND dialog = gFileHelperWindow.load();
    if (!IsMessageForWindow(dialog, message)) {
        return false;
    }
    MSG translated = message;
    return IsDialogMessageW(dialog, &translated) != FALSE;
}
