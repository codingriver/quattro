#include "AppLaunchLockerWindow.h"

#include "IconResolverService.h"
#include "TaskExecutionService.h"
#include "ThemedTaskProgressDialog.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <optional>
#include <string>
#include <utility>

namespace {
constexpr int ID_REFRESH = 1011;
constexpr int ID_CURRENT_TABLE = 1012;
constexpr int ID_CURRENT_DETAILS = 1013;
constexpr int ID_DISABLE = 1014;
constexpr int ID_ELEVATE_SCAN = 1015;
constexpr int ID_DISABLED_DETAILS = 1021;
constexpr int ID_RESTORE = 1022;
constexpr int ID_TAB_CONTROL = 1030;
constexpr UINT WM_APP_SCAN_COMPLETE = WM_APP + 0x150;
constexpr UINT WM_APP_OPERATION_COMPLETE = WM_APP + 0x151;
constexpr UINT WM_APP_ICONS_COMPLETE = WM_APP + 0x152;

struct OperationPayload {
    OperationResult result;
};

struct AppLaunchLockerIconLoadItem {
    std::intptr_t rowKey = 0;
    IconRequest request;
};

struct AppLaunchLockerIconResult {
    std::intptr_t rowKey = 0;
    ResolvedIcon icon;
};

struct AppLaunchLockerIconLoadResult {
    std::uint64_t generation = 0;
    std::vector<AppLaunchLockerIconResult> icons;
};

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
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

std::filesystem::path WindowStatePath() {
    return QuattroUserConfigDirectory() / L"app-launch-locker-window.ini";
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

bool OpenElevatedGui(HWND owner) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return false;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

std::wstring DisplayTimestamp(std::wstring value) {
    if (value.size() >= 10) return value.substr(0, 10);
    return value;
}

std::wstring MapField(const std::map<std::wstring, std::wstring>& values, const wchar_t* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::wstring{} : found->second;
}

IconRequest IconRequestFromFields(
    const StartupSourceType source,
    const std::wstring& command,
    const std::map<std::wstring, std::wstring>& original,
    const int size) {
    IconRequest request;
    request.size = size;
    request.allowFallback = true;
    request.stockIcon = source == StartupSourceType::Driver ? SIID_SHIELD : SIID_APPLICATION;

    const std::wstring originalPath = MapField(original, L"originalPath");
    if (!originalPath.empty()) {
        request.kind = IconSourceKind::FilePath;
        request.value = originalPath;
        return request;
    }

    const std::wstring targetPath = MapField(original, L"targetPath");
    if (!targetPath.empty()) {
        request.kind = IconSourceKind::FilePath;
        request.value = targetPath;
        return request;
    }

    const std::wstring valueData = MapField(original, L"valueData");
    if (!valueData.empty()) {
        request.kind = IconSourceKind::CommandLine;
        request.value = valueData;
        return request;
    }

    if (!command.empty()) {
        request.kind = IconSourceKind::CommandLine;
        request.value = command;
        return request;
    }

    request.kind = IconSourceKind::Stock;
    return request;
}

bool IsAdvancedSource(StartupSourceType source) {
    switch (source) {
    case StartupSourceType::WmiSubscription:
    case StartupSourceType::Winlogon:
    case StartupSourceType::WinlogonNotify:
    case StartupSourceType::AppInitDll:
    case StartupSourceType::AppCertDll:
    case StartupSourceType::BootExecute:
    case StartupSourceType::KnownDll:
    case StartupSourceType::ShellExtension:
    case StartupSourceType::Ifeo:
        return true;
    default:
        return false;
    }
}

bool SourceBelongsToTab(StartupSourceType source, AppLaunchLockerWindow::MainTab tab) {
    switch (tab) {
    case AppLaunchLockerWindow::MainTab::StartupItems:
        return source == StartupSourceType::Registry ||
            source == StartupSourceType::StartupFolder ||
            source == StartupSourceType::ActiveSetup;
    case AppLaunchLockerWindow::MainTab::Services:
        return source == StartupSourceType::Service;
    case AppLaunchLockerWindow::MainTab::ScheduledTasks:
        return source == StartupSourceType::ScheduledTask;
    case AppLaunchLockerWindow::MainTab::Drivers:
        return source == StartupSourceType::Driver;
    case AppLaunchLockerWindow::MainTab::Advanced:
        return IsAdvancedSource(source);
    }
    return false;
}

std::wstring EntrySummary(const StartupItem& item) {
    if (item.source == StartupSourceType::Registry) {
        const std::wstring hive = MapField(item.original, L"hive");
        const std::wstring key = MapField(item.original, L"key");
        if (!hive.empty() && !key.empty()) {
            const std::wstring lowerKey = ToLower(key);
            if (lowerKey.find(L"runonce") != std::wstring::npos) return hive + L" RunOnce";
            if (lowerKey.find(L"run") != std::wstring::npos) return hive + L" Run";
            return hive + L" 注册表";
        }
    }
    if (item.source == StartupSourceType::StartupFolder) {
        return item.requiresAdmin ? L"公共启动目录" : L"当前用户启动目录";
    }
    if (item.source == StartupSourceType::ScheduledTask) {
        return L"计划任务";
    }
    if (item.source == StartupSourceType::Service) {
        const std::wstring serviceName = MapField(item.original, L"serviceName");
        return serviceName.empty() ? L"服务" : L"服务：" + serviceName;
    }
    if (item.source == StartupSourceType::Driver) {
        return L"驱动服务";
    }
    if (IsAdvancedSource(item.source)) {
        return StartupSourceText(item.source);
    }
    return StartupSourceText(item.source);
}

std::wstring EntryStateText(const StartupItem& item) {
    if (item.readOnly || !item.canDisable) return L"仅查看";
    if (item.source == StartupSourceType::Service) return L"可管理";
    return L"已启用";
}

std::wstring SnapshotDiffSummary(const StartupSnapshotDiff& diff) {
    std::wostringstream summary;
    bool hasPart = false;
    const auto append = [&](const wchar_t* label, std::size_t count) {
        if (count == 0) return;
        if (hasPart) summary << L" · ";
        summary << label << L" " << count;
        hasPart = true;
    };
    append(L"新增", diff.added.size());
    append(L"移除", diff.removed.size());
    append(L"变化", diff.changed.size());
    append(L"状态变化", diff.stateChanged.size());
    return hasPart ? summary.str() : std::wstring(L"无变化");
}

std::wstring DetailsText(const StartupItem& item) {
    return L"名称：" + item.name +
        L"\n来源：" + StartupSourceText(item.source) +
        L"\n状态：" + (item.canDisable ? std::wstring(L"可禁用") : std::wstring(L"仅查看")) +
        L"\n位置：" + item.location +
        L"\n命令：" + (item.command.empty() ? std::wstring(L"(无)") : item.command);
}

std::wstring DetailsText(const DisabledRecord& record) {
    std::wstring location;
    for (const wchar_t* key : {L"originalPath", L"taskPath", L"serviceName", L"key"}) {
        const auto found = record.original.find(key);
        if (found != record.original.end() && !found->second.empty()) {
            location = found->second;
            break;
        }
    }
    std::wstring command;
    const auto valueData = record.original.find(L"valueData");
    if (valueData != record.original.end()) command = valueData->second;
    return L"名称：" + record.name +
        L"\n来源：" + StartupSourceText(record.source) +
        L"\n状态：已禁用" +
        L"\n禁用时间：" + record.disabledAt +
        L"\n原始位置：" + (location.empty() ? std::wstring(L"(无)") : location) +
        (command.empty() ? L"" : L"\n原始命令：" + command);
}

class DetailsDialog {
public:
    DetailsDialog(HWND owner, HINSTANCE instance, const Theme& theme, std::wstring text)
        : owner_(owner), instance_(instance), theme_(theme), text_(std::move(text)) {}

    void Run() {
        const std::wstring className = L"AppLaunchLockerDetails_" + std::to_wstring(GetTickCount64());
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        auto options = ThemedWindowUi::DialogOptions(instance_, owner_, className.c_str(), L"项目详情", Proc, this, icon, icon);
        options.clientWidth = kThemedDetailsClientWidth;
        options.clientHeight = kThemedDetailsClientHeight;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) return;
        windowUi_->ShowModal();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        DetailsDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<DetailsDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<DetailsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT result = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
            if (message == WM_DESTROY) done_ = true;
            return result;
        }
        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact,
                kThemedDetailsClientWidth, kThemedDetailsClientHeight);
            const ThemedUi ui = windowUi_->ui();
            RECT content = ui.contentRect();
            const int footerHeight = ui.footerButtonHeight();
            RECT frame{content.left, content.top, content.right,
                ui.footerButtonY(footerHeight) - ui.layout().footerGap};
            ThemedFramedTextOptions detailsOptions{};
            detailsOptions.align = ThemedTextAlign::Start;
            detailsOptions.wrap = true;
            detailsOptions.multiline = true;
            ui.FramedStatic(text_, frame, detailsOptions);
            ui.FooterButton(IDOK, L"关闭", 0, 1, true, true);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) DestroyWindow(hwnd_);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::wstring text_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool done_ = false;
};
}

