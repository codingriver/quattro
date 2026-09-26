#include "LaunchItemSearchDialog.h"

#include "IconResolverService.h"
#include "LinkSearch.h"
#include "TaskExecutionService.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "../../resources/resource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <commctrl.h>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <objbase.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
constexpr int IdQuery = 1001;
constexpr int IdClear = 1002;
constexpr int IdResults = 1003;
constexpr UINT_PTR IdDebounceTimer = 1004;
constexpr UINT WmIndexCompleted = WM_APP + 0xA0;
constexpr UINT WmSearchCompleted = WM_APP + 0xA1;
constexpr UINT WmIconsCompleted = WM_APP + 0xA2;
constexpr UINT WmQueryChanged = WM_APP + 0xA3;
constexpr UINT WmContinueRows = WM_APP + 0xA4;
constexpr UINT WmViewportChanged = WM_APP + 0xA5;
constexpr UINT kDebounceMilliseconds = 120;
constexpr UINT kCompositionRetryMilliseconds = 30;
constexpr int kDesiredVisibleRows = 10;
constexpr int kMinimumVisibleRows = 5;
constexpr std::size_t kPageSize = 200;
constexpr std::size_t kCommitSliceSize = 32;
constexpr auto kCommitSliceBudget = std::chrono::milliseconds(8);
constexpr std::size_t kIconBatchSize = 40;
constexpr int kIconCaptureSize = 32;

std::atomic_uint64_t gNextDialogToken{1};

struct IndexTaskResult {
    std::shared_ptr<const LinkSearchIndex> index;
};

struct SearchTaskResult {
    std::uint64_t generation = 0;
    std::shared_ptr<const std::vector<int>> ids;
};

struct SearchIconResult {
    int linkId = 0;
    ResolvedIcon icon;
};

struct IconTaskResult {
    std::uint64_t generation = 0;
    std::uint64_t queryGeneration = 0;
    std::vector<SearchIconResult> icons;
};

DWORD SearchTestDelayMs() {
    wchar_t testMode[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_TEST_MODE", testMode, static_cast<DWORD>(std::size(testMode))) == 0) {
        return 0;
    }
    wchar_t delayText[16]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_TEST_SEARCH_DELAY_MS",
            delayText,
            static_cast<DWORD>(std::size(delayText))) == 0) {
        return 0;
    }
    return (std::min<DWORD>)(wcstoul(delayText, nullptr, 10), 1000);
}

