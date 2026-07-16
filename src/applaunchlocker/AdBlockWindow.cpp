#include "AdBlockWindow.h"

#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "ToastFeedback.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <utility>

namespace {
constexpr int ID_TAB_CONTROL = 1200;
constexpr int ID_TAB_BLOCK = 1201;
constexpr int ID_TAB_BLOCKED = 1202;
constexpr int ID_CONTENT_PANEL = 1203;
constexpr int ID_PATH_EDIT = 1210;
constexpr int ID_PICK_PATH = 1211;
constexpr int ID_MODE_EXACT = 1213;
constexpr int ID_MODE_NAME = 1214;
constexpr int ID_MODE_STARTUP = 1212;
constexpr int ID_SCAN_TABLE = 1215;
constexpr int ID_BLOCK_SELECTED = 1216;
constexpr int ID_CLEAR_RESULTS = 1217;
constexpr int ID_BLOCKED_TABLE = 1220;
constexpr int ID_UNBLOCK = 1221;
constexpr DWORD ID_PICK_CURRENT_FOLDER = 1300;

constexpr UINT WM_APP_SCAN_COMPLETE = WM_APP + 0x160;
constexpr UINT WM_APP_BLOCKED_COMPLETE = WM_APP + 0x161;
constexpr UINT WM_APP_OPERATION_COMPLETE = WM_APP + 0x162;

constexpr int kClientWidth = 780;
constexpr int kClientHeight = 448;

struct ScanPayload {
    ScanResult scan;
};
struct BlockedPayload {
    std::vector<DisabledRecord> blocked;
    std::wstring storeError;
};
struct OperationPayload {
    OperationResult result;
    bool rescan = false;
};

bool FileSystemFolderPath(IShellItem* item, std::wstring& path) {
    if (!item) return false;
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) || !selected) return false;
    path = selected;
    CoTaskMemFree(selected);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        path.clear();
        return false;
    }
    return true;
}

class PathDialogEvents final : public IFileDialogEvents, public IFileDialogControlEvents {
public:
    explicit PathDialogEvents(IFileDialog* dialog) : dialog_(dialog) {
        if (dialog_) dialog_->AddRef();
    }

    ~PathDialogEvents() {
        if (dialog_) dialog_->Release();
    }

    const std::wstring& selectedFolder() const { return selectedFolder_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IFileDialogEvents)) {
            *object = static_cast<IFileDialogEvents*>(this);
        } else if (iid == __uuidof(IFileDialogControlEvents)) {
            *object = static_cast<IFileDialogControlEvents*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(
        IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (response) *response = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(
        IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        if (response) *response = FDEOR_DEFAULT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize*, DWORD controlId) override {
        if (controlId != ID_PICK_CURRENT_FOLDER || !dialog_) return S_OK;
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog_->GetCurrentSelection(&item)) && item) {
            FileSystemFolderPath(item, selectedFolder_);
            item->Release();
        }
        if (selectedFolder_.empty() && SUCCEEDED(dialog_->GetFolder(&item)) && item) {
            FileSystemFolderPath(item, selectedFolder_);
            item->Release();
        }
        if (!selectedFolder_.empty()) dialog_->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*, DWORD) override { return S_OK; }

private:
    LONG references_ = 1;
    IFileDialog* dialog_ = nullptr;
    std::wstring selectedFolder_;
};

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(L'"');
            slashes = 0;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0;
        output.push_back(ch);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

std::wstring CurrentExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!copied) return {};
        if (copied < path.size() - 1) {
            path.resize(copied);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

bool RunningAsAdmin() {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators)) {
        CheckTokenMembership(nullptr, administrators, &isAdmin);
        FreeSid(administrators);
    }
    return isAdmin != FALSE;
}

OperationResult RunElevated(const std::wstring& parameters) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return {false, L"无法确定程序路径。"};
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        return {false, code == ERROR_CANCELLED ? L"已取消管理员授权。" : L"无法启动管理员操作：" + FormatLastError(code)};
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return exitCode == 0 ? OperationResult{true, L"操作完成。"} : OperationResult{false, L"管理员操作失败，请刷新后重试。"};
}

std::wstring MapField(const std::map<std::wstring, std::wstring>& values, const wchar_t* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::wstring{} : found->second;
}

