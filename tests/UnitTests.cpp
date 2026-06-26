#include "../src/Config.h"
#include "../src/ConfigPackageService.h"
#include "../src/Launcher.h"
#include "../src/MenuCatalog.h"
#include "../src/PluginRegistry.h"
#include "../src/PluginStoreService.h"
#include "../src/ShellItemService.h"
#include "../src/Storage.h"
#include "../src/SystemFunctionDialog.h"
#include "../src/Theme.h"
#include "../src/TodoSchedule.h"
#include "../src/Utilities.h"
#include "../src/WebDavClient.h"

#include <cmath>
#include <filesystem>
#include <fstream>
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

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.002f;
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
    Check(NormalizeTodoTimestamp(L"2026/06/26 09:30") == L"2026-06-26 09:30", "Todo timestamp normalize");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Daily, L"2026-06-26 09:30", L"2026-06-26 10:00") == L"2026-06-27 09:30", "Todo daily next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Monthly, L"2024-01-31 09:00", L"2024-02-01 00:00") == L"2024-02-29 09:00", "Todo monthly month end");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Yearly, L"2024-02-29 09:00", L"2025-01-01 00:00") == L"2025-02-28 09:00", "Todo yearly leap day");

    const std::filesystem::path temp = std::filesystem::temp_directory_path() / L"quattro_unit_conf.ini";
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    ConfigService service(temp);
    AppConfig config;
    config.width = 500;
    config.height = 700;
    config.theme = L"default";
    config.helpUrl = L"https://help.local/";
    config.updateUrl = L"https://update.local/";
    config.pluginStoreUrl = L"https://plugins.local/index.json";
    config.webDavEnabled = true;
    config.webDavUrl = L"https://dav.local/remote.php/dav/files/unit/";
    config.webDavRemotePath = L"/Quattro/backups/";
    config.webDavUserName = L"unit";
    config.webDavKeepCount = 7;
    service.Save(config);

    AppConfig loaded = service.Load();
    Check(loaded.width == 500, "Config width");
    Check(loaded.height == 700, "Config height");
    Check(loaded.theme == L"default", "Config theme");
    Check(loaded.helpUrl == L"https://help.local/", "Config help url");
    Check(loaded.updateUrl == L"https://update.local/", "Config update url");
    Check(loaded.pluginStoreUrl == L"https://plugins.local/index.json", "Config plugin store url");
    Check(loaded.webDavEnabled, "Config webdav enabled");
    Check(loaded.webDavUrl == L"https://dav.local/remote.php/dav/files/unit/", "Config webdav url");
    Check(loaded.webDavRemotePath == L"/Quattro/backups/", "Config webdav remote path");
    Check(loaded.webDavUserName == L"unit", "Config webdav user");
    Check(loaded.webDavKeepCount == 7, "Config webdav keep count");
    const std::wstring savedConfigText = LoadUtf8File(temp);
    Check(savedConfigText.find(L"password") == std::wstring::npos && savedConfigText.find(L"Password") == std::wstring::npos, "Config does not persist webdav password");
    std::filesystem::remove(temp, ec);

    const std::filesystem::path themeRoot = std::filesystem::temp_directory_path() / L"quattro_theme_unit";
    std::filesystem::remove_all(themeRoot, ec);
    std::filesystem::create_directories(themeRoot, ec);
    {
        std::ofstream themeFile(themeRoot / L"custom.xml", std::ios::binary | std::ios::trunc);
        themeFile << R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Theme id="custom" displayName="Custom" version="2">
    <Palette>
        <Color name="accent" value="rgb(1,2,3)"/>
        <Color name="danger" value="@accent"/>
    </Palette>
    <Component name="title">
        <State name="normal" text="@accent"/>
    </Component>
    <Component name="titleCloseButton">
        <State name="hover" icon="@danger"/>
    </Component>
    <Component name="scrollbar">
        <Metric name="thickness" value="7.5"/>
    </Component>
    <Component name="label">
        <State name="normal" text="rgb(4,5,6)"/>
    </Component>
    <Component name="list">
        <Metric name="radius" value="9.5"/>
        <State name="normal" bg="rgb(7,8,9)" text="@accent"/>
    </Component>
    <Component name="settings.searchInput">
        <Metric name="height" value="99"/>
        <State name="normal" text="rgb(250,1,1)"/>
    </Component>
</Theme>
)xml";
    }
    const Theme customTheme = Theme::Load(themeRoot, L"custom");
    const Color accent = customTheme.color(L"title", L"normal", L"text");
    const Color closeHot = customTheme.color(L"titleCloseButton", L"hover", L"icon");
    Check(Near(accent.r, 1.0f / 255.0f) && Near(accent.g, 2.0f / 255.0f) && Near(accent.b, 3.0f / 255.0f), "Theme rgb parse");
    Check(Near(closeHot.r, accent.r) && Near(closeHot.g, accent.g) && Near(closeHot.b, accent.b), "Theme color reference");
    Check(Near(customTheme.metric(L"scrollbar", L"thickness", 0.0f), 7.5f), "Theme metric parse");
    Check(Near(customTheme.color(L"label", L"normal", L"text").r, 4.0f / 255.0f), "Theme label component parse");
    Check(Near(customTheme.metric(L"list", L"radius", 0.0f), 9.5f), "Theme list metric parse");
    Check(Near(customTheme.color(L"list", L"normal", L"text").r, accent.r), "Theme list component color reference");
    Check(Near(customTheme.metric(L"settings.searchInput", L"height", 11.0f), 11.0f), "Theme ignores unsupported component metric");
    Check(!Near(customTheme.color(L"settings.searchInput", L"normal", L"text").r, 250.0f / 255.0f), "Theme ignores unsupported component color");
    Check(customTheme.color(L"titleButton", L"hover", L"bg").a > 0.9f, "Theme default component state");
    const Theme fallbackTheme = Theme::Load(themeRoot, L"missing");
    Check(fallbackTheme.color(L"button", L"hover", L"bg").a > 0.9f, "Theme default button hover");
    Check(fallbackTheme.color(L"edit", L"readonly", L"border").a > 0.9f, "Theme default edit readonly");
    Check(fallbackTheme.color(L"comboBox", L"selected", L"itemBg").a > 0.9f, "Theme default combo selected");
    Check(fallbackTheme.color(L"label", L"normal", L"text").a > 0.9f, "Theme default label");
    Check(fallbackTheme.color(L"list", L"normal", L"bg").a > 0.9f, "Theme default list");
    Check(Near(fallbackTheme.metric(L"toggle", L"height", 0.0f), 22.0f), "Theme default toggle metric");
    Check(Near(fallbackTheme.metric(L"slider", L"thumbSize", 0.0f), 14.0f), "Theme default slider metric");
    Check(fallbackTheme.color(L"global", L"warning", L"text").r > 0.5f, "Theme default semantic warning");
    Check(Near(fallbackTheme.metric(L"global", L"fieldHeight", 0.0f), 32.0f), "Theme default global metric");
    std::filesystem::remove_all(themeRoot, ec);

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

    const auto systemFunctions = SystemFunctions();
    Check(systemFunctions.size() >= 30, "System function menu count");
    bool hasComputer = false;
    bool hasStartupFolders = false;
    bool hasPowerActions = false;
    bool hasAudioActions = false;
    bool genericCommandActionsHaveIconOverrides = true;
    for (std::size_t i = 0; i < systemFunctions.size(); ++i) {
        if (std::wstring(systemFunctions[i].name) == L"我的电脑") {
            hasComputer = true;
            Link systemLink;
            Check(ConfigureSystemFunctionLink(i, systemLink), "System function configure computer");
            Check(systemLink.name == L"我的电脑", "System function computer name");
            Check(systemLink.type == 3, "System function computer type");
            Check(systemLink.path == L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", "System function computer path");
            Check(systemLink.parameter.empty(), "System function computer parameter");
        }
        if (std::wstring(systemFunctions[i].name) == L"系统自启目录" ||
            std::wstring(systemFunctions[i].name) == L"用户自启目录") {
            hasStartupFolders = true;
        }
        if (std::wstring(systemFunctions[i].name) == L"关机" ||
            std::wstring(systemFunctions[i].name) == L"重启" ||
            std::wstring(systemFunctions[i].name) == L"锁定") {
            hasPowerActions = true;
            genericCommandActionsHaveIconOverrides = genericCommandActionsHaveIconOverrides &&
                (systemFunctions[i].stockIcon >= 0 || systemFunctions[i].menuIcon != MenuIconNone);
        }
        if (std::wstring(systemFunctions[i].name) == L"音量+" ||
            std::wstring(systemFunctions[i].name) == L"音量-" ||
            std::wstring(systemFunctions[i].name) == L"静音") {
            hasAudioActions = true;
            genericCommandActionsHaveIconOverrides = genericCommandActionsHaveIconOverrides &&
                (systemFunctions[i].stockIcon >= 0 || systemFunctions[i].menuIcon != MenuIconNone);
            if (std::wstring(systemFunctions[i].name) == L"音量+") {
                Check(systemFunctions[i].menuIcon == MenuIconVolumeUp, "System function volume up icon");
            }
            if (std::wstring(systemFunctions[i].name) == L"音量-") {
                Check(systemFunctions[i].menuIcon == MenuIconVolumeDown, "System function volume down icon");
            }
            if (std::wstring(systemFunctions[i].name) == L"静音") {
                Check(systemFunctions[i].menuIcon == MenuIconVolumeMute, "System function mute icon");
            }
        }
        if (std::wstring(systemFunctions[i].name) == L"关机") {
            Link shutdownLink;
            Check(ConfigureSystemFunctionLink(i, shutdownLink), "System function configure shutdown");
            Check(shutdownLink.path == L"shutdown.exe", "System function shutdown path");
            Check(shutdownLink.parameter == L"/s /t 0", "System function shutdown parameter");
            Check(shutdownLink.showCmd == SW_HIDE, "System function shutdown show cmd");
        }
    }
    Check(hasComputer, "System function contains computer");
    Check(hasStartupFolders, "System function contains startup folders");
    Check(hasPowerActions, "System function contains power actions");
    Check(hasAudioActions, "System function contains audio actions");
    Check(genericCommandActionsHaveIconOverrides, "System function generic command icon overrides");
    Link invalidSystemLink;
    Check(!ConfigureSystemFunctionLink(systemFunctions.size(), invalidSystemLink), "System function invalid index");

    Check(RequiredTopRightMenuVisuals().size() >= 20, "Top-right menu visual coverage count");
    for (const auto& item : RequiredTopRightMenuVisuals()) {
        const MenuIcon actual = static_cast<MenuIcon>(MenuIconFor(item.command, item.text));
        Check(MenuIconIsRenderable(actual), "Top-right menu icon is renderable");
        Check(actual == item.expectedIcon, "Top-right menu icon matches catalog");
        Check(std::wstring(MenuIconName(actual)) != L"unknown", "Top-right menu icon has name");
    }
    Check(MenuIconFor(ID_MENU_THEME_BASE + 5, L"custom") == MenuIconTheme, "Theme menu dynamic icon");
    Check(MenuIconFor(ID_MENU_SYSTEM_FUNCTION_BASE + 35, L"静音") == MenuIconSystem, "System function dynamic icon");
    Check(MenuIconFor(ID_MENU_MOVE_TO_BASE + 12, L"目标标签") == MenuIconMove, "Move target dynamic icon");
    Check(MenuIconFor(ID_MENU_COPY_TO_BASE + 12, L"目标标签") == MenuIconCopy, "Copy target dynamic icon");
    Check(MenuIconFor(ID_MENU_MOVE_TAG_TO_BASE + 12, L"目标分组") == MenuIconMove, "Move tag target dynamic icon");
    Check(MenuIconFor(ID_MENU_CUT_LINK, L"剪切") == MenuIconCut, "Cut command icon");
    Check(MenuIconFor(ID_MENU_PASTE_LINK, L"粘贴") == MenuIconPaste, "Paste command icon");
    Check(MenuIconFor(ID_MENU_UPLOAD_WEBDAV_BACKUP, L"上传 WebDAV 备份") == MenuIconCopy, "WebDAV upload command icon");
    Check(MenuIconFor(ID_MENU_DOWNLOAD_WEBDAV_BACKUP, L"下载 WebDAV 备份") == MenuIconPaste, "WebDAV download command icon");
    Check(MenuIconFor(ID_MENU_ADD_NOTE_TAG, L"新建便签标签页") == MenuIconNotebook, "Note tag command icon");
    Check(MenuIconFor(ID_MENU_ADD_TODO_ITEM, L"新增待办事项") == MenuIconList, "Todo item command icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"禁用待办事项") == MenuIconEyeOff, "Todo disable command icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"启用待办事项") == MenuIconEye, "Todo enable command icon");
    Check(MenuIconFor(ID_MENU_TODO_SORT_DUE, L"按提醒时间") == MenuIconSort, "Todo sort command icon");
    Check(MenuIconFor(ID_MENU_PLUGIN_STORE, L"插件商店") == MenuIconTools, "Plugin store command icon");
    Check(MenuIconFor(ID_MENU_TOOL_BASE + 2, L"秒表") == MenuIconTools, "Builtin tool dynamic icon");
    Check(MenuIconFor(ID_MENU_ALL_LAYOUT_LIST, L"列表") == MenuIconList, "List layout icon");
    Check(MenuIconFor(ID_MENU_ALL_LAYOUT_TILE, L"平铺") == MenuIconTile, "Tile layout icon");
    Check(std::filesystem::exists(L"icons/menu/tabler/tabler-icons.ttf"), "Local menu icon font exists");
    Check(std::filesystem::exists(L"icons/menu/tabler/tabler-icons.css"), "Local menu icon css exists");
    Check(std::filesystem::exists(L"icons/menu/tabler/LICENSE"), "Local menu icon license exists");

    const std::filesystem::path pluginRoot = std::filesystem::temp_directory_path() / L"quattro_plugin_unit";
    std::filesystem::remove_all(pluginRoot, ec);
    PluginRegistry pluginRegistry(pluginRoot);
    Check(pluginRegistry.Initialize(), "Plugin registry initialize");
    auto plugins = pluginRegistry.LoadPlugins();
    Check(plugins.size() >= 3, "Plugin registry builtin count");
    bool hasAutoClickerName = false;
    for (const auto& plugin : plugins) {
        if (plugin.id == L"quattro.builtin.clicker" && plugin.name == L"连点器") {
            hasAutoClickerName = true;
        }
    }
    Check(hasAutoClickerName, "Plugin clicker display name");
    Check(!pluginRegistry.IsEnabled(L"quattro.builtin.clicker"), "Plugin clicker disabled by default");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.timer"), "Plugin timer enabled by default");
    Check(pluginRegistry.SetEnabled(L"quattro.builtin.clicker", true), "Plugin enable clicker");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.clicker"), "Plugin clicker enabled persisted");
    Check(pluginRegistry.SetSetting(L"quattro.builtin.clicker", L"interval", L"250"), "Plugin setting save");
    Check(pluginRegistry.GetSetting(L"quattro.builtin.clicker", L"interval") == L"250", "Plugin setting load");
    PluginRegistry reloadedPluginRegistry(pluginRoot);
    Check(reloadedPluginRegistry.IsEnabled(L"quattro.builtin.clicker"), "Plugin registry reload enabled");
    std::filesystem::remove_all(pluginRoot, ec);

    const std::filesystem::path storeRoot = std::filesystem::temp_directory_path() / L"quattro_store_unit";
    std::filesystem::remove_all(storeRoot, ec);
    std::filesystem::create_directories(storeRoot / L"plugins" / L"store", ec);
    {
        std::ofstream storeFile(storeRoot / L"plugins" / L"store" / L"index.json", std::ios::binary | std::ios::trunc);
        storeFile << R"json({
  "schema": 1,
  "plugins": [
    {
      "id": "quattro.test.linkpack",
      "name": "Unit Link Pack",
      "version": "1.0.0",
      "category": "link-pack",
      "kind": "link-pack",
      "description": "Unit test package",
      "permissions": ["create-groups", "create-links"],
      "groups": [
        {
          "name": "Unit Store Group",
          "tags": [
            {
              "name": "Unit Store Tag",
              "links": [
                {
                  "name": "Unit Store Link",
                  "path": "https://example.com",
                  "type": 2
                }
              ]
            }
          ]
        }
      ]
    }
  ]
})json";
    }
    PluginRegistry storeRegistry(storeRoot);
    StorageService storeStorage(storeRoot);
    PluginStoreService storeService(storeRoot, L"");
    Check(storeService.Refresh(storeRegistry), "Plugin store refresh local index");
    const bool installedStorePlugin = storeService.Install(L"quattro.test.linkpack", storeRegistry, storeStorage);
    if (!installedStorePlugin) {
        std::wcerr << L"Plugin store install error: " << storeService.lastError() << L"\n";
    }
    Check(installedStorePlugin, "Plugin store install link pack");
    AppModel storeModel = storeStorage.Load();
    bool foundStoreLink = false;
    for (const auto& item : storeModel.links) {
        if (item.name == L"Unit Store Link" && item.path == L"https://example.com") {
            foundStoreLink = true;
            break;
        }
    }
    Check(foundStoreLink, "Plugin store installed link");
    Check(storeRegistry.IsEnabled(L"quattro.test.linkpack"), "Plugin store installed plugin enabled");
    Check(!storeRegistry.LoadContributions(L"quattro.test.linkpack").empty(), "Plugin store contributions recorded");
    Check(storeService.Remove(L"quattro.test.linkpack", storeRegistry, storeStorage), "Plugin store remove link pack");
    AppModel removedStoreModel = storeStorage.Load();
    bool removedStoreLinkStillExists = false;
    for (const auto& item : removedStoreModel.links) {
        removedStoreLinkStillExists = removedStoreLinkStillExists || item.name == L"Unit Store Link";
    }
    Check(!removedStoreLinkStillExists, "Plugin store remove clears link");
    Check(!storeRegistry.IsEnabled(L"quattro.test.linkpack"), "Plugin store removed plugin disabled");
    std::filesystem::remove_all(storeRoot, ec);

    Check(WebDavClient::CombineRemotePath(L"/Quattro/backups/", L"file.q4cfg") == L"/Quattro/backups/file.q4cfg", "WebDAV remote path combine");
