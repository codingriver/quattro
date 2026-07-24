#pragma once

#include "ShellItemService.h"
#include "ScanExecutionService.h"

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct ShellContextMenuRefreshRequest {
    std::vector<Link> links;
    ShellContextMenuTrackingOptions tracking;
    std::wstring progressTitle;
    std::wstring progressStatus;
};

struct ShellContextMenuRefreshUpdate {
    Link link;
    bool hasNativeSnapshot = false;
    ShellContextMenuSnapshot nativeSnapshot;
    bool hasTerminalSnapshot = false;
    ShellContextMenuSnapshot terminalSnapshot;
};

struct ShellContextMenuRefreshFailure {
    int linkId = 0;
    std::wstring linkName;
    std::wstring message;
};

struct ShellContextMenuRefreshResult {
    ShellContextMenuTrackingOptions tracking;
    std::vector<ShellContextMenuRefreshUpdate> updates;
    std::vector<ShellContextMenuRefreshFailure> failures;
    int totalLinks = 0;
    int succeededLinks = 0;
    int skippedLinks = 0;
    int menuItemCount = 0;
    bool cancelled = false;
};

class ShellContextMenuRefreshService {
public:
    using QueryFunction = std::function<bool(
        HWND,
        const Link&,
        const ShellContextMenuTrackingOptions&,
        ShellContextMenuSnapshot&)>;

    explicit ShellContextMenuRefreshService(QueryFunction query = {});

    ShellContextMenuRefreshResult Refresh(
        const ShellContextMenuRefreshRequest& request,
        std::stop_token stopToken = {}) const;
    ShellContextMenuRefreshResult RefreshInTask(
        const ShellContextMenuRefreshRequest& request,
        TaskContext& context,
        std::stop_token stopToken = {}) const;
    std::shared_ptr<ScanTaskHandle> StartRefresh(
        ShellContextMenuRefreshRequest request,
        std::function<void()> completionCallback = {}) const;

private:
    ShellContextMenuRefreshResult RefreshCore(
        const ShellContextMenuRefreshRequest& request,
        std::stop_token stopToken,
        TaskContext& context) const;
    QueryFunction query_;
};