std::wstring GetText(HWND hwnd) {
    if (!hwnd) return {};
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

// 扫描项的可读状态。
std::wstring ScanStatusText(const StartupItem& item) {
    const std::wstring status = MapField(item.original, L"adBlockStatus");
    const bool hasAutoStart = MapField(item.original, L"hasAutoStart") == L"1";
    if (status == L"blockable") return hasAutoStart ? L"可拦截·含自启动项" : L"可拦截";
    if (status == L"blockable-warn") return hasAutoStart ? L"可拦截·含自启动项（未签名）" : L"可拦截（未签名）";
    if (status == L"protected") return L"受保护";
    if (status == L"script") return L"脚本（仅提示）";
    if (status == L"unresolved") return L"无法解析";
    return L"仅查看";
}
}

AdBlockWindow::AdBlockWindow(HINSTANCE instance, Theme theme)
    : instance_(instance), theme_(std::move(theme)) {}

AdBlockWindow::~AdBlockWindow() {
    closing_ = true;
    JoinWorker();
}

int AdBlockWindow::Run() {
    HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    auto options = ThemedWindowUi::DialogOptions(instance_, nullptr, L"AdBlockMainWindow", L"广告拦截", Proc, this, icon, icon);
    options.clientWidth = kClientWidth;
    options.clientHeight = kClientHeight;
    std::wstring error;
    hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
    if (!hwnd_) {
        ThemedWindowUi::ShowMessageBox(nullptr, instance_, theme_, error, L"广告拦截", MB_OK | MB_ICONERROR);
        return 1;
    }
    wchar_t acceptanceDpiText[16]{};
    if (GetEnvironmentVariableW(L"QUATTRO_AD_BLOCK_ACCEPTANCE_DPI", acceptanceDpiText,
            static_cast<DWORD>(std::size(acceptanceDpiText))) > 0) {
        const UINT targetDpi = static_cast<UINT>(wcstoul(acceptanceDpiText, nullptr, 10));
        const UINT currentDpi = windowUi_ ? windowUi_->dpi() : USER_DEFAULT_SCREEN_DPI;
        if (targetDpi >= 96 && targetDpi <= 480 && targetDpi != currentDpi) {
            RECT windowRect{};
            GetWindowRect(hwnd_, &windowRect);
            const int targetWidth = MulDiv(
                windowRect.right - windowRect.left, static_cast<int>(targetDpi), static_cast<int>(currentDpi));
            const int targetHeight = MulDiv(
                windowRect.bottom - windowRect.top, static_cast<int>(targetDpi), static_cast<int>(currentDpi));
            const POINT targetPosition = CenterWindowOnOwnerMonitor(nullptr, targetWidth, targetHeight);
            RECT suggested{
                targetPosition.x,
                targetPosition.y,
                targetPosition.x + targetWidth,
                targetPosition.y + targetHeight};
            SendMessageW(hwnd_, WM_DPICHANGED, MAKELONG(targetDpi, targetDpi), reinterpret_cast<LPARAM>(&suggested));
        }
    }
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.hwnd == pathEdit_ && message.wParam == VK_RETURN) {
            StartScan();
            continue;
        }
        if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK AdBlockWindow::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    AdBlockWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<AdBlockWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<AdBlockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return window ? window->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AdBlockWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT result = 0;
    if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
        if (message == WM_DESTROY) {
            closing_ = true;
            JoinWorker();
            PostQuitMessage(0);
        }
        return result;
    }
    switch (message) {
    case WM_CREATE:
        windowUi_ = std::make_unique<ThemedWindowUi>(instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact,
            kClientWidth, kClientHeight);
        CreateControls();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        windowUi_->FillBackground(dc);
        windowUi_->DrawRegisteredTableFrames(dc);
        windowUi_->DrawRegisteredEditFrames(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == ID_TAB_CONTROL && HIWORD(wParam) == CBN_SELCHANGE) {
            SelectTab(ThemedUi::ActiveTab(tabControl_));
        } else if (id == ID_PICK_PATH) {
            PickPath();
        } else if (id == ID_CLEAR_RESULTS) {
            ClearScanResults();
        } else if (id == ID_BLOCK_SELECTED) {
            StartBlockSelected();
        } else if (id == ID_UNBLOCK) {
            StartUnblock();
        } else if (id == IDCANCEL) {
            DestroyWindow(hwnd_);
        }
        return 0;
    }
    case WM_NOTIFY: {
        ThemedTableEvent event{};
        if (ThemedUi::DecodeTableEvent(scanTable_, lParam, event)) {
            UpdateButtons();
            return 0;
        }
        if (ThemedUi::DecodeTableEvent(blockedTable_, lParam, event)) {
            UpdateButtons();
            return 0;
        }
        break;
    }
    case WM_APP_SCAN_COMPLETE: {
        std::unique_ptr<ScanPayload> payload(reinterpret_cast<ScanPayload*>(lParam));
        JoinWorker();
        CompleteScan(std::move(payload->scan));
        return 0;
    }
    case WM_APP_BLOCKED_COMPLETE: {
        std::unique_ptr<BlockedPayload> payload(reinterpret_cast<BlockedPayload*>(lParam));
        JoinWorker();
        CompleteBlocked(std::move(payload->blocked), std::move(payload->storeError));
        return 0;
    }
    case WM_APP_OPERATION_COMPLETE: {
        std::unique_ptr<OperationPayload> payload(reinterpret_cast<OperationPayload*>(lParam));
        JoinWorker();
        CompleteOperation(std::move(payload->result), payload->rescan);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void AdBlockWindow::CreateControls() {
    const ThemedUi ui = windowUi_->ui();
    const RECT content = ui.contentRect();
    const int gapX = ui.layout().controlGapX;
    const int rowGap = ui.layout().rowGap;

    const int footerY = ui.footerButtonY(ui.footerButtonHeight());
    const int statusY = footerY - ui.layout().sectionGap - ui.labelHeight();

    // 顶部标签页与内容面板使用公共 ConnectedTabs 语义直接连接。
    const int tabHeight = ui.tabButtonHeight();
    const RECT tabRect{content.left, content.top, content.right, content.top + tabHeight};
    const int connectionOverlap = ui.scale(1);
    const RECT panelRect{
        content.left,
        tabRect.bottom - connectionOverlap,
        content.right,
        statusY - rowGap};
    contentPanel_ = ui.Panel(ID_CONTENT_PANEL, panelRect, ThemedPanelOptions{ThemedPanelRole::Normal});
    const RECT panelContent = ThemedUi::PanelContentRect(contentPanel_);
    const RECT pageContent{
        panelRect.left + panelContent.left,
        panelRect.top + panelContent.top,
        panelRect.left + panelContent.right,
        panelRect.top + panelContent.bottom};

    ThemedTabControlOptions tabOptions{};
    tabOptions.activeIndex = 0;
    tabOptions.appearance = ThemedTabControlAppearance::ConnectedTabs;
    tabOptions.orientation = ThemedTabControlOrientation::Horizontal;
    tabOptions.containerStyle = ThemedTabControlContainerStyle::Borderless;
    tabControl_ = ui.TabControl(ID_TAB_CONTROL, tabRect,
        {{ID_TAB_BLOCK, L"拦截", true}, {ID_TAB_BLOCKED, L"已拦截", true}}, tabOptions);

    const int bodyTop = pageContent.top;
    const int tableBottom = pageContent.bottom;

    // ---- 拦截页控件 ----
    const int labelHeight = ui.labelHeight();
    const int pickWidth = ui.buttonWidth(L"选择", ThemedButtonRole::Normal, ThemedButtonSize::Normal,
        ThemedButtonWidthMode::Text);
    const int clearWidth = ui.buttonWidth(L"Clear", ThemedButtonRole::Normal, ThemedButtonSize::Normal,
        ThemedButtonWidthMode::Text);
    const int editHeight = ui.editHeight();
    const int pathY = bodyTop;
    const int editWidth = pageContent.right - pageContent.left - clearWidth - pickWidth - gapX * 2;
    pathEdit_ = ui.Edit(ID_PATH_EDIT, ui.editFrame(pageContent.left, pathY, editWidth), L"",
        ThemedEditOptions{ThemedEditMode::SingleLine, ThemedEditContent::Text, false, true, false, false, true, 0, L"选择要扫描的文件或文件夹"});
    clearButton_ = ui.Button(ID_CLEAR_RESULTS, L"Clear", pageContent.left + editWidth + gapX, pathY,
        ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    pickPathButton_ = ui.Button(ID_PICK_PATH, L"选择", pageContent.right - pickWidth, pathY,
        ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);

    const int listTop = ui.nextRowY(pathY, std::max(editHeight, labelHeight));
    const int modeY = footerY + (ui.footerButtonHeight() - ui.checkBoxHeight()) / 2;
    const int modeLabelWidth = ui.textWidth(L"拦截模式：") + ui.layout().rowGap;
    HWND modeLabel = ui.Label(L"拦截模式：", content.left,
        modeY + (ui.checkBoxHeight() - labelHeight) / 2, modeLabelWidth);
    const int radioLeft = content.left + modeLabelWidth + gapX;
    const int exactRadioWidth = ui.textWidth(L"精确路径") + ui.scale(28);
    const int nameRadioWidth = ui.textWidth(L"同名程序") + ui.scale(28);
    const int startupRadioWidth = ui.textWidth(L"禁止自启") + ui.scale(28);
    modeExactRadio_ = ui.RadioButton(ID_MODE_EXACT, L"精确路径", radioLeft, modeY, exactRadioWidth,
        ThemedRadioButtonOptions{1, true, true});
    modeNameRadio_ = ui.RadioButton(ID_MODE_NAME, L"同名程序",
        radioLeft + exactRadioWidth + gapX, modeY, nameRadioWidth,
        ThemedRadioButtonOptions{1, false, true});
    modeStartupRadio_ = ui.RadioButton(ID_MODE_STARTUP, L"禁止自启",
        radioLeft + exactRadioWidth + gapX + nameRadioWidth + gapX, modeY, startupRadioWidth,
        ThemedRadioButtonOptions{1, false, true});

    ThemedTableOptions tableOptions{};
    tableOptions.checkable = true;
    tableOptions.allowColumnResize = true;
    tableOptions.showRowGridLines = true;
    tableOptions.showColumnGridLines = true;
    scanTable_ = ui.Table(ID_SCAN_TABLE, RECT{pageContent.left, listTop, pageContent.right, tableBottom},
        {{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
         {L"path", L"路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"C:\\Program Files\\Example\\example.exe")},
         {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"可拦截（未签名）")}},
        tableOptions);

    // ---- 已拦截页控件 ----
    ThemedTableOptions blockedOptions{};
    blockedOptions.allowColumnResize = true;
    blockedOptions.showRowGridLines = true;
    blockedOptions.showColumnGridLines = true;
    blockedTable_ = ui.Table(ID_BLOCKED_TABLE, RECT{pageContent.left, bodyTop, pageContent.right, tableBottom},
        {{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
         {L"path", L"路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"C:\\Program Files\\Example\\example.exe")},
         {L"mode", L"模式", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"同名程序")},
         {L"time", L"拦截时间", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"2026-07-15")}},
        blockedOptions);

    statusText_ = ui.StatusText(L"选择文件或文件夹后开始扫描。", content.left, statusY,
        content.right - content.left, {ThemedStatusRole::Info, ThemedTextAlign::Start});

    blockButton_ = ui.FooterButton(ID_BLOCK_SELECTED, L"拦截所选", 0, 1, true, true);
    unblockButton_ = ui.FooterButton(ID_UNBLOCK, L"解除拦截", 0, 1, true, true);

    const std::vector<HWND> panelChildren{
        pathEdit_, clearButton_, pickPathButton_, scanTable_, blockedTable_};
    for (HWND child : panelChildren) {
        ThemedUi::SetControlSurface(child, ThemedControlSurface::Panel);
    }
    ThemedUi::BindPanelChildren(contentPanel_, panelChildren);

    // 绑定标签页可见性
    ThemedUi::BindTabPage(tabControl_, 0,
        {pathEdit_, clearButton_, pickPathButton_, modeLabel, modeExactRadio_, modeNameRadio_, modeStartupRadio_, scanTable_});
    ThemedUi::BindTabPage(tabControl_, 1, {blockedTable_});
    ThemedUi::SetActiveTab(tabControl_, 0, false);

    SelectTab(0);
}

void AdBlockWindow::JoinWorker() {
    if (worker_.joinable()) worker_.join();
}

std::wstring AdBlockWindow::SelectedMode() const {
    if (ThemedUi::IsChecked(modeStartupRadio_)) return L"startup";
    return ThemedUi::IsChecked(modeNameRadio_) ? L"name" : L"exact";
}

void AdBlockWindow::PickPath() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog))) || !dialog) return;

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    dialog->SetTitle(L"选择要扫描的文件或文件夹");
    const COMDLG_FILTERSPEC filters[] = {
        {L"可启动文件", L"*.exe;*.com;*.scr;*.bat;*.cmd;*.ps1;*.vbs;*.js;*.lnk"},
        {L"所有文件", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);

    auto* events = new PathDialogEvents(dialog);
    DWORD cookie = 0;
    const bool advised = SUCCEEDED(dialog->Advise(events, &cookie));
    IFileDialogCustomize* customize = nullptr;
    if (advised && SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&customize))) && customize) {
        if (SUCCEEDED(customize->AddPushButton(ID_PICK_CURRENT_FOLDER, L"选择此文件夹"))) {
            customize->MakeProminent(ID_PICK_CURRENT_FOLDER);
        }
    }

    std::wstring path;
    const HRESULT showResult = dialog->Show(hwnd_);
    path = events->selectedFolder();
    if (path.empty() && SUCCEEDED(showResult)) {
            IShellItem* selected = nullptr;
            if (SUCCEEDED(dialog->GetResult(&selected)) && selected) {
                PWSTR selectedPath = nullptr;
                if (SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)) && selectedPath) {
                    path = selectedPath;
                    CoTaskMemFree(selectedPath);
                }
                selected->Release();
            }
    }

    if (customize) customize->Release();
    if (advised) dialog->Unadvise(cookie);
    events->Release();
    dialog->Release();

    if (!path.empty()) {
        ThemedUi::SetText(pathEdit_, path);
        StartScan();
    }
}

