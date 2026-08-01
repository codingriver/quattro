#include "AdBlockWindow.h"

#include "AppLog.h"
#include "FileDialog.h"
#include "TaskExecutionService.h"
#include "ThemedTaskProgressDialog.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "ToastFeedback.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace {
constexpr int ID_TAB_CONTROL = 1200;
constexpr int ID_TAB_BLOCK = 1201;
constexpr int ID_TAB_BLOCKED = 1202;
constexpr int ID_CONTENT_PANEL = 1203;
constexpr int ID_PATH_EDIT = 1210;
constexpr int ID_PICK_PATH = 1211;
constexpr int ID_PICK_PATH_MENU = 1218;
constexpr int ID_PICK_FOLDER = 1219;
constexpr int ID_MODE_EXACT = 1213;
constexpr int ID_MODE_NAME = 1214;
constexpr int ID_SCAN_TABLE = 1215;
constexpr int ID_BLOCK_SELECTED = 1216;
constexpr int ID_CLEAR_RESULTS = 1217;
constexpr int ID_BLOCKED_TABLE = 1220;
constexpr int ID_UNBLOCK = 1221;
constexpr int ID_CHECK_PATH = 1222;
constexpr int ID_REFRESH_BLOCKED = 1223;
constexpr int ID_CLEAN_STALE = 1224;
constexpr int ID_UNBLOCK_ALL = 1225;
constexpr int ID_DETAILS = 1226;
constexpr int ID_REPAIR = 1227;
constexpr int ID_STATUS_TEXT = 1228;

constexpr UINT WM_APP_SCAN_COMPLETE = WM_APP + 0x160;
constexpr UINT WM_APP_BLOCKED_COMPLETE = WM_APP + 0x161;
constexpr UINT WM_APP_OPERATION_COMPLETE = WM_APP + 0x162;
constexpr UINT WM_APP_TEST_SHOW_CONFIRMATION = WM_APP + 0x163;
constexpr UINT WM_APP_TEST_OPERATION_COMPLETE = WM_APP + 0x164;

constexpr int kClientWidth = 780;
constexpr int kClientHeight = 448;

struct BlockedPayload {
    std::vector<DisabledRecord> blocked;
    std::wstring storeError;
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

OperationResult RunElevatedRequest(
    const std::wstring& action,
    const std::vector<std::wstring>& targets,
    const std::wstring& mode = {}) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return {false, L"无法确定程序路径。"};
    ElevatedOperationRequest request;
    std::wstring error;
    if (!CreateElevatedOperationRequest(action, targets, mode, request, error)) return {false, error};
    const std::wstring parameters = L"runas-operation --request " + QuoteArgument(request.requestPath.wstring()) +
        L" --token " + QuoteArgument(request.token);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        DeleteFileW(request.requestPath.c_str());
        DeleteFileW(request.resultPath.c_str());
        return {false, code == ERROR_CANCELLED ? L"已取消管理员授权。" : L"无法启动管理员操作：" + FormatLastError(code)};
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    CloseHandle(info.hProcess);
    OperationResult result;
    if (!ReadElevatedOperationResult(request, result, error)) {
        DeleteFileW(request.requestPath.c_str());
        return {false, error.empty() ? L"管理员操作失败，请刷新后重试。" : error};
    }
    return result;
}

std::filesystem::path WindowStatePath() {
    return AppLaunchLockerDataDirectory() / L"ad-block-window.ini";
}

void RestoreWindowPosition(HWND hwnd) {
    RECT rect{};
    if (!hwnd || !GetWindowRect(hwnd, &rect)) {
        return;
    }
    const std::filesystem::path path = WindowStatePath();
    wchar_t xBuffer[32]{};
    wchar_t yBuffer[32]{};
    GetPrivateProfileStringW(L"window", L"x", L"", xBuffer, _countof(xBuffer), path.c_str());
    GetPrivateProfileStringW(L"window", L"y", L"", yBuffer, _countof(yBuffer), path.c_str());
    const std::optional<int> x = ParseInt(xBuffer);
    const std::optional<int> y = ParseInt(yBuffer);
    if (!x || !y) {
        return;
    }
    const std::optional<POINT> restored = ThemedWindowUi::RestoredWindowPosition(
        *x,
        *y,
        rect.right - rect.left,
        rect.bottom - rect.top);
    if (!restored) {
        return;
    }
    SetWindowPos(hwnd, nullptr, restored->x, restored->y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void SaveWindowPosition(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) {
        return;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return;
    }
    const std::filesystem::path path = WindowStatePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    WritePrivateProfileStringW(L"window", L"version", L"1", path.c_str());
    WritePrivateProfileStringW(L"window", L"x", std::to_wstring(rect.left).c_str(), path.c_str());
    WritePrivateProfileStringW(L"window", L"y", std::to_wstring(rect.top).c_str(), path.c_str());
    WritePrivateProfileStringW(L"window", L"dpi", std::to_wstring(GetDpiForWindow(hwnd)).c_str(), path.c_str());
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
    if (status == L"blockable") return L"可拦截";
    if (status == L"blockable-warn") return L"可拦截（未签名）";
    if (status == L"protected") return L"受保护";
    if (status == L"script") return L"脚本（仅提示）";
    if (status == L"unresolved") return L"无法解析";
    return L"仅查看";
}

std::wstring ScanImpactText(const StartupItem& item, const std::wstring& mode) {
    const std::wstring status = MapField(item.original, L"adBlockStatus");
    if (!item.canDisable) {
        const std::wstring reason = MapField(item.original, L"guardReason");
        if (!reason.empty()) return reason;
        if (status == L"script") return L"脚本仅提示";
        if (status == L"unresolved") return L"无法解析目标";
        return L"不可拦截";
    }
    if (mode == L"name") return L"所有同名 EXE";
    return L"仅此路径";
}

std::wstring PlanConfirmationPrompt(const AdBlockPlan& plan) {
    std::wostringstream prompt;
    prompt << L"将拦截 " << plan.blockableCount << L" 个程序";
    if (plan.warningCount > 0) prompt << L"，其中 " << plan.warningCount << L" 个需要注意";
    if (plan.blockedCount > 0) prompt << L"，跳过 " << plan.blockedCount << L" 个";
    prompt << L"。\n\n";
    int shown = 0;
    for (const AdBlockPlanItem& item : plan.items) {
        if (shown >= 8) {
            prompt << L"…其余项目请在列表中查看。\n";
            break;
        }
        const bool blocked = item.riskLevel == L"blocked";
        const bool warning = item.riskLevel == L"warn";
        std::wstring name = item.imageName.empty() ? std::filesystem::path(item.targetPath).filename().wstring() : item.imageName;
        if (name.empty()) name = item.targetPath;
        prompt << (blocked ? L"× " : warning ? L"! " : L"✓ ")
               << name << L"　" << item.impactText;
        if (!item.reason.empty()) prompt << L"　" << item.reason;
        prompt << L"\n";
        ++shown;
    }
    prompt << L"\n确认继续？";
    return prompt.str();
}

}