class DialogWindow final {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme,
                 std::filesystem::path appDirectory, const AppModel& model)
        : owner_(owner), instance_(instance), theme_(theme),
          appDirectory_(std::move(appDirectory)),
          model_(std::make_shared<const AppModel>(model)),
          instanceToken_(gNextDialogToken.fetch_add(1)) {
        linksById_.reserve(model_->links.size());
        for (const Link& link : model_->links) linksById_.try_emplace(link.id, &link);
    }

    ~DialogWindow() {
        StopTasks();
        DestroyImageList();
    }

    std::optional<int> Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroLaunchItemSearchDialog", L"搜索启动项",
            DialogWindow::WindowProc, this, icon, icon);
        options.clientWidth = kThemedManagementClientWidth;
        options.clientHeight = kThemedManagementClientHeight;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) return std::nullopt;
        if (windowUi_) windowUi_->ShowModal();
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (HandleNavigationMessage(message)) continue;
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) windowUi_->RestoreModalOwner();
        return selectedLinkId_;
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
        return dialog ? dialog->HandleMessage(message, wParam, lParam)
                      : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        const HWND messageWindow = hwnd_;
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE:
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact,
                kThemedManagementClientWidth, kThemedManagementClientHeight);
            FitWindowToWorkArea();
            windowUi_->SetDpiChangedCallback([this](UINT) {
                FitWindowToWorkArea();
                LayoutControls();
                RebuildImageList();
            });
            CreateControls();
            StartIndexBuild();
            ScheduleQuery();
            SetFocus(queryEdit_);
            return 0;
        case WM_SIZE:
            LayoutControls();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd_, &paint);
            Paint(dc);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            Paint(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IdQuery && HIWORD(wParam) == EN_CHANGE) {
                activationEnabled_ = false;
                if (!suppressQueryChange_ && !queryChangePosted_) {
                    queryChangePosted_ = true;
                    pendingQueryChangeMessageSerial_ = ++queryChangeMessageSerial_;
                    if (!PostMessageW(hwnd_, WmQueryChanged,
                            static_cast<WPARAM>(pendingQueryChangeMessageSerial_),
                            static_cast<LPARAM>(instanceToken_))) {
                        ConsumePostedQueryChange();
                    }
                }
                return 0;
            }
            if (LOWORD(wParam) == IdClear) {
                suppressQueryChange_ = true;
                ThemedUi::SetText(queryEdit_, L"");
                suppressQueryChange_ = false;
                activationEnabled_ = false;
                ConsumePostedQueryChange();
                ScheduleQuery(true);
                SetFocus(queryEdit_);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            ThemedTableEvent event{};
            if (ThemedUi::DecodeTableEvent(resultsTable_, lParam, event)) {
                if (event.kind == ThemedTableEventKind::Activated) {
                    ActivateLink(static_cast<int>(event.rowKey));
                }
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (wParam == IdDebounceTimer) {
                KillTimer(hwnd_, IdDebounceTimer);
                debouncePending_ = false;
                StartQuery();
                return 0;
            }
            break;
        case WmIndexCompleted:
            if (static_cast<std::uint64_t>(lParam) == instanceToken_) ApplyIndexResult();
            return 0;
        case WmSearchCompleted:
            if (static_cast<std::uint64_t>(lParam) == instanceToken_)
                ApplySearchResult(static_cast<std::uint64_t>(wParam));
            return 0;
        case WmIconsCompleted:
            if (static_cast<std::uint64_t>(lParam) == instanceToken_)
                ApplyIconResult(static_cast<std::uint64_t>(wParam));
            return 0;
        case WmQueryChanged:
            if (static_cast<std::uint64_t>(lParam) != instanceToken_ ||
                !queryChangePosted_ ||
                static_cast<std::uint64_t>(wParam) != pendingQueryChangeMessageSerial_) {
                return 0;
            }
            ConsumePostedQueryChange();
            ScheduleQuery();
            return 0;
        case WmContinueRows:
            if (static_cast<std::uint64_t>(lParam) == instanceToken_)
                ProcessRowCommit(static_cast<std::uint64_t>(wParam));
            return 0;
        case WmViewportChanged:
            if (static_cast<std::uint64_t>(lParam) != instanceToken_) return 0;
            viewportMessagePosted_ = false;
            MaybeLoadMore();
            StartVisibleIconLoad();
            return 0;
        case WM_CLOSE:
            Close(std::nullopt);
            return 0;
        case WM_DESTROY:
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(messageWindow, GWLP_USERDATA, 0);
            {
                const LRESULT result = DefWindowProcW(messageWindow, message, wParam, lParam);
                hwnd_ = nullptr;
                return result;
            }
        default:
            break;
        }
        return DefWindowProcW(messageWindow, message, wParam, lParam);
    }

    void Paint(HDC dc) {
        if (!windowUi_) return;
        windowUi_->FillBackground(dc);
        windowUi_->DrawRegisteredEditFrames(dc);
        windowUi_->DrawRegisteredTableFrames(dc);
    }

    void FitWindowToWorkArea() {
        if (!windowUi_ || !hwnd_) return;
        const ThemedUi ui = windowUi_->ui();
        layout_ = ui.layout();
        RECT windowRect{}, clientRect{};
        GetWindowRect(hwnd_, &windowRect);
        GetClientRect(hwnd_, &clientRect);
        const int nonClientHeight = (windowRect.bottom - windowRect.top) -
            (clientRect.bottom - clientRect.top);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(owner_ ? owner_ : hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
        const int available = std::max(ui.scale(240),
            static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) -
                nonClientHeight - ui.scale(16));
        const int fixed = layout_.contentInsetY + ui.editHeight() + layout_.rowGap +
            layout_.rowGap + ui.labelHeight() + layout_.contentInsetY;
        visibleRows_ = kDesiredVisibleRows;
        while (visibleRows_ > kMinimumVisibleRows &&
               fixed + ui.tableHeightForRows(visibleRows_, false, true) > available) --visibleRows_;
        const int desired = std::min(
            available, fixed + ui.tableHeightForRows(visibleRows_, false, true));
        windowUi_->ResizeClientArea(clientRect.right - clientRect.left, desired, true);
    }

    void CreateControls() {
        const ThemedUi ui = windowUi_->ui();
        layout_ = ui.layout();
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int left = layout_.contentInsetX;
        const int right = client.right - layout_.contentInsetX;
        const int clearWidth = ui.buttonWidth(L"清空", ThemedButtonRole::Normal,
            ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int editWidth = std::max(ui.scale(160),
            right - left - clearWidth - layout_.controlGapX);
        queryFrame_ = RECT{left, layout_.contentInsetY, left + editWidth,
            layout_.contentInsetY + ui.editHeight()};
        ThemedEditOptions editOptions{};
        editOptions.placeholder = L"输入名称或路径，空格可组合关键词";
        queryEdit_ = ui.Edit(IdQuery, queryFrame_, L"", editOptions);
        clearButton_ = ui.Button(IdClear, L"清空", queryFrame_.right + layout_.controlGapX,
            queryFrame_.top, ThemedButtonRole::Normal, ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed, clearWidth);
        ThemedTableOptions tableOptions{};
        tableOptions.selection = ThemedTableSelection::Single;
        tableOptions.view = ThemedTableView::Details;
        tableOptions.showHeader = false;
        tableOptions.allowHorizontalScroll = false;
        tableOptions.reserveScrollBarGutter = true;
        tableOptions.rowPresentation = ThemedTableRowPresentation::TwoLine;
        resultsTable_ = ui.Table(IdResults, RECT{}, {
            ThemedTableColumn{L"item", L"启动项", ThemedTableColumnAlign::Start,
                ThemedTableColumnWidth::Remaining},
            ThemedTableColumn{L"location", L"分组 / 标签", ThemedTableColumnAlign::Start,
                ThemedTableColumnWidth::Fixed, ui.scale(150)},
        }, tableOptions);
        ThemedUi::BindTableSearchEdit(resultsTable_, queryEdit_);
        ThemedUi::SetTableViewportChangedHandler(resultsTable_, [this] {
            if (!hwnd_ || viewportMessagePosted_) return;
            viewportMessagePosted_ = true;
            PostMessageW(hwnd_, WmViewportChanged, 0, static_cast<LPARAM>(instanceToken_));
        });
        ui.SetTableRowTooltip(resultsTable_, [this](int, std::intptr_t rowKey) {
            const int id = static_cast<int>(rowKey);
            const LinkSearchResult* result = searchIndex_ ? searchIndex_->FindResult(id) : nullptr;
            const auto link = linksById_.find(id);
            if (!result || link == linksById_.end()) return std::wstring{};
            std::wstring text = link->second->name;
            if (!link->second->path.empty()) text += L"\n路径：" + link->second->path;
            const std::wstring location = LocationText(*result);
            if (!location.empty()) text += L"\n位置：" + location;
            return text;
        });
        statusText_ = ui.StatusText(L"全部分组与标签 · 正在准备搜索，暂不可启动", left, 0, right - left,
            ThemedStatusTextOptions{ThemedStatusRole::Normal, ThemedTextAlign::Start});
        RebuildImageList();
        LayoutControls();
    }

    void LayoutControls() {
        if (!windowUi_ || !queryEdit_) return;
        const ThemedUi ui = windowUi_->ui();
        layout_ = ui.layout();
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int left = layout_.contentInsetX;
        const int right = client.right - layout_.contentInsetX;
        const int clearWidth = ui.buttonWidth(L"清空", ThemedButtonRole::Normal,
            ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int editWidth = std::max(ui.scale(160),
            right - left - clearWidth - layout_.controlGapX);
        queryFrame_ = RECT{left, layout_.contentInsetY, left + editWidth,
            layout_.contentInsetY + ui.editHeight()};
        windowUi_->MoveEditFrame(queryEdit_, queryFrame_);
        ui.MoveControl(clearButton_, queryFrame_.right + layout_.controlGapX,
            queryFrame_.top, clearWidth);
        const int tableTop = queryFrame_.bottom + layout_.rowGap;
        const int statusY = client.bottom - layout_.contentInsetY - ui.labelHeight();
        resultsFrame_ = RECT{left, tableTop, right, statusY - layout_.rowGap};
        ui.MoveTable(resultsTable_, resultsFrame_);
        ui.MoveControl(statusText_, left, statusY, right - left);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    bool HandleNavigationMessage(const MSG& message) {
        if (!hwnd_) return false;
        if (message.message == WM_KEYDOWN &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetKeyState(VK_MENU) & 0x8000) == 0 && message.wParam == 'F') {
            SetFocus(queryEdit_);
            ThemedUi::SelectAllText(queryEdit_);
            return true;
        }
        ThemedEditTableNavigation navigation =
            ThemedUi::DecodeEditTableNavigation(message, queryEdit_);
        if (navigation == ThemedEditTableNavigation::None &&
            message.message == WM_KEYDOWN && GetFocus() == resultsTable_) {
            switch (message.wParam) {
            case VK_UP: navigation = ThemedEditTableNavigation::Previous; break;
            case VK_DOWN: navigation = ThemedEditTableNavigation::Next; break;
            case VK_PRIOR: navigation = ThemedEditTableNavigation::PagePrevious; break;
            case VK_NEXT: navigation = ThemedEditTableNavigation::PageNext; break;
            case VK_RETURN: navigation = ThemedEditTableNavigation::Activate; break;
            case VK_ESCAPE: navigation = ThemedEditTableNavigation::Cancel; break;
            default: break;
            }
        }
        switch (navigation) {
        case ThemedEditTableNavigation::Previous: MoveSelection(-1); return true;
        case ThemedEditTableNavigation::Next: MoveSelection(1); return true;
        case ThemedEditTableNavigation::PagePrevious: MoveSelection(-visibleRows_); return true;
        case ThemedEditTableNavigation::PageNext: MoveSelection(visibleRows_); return true;
        case ThemedEditTableNavigation::Activate: RequestEnterActivation(); return true;
        case ThemedEditTableNavigation::Cancel:
            Close(std::nullopt);
            return true;
        default: return false;
        }
    }

    void MoveSelection(int delta) {
        const int count = ThemedUi::TableRowCount(resultsTable_);
        if (count <= 0) return;
        int current = ThemedUi::TableSelectedIndex(resultsTable_);
        if (current < 0) current = delta >= 0 ? 0 : count - 1;
        else current = std::clamp(current + delta, 0, count - 1);
        suppressSelectionChange_ = true;
        ThemedUi::SetTableSelectedIndex(resultsTable_, current);
        suppressSelectionChange_ = false;
    }

    void StartIndexBuild() {
        if (indexTask_) indexTask_->RequestStop();
        const HWND target = hwnd_;
        const std::uint64_t token = instanceToken_;
        const auto model = model_;
        TaskOptions options{};
        options.mode = TaskExecutionMode::BackgroundSingle;
        options.completionCallback = [target, token] {
            if (IsWindow(target)) PostMessageW(target, WmIndexCompleted, 0,
                static_cast<LPARAM>(token));
        };
        indexTask_ = TaskExecutionService::StartTyped<IndexTaskResult>(
            std::move(options), [model](TaskContext& context) {
                return IndexTaskResult{LinkSearchIndex::Build(*model, context.StopToken())};
            });
    }

    void ApplyIndexResult() {
        if (!indexTask_ || !indexTask_->IsFinished()) return;
        const TaskStatus status = indexTask_->Status();
        if (status == TaskStatus::Completed) {
            IndexTaskResult result = indexTask_->ResultCopy<IndexTaskResult>();
            searchIndex_ = std::move(result.index);
        }
        indexTask_.reset();
        if (!searchIndex_) {
            activationEnabled_ = false;
            if (status == TaskStatus::Failed)
                ThemedUi::SetText(statusText_,
                    L"全部分组与标签 · 匹配失败，请修改关键词或清空后重试");
            return;
        }
        if (!debouncePending_) StartQuery();
    }

    void ConsumePostedQueryChange() {
        queryChangePosted_ = false;
        pendingQueryChangeMessageSerial_ = 0;
    }

    void ScheduleQuery(bool immediate = false) {
        if (!hwnd_) return;
        KillTimer(hwnd_, IdDebounceTimer);
        debouncePending_ = !immediate;
        ++queryGeneration_;
        ++iconGeneration_;
        ++commitGeneration_;
        if (queryTask_) queryTask_->RequestStop();
        if (iconTask_) iconTask_->RequestStop();
        iconTask_.reset();
        activationEnabled_ = false;
        commitInProgress_ = false;
        appendInProgress_ = false;
        ThemedUi::SetText(statusText_, searchIndex_
            ? L"全部分组与标签 · 正在匹配，暂不可启动"
            : L"全部分组与标签 · 正在准备搜索，暂不可启动");
        if (immediate) StartQuery();
        else SetTimer(hwnd_, IdDebounceTimer, kDebounceMilliseconds, nullptr);
    }

    void StartQuery() {
        if (!hwnd_) return;
        KillTimer(hwnd_, IdDebounceTimer);
        if (ThemedUi::IsEditComposing(queryEdit_)) {
            debouncePending_ = true;
            SetTimer(hwnd_, IdDebounceTimer, kCompositionRetryMilliseconds, nullptr);
            return;
        }
        debouncePending_ = false;
        if (!searchIndex_) {
            ThemedUi::SetText(statusText_,
                L"全部分组与标签 · 正在准备搜索，暂不可启动");
            return;
        }
        if (queryTask_) {
            if (!queryTask_->IsFinished()) {
                // Keep ownership until its completion notification arrives.
                // This guarantees one matcher at a time while the latest
                // generation remains queued implicitly by queryGeneration_.
                queryTask_->RequestStop();
                return;
            }
            // A stale completion can be waiting in the window queue. Its
            // generation no longer owns the handle, so it will be ignored.
            queryTask_.reset();
        }
        const std::uint64_t generation = queryGeneration_;
        queryTaskGeneration_ = generation;
        activationEnabled_ = false;
        ThemedUi::SetText(statusText_, L"全部分组与标签 · 正在匹配，暂不可启动");
        const HWND target = hwnd_;
        const std::uint64_t token = instanceToken_;
        const std::wstring query = ThemedUi::Text(queryEdit_);
        const auto index = searchIndex_;
        const DWORD testDelayMs = SearchTestDelayMs();
        TaskOptions options{};
        options.mode = TaskExecutionMode::BackgroundSingle;
        options.completionCallback = [target, generation, token] {
            if (IsWindow(target)) PostMessageW(target, WmSearchCompleted,
                static_cast<WPARAM>(generation), static_cast<LPARAM>(token));
        };
        queryTask_ = TaskExecutionService::StartTyped<SearchTaskResult>(
            std::move(options), [index, query, generation, testDelayMs](TaskContext& context) {
                SearchTaskResult result{};
                result.generation = generation;
                for (DWORD waited = 0; waited < testDelayMs && !context.StopRequested();) {
                    const DWORD chunk = (std::min<DWORD>)(20, testDelayMs - waited);
                    Sleep(chunk);
                    waited += chunk;
                }
                if (!context.StopRequested())
                    result.ids = index->SearchIds(query, context.StopToken());
                return result;
            });
    }

    void ApplySearchResult(std::uint64_t generation) {
        if (!queryTask_ || generation != queryTaskGeneration_ ||
            !queryTask_->IsFinished()) return;
        const TaskStatus status = queryTask_->Status();
        SearchTaskResult result{};
        if (status == TaskStatus::Completed) {
            result = queryTask_->ResultCopy<SearchTaskResult>();
        }
        queryTask_.reset();

        if (generation != queryGeneration_) {
            if (!debouncePending_) StartQuery();
            return;
        }
        if (status != TaskStatus::Completed) {
            activationEnabled_ = false;
            if (status == TaskStatus::Failed)
                ThemedUi::SetText(statusText_,
                    L"全部分组与标签 · 匹配失败，请修改关键词或清空后重试");
            return;
        }
        if (result.generation != queryGeneration_ || !result.ids) return;

        resultIds_ = std::move(result.ids);
        currentResultGeneration_ = generation;
        BeginRowCommit((std::min)(kPageSize, resultIds_->size()), true);
    }

    std::vector<int> CurrentTableIds() const {
        std::vector<int> ids;
        const int count = ThemedUi::TableRowCount(resultsTable_);
        ids.reserve((std::max)(0, count));
        for (int row = 0; row < count; ++row)
            ids.push_back(static_cast<int>(ThemedUi::TableRowKey(resultsTable_, row)));
        return ids;
    }

    std::optional<std::size_t> ResultIndex(int linkId) const {
        if (!resultIds_) return std::nullopt;
        const auto found = std::find(resultIds_->begin(), resultIds_->end(), linkId);
        if (found == resultIds_->end()) return std::nullopt;
        return static_cast<std::size_t>(found - resultIds_->begin());
    }

    void BeginRowCommit(std::size_t targetCount, bool newQuery) {
        if (!resultIds_ || currentResultGeneration_ != queryGeneration_) return;
        displayedIds_ = CurrentTableIds();
        targetCount = (std::min)(targetCount, resultIds_->size());
        if (!newQuery && (commitInProgress_ || targetCount <= displayedIds_.size())) return;

        commitSelectedId_.reset();
        commitTopKey_ = 0;
        commitTargetIds_.clear();
        if (newQuery) {
            const auto selectedKeys = ThemedUi::TableSelectedKeys(resultsTable_);
            const int selectedId = selectedKeys.empty() ? 0 : static_cast<int>(selectedKeys.front());
            const std::intptr_t topKey = ThemedUi::TableTopVisibleRowKey(resultsTable_);
            const auto includeAnchor = [this, &targetCount](int linkId) {
                const auto index = ResultIndex(linkId);
                if (!index) return false;
                const std::size_t pageEnd = ((*index / kPageSize) + 1) * kPageSize;
                targetCount = (std::min)(resultIds_->size(), (std::max)(targetCount, pageEnd));
                return true;
            };
            if (selectedId > 0 && includeAnchor(selectedId)) commitSelectedId_ = selectedId;
            if (topKey != 0 && includeAnchor(static_cast<int>(topKey))) commitTopKey_ = topKey;

            commitTargetIds_.reserve(targetCount);
            for (std::size_t index = 0; index < targetCount; ++index)
                commitTargetIds_.insert((*resultIds_)[index]);
        }

        commitGeneration_ = queryGeneration_;
        commitCursor_ = newQuery ? 0 : displayedIds_.size();
        commitTargetCount_ = targetCount;
        commitSliceSize_ = kCommitSliceSize;
        commitNewQuery_ = newQuery;
        commitInProgress_ = true;
        appendInProgress_ = !newQuery;
        if (newQuery) activationEnabled_ = false;
        PostMessageW(hwnd_, WmContinueRows, static_cast<WPARAM>(commitGeneration_),
            static_cast<LPARAM>(instanceToken_));
    }

    void ProcessRowCommit(std::uint64_t generation) {
        if (!commitInProgress_ || generation != commitGeneration_ ||
            generation != queryGeneration_ || generation != currentResultGeneration_ ||
            !resultIds_) return;

        const std::size_t end = (std::min)(commitTargetCount_, commitCursor_ + commitSliceSize_);
        const bool finalSlice = end >= commitTargetCount_;
        const std::vector<int> currentIds = CurrentTableIds();
        const std::unordered_set<int> existingIds(currentIds.begin(), currentIds.end());
        ThemedTableRowBatch batch{};
        batch.upserts.reserve(end - commitCursor_);
        for (std::size_t index = commitCursor_; index < end; ++index) {
            const int id = (*resultIds_)[index];
            if (!existingIds.contains(id)) batch.upserts.push_back(TableRow(id));
        }
        if (commitNewQuery_ && finalSlice) {
            batch.removeKeys.reserve(currentIds.size());
            std::vector<int> projectedIds;
            projectedIds.reserve(commitTargetCount_);
            for (int id : currentIds) {
                if (commitTargetIds_.contains(id)) projectedIds.push_back(id);
                else batch.removeKeys.push_back(id);
            }
            for (const auto& row : batch.upserts)
                projectedIds.push_back(static_cast<int>(row.key));

            bool orderChanged = projectedIds.size() != commitTargetCount_;
            for (std::size_t index = 0; !orderChanged && index < commitTargetCount_; ++index)
                orderChanged = projectedIds[index] != (*resultIds_)[index];
            if (orderChanged) {
                batch.order.reserve(commitTargetCount_);
                for (std::size_t index = 0; index < commitTargetCount_; ++index)
                    batch.order.push_back((*resultIds_)[index]);
            }
        }
        const auto commitStarted = std::chrono::steady_clock::now();
        const bool hasMutations = !batch.upserts.empty() ||
            !batch.removeKeys.empty() || !batch.order.empty();
        if (hasMutations && !ThemedUi::ApplyTableRowBatch(resultsTable_, batch)) {
            commitInProgress_ = false;
            appendInProgress_ = false;
            activationEnabled_ = false;
            ThemedUi::SetText(statusText_,
                L"全部分组与标签 · 更新结果失败，请修改关键词或清空后重试");
            return;
        }
        const auto commitElapsed = std::chrono::steady_clock::now() - commitStarted;
        if (commitElapsed > kCommitSliceBudget + kCommitSliceBudget / 2) {
            commitSliceSize_ = (std::max<std::size_t>)(8, commitSliceSize_ / 2);
        } else if (commitElapsed < kCommitSliceBudget / 2) {
            commitSliceSize_ = (std::min<std::size_t>)(64, commitSliceSize_ * 2);
        }

        displayedIds_ = CurrentTableIds();
        commitCursor_ = end;
        if (commitNewQuery_) UpdateResultStatus(false);
        if (commitCursor_ < commitTargetCount_) {
            PostMessageW(hwnd_, WmContinueRows, static_cast<WPARAM>(generation),
                static_cast<LPARAM>(instanceToken_));
            return;
        }

        const bool newQuery = commitNewQuery_;
        commitInProgress_ = false;
        commitNewQuery_ = false;
        appendInProgress_ = false;
        activationEnabled_ = !displayedIds_.empty() && !queryChangePosted_ &&
            !debouncePending_ && !queryTask_ && currentResultGeneration_ == queryGeneration_;
        if (newQuery) {
            suppressSelectionChange_ = true;
            if (commitSelectedId_ &&
                ThemedUi::FindTableRowByKey(resultsTable_, *commitSelectedId_) >= 0) {
                const auto selectedKeys = ThemedUi::TableSelectedKeys(resultsTable_);
                if (selectedKeys != std::vector<std::intptr_t>{*commitSelectedId_}) {
                    ThemedUi::SetTableSelectedKeys(resultsTable_, {*commitSelectedId_});
                }
            } else if (!displayedIds_.empty()) {
                if (ThemedUi::TableSelectedIndex(resultsTable_) != 0) {
                    ThemedUi::SetTableSelectedIndex(resultsTable_, 0);
                }
            } else {
                if (!ThemedUi::TableSelectedKeys(resultsTable_).empty()) {
                    ThemedUi::SetTableSelectedKeys(resultsTable_, {});
                }
            }
            suppressSelectionChange_ = false;
            commitSelectedId_.reset();
            commitTopKey_ = 0;
            commitTargetIds_.clear();
        }
        UpdateResultStatus(true);
        ThemedUi::RefreshTableRowTooltip(resultsTable_);
        StartVisibleIconLoad();
    }

    void MaybeLoadMore() {
        if (!resultIds_ || currentResultGeneration_ != queryGeneration_ ||
            commitInProgress_ || appendInProgress_ || displayedIds_.size() >= resultIds_->size()) return;
        const ThemedTableVisibleRange visible = ThemedUi::TableVisibleRange(resultsTable_);
        if (visible.last < 0) return;
        const int trigger = (std::max)(0,
            static_cast<int>(displayedIds_.size()) - visibleRows_);
        if (visible.last < trigger) return;
        BeginRowCommit((std::min)(resultIds_->size(), displayedIds_.size() + kPageSize), false);
    }

    ThemedTableRow TableRow(int linkId) const {
        ThemedTableRow row{};
        row.key = linkId;
        const auto link = linksById_.find(linkId);
        if (link == linksById_.end()) return row;
        const auto image = iconImageById_.find(linkId);
        ThemedTableCell primary{link->second->name,
            image == iconImageById_.end() ? 0 : image->second};
        primary.secondaryText = link->second->path;
        const LinkSearchResult* result = searchIndex_ ? searchIndex_->FindResult(linkId) : nullptr;
        row.cells = {std::move(primary), ThemedTableCell{result ? LocationText(*result) : L""}};
        return row;
    }

    static std::wstring LocationText(const LinkSearchResult& item) {
        if (item.groupName.empty()) return item.tagName;
        if (item.tagName.empty()) return item.groupName;
        return item.groupName + L" / " + item.tagName;
    }

    void UpdateResultStatus(bool complete) {
        std::wstring text;
        if (!complete) {
            text = L"全部分组与标签 · 正在匹配，暂不可启动";
        } else if (model_->links.empty()) {
            text = L"全部分组与标签 · 暂无启动项，请先在主界面添加";
        } else if (!resultIds_ || resultIds_->empty()) {
            text = L"全部分组与标签 · 未找到结果，请缩短关键词或清空";
        } else {
            text = L"全部分组与标签 · 找到 " +
                std::to_wstring(resultIds_->size()) +
                L" 项 · ↑/↓ 选择，Enter 启动，Esc 关闭";
        }
        if (ThemedUi::Text(statusText_) != text) ThemedUi::SetText(statusText_, text);
    }

    int SelectedLinkId() const {
        const int selected = ThemedUi::TableSelectedIndex(resultsTable_);
        return selected < 0 ? 0 :
            static_cast<int>(ThemedUi::TableRowKey(resultsTable_, selected));
    }

    bool CanActivate(int linkId) const {
        if (linkId <= 0 || !activationEnabled_ || queryChangePosted_ ||
            debouncePending_ || queryTask_ || (commitInProgress_ && commitNewQuery_) ||
            currentResultGeneration_ != queryGeneration_ || !resultIds_) {
            return false;
        }
        if (linksById_.find(linkId) == linksById_.end() ||
            ThemedUi::FindTableRowByKey(resultsTable_, linkId) < 0) {
            return false;
        }
        return std::find(resultIds_->begin(), resultIds_->end(), linkId) != resultIds_->end();
    }

    void ActivateLink(int linkId) {
        if (CanActivate(linkId)) Close(linkId);
    }

    void RequestEnterActivation() {
        const int linkId = SelectedLinkId();
        if (CanActivate(linkId)) {
            Close(linkId);
            return;
        }
        if (queryChangePosted_) {
            ConsumePostedQueryChange();
            ScheduleQuery(true);
            return;
        }
        if (debouncePending_) {
            KillTimer(hwnd_, IdDebounceTimer);
            debouncePending_ = false;
            StartQuery();
        } else if (!queryTask_ && currentResultGeneration_ != queryGeneration_) {
            StartQuery();
        }
    }

    void RebuildImageList() {
        if (iconTask_) iconTask_->RequestStop();
        iconTask_.reset();
        ++iconGeneration_;
        DestroyImageList(false);
        if (!resultsTable_ || !windowUi_) return;
        const int iconSize = std::max(16, windowUi_->ui().scale(16));
        images_ = ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, 16, 16);
        if (!images_) return;
        HICON placeholder = static_cast<HICON>(LoadImageW(instance_,
            MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON), IMAGE_ICON,
            iconSize, iconSize, LR_DEFAULTCOLOR));
        if (placeholder) {
            ImageList_AddIcon(images_, placeholder);
            DestroyIcon(placeholder);
        }
        if (ImageList_GetImageCount(images_) == 0)
            ImageList_AddIcon(images_, LoadIconW(nullptr, IDI_APPLICATION));
        iconImageById_.clear();
        iconAttemptedIds_.clear();
        ThemedUi::SetTableImageLists(resultsTable_, images_, nullptr);
        if (!displayedIds_.empty()) {
            ThemedTableRowBatch batch{};
            for (int id : displayedIds_) batch.upserts.push_back(TableRow(id));
            if (!batch.upserts.empty()) ThemedUi::ApplyTableRowBatch(resultsTable_, batch);
            StartVisibleIconLoad();
        }
    }

    void DestroyImageList(bool detachInteractions = true) {
        if (resultsTable_ && IsWindow(resultsTable_)) {
            if (detachInteractions) {
                ThemedUi::SetTableViewportChangedHandler(resultsTable_, {});
                ThemedUi::BindTableSearchEdit(resultsTable_, nullptr);
            }
            ThemedUi::SetTableImageLists(resultsTable_, nullptr, nullptr);
        }
        if (images_) {
            ImageList_Destroy(images_);
            images_ = nullptr;
        }
    }

    void StartVisibleIconLoad() {
        if (!images_ || !resultIds_ || currentResultGeneration_ != queryGeneration_ ||
            displayedIds_.empty() || iconTask_) return;
        ThemedTableVisibleRange visible = ThemedUi::TableVisibleRange(resultsTable_);
        if (visible.first < 0 || visible.last < 0) {
            visible.first = 0;
            visible.last = (std::min)(static_cast<int>(displayedIds_.size()) - 1,
                visibleRows_ - 1);
        }
        const int first = (std::max)(0, visible.first - visibleRows_);
        const int last = (std::min)(static_cast<int>(displayedIds_.size()) - 1,
            visible.last + visibleRows_);
        std::vector<Link> links;
        links.reserve(kIconBatchSize);
        for (int row = first; row <= last && links.size() < kIconBatchSize; ++row) {
            const int linkId = displayedIds_[static_cast<std::size_t>(row)];
            if (iconImageById_.contains(linkId) || iconAttemptedIds_.contains(linkId)) continue;
            const auto link = linksById_.find(linkId);
            if (link != linksById_.end()) links.push_back(*link->second);
        }
        if (links.empty()) return;
        const std::uint64_t generation = iconGeneration_;
        const std::uint64_t queryGeneration = queryGeneration_;
        const HWND target = hwnd_;
        const std::uint64_t token = instanceToken_;
        const std::filesystem::path appDirectory = appDirectory_;
        TaskOptions options{};
        options.mode = TaskExecutionMode::BackgroundSingle;
        options.completionCallback = [target, generation, token] {
            if (IsWindow(target)) PostMessageW(target, WmIconsCompleted,
                static_cast<WPARAM>(generation), static_cast<LPARAM>(token));
        };
        iconTask_ = TaskExecutionService::StartTyped<IconTaskResult>(
            std::move(options),
            [generation, queryGeneration, links = std::move(links), appDirectory](TaskContext& context) {
                IconTaskResult result{};
                result.generation = generation;
                result.queryGeneration = queryGeneration;
                const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                IconResolverService resolver(appDirectory);
                result.icons.reserve(links.size());
                for (const Link& link : links) {
                    if (context.StopRequested()) break;
                    SearchIconResult icon{};
                    icon.linkId = link.id;
                    icon.icon = resolver.Resolve(IconResolverService::ForLink(link, kIconCaptureSize),
                        context.StopToken());
                    if (context.StopRequested()) break;
                    result.icons.push_back(std::move(icon));
                }
                if (SUCCEEDED(comResult)) CoUninitialize();
                return result;
            });
    }

    void ApplyIconResult(std::uint64_t generation) {
        if (!iconTask_ || generation != iconGeneration_ || !iconTask_->IsFinished()) return;
        if (iconTask_->Status() != TaskStatus::Completed) {
            iconTask_.reset();
            StartVisibleIconLoad();
            return;
        }
        IconTaskResult result = iconTask_->ResultCopy<IconTaskResult>();
        iconTask_.reset();
        if (result.generation != iconGeneration_ ||
            result.queryGeneration != queryGeneration_ || !images_ || !windowUi_) return;
        ThemedTableRowBatch batch{};
        for (const SearchIconResult& value : result.icons) {
            iconAttemptedIds_.insert(value.linkId);
            if (ThemedUi::FindTableRowByKey(resultsTable_, value.linkId) < 0) continue;
            HBITMAP bitmap = IconResolverService::CreateBitmapFromPixels(value.icon,
                std::max(16, windowUi_->ui().scale(16)),
                ThemedUi::ListSurfaceColor(theme_), true);
            if (!bitmap) continue;
            const int imageIndex = ImageList_Add(images_, bitmap, nullptr);
            DeleteObject(bitmap);
            if (imageIndex < 0) continue;
            iconImageById_[value.linkId] = imageIndex;
            batch.upserts.push_back(TableRow(value.linkId));
        }
        if (!batch.upserts.empty()) ThemedUi::ApplyTableRowBatch(resultsTable_, batch);
        StartVisibleIconLoad();
    }

    void StopTasks() {
        if (hwnd_) KillTimer(hwnd_, IdDebounceTimer);
        ++commitGeneration_;
        commitInProgress_ = false;
        if (indexTask_) indexTask_->RequestStop();
        if (queryTask_) queryTask_->RequestStop();
        if (iconTask_) iconTask_->RequestStop();
        indexTask_.reset();
        queryTask_.reset();
        iconTask_.reset();
    }

    void Close(std::optional<int> linkId) {
        if (done_) return;
        StopTasks();
        selectedLinkId_ = linkId;
        done_ = true;
        DestroyImageList();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::filesystem::path appDirectory_;
    std::shared_ptr<const AppModel> model_;
    std::uint64_t instanceToken_ = 0;
    std::uint64_t queryGeneration_ = 0;
    std::uint64_t queryTaskGeneration_ = 0;
    std::uint64_t currentResultGeneration_ = 0;
    std::uint64_t commitGeneration_ = 0;
    std::uint64_t iconGeneration_ = 0;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    std::shared_ptr<const LinkSearchIndex> searchIndex_;
    std::shared_ptr<const std::vector<int>> resultIds_;
    std::shared_ptr<TaskHandle> indexTask_;
    std::shared_ptr<TaskHandle> queryTask_;
    std::shared_ptr<TaskHandle> iconTask_;
    DialogLayoutMetrics layout_{};
    HWND queryEdit_ = nullptr;
    HWND clearButton_ = nullptr;
    HWND resultsTable_ = nullptr;
    HWND statusText_ = nullptr;
    RECT queryFrame_{};
    RECT resultsFrame_{};
    HIMAGELIST images_ = nullptr;
    std::unordered_map<int, const Link*> linksById_;
    std::unordered_map<int, int> iconImageById_;
    std::unordered_set<int> iconAttemptedIds_;
    std::unordered_set<int> commitTargetIds_;
    std::vector<int> displayedIds_;
    std::optional<int> selectedLinkId_;
    std::optional<int> commitSelectedId_;
    std::intptr_t commitTopKey_ = 0;
    std::uint64_t queryChangeMessageSerial_ = 0;
    std::uint64_t pendingQueryChangeMessageSerial_ = 0;
    std::size_t commitCursor_ = 0;
    std::size_t commitTargetCount_ = 0;
    std::size_t commitSliceSize_ = kCommitSliceSize;
    int visibleRows_ = kDesiredVisibleRows;
    bool suppressQueryChange_ = false;
    bool suppressSelectionChange_ = false;
    bool activationEnabled_ = false;
    bool debouncePending_ = false;
    bool commitInProgress_ = false;
    bool commitNewQuery_ = false;
    bool appendInProgress_ = false;
    bool viewportMessagePosted_ = false;
    bool queryChangePosted_ = false;
    bool done_ = false;
};
}

std::optional<int> LaunchItemSearchDialog::Show(
    HWND owner, HINSTANCE instance, const Theme& theme,
    const std::filesystem::path& appDirectory, const AppModel& model) {
    DialogWindow dialog(owner, instance, theme, appDirectory, model);
    return dialog.Run();
}