void AdBlockWindow::StartScan() {
    if (busy_) return;
    const std::wstring path = Trim(GetText(pathEdit_));
    if (path.empty()) {
        ThemedUi::SetText(statusText_, L"请先选择文件或文件夹。");
        return;
    }
    JoinWorker();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在扫描…");
    UpdateButtons();
    const HWND target = hwnd_;
    worker_ = std::thread([target, path]() {
        auto payload = std::make_unique<ScanPayload>();
        payload->scan = AdBlockManager().ScanPath(path);
        if (!PostMessageW(target, WM_APP_SCAN_COMPLETE, 0, reinterpret_cast<LPARAM>(payload.get()))) return;
        payload.release();
    });
}

void AdBlockWindow::ClearScanResults() {
    if (busy_ || activeTab_ != 0) return;
    scanItems_.clear();
    ThemedUi::ClearTable(scanTable_);
    ThemedUi::SetText(statusText_, L"选择文件或文件夹后开始扫描。");
    UpdateButtons();
}

void AdBlockWindow::StartBlockSelected() {
    if (busy_ || activeTab_ != 0) return;
    const std::wstring mode = SelectedMode();
    std::vector<std::wstring> targets;
    int skippedNoAutoStart = 0;
    bool anyRequiresAdmin = false;
    for (int index = 0; index < static_cast<int>(scanItems_.size()); ++index) {
        if (!ThemedUi::IsTableChecked(scanTable_, index)) continue;
        const StartupItem& item = scanItems_[static_cast<std::size_t>(index)];
        if (!item.canDisable) continue;
        // 启动拦截仅适用于已注册开机自启动的程序；无自启动项的勾选项跳过。
        if (mode == L"startup" && MapField(item.original, L"hasAutoStart") != L"1") {
            ++skippedNoAutoStart;
            continue;
        }
        if (MapField(item.original, L"autoStartRequiresAdmin") == L"1") anyRequiresAdmin = true;
        targets.push_back(MapField(item.original, L"targetPath"));
    }
    if (targets.empty()) {
        const std::wstring message = (mode == L"startup" && skippedNoAutoStart > 0)
            ? L"所选程序均未注册开机自启动项，「禁止自启」模式无可处理项。"
            : L"请勾选至少一个可拦截的程序。";
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, message, L"广告拦截",
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring modeText;
    std::wstring mechanismText;
    if (mode == L"startup") {
        modeText = L"禁止自启（仅禁止开机/登录自启动，不阻止手动运行）";
        mechanismText = L"\n写入系统「启动」开关，与任务管理器→启动同步，可随时在「已拦截」中解除。";
    } else if (mode == L"name") {
        modeText = L"同名程序（拦截所有同名 exe）";
        mechanismText = L"\n拦截写入系统 IFEO，需要管理员权限，可随时在「已拦截」中解除。";
    } else {
        modeText = L"精确路径（仅拦截所选文件）";
        mechanismText = L"\n拦截写入系统 IFEO，需要管理员权限，可随时在「已拦截」中解除。";
    }
    std::wstring prompt = L"确定拦截所选 " + std::to_wstring(targets.size()) + L" 个程序？\n\n模式：" + modeText +
        mechanismText;
    if (skippedNoAutoStart > 0) {
        prompt += L"\n（已跳过 " + std::to_wstring(skippedNoAutoStart) + L" 个无自启动项的所选程序）";
    }
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, prompt, L"确认拦截",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;

    // 启动拦截：仅 HKLM 项需提权，全 HKCU 时免 UAC；IFEO 始终需管理员。
    const bool needAdmin = (mode == L"startup") ? anyRequiresAdmin : true;
    const bool elevate = needAdmin && !RunningAsAdmin();
    JoinWorker();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在拦截…");
    UpdateButtons();
    const HWND target = hwnd_;
    worker_ = std::thread([target, targets, mode, elevate]() {
        auto payload = std::make_unique<OperationPayload>();
        payload->rescan = true;
        int ok = 0;
        int fail = 0;
        std::wstring lastError;
        for (const std::wstring& path : targets) {
            OperationResult result;
            if (elevate) result = RunElevated(L"block --path " + QuoteArgument(path) + L" --mode " + mode);
            else result = AdBlockManager().Block(path, mode);
            if (result.success) ++ok; else { ++fail; lastError = result.message; }
        }
        if (fail == 0) payload->result = {true, L"已拦截 " + std::to_wstring(ok) + L" 个程序。"};
        else payload->result = {ok > 0, L"已拦截 " + std::to_wstring(ok) + L" 个，" + std::to_wstring(fail) +
            L" 个失败：" + lastError, ok > 0};
        if (!PostMessageW(target, WM_APP_OPERATION_COMPLETE, 0, reinterpret_cast<LPARAM>(payload.get()))) return;
        payload.release();
    });
}

void AdBlockWindow::StartUnblock() {
    if (busy_ || activeTab_ != 1) return;
    const int row = ThemedUi::TableSelectedIndex(blockedTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= blocked_.size()) return;
    const DisabledRecord& record = blocked_[static_cast<std::size_t>(row)];
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, L"确定解除对“" + record.name + L"”的拦截？",
            L"解除拦截", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const std::wstring recordId = record.recordId;
    const bool elevate = !RunningAsAdmin();
    JoinWorker();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在解除…");
    UpdateButtons();
    const HWND target = hwnd_;
    worker_ = std::thread([target, recordId, elevate]() {
        auto payload = std::make_unique<OperationPayload>();
        payload->rescan = false;
        if (elevate) payload->result = RunElevated(L"unblock --record-id " + QuoteArgument(recordId));
        else payload->result = AdBlockManager().Unblock(recordId);
        if (!PostMessageW(target, WM_APP_OPERATION_COMPLETE, 0, reinterpret_cast<LPARAM>(payload.get()))) return;
        payload.release();
    });
}

void AdBlockWindow::CompleteScan(ScanResult scan) {
    busy_ = false;
    scanItems_ = std::move(scan.items);
    RebuildScanRows();
    const int blockable = static_cast<int>(std::count_if(scanItems_.begin(), scanItems_.end(),
        [](const StartupItem& item) { return item.canDisable; }));
    std::wstring status = L"共 " + std::to_wstring(scanItems_.size()) + L" 个可启动文件，其中 " +
        std::to_wstring(blockable) + L" 个可拦截。";
    if (!scan.warnings.empty()) {
        for (const auto& warning : scan.warnings) AppendAppLaunchLockerLog(L"广告拦截扫描警告：" + warning);
        status += L" 部分项目未能读取。";
    }
    ThemedUi::SetText(statusText_, status);
    UpdateButtons();
}

void AdBlockWindow::CompleteBlocked(std::vector<DisabledRecord> blocked, std::wstring storeError) {
    busy_ = false;
    storeAvailable_ = storeError.empty();
    blocked_ = std::move(blocked);
    RebuildBlockedRows();
    if (!storeError.empty()) {
        AppendAppLaunchLockerLog(storeError);
        ThemedUi::SetText(statusText_, storeError);
    } else {
        ThemedUi::SetText(statusText_, L"已拦截 " + std::to_wstring(blocked_.size()) + L" 个程序。");
    }
    UpdateButtons();
}

void AdBlockWindow::CompleteOperation(OperationResult result, bool rescan) {
    busy_ = false;
    if (!result.success) {
        AppendAppLaunchLockerLog(L"广告拦截操作失败：" + result.message);
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, result.message, L"广告拦截", MB_OK | MB_ICONWARNING);
    } else if (windowUi_) {
        ThemedToastOptions toast{};
        toast.role = OperationToastRole(result.success, result.partial);
        if (result.partial) toast.durationMs = 5000;
        windowUi_->ui().ShowToast(result.message.empty() ? L"操作完成。" : result.message, toast);
    }
    // 重新扫描以刷新已拦截列表；若在拦截页则也刷新扫描列表。
    LoadBlockedAsync();
    if (rescan && !Trim(GetText(pathEdit_)).empty()) StartScan();
}

