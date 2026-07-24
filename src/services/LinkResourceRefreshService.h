#pragma once

#include "IconResolverService.h"
#include "ShellContextMenuRefreshService.h"
#include "TaskExecutionService.h"

#include <filesystem>
#include <string>
#include <vector>

struct LinkIconRefreshUpdate {
    int linkId = 0;
    ResolvedIcon icon;
};

struct UrlIconRefreshUpdate {
    std::vector<int> linkIds;
    bool succeeded = false;
};

struct LinkResourceRefreshRequest {
    std::filesystem::path appDirectory;
    std::vector<Link> links;
    ShellContextMenuTrackingOptions tracking;
    std::wstring scopeText;
};

struct LinkResourceRefreshResult {
    std::vector<LinkIconRefreshUpdate> iconUpdates;
    std::vector<UrlIconRefreshUpdate> urlUpdates;
    ShellContextMenuRefreshResult shellMenuResult;
    int completed = 0;
    int failed = 0;
    int skipped = 0;
    bool cancelled = false;
};

class LinkResourceRefreshService final {
public:
    LinkResourceRefreshResult Refresh(
        const LinkResourceRefreshRequest& request,
        TaskContext& context) const;
};
