#include "../src/Storage.h"
#include "../src/Utilities.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {
std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), output.data(), length, nullptr, nullptr);
    return output;
}

void PrintField(const char* key, const std::wstring& value) {
    std::cout << key << "=" << WideToUtf8(value) << "\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << "usage: QuattroDbProbe <app-directory>\n";
        return 2;
    }

    StorageService storage{std::filesystem::path(argv[1])};
    AppModel model = storage.Load();
    int majorGroups = 0;
    int tags = 0;
    for (const auto& group : model.groups) {
        if (group.parentGroup == 0) {
            ++majorGroups;
        } else {
            ++tags;
        }
    }

    std::cout << "sqlite=" << (storage.sqliteAvailable() ? 1 : 0) << "\n";
    std::cout << "groups=" << model.groups.size() << "\n";
    std::cout << "major_groups=" << majorGroups << "\n";
    std::cout << "tags=" << tags << "\n";
    std::cout << "links=" << model.links.size() << "\n";

    for (const auto& group : model.groups) {
        std::cout << "GROUP"
                  << "\tid=" << group.id
                  << "\tparent=" << group.parentGroup
                  << "\tpos=" << group.pos
                  << "\ttype=" << group.type
                  << "\tsort=" << group.sort
                  << "\tlayout=" << group.layout
                  << "\ticonSize=" << group.iconSize
                  << "\tname=" << WideToUtf8(group.name)
                  << "\n";
    }

    for (const auto& link : model.links) {
        std::cout << "LINK"
                  << "\tid=" << link.id
                  << "\tparent=" << link.parentGroup
                  << "\tpos=" << link.pos
                  << "\ttype=" << link.type
                  << "\trunCount=" << link.runCount
                  << "\tshowCmd=" << link.showCmd
                  << "\tname=" << WideToUtf8(link.name)
                  << "\tpath=" << WideToUtf8(link.path)
                  << "\tremark=" << WideToUtf8(link.remark)
                  << "\n";
    }

    if (!storage.lastError().empty()) {
        PrintField("message", storage.lastError());
    }
    return storage.sqliteAvailable() ? 0 : 1;
}
