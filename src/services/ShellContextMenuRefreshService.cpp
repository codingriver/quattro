#include "ShellContextMenuRefreshService.h"

#include "SystemFunctions.h"
#include "TerminalContextMenuService.h"
#include "Utilities.h"

#include <objbase.h>

#include <algorithm>

namespace {
class ComApartment final {
public:
    ComApartment() {
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    bool ready() const {
        return SUCCEEDED(result_);
    }

private:
    HRESULT result_ = E_FAIL;
};

class HiddenOwnerWindow final {
public:
    HiddenOwnerWindow() {
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC",
            L"",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
    }

    ~HiddenOwnerWindow() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }

    HWND get() const {
        return hwnd_;
    }

private:
    HWND hwnd_ = nullptr;
};

bool IsUrlLink(const Link& link) {
    if (link.type == 2) {
        return true;
    }
    const std::wstring lower = ToLower(Trim(link.path));
    return lower.rfind(L"http://", 0) == 0
        || lower.rfind(L"https://", 0) == 0
        || lower.rfind(L"ftp://", 0) == 0
        || lower.rfind(L"www.", 0) == 0;
}

void CountItems(const std::vector<ShellContextMenuItem>& items, int& count) {
    for (const auto& item : items) {
        if (!item.separator) {
            ++count;
        }
        CountItems(item.children, count);
    }
}
}

ShellContextMenuRefreshService::ShellContextMenuRefreshService(QueryFunction query)
    : query_(query ? std::move(query) : ShellItemService::QueryTrackedContextMenu) {}

ShellContextMenuRefreshResult ShellContextMenuRefreshService::Refresh(
    const ShellContextMenuRefreshRequest& request,
    std::stop_token stopToken) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::CallerSingle;
    return ScanExecutionService::Run<ShellContextMenuRefreshResult>(options,
        [&](TaskContext& context) {
            return RefreshCore(request, stopToken, context);
        });
}

ShellContextMenuRefreshResult ShellContextMenuRefreshService::RefreshInTask(
    const ShellContextMenuRefreshRequest& request,
    TaskContext& context,
    std::stop_token stopToken) const {
    return RefreshCore(request, stopToken, context);
}

std::shared_ptr<ScanTaskHandle> ShellContextMenuRefreshService::StartRefresh(
    ShellContextMenuRefreshRequest request,
    std::function<void()> completionCallback) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::BackgroundSingle;
    options.completionCallback = std::move(completionCallback);
    const QueryFunction query = query_;
    return ScanExecutionService::StartTyped<ShellContextMenuRefreshResult>(options,
        [request = std::move(request), query](TaskContext& context) {
            return ShellContextMenuRefreshService(query).RefreshCore(request, {}, context);
        });
}

ShellContextMenuRefreshResult ShellContextMenuRefreshService::RefreshCore(
    const ShellContextMenuRefreshRequest& request,
    std::stop_token stopToken,
    TaskContext& context) const {
    ShellContextMenuRefreshResult result;
    result.tracking = request.tracking;
    result.totalLinks = static_cast<int>(request.links.size());
    if (!request.tracking.Any()) {
        return result;
    }
    ScanProgressUpdate progress;
    progress.phase = L"shell-context-menu";
    progress.title = request.progressTitle.empty()
        ? L"Windows 菜单扫描进度"
        : request.progressTitle;
    progress.status = request.progressStatus.empty()
        ? L"正在扫描 Windows 原生菜单"
        : request.progressStatus;
    progress.total = request.links.size();
    progress.indeterminate = false;
    context.Report(std::move(progress));

    ComApartment apartment;
    HiddenOwnerWindow owner;
    if (!apartment.ready() || !owner.get()) {
        result.failures.push_back(ShellContextMenuRefreshFailure{
            0,
            L"",
            L"无法初始化 Windows Shell 扫描线程。",
        });
        return result;
    }

    ShellContextMenuTrackingOptions nativeTracking = request.tracking;
    nativeTracking.terminal = false;
    TerminalContextMenuRefreshContext terminalContext;
    if (request.tracking.terminal) {
        terminalContext = TerminalContextMenuService::DetectAvailablePrograms();
    }

    const auto reportLink = [&context](const Link& link, bool succeeded, bool skipped, bool failed) {
        context.UpdateProgress([&](ScanProgressUpdate& value) {
            ++value.completed;
            value.current = value.completed;
            if (succeeded) ++value.succeeded;
            if (skipped) ++value.skipped;
            if (failed) ++value.failed;
            value.detail = link.name;
        });
    };

    for (Link link : request.links) {
        if (stopToken.stop_requested() || context.StopRequested()) {
            result.cancelled = true;
            break;
        }
        if (IsUrlLink(link) || BuiltinSystemFunctionForLink(link)) {
            ++result.skippedLinks;
            reportLink(link, false, true, false);
            continue;
        }
        if ((!ShellItemService::IsPidlBlobPlausible(link.pidl)
                && !ShellItemService::RefreshLinkShellData(link, false))) {
            result.failures.push_back(ShellContextMenuRefreshFailure{
                link.id,
                link.name,
                L"目标路径不存在或无法解析为 Windows Shell 对象。",
            });
            reportLink(link, false, false, true);
            continue;
        }

        ShellContextMenuRefreshUpdate update;
        update.link = link;
        bool ok = true;
        if (nativeTracking.Any()) {
            update.hasNativeSnapshot = query_(
                owner.get(), link, nativeTracking, update.nativeSnapshot);
            ok = update.hasNativeSnapshot && update.nativeSnapshot.complete;
        }
        if (request.tracking.terminal) {
            update.hasTerminalSnapshot = true;
            update.terminalSnapshot.complete = true;
            update.terminalSnapshot.items = TerminalContextMenuService::ItemsFor(link, terminalContext);
        }
        if (!ok) {
            result.failures.push_back(ShellContextMenuRefreshFailure{
                link.id,
                link.name,
                L"无法读取该启动项的 Windows 原生菜单。",
            });
            reportLink(link, false, false, true);
            continue;
        }

        CountItems(update.nativeSnapshot.items, result.menuItemCount);
        CountItems(update.terminalSnapshot.items, result.menuItemCount);
        result.updates.push_back(std::move(update));
        ++result.succeededLinks;
        reportLink(link, true, false, false);
    }
    context.UpdateProgress([&result](ScanProgressUpdate& value) {
        value.status = result.cancelled ? L"扫描已停止" : L"扫描完成";
        value.skipped = result.skippedLinks;
        value.failed = result.failures.size();
    });
    return result;
}
