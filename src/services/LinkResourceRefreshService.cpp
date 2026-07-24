#include "LinkResourceRefreshService.h"

#include "AppLog.h"
#include "SystemFunctions.h"
#include "UrlIconDownloadService.h"
#include "Utilities.h"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace {
class WorkerComApartment final {
public:
    WorkerComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~WorkerComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    bool ready() const { return SUCCEEDED(result_); }

private:
    HRESULT result_ = E_FAIL;
};

bool IsUrlLink(const Link& link) {
    if (link.type == 2) {
        return true;
    }
    const std::wstring lower = ToLower(Trim(link.path));
    return lower.rfind(L"http://", 0) == 0 ||
        lower.rfind(L"https://", 0) == 0 ||
        lower.rfind(L"www.", 0) == 0;
}

struct UrlGroup {
    Link representative;
    std::vector<int> linkIds;
};

long long ElapsedMilliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}
}

LinkResourceRefreshResult LinkResourceRefreshService::Refresh(
    const LinkResourceRefreshRequest& request,
    TaskContext& context) const {
    const auto taskStarted = std::chrono::steady_clock::now();
    LinkResourceRefreshResult result;
    std::vector<Link> localLinks;
    std::vector<Link> shellLinks;
    std::vector<UrlGroup> urlGroups;
    std::unordered_map<std::wstring, std::size_t> urlGroupByHost;
    std::unordered_set<int> invalidUrlIds;

    for (const Link& link : request.links) {
        if (BuiltinSystemFunctionForLink(link)) {
            ++result.skipped;
            continue;
        }
        if (IsUrlLink(link)) {
            const std::wstring host = UrlIconDownloadService::HostForLink(link);
            if (host.empty()) {
                invalidUrlIds.insert(link.id);
                continue;
            }
            const auto [found, inserted] = urlGroupByHost.emplace(host, urlGroups.size());
            if (inserted) {
                urlGroups.push_back(UrlGroup{link, {}});
            }
            urlGroups[found->second].linkIds.push_back(link.id);
            continue;
        }
        localLinks.push_back(link);
        shellLinks.push_back(link);
    }

    TaskProgressUpdate progress;
    progress.phase = L"icons";
    progress.title = L"刷新" + request.scopeText;
    progress.status = L"正在更新启动项图标";
    progress.total = localLinks.size();
    progress.indeterminate = localLinks.empty();
    context.Report(std::move(progress));

    struct IconLocalResult {
        std::vector<LinkIconRefreshUpdate> updates;
        std::vector<int> failedIds;
    };
    std::unordered_set<int> refreshedIconIds;
    std::unordered_set<int> failedIconIds;
    context.ForEach<Link, IconLocalResult>(
        localLinks,
        TaskForEachOptions{TaskForEachMode::Parallel, 8},
        [] { return IconLocalResult{}; },
        [&](const Link& link, IconLocalResult& local, TaskContext& workerContext) {
            thread_local WorkerComApartment apartment;
            ResolvedIcon icon;
            if (apartment.ready()) {
                icon = IconResolverService(request.appDirectory).Resolve(
                    IconResolverService::ForLink(link, 54),
                    workerContext.StopToken());
            }
            if (IconResolverService::HasPixels(icon)) {
                local.updates.push_back(LinkIconRefreshUpdate{link.id, std::move(icon)});
            } else if (!workerContext.StopRequested()) {
                local.failedIds.push_back(link.id);
            }
            workerContext.UpdateProgress([&](TaskProgressUpdate& value) {
                ++value.completed;
                value.current = value.completed;
                value.detail = link.name;
                if (!local.updates.empty() && local.updates.back().linkId == link.id) {
                    ++value.succeeded;
                } else if (!workerContext.StopRequested()) {
                    ++value.failed;
                }
            });
        },
        [&](IconLocalResult&& local) {
            for (const auto& update : local.updates) {
                refreshedIconIds.insert(update.linkId);
            }
            failedIconIds.insert(local.failedIds.begin(), local.failedIds.end());
            result.iconUpdates.insert(
                result.iconUpdates.end(),
                std::make_move_iterator(local.updates.begin()),
                std::make_move_iterator(local.updates.end()));
        });
    const long long iconElapsedMs = ElapsedMilliseconds(taskStarted);

    if (!context.StopRequested() && !urlGroups.empty()) {
        context.Report(TaskProgressUpdate{
            L"url-icons",
            L"刷新" + request.scopeText,
            L"正在下载网址图标",
            L"按网站合并重复请求",
            0, 0, 0, 0, 0, 0,
            urlGroups.size(), 0, false});
        struct UrlLocalResult {
            std::vector<UrlIconRefreshUpdate> updates;
        };
        std::unordered_set<int> refreshedUrlIds;
        std::unordered_set<int> failedUrlIds;
        context.ForEach<UrlGroup, UrlLocalResult>(
            urlGroups,
            TaskForEachOptions{TaskForEachMode::Parallel, 4},
            [] { return UrlLocalResult{}; },
            [&](const UrlGroup& group, UrlLocalResult& local, TaskContext& workerContext) {
                const bool succeeded = UrlIconDownloadService::RefreshNow(
                    request.appDirectory, group.representative, workerContext.StopToken());
                local.updates.push_back(UrlIconRefreshUpdate{group.linkIds, succeeded});
                workerContext.UpdateProgress([&](TaskProgressUpdate& value) {
                    ++value.completed;
                    value.current = value.completed;
                    value.detail = group.representative.name;
                    if (succeeded) ++value.succeeded;
                    else if (!workerContext.StopRequested()) ++value.failed;
                });
            },
            [&](UrlLocalResult&& local) {
                for (const auto& update : local.updates) {
                    auto& target = update.succeeded ? refreshedUrlIds : failedUrlIds;
                    target.insert(update.linkIds.begin(), update.linkIds.end());
                }
                result.urlUpdates.insert(
                    result.urlUpdates.end(),
                    std::make_move_iterator(local.updates.begin()),
                    std::make_move_iterator(local.updates.end()));
            });

        invalidUrlIds.insert(failedUrlIds.begin(), failedUrlIds.end());
        for (const int linkId : refreshedUrlIds) {
            invalidUrlIds.erase(linkId);
        }
    }
    const long long urlElapsedMs = ElapsedMilliseconds(taskStarted) - iconElapsedMs;

    const auto shellStarted = std::chrono::steady_clock::now();
    if (!context.StopRequested() && request.tracking.Any() && !shellLinks.empty()) {
        ShellContextMenuRefreshRequest shellRequest{
            std::move(shellLinks),
            request.tracking,
            L"刷新" + request.scopeText,
            L"正在更新 Windows 菜单"};
        result.shellMenuResult = ShellContextMenuRefreshService().RefreshInTask(
            shellRequest, context, context.StopToken());
    }
    const long long shellElapsedMs = ElapsedMilliseconds(shellStarted);

    std::unordered_set<int> shellSucceededIds;
    std::unordered_set<int> shellFailedIds;
    for (const auto& update : result.shellMenuResult.updates) {
        shellSucceededIds.insert(update.link.id);
    }
    for (const auto& failure : result.shellMenuResult.failures) {
        shellFailedIds.insert(failure.linkId);
    }
    for (const Link& link : request.links) {
        if (BuiltinSystemFunctionForLink(link)) {
            continue;
        }
        bool succeeded = false;
        if (IsUrlLink(link)) {
            succeeded = !invalidUrlIds.contains(link.id);
            for (const auto& update : result.urlUpdates) {
                if (std::find(update.linkIds.begin(), update.linkIds.end(), link.id) != update.linkIds.end()) {
                    succeeded = update.succeeded;
                    break;
                }
            }
        } else {
            const bool iconSucceeded = refreshedIconIds.contains(link.id) && !failedIconIds.contains(link.id);
            const bool shellSucceeded = !request.tracking.Any() ||
                (shellSucceededIds.contains(link.id) && !shellFailedIds.contains(link.id));
            succeeded = iconSucceeded && shellSucceeded;
        }
        if (succeeded) ++result.completed;
        else if (!context.StopRequested()) ++result.failed;
    }
    result.skipped += result.shellMenuResult.skippedLinks;
    result.cancelled = context.StopRequested() || result.shellMenuResult.cancelled;
    context.UpdateProgress([&](TaskProgressUpdate& value) {
        value.status = result.cancelled ? L"刷新已停止" : L"刷新完成";
        value.detail = L"已处理 " + std::to_wstring(result.completed) +
            L" 项，失败 " + std::to_wstring(result.failed) + L" 项";
    });
    WriteAppLog(
        L"启动项资源刷新完成。scope=" + request.scopeText +
        L"，links=" + std::to_wstring(request.links.size()) +
        L"，icon_ms=" + std::to_wstring(iconElapsedMs) +
        L"，url_ms=" + std::to_wstring(urlElapsedMs) +
        L"，shell_ms=" + std::to_wstring(shellElapsedMs) +
        L"，total_ms=" + std::to_wstring(ElapsedMilliseconds(taskStarted)) +
        L"，completed=" + std::to_wstring(result.completed) +
        L"，failed=" + std::to_wstring(result.failed) +
        L"，cancelled=" + std::wstring(result.cancelled ? L"1" : L"0"));
    return result;
}