AdBlockWindow::AdBlockWindow(HINSTANCE instance, Theme theme)
    : instance_(instance), theme_(std::move(theme)) {}

AdBlockWindow::~AdBlockWindow() {
    closing_ = true;
    if (scanTask_) scanTask_->RequestStop();
    if (operationTask_) { operationTask_->RequestStop(); operationTask_.reset(); }
    if (blockedTask_) { blockedTask_->RequestStop(); blockedTask_.reset(); }
    if (scanProgressDialog_) scanProgressDialog_->Close();
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
    RestoreWindowPosition(hwnd_);
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
            SaveWindowPosition(hwnd_);
            closing_ = true;
            if (scanTask_) scanTask_->RequestStop();
            if (operationTask_) { operationTask_->RequestStop(); operationTask_.reset(); }
            if (blockedTask_) { blockedTask_->RequestStop(); blockedTask_.reset(); }
            if (scanProgressDialog_) scanProgressDialog_->Close();
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
            PickFile();
        } else if (id == ID_PICK_PATH_MENU) {
            const UINT command = windowUi_->ui().ShowSplitButtonMenu(
                hwnd_,
                pickPathSplit_.menu,
                {{ID_PICK_FOLDER, L"选择文件夹", true, TablerIconId::Folder}});
            if (command != 0) {
                SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
            }
        } else if (id == ID_PICK_FOLDER) {
            PickFolder();
        } else if (id == ID_CLEAR_RESULTS) {
            ClearScanResults();
        } else if (id == ID_CHECK_PATH) {
            StartScan();
        } else if (id == ID_BLOCK_SELECTED) {
            StartBlockSelected();
        } else if (id == ID_UNBLOCK) {
            StartUnblock();
        } else if (id == ID_UNBLOCK_ALL) {
            StartUnblockAll();
        } else if (id == ID_CLEAN_STALE) {
            StartCleanStale();
        } else if (id == ID_REFRESH_BLOCKED) {
            LoadBlockedAsync();
        } else if (id == ID_DETAILS) {
            ShowSelectedDetails();
        } else if (id == ID_REPAIR) {
            StartRepairSelected();
        } else if (id == ID_MODE_EXACT || id == ID_MODE_NAME) {
            RebuildScanRows();
            UpdateButtons();
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
        if (!scanTask_ || !scanTask_->IsFinished()) return 0;
        scanTask_->Wait();
        AdBlockScanResult scan;
        if (scanTask_->Status() == ScanTaskStatus::Failed) {
            scan.error = scanTask_->Snapshot().error;
        } else {
            scan = scanTask_->ResultCopy<AdBlockScanResult>();
        }
        scanTask_.reset();
        CompleteScan(std::move(scan));
        return 0;
    }
    case WM_APP_BLOCKED_COMPLETE: {
        if (!blockedTask_ || !blockedTask_->IsFinished()) return 0;
        BlockedPayload payload;
        if (blockedTask_->Status() == TaskStatus::Completed) {
            payload = blockedTask_->ResultCopy<BlockedPayload>();
        } else {
            payload.storeError = blockedTask_->Snapshot().error.empty()
                ? L"加载已拦截记录已停止。" : blockedTask_->Snapshot().error;
        }
        blockedTask_.reset();
        CompleteBlocked(std::move(payload.blocked), std::move(payload.storeError));
        return 0;
    }
    case WM_APP_OPERATION_COMPLETE: {
        if (!operationTask_ || !operationTask_->IsFinished()) return 0;
        OperationResult operation;
        if (operationTask_->Status() == TaskStatus::Completed) operation = operationTask_->ResultCopy<OperationResult>();
        else operation = {false, operationTask_->Snapshot().error.empty()
            ? std::wstring(L"操作已停止。") : operationTask_->Snapshot().error};
        operationTask_.reset();
        CompleteOperation(std::move(operation));
        return 0;
    }
    case WM_APP_TEST_SHOW_CONFIRMATION:
        if (QuattroTestMode() && !testConfirmationShown_) {
            wchar_t enabled[8]{};
            if (GetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_SHOW_CONFIRMATION", enabled,
                    static_cast<DWORD>(std::size(enabled))) > 0) {
                testConfirmationShown_ = true;
                ThemedWindowUi::ShowMessageBox(
                    hwnd_, instance_, theme_,
                    L"将对 1 个程序启用广告拦截。\n\n模式：精确路径\n\n确认后会写入 IFEO 拦截规则，解除拦截前目标程序将无法启动。",
                    L"确认拦截", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            }
        }
        return 0;
    case WM_APP_TEST_OPERATION_COMPLETE:
        if (QuattroTestMode()) {
            if (activeTab_ == 0 && !lastScanStatus_.empty()) {
                ThemedUi::SetText(statusText_, lastScanStatus_);
            }
            UpdateButtons();
        }
        return 0;
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
    const int pickWidth = ui.splitButtonWidth(L"文件", ThemedButtonRole::Normal, ThemedButtonSize::Normal,
        ThemedButtonWidthMode::Text);
    const int clearWidth = ui.buttonWidth(L"清空", ThemedButtonRole::Normal, ThemedButtonSize::Normal,
        ThemedButtonWidthMode::Text);
    const int checkWidth = ui.buttonWidth(L"查看进度", ThemedButtonRole::Primary, ThemedButtonSize::Normal,
        ThemedButtonWidthMode::Text);
    const int editHeight = ui.editHeight();
    const int pathY = bodyTop;
    const int editWidth = pageContent.right - pageContent.left - clearWidth - pickWidth - checkWidth - gapX * 3;
    ThemedEditOptions pathOptions{};
    pathOptions.mode = ThemedEditMode::SingleLine;
    pathOptions.content = ThemedEditContent::Text;
    pathOptions.acceptsReturn = true;
    pathOptions.placeholder = L"输入或选择要检查的文件或文件夹";
    pathEdit_ = ui.Edit(ID_PATH_EDIT, ui.editFrame(pageContent.left, pathY, editWidth), L"", pathOptions);
    clearButton_ = ui.Button(ID_CLEAR_RESULTS, L"清空", pageContent.left + editWidth + gapX, pathY,
        ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    pickPathSplit_ = ui.SplitButton(ID_PICK_PATH, ID_PICK_PATH_MENU, L"文件",
        pageContent.left + editWidth + gapX + clearWidth + gapX, pathY,
        ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, pickWidth);
    checkButton_ = ui.Button(ID_CHECK_PATH, L"检查", pageContent.right - checkWidth, pathY,
        ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, checkWidth, true);

    const int listTop = ui.nextRowY(pathY, std::max(editHeight, labelHeight));
    const int modeY = footerY + (ui.footerButtonHeight() - ui.checkBoxHeight()) / 2;
    const int modeLabelWidth = ui.textWidth(L"拦截模式：") + ui.layout().rowGap;
    HWND modeLabel = ui.SelectableLabel(L"拦截模式：", content.left,
        modeY + (ui.checkBoxHeight() - labelHeight) / 2, modeLabelWidth);
    const int radioLeft = content.left + modeLabelWidth + gapX;
    const int exactRadioWidth = ui.textWidth(L"精确路径") + ui.scale(28);
    const int nameRadioWidth = ui.textWidth(L"同名程序") + ui.scale(28);
    modeExactRadio_ = ui.RadioButton(ID_MODE_EXACT, L"精确路径", radioLeft, modeY, exactRadioWidth,
        ThemedRadioButtonOptions{1, true, true});
    modeNameRadio_ = ui.RadioButton(ID_MODE_NAME, L"同名程序",
        radioLeft + exactRadioWidth + gapX, modeY, nameRadioWidth,
        ThemedRadioButtonOptions{1, false, true});

    ThemedTableOptions tableOptions{};
    tableOptions.checkable = true;
    tableOptions.allowColumnResize = true;
    tableOptions.showRowGridLines = true;
    tableOptions.showColumnGridLines = true;
    scanTable_ = ui.Table(ID_SCAN_TABLE, RECT{pageContent.left, listTop, pageContent.right, tableBottom},
        {{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
         {L"path", L"路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"C:\\Program Files\\Example\\example.exe")},
         {L"impact", L"影响范围", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"所有同名 EXE")},
         {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"可拦截（未签名）")}},
        tableOptions);

    // ---- 已拦截页控件 ----
    ThemedTableOptions blockedOptions{};
    blockedOptions.allowColumnResize = true;
    blockedOptions.showRowGridLines = true;
    blockedOptions.showColumnGridLines = true;
    blockedTable_ = ui.Table(ID_BLOCKED_TABLE, RECT{pageContent.left, bodyTop, pageContent.right, tableBottom},
        {{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
         {L"mode", L"模式", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"同名程序")},
         {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"被外部修改")},
         {L"path", L"路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"C:\\Program Files\\Example\\example.exe")},
         {L"time", L"拦截时间", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"2026-07-15")}},
        blockedOptions);

    ThemedSelectableTextOptions statusOptions{};
    statusOptions.role = ThemedSelectableTextRole::StatusLike;
    statusOptions.align = ThemedTextAlign::Start;
    statusOptions.statusRole = ThemedStatusRole::Info;
    statusOptions.showFrame = false;
    statusOptions.transparentBackground = true;
    statusOptions.tabStop = false;
    statusText_ = ui.SelectableText(ID_STATUS_TEXT, ui.rect(content.left, statusY, content.right - content.left, ui.labelHeight()),
        L"输入或选择文件、文件夹后点击“检查”。", statusOptions);

    detailsButton_ = ui.FooterButton(ID_DETAILS, L"详情", 0, 6);
    repairButton_ = ui.FooterButton(ID_REPAIR, L"修复", 1, 6);
    refreshBlockedButton_ = ui.FooterButton(ID_REFRESH_BLOCKED, L"刷新状态", 2, 6);
    cleanStaleButton_ = ui.FooterButton(ID_CLEAN_STALE, L"清理失效", 3, 6);
    unblockAllButton_ = ui.FooterButton(ID_UNBLOCK_ALL, L"全部解除", 4, 6);
    blockButton_ = ui.FooterButton(ID_BLOCK_SELECTED, L"拦截所选", 5, 6, true, true);
    unblockButton_ = ui.FooterButton(ID_UNBLOCK, L"解除拦截", 5, 6, true, true);

    const std::vector<HWND> panelChildren{
        pathEdit_, clearButton_, pickPathSplit_.primary, pickPathSplit_.menu, checkButton_, scanTable_, blockedTable_};
    for (HWND child : panelChildren) {
        ThemedUi::SetControlSurface(child, ThemedControlSurface::Panel);
    }
    ThemedUi::BindPanelChildren(contentPanel_, panelChildren);

    // 绑定标签页可见性
    ThemedUi::BindTabPage(tabControl_, 0,
        {pathEdit_, clearButton_, pickPathSplit_.primary, pickPathSplit_.menu, checkButton_, modeLabel, modeExactRadio_, modeNameRadio_, scanTable_});
    ThemedUi::BindTabPage(tabControl_, 1, {blockedTable_});
    ThemedUi::SetActiveTab(tabControl_, 0, false);

    SelectTab(0);
}