AppLaunchLockerWindow::AppLaunchLockerWindow(HINSTANCE instance, Theme theme)
    : instance_(instance), theme_(std::move(theme)) {}

AppLaunchLockerWindow::~AppLaunchLockerWindow() {
    closing_ = true;
    if (scanTask_) scanTask_->RequestStop();
    StopIconLoadTask();
    if (scanProgressDialog_) scanProgressDialog_->Close();
    JoinWorker();
    DestroyItemImages();
}

int AppLaunchLockerWindow::Run() {
    HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    auto options = ThemedWindowUi::DialogOptions(instance_, nullptr, L"AppLaunchLockerMainWindow", L"自启动管理", Proc, this, icon, icon);
    options.clientWidth = 860;
    options.clientHeight = 580;
    std::wstring error;
    hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
    if (!hwnd_) {
        ThemedWindowUi::ShowMessageBox(nullptr, instance_, theme_, error, L"自启动管理", MB_OK | MB_ICONERROR);
        return 1;
    }
    wchar_t acceptanceDpiText[16]{};
    if (GetEnvironmentVariableW(L"QUATTRO_APP_LAUNCH_LOCKER_ACCEPTANCE_DPI", acceptanceDpiText,
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
    StartScan();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK AppLaunchLockerWindow::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    AppLaunchLockerWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<AppLaunchLockerWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<AppLaunchLockerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return window ? window->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AppLaunchLockerWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT result = 0;
    if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
        if (message == WM_DESTROY) {
            SaveWindowPosition(hwnd_);
            closing_ = true;
            if (scanTask_) scanTask_->RequestStop();
            StopIconLoadTask();
            if (scanProgressDialog_) scanProgressDialog_->Close();
            JoinWorker();
            DestroyItemImages();
            PostQuitMessage(0);
        }
        return result;
    }
    switch (message) {
    case WM_CREATE:
        windowUi_ = std::make_unique<ThemedWindowUi>(instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact,
            860, 580);
        CreateControls();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        windowUi_->FillBackground(dc);
        windowUi_->DrawRegisteredTableFrames(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == ID_TAB_CONTROL && HIWORD(wParam) == CBN_SELCHANGE) SelectTab(ThemedUi::ActiveTab(tabControl_));
        else if (id == ID_REFRESH) StartScan();
        else if (id == ID_DISABLE) StartDisable();
        else if (id == ID_RESTORE) StartRestore();
        else if (id == ID_CURRENT_DETAILS || id == ID_DISABLED_DETAILS) ShowSelectedDetails();
        else if (id == ID_ELEVATE_SCAN && OpenElevatedGui(hwnd_)) DestroyWindow(hwnd_);
        else if (id == IDCANCEL) DestroyWindow(hwnd_);
        return 0;
    }
    case WM_NOTIFY: {
        ThemedTableEvent event{};
        if (ThemedUi::DecodeTableEvent(itemTable_, lParam, event)) {
            if (event.kind == ThemedTableEventKind::Activated) ShowSelectedDetails();
            UpdateButtons();
            return 0;
        }
        break;
    }
    case WM_APP_SCAN_COMPLETE: {
        if (!scanTask_ || !scanTask_->IsFinished()) return 0;
        scanTask_->Wait();
        ScanResult scan;
        if (scanTask_->Status() == ScanTaskStatus::Failed) {
            scan.warnings.push_back(scanTask_->Snapshot().error);
        } else {
            scan = scanTask_->ResultCopy<ScanResult>();
        }
        scanTask_.reset();
        if (scanProgressDialog_) scanProgressDialog_->Close();
            std::vector<DisabledRecord> disabled;
            std::wstring storeError;
            StartupManager().LoadDisabled(disabled, storeError);
            CompleteScan(std::move(scan), std::move(disabled), std::move(storeError));
        return 0;
    }
    case WM_APP_ICONS_COMPLETE:
        ApplyIconLoadResult(static_cast<std::uint64_t>(wParam));
        return 0;
    case WM_APP_OPERATION_COMPLETE: {
        std::unique_ptr<OperationPayload> payload(reinterpret_cast<OperationPayload*>(lParam));
        JoinWorker();
        CompleteOperation(std::move(payload->result));
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

void AppLaunchLockerWindow::CreateControls() {
    const ThemedUi ui = windowUi_->ui();
    const RECT content = ui.contentRect();
    const int scanWidth = ui.buttonWidth(L"扫描", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    const int headerY = content.top;
    const int tabTop = ui.nextRowY(headerY, ui.buttonHeight(ThemedButtonRole::Normal, ThemedButtonSize::Normal));
    const int tabHeight = ui.tabButtonHeight() + ui.layout().rowGap;
    const int listTop = tabTop + tabHeight + ui.layout().rowGap;
    const int footerY = ui.footerButtonY(ui.footerButtonHeight());
    const int statusY = footerY - ui.layout().sectionGap - ui.labelHeight();
    const int tableBottom = statusY - ui.layout().rowGap;
    const int elevateWidth = ui.textWidth(L"以管理员身份重新打开");

    ui.Label(L"AppLaunchLocker 自启动管理", content.left, headerY, content.right - content.left - scanWidth - ui.layout().controlGapX);
    ui.Button(ID_REFRESH, L"扫描", content.right - scanWidth, headerY,
        ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    ThemedTabControlOptions tabOptions{};
    tabOptions.activeIndex = 0;
    tabOptions.appearance = ThemedTabControlAppearance::ConnectedTabs;
    tabOptions.orientation = ThemedTabControlOrientation::Horizontal;
    tabOptions.containerStyle = ThemedTabControlContainerStyle::Borderless;
    tabControl_ = ui.TabControl(ID_TAB_CONTROL, RECT{content.left, tabTop, content.right, tabTop + tabHeight},
        {{100, L"自启动项", true},
         {101, L"服务", true},
         {102, L"计划任务", true},
         {103, L"驱动", true},
         {104, L"系统高级项", true}},
        tabOptions);
    ThemedTableOptions gridTableOptions{};
    gridTableOptions.allowColumnResize = true;
    gridTableOptions.showRowGridLines = true;
    gridTableOptions.showColumnGridLines = true;
    itemTable_ = ui.Table(ID_CURRENT_TABLE, RECT{content.left, listTop, content.right, tableBottom},
        {{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
         {L"source", L"来源", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"WMI 永久订阅")},
         {L"entry", L"入口", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"公共启动目录")},
         {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"外部状态变化")},
         {L"details", L"详情", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"详情")},
         {L"operation", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"改手动")}},
        gridTableOptions);
    statusText_ = ui.StatusText(L"正在扫描…", content.left, statusY,
        content.right - content.left - elevateWidth - ui.layout().controlGapX,
        {ThemedStatusRole::Info, ThemedTextAlign::Start});
    elevateLink_ = ui.LinkText(ID_ELEVATE_SCAN, L"以管理员身份重新打开", content.right - elevateWidth, statusY, elevateWidth,
        {ThemedLinkRole::Normal, ThemedTextAlign::End, true, false});
    ThemedUi::SetVisible(elevateLink_, false);
    detailsButton_ = ui.FooterButton(ID_CURRENT_DETAILS, L"详情", 0, 2);
    disableButton_ = ui.FooterButton(ID_DISABLE, L"禁用", 1, 2, true, true);
    restoreButton_ = ui.FooterButton(ID_RESTORE, L"恢复", 1, 2, true, true);
    ThemedUi::SetVisible(restoreButton_, false);
    RebuildTabs();
    RebuildRows();
    UpdateButtons();
}

void AppLaunchLockerWindow::JoinWorker() {
    if (worker_.joinable()) worker_.join();
}

void AppLaunchLockerWindow::StartScan() {
    if (busy_) return;
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在扫描…");
    UpdateButtons();
    const HWND target = hwnd_;
    scanTask_ = StartupManager().StartScanAll(
        [target]() { PostMessageW(target, WM_APP_SCAN_COMPLETE, 0, 0); });
    ThemedTaskProgressDialogOptions progressOptions{};
    progressOptions.owner = hwnd_;
    progressOptions.instance = instance_;
    progressOptions.theme = theme_;
    progressOptions.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    progressOptions.className = L"AppLaunchLockerScanProgress_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    progressOptions.title = L"启动项扫描进度";
    progressOptions.readSnapshot = [task = scanTask_]() {
        return ToThemedTaskProgressSnapshot(task->Snapshot());
    };
    progressOptions.requestStop = [task = scanTask_]() { task->RequestStop(); };
    scanProgressDialog_ = std::make_unique<ThemedTaskProgressDialog>(std::move(progressOptions));
    scanProgressDialog_->Show();
}

void AppLaunchLockerWindow::StartDisable() {
    if (busy_ || !storeAvailable_) return;
    const StartupItem* selected = SelectedStartupItem();
    if (!selected || !selected->canDisable) return;
    std::wstring prompt = L"确定禁用“" + selected->name + L"”？\n\n禁用后它将不再随系统启动，仍可手动运行。";
    if (selected->source == StartupSourceType::Service) {
        prompt = L"确定将“" + selected->name + L"”改为手动启动？\n\n不会停止当前正在运行的服务。";
    }
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, prompt, L"禁用自启动",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const std::wstring itemId = selected->id;
    const bool elevate = selected->requiresAdmin && !RunningAsAdmin();
    JoinWorker();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在禁用…");
    UpdateButtons();
    const HWND target = hwnd_;
    worker_ = std::thread([target, itemId, elevate]() {
        auto payload = std::make_unique<OperationPayload>();
        if (elevate) payload->result = RunElevated(L"disable --id " + QuoteArgument(itemId));
        else payload->result = StartupManager().Disable(itemId);
        if (!PostMessageW(target, WM_APP_OPERATION_COMPLETE, 0, reinterpret_cast<LPARAM>(payload.get()))) return;
        payload.release();
    });
}

void AppLaunchLockerWindow::StartRestore() {
    if (busy_ || !storeAvailable_) return;
    const DisabledRecord* selected = SelectedDisabledRecord();
    if (!selected) return;
    if (ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, L"确定恢复“" + selected->name + L"”？", L"恢复自启动",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const std::wstring recordId = selected->recordId;
    const bool elevate = selected->requiresAdmin && !RunningAsAdmin();
    JoinWorker();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在恢复…");
    UpdateButtons();
    const HWND target = hwnd_;
    worker_ = std::thread([target, recordId, elevate]() {
        auto payload = std::make_unique<OperationPayload>();
        if (elevate) payload->result = RunElevated(L"restore --record-id " + QuoteArgument(recordId));
        else payload->result = StartupManager().Restore(recordId);
        if (!PostMessageW(target, WM_APP_OPERATION_COMPLETE, 0, reinterpret_cast<LPARAM>(payload.get()))) return;
        payload.release();
    });
}

void AppLaunchLockerWindow::CompleteScan(ScanResult result, std::vector<DisabledRecord> disabled, std::wstring storeError) {
    busy_ = false;
    storeAvailable_ = storeError.empty();
    const StartupSnapshot currentSnapshot = BuildStartupSnapshot(result);
    const bool completeScan = storeError.empty() && result.warnings.empty();
    items_ = std::move(result.items);
    disabled_ = std::move(disabled);
    RebuildTabs();
    RebuildRows();
    if (!storeError.empty()) {
        AppendAppLaunchLockerLog(storeError);
        ThemedUi::SetText(statusText_, storeError);
        windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Danger);
    }
    if (!result.warnings.empty()) {
        for (const auto& warning : result.warnings) AppendAppLaunchLockerLog(L"扫描警告：" + warning);
        const std::wstring title = activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())
            ? tabs_[static_cast<std::size_t>(activeTab_)].title
            : std::wstring(L"自启动项");
        const std::wstring status = L"当前页：" + title +
            L"，共 " + std::to_wstring(visibleItemIndexes_.size() + visibleDisabledIndexes_.size()) + L" 项；部分系统项目未能读取。";
        ThemedUi::SetText(statusText_, status);
        windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Warning);
    }
    if (completeScan) {
        StartupSnapshot previousSnapshot;
        std::wstring snapshotError;
        StartupSnapshotStore snapshotStore;
        if (!snapshotStore.Load(previousSnapshot, snapshotError)) {
            AppendAppLaunchLockerLog(snapshotError);
            ThemedUi::SetText(statusText_, L"扫描完成，但无法读取上次快照。");
            windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Warning);
        } else {
            const StartupSnapshotDiff diff = DiffStartupSnapshots(previousSnapshot, currentSnapshot);
            if (!snapshotStore.Save(currentSnapshot, snapshotError)) {
                AppendAppLaunchLockerLog(snapshotError);
                ThemedUi::SetText(statusText_, L"扫描完成，但无法保存本次快照。");
                windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Warning);
            } else {
                const std::wstring title = activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())
                    ? tabs_[static_cast<std::size_t>(activeTab_)].title
                    : std::wstring(L"自启动项");
                ThemedUi::SetText(statusText_, L"当前页：" + title +
                    L"，共 " + std::to_wstring(visibleItemIndexes_.size() + visibleDisabledIndexes_.size()) +
                    L" 项；" + SnapshotDiffSummary(diff));
                windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Normal);
            }
        }
    } else if (!result.warnings.empty()) {
        AppendAppLaunchLockerLog(L"扫描不完整，未覆盖启动项快照。");
    }
    showElevateLink_ = !result.warnings.empty() && !RunningAsAdmin();
    UpdateButtons();
}

