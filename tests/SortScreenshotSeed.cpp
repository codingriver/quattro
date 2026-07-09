#include "../src/services/Storage.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void Fail(const std::wstring& message) {
    std::wcerr << message << L"\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::wcerr << L"usage: QuattroSortScreenshotSeed <app-directory> <sort-mode>\n";
        return 2;
    }

    const std::filesystem::path appDirectory(argv[1]);
    const std::wstring mode = argv[2];
    StorageService storage{appDirectory};
    AppModel model = storage.Load();
    if (!storage.sqliteAvailable()) {
        Fail(L"sqlite unavailable: " + storage.lastError());
        return 1;
    }

    Group* targetTag = nullptr;
    for (auto& group : model.groups) {
        if (group.parentGroup != 0) {
            targetTag = &group;
            break;
        }
    }
    if (!targetTag) {
        Fail(L"default tag missing");
        return 1;
    }

    Group tag = *targetTag;
    tag.name = L"排序验收";
    tag.layout = 0;
    tag.iconSize = 32;
    if (mode == L"name-desc") {
        tag.sort = 2;
        tag.sortDirection = 1;
    } else if (mode == L"run-asc") {
        tag.sort = 1;
        tag.sortDirection = 0;
    } else if (mode == L"run-desc") {
        tag.sort = 1;
        tag.sortDirection = 1;
    } else {
        tag.sort = 0;
        tag.sortDirection = 0;
    }
    if (!storage.UpdateGroup(tag)) {
        Fail(L"update tag failed: " + storage.lastError());
        return 1;
    }

    const std::filesystem::path target = appDirectory / L"sort-screenshot-target.txt";
    {
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file << "sort screenshot target";
    }

    const std::vector<Link> existingLinks = model.links;
    for (const auto& link : existingLinks) {
        if (link.parentGroup == tag.id && !storage.DeleteLink(link.id)) {
            Fail(L"delete link failed: " + storage.lastError());
            return 1;
        }
    }

    struct SeedLink {
        const wchar_t* name;
        int pos;
        int runCount;
    };
    const SeedLink links[] = {
        {L"Alpha-低频", 0, 2},
        {L"Beta-高频", 1, 90},
        {L"Gamma-中频", 2, 45},
    };
    for (const auto& seed : links) {
        Link link;
        link.name = seed.name;
        link.parentGroup = tag.id;
        link.path = target.wstring();
        link.pos = seed.pos;
        link.runCount = seed.runCount;
        link.showCmd = SW_SHOWNORMAL;
        if (!storage.InsertLink(link)) {
            Fail(L"insert link failed: " + storage.lastError());
            return 1;
        }
    }

    std::wcout << L"sort_screenshot_seed=passed\n";
    return 0;
}