#ifdef QUATTRO_WITH_WEBDAV_LIBS
    const std::string propFindXml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:">
  <d:response>
    <d:href>/remote.php/dav/files/unit/Quattro/backups/</d:href>
    <d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat>
  </d:response>
  <d:response>
    <d:href>/remote.php/dav/files/unit/Quattro/backups/quattro-backup-20260626-153012.q4cfg</d:href>
    <d:propstat><d:prop><d:getcontentlength>12345</d:getcontentlength><d:getlastmodified>Fri, 26 Jun 2026 07:30:12 GMT</d:getlastmodified></d:prop></d:propstat>
  </d:response>
</d:multistatus>)xml";
    const auto davFiles = WebDavClient::ParsePropFindResponse(propFindXml);
    Check(davFiles.size() == 2, "WebDAV PROPFIND parse count");
    Check(davFiles[0].collection, "WebDAV PROPFIND collection");
    Check(davFiles[1].name == L"quattro-backup-20260626-153012.q4cfg", "WebDAV PROPFIND file name");
    Check(davFiles[1].size == 12345, "WebDAV PROPFIND file size");
#endif

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

    Group noteTag;
    noteTag.name = L"UnitNote";
    noteTag.parentGroup = group.id;
    noteTag.pos = -1;
    noteTag.type = 3;
    noteTag.content = L"note";
    Check(storage.InsertGroup(noteTag) && noteTag.id > 0, "Storage insert note tag");
    Check(storage.SaveNotePage(noteTag.id, L"line1\nline2"), "Storage save note");

    Group todoTag;
    todoTag.name = L"UnitTodo";
    todoTag.parentGroup = group.id;
    todoTag.pos = -1;
    todoTag.type = 4;
    todoTag.content = L"todoItems";
    Check(storage.InsertGroup(todoTag) && todoTag.id > 0, "Storage insert todo tag");

    TodoItem todo;
    todo.tagId = todoTag.id;
    todo.title = L"UnitTodoItem";
    todo.content = L"details";
    todo.scheduleKind = TodoScheduleKind::Once;
    todo.anchorAt = L"2026-06-26 09:30";
    todo.pos = -1;
    Check(storage.InsertTodoItem(todo) && todo.id > 0 && todo.nextDueAt == L"2026-06-26 09:30" && todo.enabled, "Storage insert todo");
    todo.content = L"updated details";
    Check(storage.UpdateTodoItem(todo), "Storage update todo");
    Check(storage.SetTodoEnabled(todo.id, false), "Storage disable todo");
    Check(storage.SetTodoCompleted(todo.id, true), "Storage complete todo");

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
    bool foundNote = false;
    for (const auto& item : loadedModel.notes) {
        if (item.tagId == noteTag.id && item.content == L"line1\nline2") {
            foundNote = true;
            break;
        }
    }
    Check(foundNote, "Storage reload note");
    bool foundTodo = false;
    for (const auto& item : loadedModel.todos) {
        if (item.id == todo.id && item.content == L"updated details" && !item.enabled && !item.completedAt.empty()) {
            foundTodo = true;
            break;
        }
    }
    Check(foundTodo, "Storage reload todo");
    Check(storage.SetTodoEnabled(todo.id, true), "Storage enable todo");
    Check(storage.SetTodoCompleted(todo.id, false), "Storage reopen todo");
    Check(storage.DeleteLink(link.id), "Storage delete link");
    Check(storage.DeleteGroup(group.id), "Storage delete group tree");
    AppModel afterDelete = storage.Load();
    bool cascadeLeftovers = false;
    for (const auto& item : afterDelete.notes) {
        cascadeLeftovers = cascadeLeftovers || item.tagId == noteTag.id;
    }
    for (const auto& item : afterDelete.todos) {
        cascadeLeftovers = cascadeLeftovers || item.tagId == todoTag.id;
    }
    Check(!cascadeLeftovers, "Storage delete group clears notes and todos");
    std::filesystem::remove_all(storageRoot, ec);

    const std::filesystem::path packageSourceRoot = std::filesystem::temp_directory_path() / L"quattro_package_source_unit";
    const std::filesystem::path packageTargetRoot = std::filesystem::temp_directory_path() / L"quattro_package_target_unit";
    const std::filesystem::path packagePath = std::filesystem::temp_directory_path() / L"quattro_package_unit.q4cfg";
    std::filesystem::remove_all(packageSourceRoot, ec);
    std::filesystem::remove_all(packageTargetRoot, ec);
    std::filesystem::remove(packagePath, ec);
    std::filesystem::create_directories(packageSourceRoot / L"icons" / L"url", ec);
    std::filesystem::create_directories(packageSourceRoot / L"theme", ec);
    {
        ConfigService sourceConfig(packageSourceRoot / L"conf.ini");
        AppConfig sourceAppConfig;
        sourceAppConfig.width = 777;
        sourceAppConfig.theme = L"sourceTheme";
        sourceConfig.Save(sourceAppConfig);
        std::ofstream icon(packageSourceRoot / L"icons" / L"url" / L"example.png", std::ios::binary | std::ios::trunc);
        icon << "png";
        std::ofstream theme(packageSourceRoot / L"theme" / L"source.xml", std::ios::binary | std::ios::trunc);
        theme << "<Theme id=\"source\"/>";
    }
    StorageService sourceStorage(packageSourceRoot);
    sourceStorage.Load();
    Group sourceGroup;
    sourceGroup.name = L"MergeGroup";
    sourceGroup.parentGroup = 0;
    sourceGroup.pos = -1;
    Check(sourceStorage.InsertGroup(sourceGroup), "Package source group insert");
    Group sourceTag;
    sourceTag.name = L"MergeTag";
    sourceTag.parentGroup = sourceGroup.id;
    sourceTag.pos = -1;
    Check(sourceStorage.InsertGroup(sourceTag), "Package source tag insert");
    Group sourceNoteTag;
    sourceNoteTag.name = L"PackageNote";
    sourceNoteTag.parentGroup = sourceGroup.id;
    sourceNoteTag.type = 3;
    sourceNoteTag.content = L"note";
    sourceNoteTag.pos = -1;
    Check(sourceStorage.InsertGroup(sourceNoteTag), "Package source note tag insert");
    Check(sourceStorage.SaveNotePage(sourceNoteTag.id, L"package note"), "Package source note save");
    Group sourceTodoTag;
    sourceTodoTag.name = L"PackageTodo";
    sourceTodoTag.parentGroup = sourceGroup.id;
    sourceTodoTag.type = 4;
    sourceTodoTag.content = L"todoItems";
    sourceTodoTag.pos = -1;
    Check(sourceStorage.InsertGroup(sourceTodoTag), "Package source todo tag insert");
    TodoItem sourceTodo;
    sourceTodo.tagId = sourceTodoTag.id;
    sourceTodo.title = L"PackageTodoItem";
    sourceTodo.scheduleKind = TodoScheduleKind::Once;
    sourceTodo.anchorAt = L"2026-06-26 09:30";
    sourceTodo.pos = -1;
    Check(sourceStorage.InsertTodoItem(sourceTodo), "Package source todo insert");
    Link sourceLink;
    sourceLink.name = L"PackageLink";
    sourceLink.parentGroup = sourceTag.id;
    sourceLink.path = L"https://package.example";
    sourceLink.type = 2;
    sourceLink.pos = -1;
    sourceLink.runCount = 9;
    Check(sourceStorage.InsertLink(sourceLink), "Package source link insert");
    PluginRegistry sourcePlugins(packageSourceRoot);
    Check(sourcePlugins.Initialize(), "Package source plugin init");
    Check(sourcePlugins.SetSetting(L"quattro.builtin.clicker", L"interval", L"250"), "Package source plugin existing setting");
    Check(sourcePlugins.SetSetting(L"quattro.builtin.clicker", L"x", L"42"), "Package source plugin new setting");

    StorageService targetStorage(packageTargetRoot);
    targetStorage.Load();
    Group targetGroup;
    targetGroup.name = L"MergeGroup";
    targetGroup.parentGroup = 0;
    targetGroup.pos = -1;
    Check(targetStorage.InsertGroup(targetGroup), "Package target group insert");
    Group targetTag;
    targetTag.name = L"MergeTag";
    targetTag.parentGroup = targetGroup.id;
    targetTag.pos = -1;
    Check(targetStorage.InsertGroup(targetTag), "Package target tag insert");
    Link targetLink;
    targetLink.name = L"ExistingLink";
    targetLink.parentGroup = targetTag.id;
    targetLink.path = L"https://target.example";
    targetLink.type = 2;
    targetLink.pos = -1;
    Check(targetStorage.InsertLink(targetLink), "Package target link insert");
    PluginRegistry targetPlugins(packageTargetRoot);
    Check(targetPlugins.Initialize(), "Package target plugin init");
    Check(targetPlugins.SetSetting(L"quattro.builtin.clicker", L"interval", L"100"), "Package target plugin existing setting");

    ConfigPackageService packageExporter(packageSourceRoot);
    ConfigPackageOptions packageOptions;
    ConfigPackageReport exportReport = packageExporter.ExportPackage(packagePath, packageOptions);
    Check(exportReport.ok && FileExists(packagePath), "Package export succeeds");

    ConfigPackageService packageImporter(packageTargetRoot);
    ConfigPackageReport importReport = packageImporter.ImportPackageMerge(packagePath, packageOptions);
    Check(importReport.ok, "Package merge import succeeds");
    Check(importReport.groupsAdded >= 2, "Package merge import groups");
    Check(importReport.tagsAdded >= 3, "Package merge import tags");
    Check(importReport.linksAdded == 1, "Package merge import link count");
    Check(importReport.notesAdded == 1, "Package merge import note count");
    Check(importReport.todosAdded == 1, "Package merge import todo count");
    Check(importReport.urlIconsAdded == 1, "Package merge import url icon count");

    AppModel mergedModel = targetStorage.Load();
    bool hasExistingLink = false;
    bool hasImportedLink = false;
    bool importedLinkParentExists = false;
    bool hasRenamedGroup = false;
    for (const auto& item : mergedModel.groups) {
        if (item.parentGroup == 0 && item.name == L"MergeGroup (导入)") {
            hasRenamedGroup = true;
        }
    }
    for (const auto& item : mergedModel.links) {
        if (item.name == L"ExistingLink" && item.path == L"https://target.example") {
            hasExistingLink = true;
        }
        if (item.name == L"PackageLink" && item.path == L"https://package.example" && item.runCount == 9) {
            hasImportedLink = true;
            for (const auto& groupItem : mergedModel.groups) {
                importedLinkParentExists = importedLinkParentExists || groupItem.id == item.parentGroup;
            }
        }
    }
    bool hasImportedNote = false;
    for (const auto& item : mergedModel.notes) {
        hasImportedNote = hasImportedNote || item.content == L"package note";
    }
    bool hasImportedTodo = false;
    for (const auto& item : mergedModel.todos) {
        hasImportedTodo = hasImportedTodo || item.title == L"PackageTodoItem";
    }
    Check(hasExistingLink, "Package merge keeps existing link");
    Check(hasImportedLink, "Package merge imports link");
    Check(importedLinkParentExists, "Package merge remaps link parent");
    Check(hasRenamedGroup, "Package merge renames conflicting group");
    Check(hasImportedNote, "Package merge imports note");
    Check(hasImportedTodo, "Package merge imports todo");
    Check(targetPlugins.GetSetting(L"quattro.builtin.clicker", L"interval") == L"100", "Package merge keeps plugin setting conflict");
    Check(targetPlugins.GetSetting(L"quattro.builtin.clicker", L"x") == L"42", "Package merge adds plugin setting");
    Check(FileExists(packageTargetRoot / L"icons" / L"url" / L"example.png"), "Package merge imports url icon");
    Check(!FileExists(packageTargetRoot / L"theme" / L"source.xml"), "Package merge does not import theme");
    Check(DirectoryExists(packageTargetRoot / L"backups"), "Package merge creates backup");
    std::filesystem::remove_all(packageSourceRoot, ec);
    std::filesystem::remove_all(packageTargetRoot, ec);
    std::filesystem::remove(packagePath, ec);

    if (failures == 0) {
        std::cout << "unit tests complete\n";
    }
    return failures == 0 ? 0 : 1;
}