void AppLaunchLockerWindow::CompleteOperation(OperationResult result) {
    busy_ = false;
    if (!result.success) {
        AppendAppLaunchLockerLog(L"操作失败：" + result.message);
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, result.message, L"自启动管理", MB_OK | MB_ICONWARNING);
    }
    else if (windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Success;
        windowUi_->ui().ShowToast(result.message.empty() ? L"操作完成。" : result.message, toast);
    }
    StartScan();
}

void AppLaunchLockerWindow::RebuildTabs() {
    const auto countFor = [&](MainTab tab) {
        const int live = static_cast<int>(std::count_if(items_.begin(), items_.end(), [&](const StartupItem& item) {
            return SourceBelongsToTab(item.source, tab);
        }));
        const int disabled = static_cast<int>(std::count_if(disabled_.begin(), disabled_.end(), [&](const DisabledRecord& record) {
            return SourceBelongsToTab(record.source, tab);
        }));
        return live + disabled;
    };

    tabs_ = {
        {MainTab::StartupItems, L"自启动项", countFor(MainTab::StartupItems)},
        {MainTab::Services, L"服务", countFor(MainTab::Services)},
        {MainTab::ScheduledTasks, L"计划任务", countFor(MainTab::ScheduledTasks)},
        {MainTab::Drivers, L"驱动", countFor(MainTab::Drivers)},
        {MainTab::Advanced, L"系统高级项", countFor(MainTab::Advanced)},
    };

    if (activeTab_ < 0 || activeTab_ >= static_cast<int>(tabs_.size())) activeTab_ = 0;
    if (tabControl_) ThemedUi::SetActiveTab(tabControl_, activeTab_, false);
}

