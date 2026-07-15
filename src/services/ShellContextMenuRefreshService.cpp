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
    ShellContextMenuRefreshResult result;
    result.tracking = request.tracking;
    result.totalLinks = static_cast<int>(request.links.size());
    if (!request.tracking.Any()) {
        return result;
    }

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

    for (Link link : request.links) {
        if (stopToken.stop_requested()) {
            result.cancelled = true;
            break;
        }
        if (IsUrlLink(link) || BuiltinSystemFunctionForLink(link)) {
            ++result.skippedLinks;
            continue;
        }
        if ((!ShellItemService::IsPidlBlobPlausible(link.pidl)
                && !ShellItemService::RefreshLinkShellData(link, false))) {
            result.failures.push_back(ShellContextMenuRefreshFailure{
                link.id,
                link.name,
                L"目标路径不存在或无法解析为 Windows Shell 对象。",
            });
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
            continue;
        }

        CountItems(update.nativeSnapshot.items, result.menuItemCount);
        CountItems(update.terminalSnapshot.items, result.menuItemCount);
        result.updates.push_back(std::move(update));
        ++result.succeededLinks;
    }
    return result;
}