void AdBlockWindow::LoadBlockedAsync() {
    JoinWorker();
    const HWND target = hwnd_;
    worker_ = std::thread([target]() {
        auto payload = std::make_unique<BlockedPayload>();
        AdBlockManager().ListBlocked(payload->blocked, payload->storeError);
        if (!PostMessageW(target, WM_APP_BLOCKED_COMPLETE, 0, reinterpret_cast<LPARAM>(payload.get()))) return;
        payload.release();
    });
}

void AdBlockWindow::SelectTab(int index) {
    if (index < 0 || index > 1) return;
    activeTab_ = index;
    ThemedUi::SetActiveTab(tabControl_, index, false);
    if (index == 1) LoadBlockedAsync();
    UpdateButtons();
}

void AdBlockWindow::RebuildScanRows() {
    std::vector<ThemedTableRow> rows;
    rows.reserve(scanItems_.size());
    for (std::size_t index = 0; index < scanItems_.size(); ++index) {
        const StartupItem& item = scanItems_[index];
        const std::wstring path = MapField(item.original, L"targetPath");
        rows.push_back({static_cast<std::intptr_t>(index + 1),
            {{item.name}, {path.empty() ? item.location : path}, {ScanStatusText(item)}},
            false, item.canDisable});
    }
    ThemedUi::SetTableRows(scanTable_, rows);
}