void AppLaunchLockerWindow::RebuildRows() {
    StopIconLoadTask();
    visibleItemIndexes_.clear();
    visibleDisabledIndexes_.clear();
    std::vector<ThemedTableRow> rows;
    const TabEntry tab = activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())
        ? tabs_[static_cast<std::size_t>(activeTab_)]
        : TabEntry{MainTab::StartupItems, L"自启动项", 0};

    DestroyItemImages();
    itemIconSize_ = std::max(16, GetSystemMetrics(SM_CXSMICON));
    itemSmallImages_ = ImageList_Create(itemIconSize_, itemIconSize_, ILC_COLOR32 | ILC_MASK,
        std::max(1, static_cast<int>(items_.size() + disabled_.size())), 8);

    for (std::size_t index = 0; index < items_.size(); ++index) {
        const StartupItem& item = items_[index];
        if (!SourceBelongsToTab(item.source, tab.tab)) continue;
        visibleItemIndexes_.push_back(index);
        const std::wstring operation = item.canDisable
            ? (item.source == StartupSourceType::Service ? std::wstring(L"改手动") : std::wstring(L"禁用"))
            : std::wstring(L"—");
        rows.push_back({static_cast<std::intptr_t>(visibleItemIndexes_.size()),
            {{item.name, -1},
             {StartupSourceText(item.source)},
             {EntrySummary(item)},
             {EntryStateText(item)},
             {L"详情"},
             {operation}},
            false, true});
    }
    for (std::size_t index = 0; index < disabled_.size(); ++index) {
        const DisabledRecord& record = disabled_[index];
        if (!SourceBelongsToTab(record.source, tab.tab)) continue;
        visibleDisabledIndexes_.push_back(index);
        rows.push_back({static_cast<std::intptr_t>(100000 + visibleDisabledIndexes_.size()),
            {{record.name, -1},
             {StartupSourceText(record.source)},
             {EntrySummary(StartupItem{record.itemId, record.name, record.source, L"", L"", record.requiresAdmin, false, true, record.original})},
             {L"已禁用"},
             {L"详情"},
             {L"恢复"}},
            false, true});
    }

    ThemedUi::SetTableImageLists(itemTable_, itemSmallImages_, nullptr);
    ThemedUi::SetTableRows(itemTable_, rows);
    const std::wstring status = L"当前页：" + tab.title + L"，共 " + std::to_wstring(rows.size()) + L" 项";
    ThemedUi::SetText(statusText_, status);
    windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Normal);
    UpdateButtons();
    StartIconLoadTask();
}

