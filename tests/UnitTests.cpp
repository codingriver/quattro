#include "../src/Config.h"
#include "../src/Launcher.h"
#include "../src/ShellItemService.h"
#include "../src/Storage.h"
#include "../src/Utilities.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAILED: " << name << "\n";
        ++failures;
    }
}
}

int wmain() {
    Check(Trim(L"  abc \t") == L"abc", "Trim");
    Check(ToLower(L"AbC") == L"abc", "ToLower");
    Check(HasUrlScheme(L"https://example.com"), "HasUrlScheme");
    Check(NormalizeUrl(L"example.com") == L"https://example.com", "NormalizeUrl");
    Check(ParseInt(L"42").value_or(0) == 42, "ParseInt valid");
    Check(!ParseInt(L"42x").has_value(), "ParseInt invalid");
    Check(!Hex8(StablePathHash(L"abc")).empty(), "StablePathHash");

    const std::filesystem::path temp = std::filesystem::temp_directory_path() / L"quattro_unit_conf.ini";
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    ConfigService service(temp);
    AppConfig config;
    config.width = 500;
    config.height = 700;
    config.theme = L"dark";
    config.helpUrl = L"https://help.local/";
    config.updateUrl = L"https://update.local/";
    service.Save(config);

    AppConfig loaded = service.Load();
    Check(loaded.width == 500, "Config width");
    Check(loaded.height == 700, "Config height");
    Check(loaded.theme == L"dark", "Config theme");
    Check(loaded.helpUrl == L"https://help.local/", "Config help url");
    Check(loaded.updateUrl == L"https://update.local/", "Config update url");
    std::filesystem::remove(temp, ec);

    Link urlLink;
    urlLink.type = 2;
    urlLink.path = L"example.com";
    Check(Launcher::IsUrlTarget(urlLink), "Launcher url target by type");

    Link shellLink;
    shellLink.type = 0;
    shellLink.path = L"shell:Downloads";
    Check(Launcher::IsShellTarget(shellLink), "Launcher shell target");
    Check(ShellItemService::IsShellParseName(shellLink.path), "Shell parse name");
    Check(ShellItemService::IsPidlBlobPlausible(std::vector<std::uint8_t>{0, 0}), "PIDL terminator blob");
    Check(!ShellItemService::IsPidlBlobPlausible(std::vector<std::uint8_t>{4, 0, 1}), "PIDL malformed blob");

    Link dirLink;
    dirLink.name = L"My Dir";
    dirLink.path = L"C:\\Temp Dir";
    const std::wstring command = Launcher::BuildOpenDirCommand(L"tool.exe --open {path} --name {name}", dirLink);
    Check(command.find(L"tool.exe --open") != std::wstring::npos, "Launcher command prefix");
    Check(command.find(L"\"C:\\Temp Dir\"") != std::wstring::npos, "Launcher command path");
    Check(command.find(L"\"My Dir\"") != std::wstring::npos, "Launcher command name");

    const std::filesystem::path storageRoot = std::filesystem::temp_directory_path() / L"quattro_storage_unit";
    std::filesystem::remove_all(storageRoot, ec);
    StorageService storage(storageRoot);
    AppModel initial = storage.Load();
    Check(storage.sqliteAvailable(), "Storage sqlite available");
    Check(!initial.groups.empty(), "Storage default groups");

    Group group;
    group.name = L"UnitGroup";
    group.parentGroup = 0;
    group.pos = -1;
    Check(storage.InsertGroup(group) && group.id > 0, "Storage insert group");

    Group tag;
    tag.name = L"UnitTag";
    tag.parentGroup = group.id;
    tag.pos = -1;
    tag.type = 1;
    tag.content = L"all";
    Check(storage.InsertGroup(tag) && tag.id > 0, "Storage insert tag");

    Link link;
    link.name = L"UnitLink";
    link.parentGroup = tag.id;
    link.path = L"https://example.com";
    link.type = 2;
    link.pos = -1;
    link.pidl = {6, 0, 1, 2, 3, 4, 0, 0};
    Check(storage.InsertLink(link) && link.id > 0, "Storage insert link");

    link.remark = L"updated";
    Check(storage.UpdateLink(link), "Storage update link");
    Check(storage.IncrementRunCount(link.id, 5), "Storage run count");

    AppModel loadedModel = storage.Load();
    bool foundLink = false;
    for (const auto& item : loadedModel.links) {
        if (item.id == link.id && item.remark == L"updated" && item.runCount == 5 && item.pidl == link.pidl) {
            foundLink = true;
            break;
        }
    }
    Check(foundLink, "Storage reload link");
    Check(storage.DeleteLink(link.id), "Storage delete link");
    Check(storage.DeleteGroup(group.id), "Storage delete group tree");
    std::filesystem::remove_all(storageRoot, ec);

    if (failures == 0) {
        std::cout << "unit tests complete\n";
    }
    return failures == 0 ? 0 : 1;
}
