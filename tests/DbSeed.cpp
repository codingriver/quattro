#include "../src/Storage.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::wstring NumberedName(const wchar_t* prefix, int index) {
    std::wostringstream stream;
    stream << prefix << std::setw(2) << std::setfill(L'0') << index;
    return stream.str();
}

void Fail(const std::wstring& message) {
    std::wcerr << message << L"\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"usage: QuattroDbSeed <app-directory>\n";
        return 2;
    }

    const std::filesystem::path appDirectory(argv[1]);
    StorageService storage{appDirectory};
    AppModel model = storage.Load();
    if (!storage.sqliteAvailable()) {
        Fail(L"sqlite unavailable: " + storage.lastError());
        return 1;
    }

    int firstGroupId = 0;
    int firstTagId = 0;
    for (const auto& group : model.groups) {
        if (group.parentGroup == 0 && firstGroupId == 0) {
            firstGroupId = group.id;
        }
    }
    for (const auto& group : model.groups) {
        if (group.parentGroup == firstGroupId && firstTagId == 0) {
            firstTagId = group.id;
        }
    }
    if (firstGroupId <= 0 || firstTagId <= 0) {
        Fail(L"default group or tag missing");
        return 1;
    }

    for (int index = 1; index <= 28; ++index) {
        Group group;
        group.name = NumberedName(L"ScrollGroup", index);
        group.parentGroup = 0;
        group.pos = -1;
        if (!storage.InsertGroup(group)) {
            Fail(L"insert group failed: " + storage.lastError());
            return 1;
        }

        Group tag;
        tag.name = L"Home";
        tag.parentGroup = group.id;
        tag.pos = -1;
        if (!storage.InsertGroup(tag)) {
            Fail(L"insert group tag failed: " + storage.lastError());
            return 1;
        }
    }

    for (int index = 1; index <= 42; ++index) {
        Group tag;
        tag.name = NumberedName(L"ScrollTag", index);
        tag.parentGroup = firstGroupId;
        tag.pos = -1;
        if (!storage.InsertGroup(tag)) {
            Fail(L"insert tag failed: " + storage.lastError());
            return 1;
        }
    }

    const std::filesystem::path target = appDirectory / L"scroll-target.txt";
    {
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file << "scroll target";
    }

    for (int index = 1; index <= 90; ++index) {
        Link link;
        link.name = NumberedName(L"ScrollLink", index);
        link.parentGroup = firstTagId;
        link.path = target.wstring();
        link.remark = NumberedName(L"Remark", index);
        link.pos = -1;
        link.showCmd = SW_SHOWNORMAL;
        if (!storage.InsertLink(link)) {
            Fail(L"insert link failed: " + storage.lastError());
            return 1;
        }
    }

    AppModel seeded = storage.Load();
    int majorGroups = 0;
    int tags = 0;
    for (const auto& group : seeded.groups) {
        if (group.parentGroup == 0) {
            ++majorGroups;
        } else {
            ++tags;
        }
    }

    std::wcout << L"seed=passed\n";
    std::wcout << L"major_groups=" << majorGroups << L"\n";
    std::wcout << L"tags=" << tags << L"\n";
    std::wcout << L"links=" << seeded.links.size() << L"\n";
    return 0;
}