void AppLaunchLockerWindow::DestroyItemImages() {
    if (itemTable_ && IsWindow(itemTable_)) {
        ThemedUi::SetTableImageLists(itemTable_, nullptr, nullptr);
    }
    if (itemSmallImages_) {
        ImageList_Destroy(itemSmallImages_);
        itemSmallImages_ = nullptr;
    }
}

void AppLaunchLockerWindow::StopIconLoadTask() {
    ++iconGeneration_;
    if (iconTask_) {
        iconTask_->RequestStop();
        if (iconTask_->IsFinished()) {
            iconTask_.reset();
        }
    }
}

void AppLaunchLockerWindow::StartIconLoadTask() {
    if (!hwnd_ || !itemTable_ || !itemSmallImages_) return;

    std::vector<AppLaunchLockerIconLoadItem> iconItems;
    iconItems.reserve(visibleItemIndexes_.size() + visibleDisabledIndexes_.size());
    for (std::size_t row = 0; row < visibleItemIndexes_.size(); ++row) {
        const std::size_t itemIndex = visibleItemIndexes_[row];
        if (itemIndex >= items_.size()) continue;
        const StartupItem& item = items_[itemIndex];
        iconItems.push_back({static_cast<std::intptr_t>(row + 1),
            IconRequestFromFields(item.source, item.command, item.original, itemIconSize_)});
    }
    for (std::size_t row = 0; row < visibleDisabledIndexes_.size(); ++row) {
        const std::size_t recordIndex = visibleDisabledIndexes_[row];
        if (recordIndex >= disabled_.size()) continue;
        const DisabledRecord& record = disabled_[recordIndex];
        iconItems.push_back({static_cast<std::intptr_t>(100000 + row + 1),
            IconRequestFromFields(record.source, std::wstring{}, record.original, itemIconSize_)});
    }
    if (iconItems.empty()) return;

    const std::uint64_t generation = iconGeneration_;
    const HWND target = hwnd_;
    TaskOptions options{};
    options.mode = TaskExecutionMode::BackgroundSingle;
    options.completionCallback = [target, generation] {
        if (IsWindow(target)) {
            PostMessageW(target, WM_APP_ICONS_COMPLETE, static_cast<WPARAM>(generation), 0);
        }
    };
    iconTask_ = TaskExecutionService::StartTyped<AppLaunchLockerIconLoadResult>(
        std::move(options),
        [generation, iconItems = std::move(iconItems)](TaskContext& context) {
            AppLaunchLockerIconLoadResult result;
            result.generation = generation;
            result.icons.reserve(iconItems.size());

            TaskProgressUpdate progress{};
            progress.phase = L"app-launch-locker-icons";
            progress.title = L"自启动图标刷新";
            progress.status = L"正在刷新图标";
            progress.total = iconItems.size();
            progress.current = 0;
            progress.workerCount = 1;
            progress.indeterminate = false;
            context.Report(std::move(progress));

            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            IconResolverService resolver;
            std::uint64_t completed = 0;
            for (const AppLaunchLockerIconLoadItem& item : iconItems) {
                if (context.StopRequested()) break;
                AppLaunchLockerIconResult iconResult;
                iconResult.rowKey = item.rowKey;
                iconResult.icon = resolver.Resolve(item.request, context.StopToken());
                result.icons.push_back(std::move(iconResult));
                ++completed;
                context.UpdateProgress([completed, total = static_cast<std::uint64_t>(iconItems.size())](TaskProgressUpdate& value) {
                    value.current = completed;
                    value.completed = completed;
                    value.total = total;
                    value.status = L"正在刷新图标";
                    value.detail = L"已刷新 " + std::to_wstring(completed) + L" / " + std::to_wstring(total) + L" 个图标";
                    value.indeterminate = false;
                });
            }
            if (SUCCEEDED(comResult)) {
                CoUninitialize();
            }
            return result;
        });
}