void AdBlockWindow::StartOperationTask(std::function<OperationResult()> operation) {
    const HWND target = hwnd_;
    TaskOptions options{};
    options.mode = TaskExecutionMode::BackgroundSingle;
    options.maxWorkers = 1;
    options.completionCallback = [target]() { PostMessageW(target, WM_APP_OPERATION_COMPLETE, 0, 0); };
    operationTask_ = TaskExecutionService::StartTyped<OperationResult>(
        std::move(options),
        [operation = std::move(operation)](TaskContext&) mutable {
            SetOperationAuditContext(
                L"gui-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()),
                L"gui",
                RunningAsAdmin());
            return operation();
        });
}

std::wstring AdBlockWindow::SelectedMode() const {
    return ThemedUi::IsChecked(modeNameRadio_) ? L"name" : L"exact";
}

void AdBlockWindow::PickFile() {
    CommonFileDialogOptions options{};
    options.owner = hwnd_;
    options.mode = CommonFileDialogMode::FileOnly;
    options.context = L"应用拦截扫描文件";
    options.title = L"选择要检查的文件";
    options.defaultPath = GetText(pathEdit_);
    options.legacyFilter = L"可启动文件\0*.exe;*.com;*.scr;*.bat;*.cmd;*.ps1;*.vbs;*.js;*.lnk\0所有文件\0*.*\0";

    CommonFileDialogResult result{};
    WriteAppLog(L"广告拦截 文件选择开始");
    const bool selected = ShowCommonFileDialog(options, result);
    WriteAppLog(
        L"广告拦截 文件选择返回: selected=" + std::wstring(selected ? L"1" : L"0") +
        L", elapsedMs=" + std::to_wstring(result.elapsedMs));
    if (selected) {
        const ULONGLONG updateStarted = GetTickCount64();
        WriteAppLog(L"广告拦截 文件选择后界面更新开始");
        ThemedUi::SetText(pathEdit_, result.path);
        ThemedUi::SetText(statusText_, L"已选择文件，点击“检查”开始。" );
        UpdateButtons();
        WriteAppLog(
            L"广告拦截 文件选择后界面更新完成: elapsedMs=" +
            std::to_wstring(GetTickCount64() - updateStarted));
    }
}

