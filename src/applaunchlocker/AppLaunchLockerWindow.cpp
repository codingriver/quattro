#include "AppLaunchLockerWindow.h"

#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
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
constexpr int ID_CATEGORY_TABLE = 1030;
constexpr UINT WM_APP_SCAN_COMPLETE = WM_APP + 0x150;
constexpr UINT WM_APP_OPERATION_COMPLETE = WM_APP + 0x151;

struct ScanPayload {
    ScanResult scan;
    std::vector<DisabledRecord> disabled;
    std::wstring storeError;
};

struct OperationPayload {
    OperationResult result;
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
    JoinWorker();
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
            closing_ = true;
            JoinWorker();
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
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == ID_REFRESH) StartScan();
        else if (id == ID_DISABLE) StartDisable();
        else if (id == ID_RESTORE) StartRestore();
        else if (id == ID_CURRENT_DETAILS || id == ID_DISABLED_DETAILS) ShowSelectedDetails();
        else if (id == ID_ELEVATE_SCAN && OpenElevatedGui(hwnd_)) DestroyWindow(hwnd_);
        else if (id == IDCANCEL) DestroyWindow(hwnd_);
        return 0;
    }
    case WM_NOTIFY: {
        ThemedTableEvent event{};
        if (ThemedUi::DecodeTableEvent(categoryTable_, lParam, event)) {
            if (event.kind == ThemedTableEventKind::SelectionChanged || event.kind == ThemedTableEventKind::Click) {
                SelectCategory(event.row);
            }
            return 0;
        }
        if (ThemedUi::DecodeTableEvent(itemTable_, lParam, event)) {
            if (event.kind == ThemedTableEventKind::Activated) ShowSelectedDetails();
            UpdateButtons();
            return 0;
        }
        break;
    }
    case WM_APP_SCAN_COMPLETE: {
        std::unique_ptr<ScanPayload> payload(reinterpret_cast<ScanPayload*>(lParam));
        JoinWorker();
        CompleteScan(std::move(payload->scan), std::move(payload->disabled), std::move(payload->storeError));
        return 0;
    }
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
    const int listTop = ui.nextRowY(headerY, ui.buttonHeight(ThemedButtonRole::Normal, ThemedButtonSize::Normal));
    const int footerY = ui.footerButtonY(ui.footerButtonHeight());
    const int statusY = footerY - ui.layout().sectionGap - ui.labelHeight();
    const int tableBottom = statusY - ui.layout().rowGap;
    const int categoryWidth = ui.scale(176);
    const int splitGap = ui.layout().controlGapX;
    const int categoryRight = content.left + categoryWidth;
    const int itemLeft = categoryRight + splitGap;
    const int elevateWidth = ui.textWidth(L"以管理员身份重新打开");

    ui.Label(L"自启动项管理", content.left, headerY, content.right - content.left - scanWidth - ui.layout().controlGapX);
    ui.Button(ID_REFRESH, L"扫描", content.right - scanWidth, headerY,
        ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    categoryTable_ = ui.Table(ID_CATEGORY_TABLE, RECT{content.left, listTop, categoryRight, tableBottom},
        {{L"name", L"范围与分类", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
         {L"count", L"数量", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"9999")}},
        ThemedTableOptions{ThemedTableSelection::Single, ThemedTableView::Details, false, true, true, true, false});
    itemTable_ = ui.Table(ID_CURRENT_TABLE, RECT{itemLeft, listTop, content.right, tableBottom},
        {{L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
         {L"source", L"来源", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"WMI 永久订阅")},
         {L"state", L"状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, ui.tableColumnWidth(L"可禁用")}});
    statusText_ = ui.StatusText(L"正在扫描…", itemLeft, statusY,
        content.right - itemLeft - elevateWidth - ui.layout().controlGapX,
        {ThemedStatusRole::Info, ThemedTextAlign::Start});
    elevateLink_ = ui.LinkText(ID_ELEVATE_SCAN, L"以管理员身份重新打开", content.right - elevateWidth, statusY, elevateWidth,
        {ThemedLinkRole::Normal, ThemedTextAlign::End, true, false});
    ThemedUi::SetVisible(elevateLink_, false);
    detailsButton_ = ui.FooterButton(ID_CURRENT_DETAILS, L"详情", 0, 2);
    disableButton_ = ui.FooterButton(ID_DISABLE, L"禁用", 1, 2, true, true);
    restoreButton_ = ui.FooterButton(ID_RESTORE, L"恢复", 1, 2, true, true);
    ThemedUi::SetVisible(restoreButton_, false);
    RebuildCategories();
    RebuildRows();
    UpdateButtons();
}

void AppLaunchLockerWindow::JoinWorker() {
    if (worker_.joinable()) worker_.join();
}

void AppLaunchLockerWindow::StartScan() {
    if (busy_) return;
    JoinWorker();
    busy_ = true;
    ThemedUi::SetText(statusText_, L"正在扫描…");
    UpdateButtons();
    const HWND target = hwnd_;
    worker_ = std::thread([target]() {
        auto payload = std::make_unique<ScanPayload>();
        StartupManager manager;
        payload->scan = manager.ScanAll();
        manager.LoadDisabled(payload->disabled, payload->storeError);
        if (!PostMessageW(target, WM_APP_SCAN_COMPLETE, 0, reinterpret_cast<LPARAM>(payload.get()))) return;
        payload.release();
    });
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
    items_ = std::move(result.items);
    disabled_ = std::move(disabled);
    RebuildCategories();
    RebuildRows();
    if (!storeError.empty()) {
        AppendAppLaunchLockerLog(storeError);
        ThemedUi::SetText(statusText_, storeError);
        windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Danger);
    }
    if (!result.warnings.empty()) {
        for (const auto& warning : result.warnings) AppendAppLaunchLockerLog(L"扫描警告：" + warning);
        const std::wstring status = L"当前范围：" + (categories_.empty() ? std::wstring(L"当前自启动") : categories_[static_cast<std::size_t>(selectedCategory_)].title) +
            L"，共 " + std::to_wstring(visibleItemIndexes_.size() + visibleDisabledIndexes_.size()) + L" 项；部分系统项目未能读取。";
        ThemedUi::SetText(statusText_, status);
        windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Warning);
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
    else if (!result.message.empty() && result.message != L"操作完成。") {
        ThemedWindowUi::ShowMessageBox(hwnd_, instance_, theme_, result.message, L"自启动管理", MB_OK | MB_ICONINFORMATION);
    }
    else if (windowUi_) {
        ThemedToastOptions toast{};
        toast.role = ThemedToastRole::Success;
        windowUi_->ui().ShowToast(L"操作完成。", toast);
    }
    StartScan();
}

void AppLaunchLockerWindow::RebuildCategories() {
    const StartupSourceType sources[] = {
        StartupSourceType::Registry,
        StartupSourceType::StartupFolder,
        StartupSourceType::ScheduledTask,
        StartupSourceType::Service,
        StartupSourceType::Driver,
        StartupSourceType::WmiSubscription,
        StartupSourceType::Winlogon,
        StartupSourceType::AppInitDll,
        StartupSourceType::Ifeo,
    };

    categories_.clear();
    categories_.push_back(CategoryEntry{CategoryKind::Current, StartupSourceType::Registry, L"当前自启动", static_cast<int>(items_.size())});
    categories_.push_back(CategoryEntry{CategoryKind::Disabled, StartupSourceType::Registry, L"已禁用", static_cast<int>(disabled_.size())});
    for (StartupSourceType source : sources) {
        const int count = static_cast<int>(std::count_if(items_.begin(), items_.end(), [&](const StartupItem& item) {
            return item.source == source;
        }));
        if (count > 0) {
            categories_.push_back(CategoryEntry{CategoryKind::Source, source, StartupSourceText(source), count});
        }
    }

    if (selectedCategory_ < 0 || selectedCategory_ >= static_cast<int>(categories_.size())) {
        selectedCategory_ = 0;
    }

    std::vector<ThemedTableRow> rows;
    rows.reserve(categories_.size());
    for (std::size_t index = 0; index < categories_.size(); ++index) {
        rows.push_back({static_cast<std::intptr_t>(index + 1),
            {{categories_[index].title}, {std::to_wstring(categories_[index].count)}},
            false, true});
    }
    ThemedUi::SetTableRows(categoryTable_, rows);
    ThemedUi::SetTableSelectedIndex(categoryTable_, selectedCategory_);
}

void AppLaunchLockerWindow::RebuildRows() {
    visibleItemIndexes_.clear();
    visibleDisabledIndexes_.clear();
    std::vector<ThemedTableRow> rows;
    CategoryEntry category{};
    if (!categories_.empty() && selectedCategory_ >= 0 && selectedCategory_ < static_cast<int>(categories_.size())) {
        category = categories_[static_cast<std::size_t>(selectedCategory_)];
    } else {
        category.title = L"当前自启动";
    }

    if (category.kind == CategoryKind::Disabled) {
        rows.reserve(disabled_.size());
        for (std::size_t index = 0; index < disabled_.size(); ++index) {
            visibleDisabledIndexes_.push_back(index);
            rows.push_back({static_cast<std::intptr_t>(visibleDisabledIndexes_.size()),
                {{disabled_[index].name}, {StartupSourceText(disabled_[index].source)}, {L"已禁用"}},
                false, true});
        }
    } else {
        for (std::size_t index = 0; index < items_.size(); ++index) {
            if (category.kind == CategoryKind::Source && items_[index].source != category.source) continue;
            visibleItemIndexes_.push_back(index);
            rows.push_back({static_cast<std::intptr_t>(visibleItemIndexes_.size()),
                {{items_[index].name}, {StartupSourceText(items_[index].source)}, {items_[index].canDisable ? L"可禁用" : L"仅查看"}},
                false, true});
        }
    }

    ThemedUi::SetTableRows(itemTable_, rows);
    const std::wstring status = L"当前范围：" + category.title + L"，共 " + std::to_wstring(rows.size()) + L" 项";
    ThemedUi::SetText(statusText_, status);
    windowUi_->ui().SetStatusTextRole(statusText_, ThemedStatusRole::Normal);
    UpdateButtons();
}

void AppLaunchLockerWindow::SelectCategory(int index) {
    if (index < 0 || index >= static_cast<int>(categories_.size()) || index == selectedCategory_) {
        return;
    }
    selectedCategory_ = index;
    ThemedUi::SetTableSelectedIndex(categoryTable_, selectedCategory_);
    RebuildRows();
}

const StartupItem* AppLaunchLockerWindow::SelectedStartupItem() const {
    if (categories_.empty() || selectedCategory_ < 0 || selectedCategory_ >= static_cast<int>(categories_.size()) ||
        categories_[static_cast<std::size_t>(selectedCategory_)].kind == CategoryKind::Disabled) {
        return nullptr;
    }
    const int row = ThemedUi::TableSelectedIndex(itemTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= visibleItemIndexes_.size()) return nullptr;
    const std::size_t index = visibleItemIndexes_[static_cast<std::size_t>(row)];
    return index < items_.size() ? &items_[index] : nullptr;
}

const DisabledRecord* AppLaunchLockerWindow::SelectedDisabledRecord() const {
    if (categories_.empty() || selectedCategory_ < 0 || selectedCategory_ >= static_cast<int>(categories_.size()) ||
        categories_[static_cast<std::size_t>(selectedCategory_)].kind != CategoryKind::Disabled) {
        return nullptr;
    }
    const int row = ThemedUi::TableSelectedIndex(itemTable_);
    if (row < 0 || static_cast<std::size_t>(row) >= visibleDisabledIndexes_.size()) return nullptr;
    const std::size_t index = visibleDisabledIndexes_[static_cast<std::size_t>(row)];
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
    const bool disabledCategory = !categories_.empty() &&
        selectedCategory_ >= 0 &&
        selectedCategory_ < static_cast<int>(categories_.size()) &&
        categories_[static_cast<std::size_t>(selectedCategory_)].kind == CategoryKind::Disabled;
    ThemedUi::SetVisible(disableButton_, !disabledCategory);
    ThemedUi::SetVisible(restoreButton_, disabledCategory);
    ThemedUi::SetVisible(elevateLink_, !disabledCategory && showElevateLink_);
}

void AppLaunchLockerWindow::ShowSelectedDetails() {
    if (busy_) return;
    if (const StartupItem* item = SelectedStartupItem()) DetailsDialog(hwnd_, instance_, theme_, DetailsText(*item)).Run();
    else if (const DisabledRecord* record = SelectedDisabledRecord()) DetailsDialog(hwnd_, instance_, theme_, DetailsText(*record)).Run();
}