void AppLaunchLockerWindow::ApplyIconLoadResult(std::uint64_t generation) {
    if (!iconTask_ || generation != iconGeneration_ || !iconTask_->IsFinished()) return;
    if (iconTask_->Status() != TaskStatus::Completed) {
        iconTask_.reset();
        return;
    }

    AppLaunchLockerIconLoadResult result = iconTask_->ResultCopy<AppLaunchLockerIconLoadResult>();
    iconTask_.reset();
    if (result.generation != iconGeneration_ || !itemTable_ || !itemSmallImages_) return;

    bool updated = false;
    for (const AppLaunchLockerIconResult& icon : result.icons) {
        HBITMAP bitmap = IconResolverService::CreateBitmapFromPixels(
            icon.icon,
            itemIconSize_,
            ThemedUi::ListSurfaceColor(theme_),
            true);
        if (!bitmap) continue;
        const int imageIndex = ImageList_Add(itemSmallImages_, bitmap, nullptr);
        DeleteObject(bitmap);
        if (imageIndex < 0) continue;

        const int rowIndex = ThemedUi::FindTableRowByKey(itemTable_, icon.rowKey);
        if (rowIndex < 0) continue;

        if (icon.rowKey > 0 && icon.rowKey < 100000) {
            const std::size_t visibleIndex = static_cast<std::size_t>(icon.rowKey - 1);
            if (visibleIndex >= visibleItemIndexes_.size()) continue;
            const std::size_t itemIndex = visibleItemIndexes_[visibleIndex];
            if (itemIndex >= items_.size()) continue;
            const StartupItem& item = items_[itemIndex];
            const std::wstring operation = item.canDisable
                ? (item.source == StartupSourceType::Service ? std::wstring(L"改手动") : std::wstring(L"禁用"))
                : std::wstring(L"—");
            ThemedUi::UpdateTableRow(itemTable_, rowIndex, {icon.rowKey,
                {{item.name, imageIndex},
                 {StartupSourceText(item.source)},
                 {EntrySummary(item)},
                 {EntryStateText(item)},
                 {L"详情"},
                 {operation}},
                false, true});
            updated = true;
        } else if (icon.rowKey > 100000) {
            const std::size_t visibleIndex = static_cast<std::size_t>(icon.rowKey - 100001);
            if (visibleIndex >= visibleDisabledIndexes_.size()) continue;
            const std::size_t recordIndex = visibleDisabledIndexes_[visibleIndex];
            if (recordIndex >= disabled_.size()) continue;
            const DisabledRecord& record = disabled_[recordIndex];
            ThemedUi::UpdateTableRow(itemTable_, rowIndex, {icon.rowKey,
                {{record.name, imageIndex},
                 {StartupSourceText(record.source)},
                 {EntrySummary(StartupItem{record.itemId, record.name, record.source, L"", L"", record.requiresAdmin, false, true, record.original})},
                 {L"已禁用"},
                 {L"详情"},
                 {L"恢复"}},
                false, true});
            updated = true;
        }
    }
    if (updated) {
        InvalidateRect(itemTable_, nullptr, FALSE);
    }
}