void AdBlockWindow::PickFolder() {
    CommonFileDialogOptions options{};
    options.owner = hwnd_;
    options.mode = CommonFileDialogMode::FolderOnly;
    options.context = L"应用拦截扫描文件夹";
    options.title = L"选择要检查的文件夹";
    options.defaultPath = GetText(pathEdit_);

    CommonFileDialogResult result{};
    WriteAppLog(L"广告拦截 文件夹选择开始");
    const bool selected = ShowCommonFileDialog(options, result);
    WriteAppLog(
        L"广告拦截 文件夹选择返回: selected=" + std::wstring(selected ? L"1" : L"0") +
        L", elapsedMs=" + std::to_wstring(result.elapsedMs));
    if (selected) {
        const ULONGLONG updateStarted = GetTickCount64();
        WriteAppLog(L"广告拦截 文件夹选择后界面更新开始");
        ThemedUi::SetText(pathEdit_, result.path);
        ThemedUi::SetText(statusText_, L"已选择文件夹，点击“检查”开始。" );
        UpdateButtons();
        WriteAppLog(
            L"广告拦截 文件夹选择后界面更新完成: elapsedMs=" +
            std::to_wstring(GetTickCount64() - updateStarted));
    }
}

void AdBlockWindow::StartScan() {
    if (scanRunning_) {
        if (scanProgressDialog_) scanProgressDialog_->Show();
        return;
    }
    if (busy_) return;
    const std::wstring path = Trim(GetText(pathEdit_));
    if (path.empty()) {
        ThemedUi::SetText(statusText_, L"请先选择文件或文件夹。");
        return;
    }
    busy_ = true;
    scanRunning_ = true;
    scanItems_.clear();
    lastScanStatus_.clear();
    ThemedUi::ClearTable(scanTable_);
    ThemedUi::SetText(statusText_, L"正在后台递归检查目录…");
    AdBlockScanOptions scanOptions{};
    wchar_t delayText[32]{};
    if (GetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_BATCH_DELAY_MS", delayText,
            static_cast<DWORD>(std::size(delayText))) > 0) {
        scanOptions.batchDelay = std::chrono::milliseconds(
            std::min<unsigned long>(wcstoul(delayText, nullptr, 10), 1000));
    }
    const HWND target = hwnd_;
    scanTask_ = AdBlockManager().StartScanPathDetailed(
        path,
        scanOptions,
        [target]() { PostMessageW(target, WM_APP_SCAN_COMPLETE, 0, 0); });
    ThemedTaskProgressDialogOptions progressOptions{};
    progressOptions.owner = hwnd_;
    progressOptions.instance = instance_;
    progressOptions.theme = theme_;
    progressOptions.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    progressOptions.className = L"AppLaunchLockerAdBlockProgress_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    progressOptions.title = L"广告拦截检查进度";
    progressOptions.clientWidth = 520;
    progressOptions.initialStatus = L"正在递归检查可启动程序";
    progressOptions.initialDetail = L"最多使用 8 个工作线程，并分析可启动程序。";
    progressOptions.readSnapshot = [task = scanTask_]() {
        return ToThemedTaskProgressSnapshot(task->Snapshot());
    };
    progressOptions.requestStop = [task = scanTask_]() { task->RequestStop(); };
    scanProgressDialog_ = std::make_unique<ThemedTaskProgressDialog>(std::move(progressOptions));
    scanProgressDialog_->Show();
    UpdateButtons();
}