void AdBlockWindow::RebuildBlockedRows() {
    std::vector<ThemedTableRow> rows;
    rows.reserve(blocked_.size());
    for (std::size_t index = 0; index < blocked_.size(); ++index) {
        const DisabledRecord& record = blocked_[index];
        const std::wstring blockMode = MapField(record.original, L"blockMode");
        const std::wstring mode = blockMode == L"name" ? L"同名程序"
            : blockMode == L"startup" ? L"禁止自启" : L"精确路径";
        std::wstring when = record.disabledAt;
        if (when.size() >= 10) when = when.substr(0, 10);
        rows.push_back({static_cast<std::intptr_t>(index + 1),
            {{record.name}, {MapField(record.original, L"targetPath")}, {mode}, {when}},
            false, true});
    }
    ThemedUi::SetTableRows(blockedTable_, rows);
}

void AdBlockWindow::UpdateButtons() {
    if (!windowUi_) return;
    const ThemedUi ui = windowUi_->ui();
    const bool blockTab = activeTab_ == 0;
    ThemedUi::SetVisible(blockButton_, blockTab);
    ThemedUi::SetVisible(unblockButton_, !blockTab);
    ui.SetEnabled(blockButton_, blockTab && !busy_);
    ui.SetEnabled(clearButton_, blockTab && !busy_ && !scanItems_.empty());
    const int selected = ThemedUi::TableSelectedIndex(blockedTable_);
    ui.SetEnabled(unblockButton_, !blockTab && !busy_ && storeAvailable_ &&
        selected >= 0 && static_cast<std::size_t>(selected) < blocked_.size());
}