void AppLaunchLockerWindow::SelectTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size()) || index == activeTab_) return;
    activeTab_ = index;
    ThemedUi::SetActiveTab(tabControl_, activeTab_, false);
    RebuildRows();
}

const StartupItem* AppLaunchLockerWindow::SelectedStartupItem() const {
    const int row = ThemedUi::TableSelectedIndex(itemTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= visibleItemIndexes_.size()) return nullptr;
    const std::size_t index = visibleItemIndexes_[static_cast<std::size_t>(row)];
    return index < items_.size() ? &items_[index] : nullptr;
}

const DisabledRecord* AppLaunchLockerWindow::SelectedDisabledRecord() const {
    const int row = ThemedUi::TableSelectedIndex(itemTable_);
    const std::size_t disabledOffset = visibleItemIndexes_.size();
    if (row < 0 || static_cast<std::size_t>(row) < disabledOffset) return nullptr;
    const std::size_t disabledRow = static_cast<std::size_t>(row) - disabledOffset;
    if (disabledRow >= visibleDisabledIndexes_.size()) return nullptr;
    const std::size_t index = visibleDisabledIndexes_[disabledRow];
    return index < disabled_.size() ? &disabled_[index] : nullptr;
}

void AppLaunchLockerWindow::UpdateButtons() {
    if (!windowUi_) return;
    const ThemedUi ui = windowUi_->ui();
    const StartupItem* item = SelectedStartupItem();
    const DisabledRecord* record = SelectedDisabledRecord();
    ui.SetEnabled(detailsButton_, !busy_ && (item != nullptr || record != nullptr));
    ui.SetEnabled(disableButton_, !busy_ && storeAvailable_ && item && item->canDisable);
    ui.SetEnabled(restoreButton_, !busy_ && storeAvailable_ && record != nullptr);
    ThemedUi::SetVisible(disableButton_, record == nullptr);
    ThemedUi::SetVisible(restoreButton_, record != nullptr);
    ThemedUi::SetVisible(elevateLink_, showElevateLink_);
}

void AppLaunchLockerWindow::ShowSelectedDetails() {
    if (busy_) return;
    if (const StartupItem* item = SelectedStartupItem()) DetailsDialog(hwnd_, instance_, theme_, DetailsText(*item)).Run();
    else if (const DisabledRecord* record = SelectedDisabledRecord()) DetailsDialog(hwnd_, instance_, theme_, DetailsText(*record)).Run();
}