void AdBlockWindow::ClearScanResults() {
    if (busy_ || activeTab_ != 0) return;
    scanItems_.clear();
    lastScanStatus_.clear();
    ThemedUi::ClearTable(scanTable_);
    ThemedUi::SetText(pathEdit_, L"");
    ThemedUi::SetText(statusText_, L"输入或选择文件、文件夹后点击“检查”。");
    UpdateButtons();
}

void AdBlockWindow::StartBlockSelected() {
    if (busy_ || activeTab_ != 0) return;
    const std::wstring mode = SelectedMode();
    std::vector<std::wstring> targets;
    for (int index = 0; index < static_cast<int>(scanItems_.size()); ++index) {
        if (!ThemedUi::IsTableChecked(scanTable_, index)) continue;
        const StartupItem& item = scanItems_[static_cast<std::size_t>(index)];
        if (!item.canDisable) continue;
        targets.push_back(MapField(item.original, L"targetPath"));
    }
    if (targets.empty()) {
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, L"请勾选至少一个可拦截的程序。", L"广告拦截",
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    const AdBlockPlan plan = AdBlockManager().BuildBlockPlan(targets, mode);
    std::vector<std::wstring> runnableTargets;
    for (const AdBlockPlanItem& item : plan.items) {
        if (item.riskLevel != L"blocked") runnableTargets.push_back(item.targetPath);
    }
    if (runnableTargets.empty()) {
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_,
            PlanConfirmationPrompt(plan), L"广告拦截", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring prompt = PlanConfirmationPrompt(plan);
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, prompt, L"确认拦截",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;

    const bool elevate = !RunningAsAdmin();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在拦截…");
    UpdateButtons();
    StartOperationTask([targets = std::move(runnableTargets), mode, elevate]() {
        if (elevate) {
            return RunElevatedRequest(L"adblock-block", targets, mode);
        }
        int ok = 0;
        int fail = 0;
        std::wstring lastError;
        for (const std::wstring& path : targets) {
            OperationResult result;
            result = AdBlockManager().Block(path, mode);
            if (result.success) ++ok; else { ++fail; lastError = result.message; }
        }
        if (fail == 0) return OperationResult{true, L"已拦截 " + std::to_wstring(ok) + L" 个程序。"};
        return OperationResult{ok > 0, L"已拦截 " + std::to_wstring(ok) + L" 个，" + std::to_wstring(fail) +
            L" 个失败：" + lastError, ok > 0};
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
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在解除…");
    UpdateButtons();
    StartOperationTask([recordId, elevate]() {
        return elevate ? RunElevatedRequest(L"adblock-unblock", {recordId}) : AdBlockManager().Unblock(recordId);
    });
}

void AdBlockWindow::StartUnblockAll() {
    if (busy_ || activeTab_ != 1 || blocked_.empty()) return;
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_,
            L"确定解除全部广告拦截记录？", L"全部解除", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const bool elevate = !RunningAsAdmin();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在解除全部拦截…");
    UpdateButtons();
    StartOperationTask([elevate]() {
        return elevate ? RunElevatedRequest(L"adblock-unblock-all", std::vector<std::wstring>{})
            : AdBlockManager().UnblockAll();
    });
}

void AdBlockWindow::StartCleanStale() {
    if (busy_ || activeTab_ != 1 || blocked_.empty()) return;
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在清理失效记录…");
    UpdateButtons();
    StartOperationTask([]() { return AdBlockManager().CleanStaleRecords(); });
}

void AdBlockWindow::StartRepairSelected() {
    if (busy_ || activeTab_ != 1) return;
    const int row = ThemedUi::TableSelectedIndex(blockedTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= blocked_.size()) return;
    const DisabledRecord& record = blocked_[static_cast<std::size_t>(row)];
    const AdBlockRecordStatus status = AdBlockManager().CheckRecordStatus(record);
    if (!status.canRepair) return;
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_,
            L"确定修复“" + record.name + L"”的拦截状态？\n" + status.message,
            L"修复拦截", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const std::wstring recordId = record.recordId;
    const bool elevate = !RunningAsAdmin();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在修复拦截…");
    UpdateButtons();
    StartOperationTask([recordId, elevate]() {
        return elevate ? RunElevatedRequest(L"adblock-repair", {recordId}) : AdBlockManager().RepairRecord(recordId);
    });
}

void AdBlockWindow::ShowSelectedDetails() {
    if (busy_) return;
    if (activeTab_ == 0) {
        const int row = ThemedUi::TableSelectedIndex(scanTable_);
        if (row < 0 || static_cast<std::size_t>(row) >= scanItems_.size()) return;
        const StartupItem& item = scanItems_[static_cast<std::size_t>(row)];
        const std::wstring path = MapField(item.original, L"targetPath");
        const std::wstring mode = SelectedMode();
        const AdBlockPlan plan = AdBlockManager().BuildBlockPlan({path.empty() ? item.location : path}, mode);
        std::wstring message = L"名称：" + item.name +
            L"\n路径：" + (path.empty() ? item.location : path) +
            L"\n状态：" + ScanStatusText(item) +
            L"\n影响范围：" + ScanImpactText(item, mode);
        if (!plan.items.empty()) {
            const AdBlockPlanItem& planned = plan.items.front();
            if (!planned.reason.empty()) message += L"\n提示：" + planned.reason;
            if (planned.hasExistingIfeoDebugger) message += L"\nIFEO：已有 Debugger，将备份后再写入。";
            if (planned.willModifyIfeo) message += L"\n机制：IFEO Debugger。";
        }
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, message, L"候选详情", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int row = ThemedUi::TableSelectedIndex(blockedTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= blocked_.size()) return;
    const DisabledRecord& record = blocked_[static_cast<std::size_t>(row)];
    const AdBlockRecordStatus status = AdBlockManager().CheckRecordStatus(record);
    const std::wstring blockMode = MapField(record.original, L"blockMode");
    const std::wstring mode = blockMode == L"name" ? L"同名程序"
        : blockMode == L"startup" ? L"已移除的禁止自启" : L"精确路径";
    std::wstring message = L"名称：" + record.name +
        L"\n模式：" + mode +
        L"\n状态：" + AdBlockRecordStateText(status.state) +
        L"\n说明：" + status.message +
        L"\n路径：" + MapField(record.original, L"targetPath") +
        L"\n记录 ID：" + record.recordId;
    const std::wstring mechanism = MapField(record.original, L"mechanism");
    if (mechanism == L"ifeo") {
        message += L"\n机制：IFEO Debugger";
        message += L"\n映像名：" + MapField(record.original, L"ifeoImageName");
        message += L"\n注册表视图：" + MapField(record.original, L"ifeoView");
    } else if (mechanism == L"startup-approved") {
        message += L"\n机制：StartupApproved 系统开关（历史记录）";
    }
    ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, message, L"拦截详情", MB_OK | MB_ICONINFORMATION);
}

void AdBlockWindow::CompleteScan(AdBlockScanResult scan) {
    busy_ = false;
    scanRunning_ = false;
    scanItems_ = std::move(scan.scan.items);
    RebuildScanRows();
    const int blockable = static_cast<int>(std::count_if(scanItems_.begin(), scanItems_.end(),
        [](const StartupItem& item) { return item.canDisable; }));
    std::wstring status;
    if (!scan.error.empty()) {
        status = scan.error;
    } else if (scan.cancelled) {
        status = L"检查已停止：已检查 " + std::to_wstring(scan.checkedCandidates) + L" / " +
            std::to_wstring(scan.totalCandidates) + L" 个候选，当前发现 " +
            std::to_wstring(scanItems_.size()) + L" 个可启动文件，结果可能不完整。";
    } else {
        status = L"检查完成：枚举 " + std::to_wstring(scan.enumeratedFiles) + L" 个文件，发现 " +
            std::to_wstring(scanItems_.size()) + L" 个可启动程序，" +
            std::to_wstring(blockable) + L" 个可拦截";
        if (scan.workerCount > 0) status += L"，使用 " + std::to_wstring(scan.workerCount) + L" 个工作线程";
        status += L"。";
    }
    if (!scan.scan.warnings.empty()) {
        for (const auto& warning : scan.scan.warnings) AppendAppLaunchLockerLog(L"广告拦截扫描警告：" + warning);
        status += L" 部分项目未能读取。";
    }
    lastScanStatus_ = status;
    ThemedUi::SetText(statusText_, status);
    UpdateButtons();
    if (QuattroTestMode() && !testConfirmationShown_) {
        PostMessageW(hwnd_, WM_APP_TEST_SHOW_CONFIRMATION, 0, 0);
    }
}

void AdBlockWindow::CompleteBlocked(std::vector<DisabledRecord> blocked, std::wstring storeError) {
    busy_ = false;
    storeAvailable_ = storeError.empty();
    blocked_ = std::move(blocked);
    RebuildBlockedRows();
    if (!storeError.empty()) {
        AppendAppLaunchLockerLog(storeError);
        if (activeTab_ == 1) {
            ThemedUi::SetText(statusText_, storeError);
        }
    } else {
        if (activeTab_ == 1) {
            ThemedUi::SetText(statusText_, BlockedSummaryText());
        }
    }
    UpdateButtons();
}

void AdBlockWindow::CompleteOperation(OperationResult result) {
    busy_ = false;
    if (!result.success) {
        AppendAppLaunchLockerLog(L"广告拦截操作失败：" + result.message);
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, result.message, L"广告拦截", MB_OK | MB_ICONWARNING);
    } else if (windowUi_) {
        ThemedToastOptions toast{};
        toast.role = OperationToastRole(result.success, result.partial);
        if (result.partial) toast.durationMs = 5000;
        windowUi_->ui().ShowToast(result.message.empty() ? L"操作完成。" : result.message, toast);
        if (activeTab_ == 0 && !lastScanStatus_.empty()) {
            ThemedUi::SetText(statusText_, lastScanStatus_);
        }
    }
    // 操作完成后只刷新“已拦截”数据。目录检查只能由用户点击“检查”或在路径框按 Enter 触发。
    LoadBlockedAsync();
}

void AdBlockWindow::LoadBlockedAsync() {
    if (blockedTask_) {
        blockedTask_->RequestStop();
        blockedTask_.reset();
    }
    const HWND target = hwnd_;
    TaskOptions options{};
    options.mode = TaskExecutionMode::BackgroundSingle;
    options.maxWorkers = 1;
    options.completionCallback = [target]() { PostMessageW(target, WM_APP_BLOCKED_COMPLETE, 0, 0); };
    blockedTask_ = TaskExecutionService::StartTyped<BlockedPayload>(
        std::move(options),
        [](TaskContext&) {
            BlockedPayload payload;
            AdBlockManager().ListBlocked(payload.blocked, payload.storeError);
            return payload;
        });
}

void AdBlockWindow::SelectTab(int index) {
    if (index < 0 || index > 1) return;
    activeTab_ = index;
    ThemedUi::SetActiveTab(tabControl_, index, false);
    if (index == 1) {
        ThemedUi::SetText(statusText_, blockedTask_ ? L"正在加载已拦截记录…" : BlockedSummaryText());
        LoadBlockedAsync();
    } else if (!scanRunning_) {
        ThemedUi::SetText(
            statusText_,
            !lastScanStatus_.empty()
                ? lastScanStatus_
                : scanItems_.empty()
                ? std::wstring(L"输入或选择文件、文件夹后点击“检查”。")
                : L"检查完成：当前列表 " + std::to_wstring(scanItems_.size()) + L" 个可启动程序。");
    }
    UpdateButtons();
}

void AdBlockWindow::RebuildScanRows() {
    const int previousIndex = ThemedUi::TableSelectedIndex(scanTable_);
    const std::intptr_t previousKey = previousIndex >= 0 ? ThemedUi::TableRowKey(scanTable_, previousIndex) : 0;
    const std::intptr_t previousTopKey = ThemedUi::TableTopVisibleRowKey(scanTable_);
    std::vector<ThemedTableRow> rows;
    rows.reserve(scanItems_.size());
    const std::wstring mode = SelectedMode();
    for (std::size_t index = 0; index < scanItems_.size(); ++index) {
        const StartupItem& item = scanItems_[index];
        const std::wstring path = MapField(item.original, L"targetPath");
        rows.push_back({RowKeyForIdentity(L"scan:" + item.id),
            {{item.name}, {path.empty() ? item.location : path}, {ScanImpactText(item, mode)}, {ScanStatusText(item)}},
            false, item.canDisable});
    }
    ThemedUi::SetTableRows(scanTable_, rows);
    if (previousKey != 0) {
        const int restored = ThemedUi::FindTableRowByKey(scanTable_, previousKey);
        if (restored >= 0) ThemedUi::SetTableSelectedIndex(scanTable_, restored);
    }
    ThemedUi::RestoreTableTopVisibleRowByKey(scanTable_, previousTopKey);
}

void AdBlockWindow::RebuildBlockedRows() {
    const int previousIndex = ThemedUi::TableSelectedIndex(blockedTable_);
    const std::intptr_t previousKey = previousIndex >= 0 ? ThemedUi::TableRowKey(blockedTable_, previousIndex) : 0;
    const std::intptr_t previousTopKey = ThemedUi::TableTopVisibleRowKey(blockedTable_);
    std::vector<ThemedTableRow> rows;
    rows.reserve(blocked_.size());
    AdBlockManager manager;
    for (std::size_t index = 0; index < blocked_.size(); ++index) {
        const DisabledRecord& record = blocked_[index];
        const std::wstring blockMode = MapField(record.original, L"blockMode");
        const std::wstring mode = blockMode == L"name" ? L"同名程序"
            : blockMode == L"startup" ? L"已移除的禁止自启" : L"精确路径";
        const AdBlockRecordStatus status = manager.CheckRecordStatus(record);
        std::wstring when = record.disabledAt;
        if (when.size() >= 10) when = when.substr(0, 10);
        rows.push_back({RowKeyForIdentity(L"blocked:" + record.recordId),
            {{record.name}, {mode}, {AdBlockRecordStateText(status.state)}, {MapField(record.original, L"targetPath")}, {when}},
            false, true});
    }
    ThemedUi::SetTableRows(blockedTable_, rows);
    if (previousKey != 0) {
        const int restored = ThemedUi::FindTableRowByKey(blockedTable_, previousKey);
        if (restored >= 0) ThemedUi::SetTableSelectedIndex(blockedTable_, restored);
    }
    ThemedUi::RestoreTableTopVisibleRowByKey(blockedTable_, previousTopKey);
}

std::wstring AdBlockWindow::BlockedSummaryText() const {
    int attention = 0;
    AdBlockManager manager;
    for (const DisabledRecord& record : blocked_) {
        const AdBlockRecordStatus status = manager.CheckRecordStatus(record);
        if (status.state != AdBlockRecordState::Active) ++attention;
    }
    std::wstring text = L"已拦截 " + std::to_wstring(blocked_.size()) + L" 个程序";
    if (attention > 0) text += L"，" + std::to_wstring(attention) + L" 条需要处理";
    text += L"。";
    return text;
}

std::intptr_t AdBlockWindow::RowKeyForIdentity(const std::wstring& identity) {
    const auto found = stableRowKeys_.find(identity);
    if (found != stableRowKeys_.end()) return found->second;
    const std::intptr_t key = nextStableRowKey_++;
    stableRowKeys_.emplace(identity, key);
    return key;
}

void AdBlockWindow::UpdateButtons() {
    if (!windowUi_) return;
    const ThemedUi ui = windowUi_->ui();
    const bool blockTab = activeTab_ == 0;
    ThemedUi::SetVisible(detailsButton_, true);
    ThemedUi::SetVisible(repairButton_, !blockTab);
    ThemedUi::SetVisible(blockButton_, blockTab);
    ThemedUi::SetVisible(refreshBlockedButton_, !blockTab);
    ThemedUi::SetVisible(cleanStaleButton_, !blockTab);
    ThemedUi::SetVisible(unblockAllButton_, !blockTab);
    ThemedUi::SetVisible(unblockButton_, !blockTab);
    ui.SetEnabled(blockButton_, blockTab && !busy_);
    ui.SetEnabled(refreshBlockedButton_, !blockTab && !busy_);
    ui.SetEnabled(cleanStaleButton_, !blockTab && !busy_ && storeAvailable_ && !blocked_.empty());
    ui.SetEnabled(unblockAllButton_, !blockTab && !busy_ && storeAvailable_ && !blocked_.empty());
    ui.SetEnabled(clearButton_, blockTab && !busy_ && (!scanItems_.empty() || !Trim(GetText(pathEdit_)).empty()));
    ui.SetEnabled(pickPathSplit_.primary, blockTab && !busy_);
    ui.SetEnabled(pickPathSplit_.menu, blockTab && !busy_);
    windowUi_->SetEditReadOnly(pathEdit_, busy_);
    ui.SetEnabled(checkButton_, blockTab && (!busy_ || scanRunning_));
    ThemedUi::SetText(checkButton_, scanRunning_ ? L"查看进度" : L"检查");
    ThemedUi::SetTabEnabled(tabControl_, 1, !scanRunning_);
    const int selectedScan = ThemedUi::TableSelectedIndex(scanTable_);
    const int selected = ThemedUi::TableSelectedIndex(blockedTable_);
    bool canRepair = false;
    if (!blockTab && selected >= 0 && static_cast<std::size_t>(selected) < blocked_.size()) {
        canRepair = AdBlockManager().CheckRecordStatus(blocked_[static_cast<std::size_t>(selected)]).canRepair;
    }
    ui.SetEnabled(detailsButton_, !busy_ && ((blockTab && selectedScan >= 0 &&
        static_cast<std::size_t>(selectedScan) < scanItems_.size()) ||
        (!blockTab && selected >= 0 && static_cast<std::size_t>(selected) < blocked_.size())));
    ui.SetEnabled(repairButton_, !blockTab && !busy_ && storeAvailable_ && canRepair);
    ui.SetEnabled(unblockButton_, !blockTab && !busy_ && storeAvailable_ &&
        selected >= 0 && static_cast<std::size_t>(selected) < blocked_.size());
}
