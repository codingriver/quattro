#include "MenuCatalog.h"

#include "TablerIconManifest.h"

#include <array>
#include <cwchar>

namespace {
constexpr const wchar_t* kMenuIconLinkPrefix = L"#menu:";

constexpr std::array<MenuVisualRequirement, 27> kTopRightMenuVisuals{{
    {ID_MENU_QUICK_IMPORT, L"快速导入", MenuIconPaste},
    {ID_MENU_TOGGLE_TITLE, L"隐藏标题栏", MenuIconEyeOff},
    {ID_MENU_TOGGLE_GROUP, L"隐藏分组", MenuIconEyeOff},
    {ID_MENU_TOGGLE_TAG, L"隐藏标签", MenuIconEyeOff},
    {ID_MENU_TOGGLE_TOPMOST, L"置顶", MenuIconPin},
    {ID_MENU_THEME_BASE, L"皮肤", MenuIconTheme},
    {ID_MENU_SETTINGS, L"设置", MenuIconSettings},
    {ID_MENU_REFRESH_ALL_ICONS, L"刷新", MenuIconRefresh},
    {ID_MENU_CLEAR_ICON_CACHE, L"清理图标缓存", MenuIconClear},
    {ID_MENU_SYSTEM_FUNCTION_BASE, L"系统功能", MenuIconSystem},
    {ID_MENU_TOOL_BASE, L"工具箱", MenuIconTools},
    {ID_MENU_ALL_ICON_SMALL, L"小图标", MenuIconSize},
    {ID_MENU_ALL_ICON_MEDIUM, L"中图标", MenuIconSize},
    {ID_MENU_ALL_ICON_LARGE, L"大图标", MenuIconSize},
    {ID_MENU_ALL_LAYOUT_LIST, L"列表", MenuIconList},
    {ID_MENU_ALL_LAYOUT_TILE, L"平铺", MenuIconTile},
    {ID_MENU_ALL_SORT_POS, L"手动排序", MenuIconSort},
    {ID_MENU_ALL_SORT_RUNCOUNT, L"按运行次数", MenuIconSort},
    {ID_MENU_ALL_SORT_NAME, L"按名称", MenuIconSort},
    {0, L"统一图标大小", MenuIconSize},
    {0, L"统一查看方式", MenuIconView},
    {0, L"统一排序方式", MenuIconSort},
    {ID_MENU_HELP, L"帮助说明", MenuIconHelp},
    {ID_MENU_CHECK_UPDATE, L"检查更新", MenuIconDownload},
    {ID_MENU_EXIT, L"关闭退出", MenuIconExit},
    {ID_MENU_FAQ, L"常见问题", MenuIconHelp},
    {ID_MENU_REWARD, L"赞赏支持", MenuIconReward},
}};

const wchar_t* MenuIconStorageName(MenuIcon icon) {
    return MenuIconName(icon);
}

MenuIcon MenuIconFromStorageName(const std::wstring& name) {
    for (int value = MenuIconNone + 1; value <= MenuIconDownload; ++value) {
        const auto icon = static_cast<MenuIcon>(value);
        if (name == MenuIconStorageName(icon)) {
            return icon;
        }
    }
    return MenuIconNone;
}
}

int MenuIconFor(UINT_PTR id, const std::wstring& text) {
    if (id >= ID_MENU_SYSTEM_FUNCTION_BASE && id < ID_MENU_SYSTEM_FUNCTION_BASE + ID_MENU_SYSTEM_FUNCTION_LIMIT) {
        return MenuIconSystem;
    }
    if (id >= ID_MENU_TOOL_BASE && id < ID_MENU_TOOL_BASE + ID_MENU_TOOL_LIMIT) {
        return MenuIconTools;
    }
    if (id >= ID_MENU_THEME_BASE && id < ID_MENU_THEME_BASE + 100) {
        return MenuIconTheme;
    }
    if (id >= ID_MENU_MOVE_TO_BASE && id < ID_MENU_MOVE_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
        return MenuIconMove;
    }
    if (id >= ID_MENU_COPY_TO_BASE && id < ID_MENU_COPY_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
        return MenuIconCopy;
    }
    if (id >= ID_MENU_MOVE_TAG_TO_BASE && id < ID_MENU_MOVE_TAG_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
        return MenuIconMove;
    }
    if (id >= ID_MENU_BUILTIN_SYSTEM_ACTION_BASE && id < ID_MENU_BUILTIN_SYSTEM_ACTION_BASE + ID_MENU_BUILTIN_SYSTEM_ACTION_LIMIT) {
        return MenuIconSystem;
    }

    switch (id) {
    case ID_MENU_ADD_LINK: return MenuIconFile;
    case ID_MENU_ADD_FILE: return MenuIconFile;
    case ID_MENU_ADD_FOLDER: return MenuIconFolder;
    case ID_MENU_ADD_URL: return MenuIconUrl;
    case ID_MENU_RUN_ADMIN: return MenuIconShield;
    case ID_MENU_RUN_PRIVATE: return MenuIconUrl;
    case ID_MENU_OPEN_LOCATION: return MenuIconOpenFolder;
    case ID_MENU_COPY_URL: return MenuIconUrl;
    case ID_MENU_WINDOWS_CONTEXT: return MenuIconWindows;
    case ID_MENU_CREATE_DESKTOP_SHORTCUT: return MenuIconShortcut;
    case ID_MENU_REFRESH_LINK_ICON:
    case ID_MENU_REFRESH_PAGE_ICONS:
    case ID_MENU_REFRESH_ALL_ICONS:
    case ID_MENU_REFRESH_GROUP_LINKS:
    case ID_MENU_REPAIR_LINK: return MenuIconRefresh;
    case ID_MENU_CLEAR_ICON_CACHE: return MenuIconClear;
    case ID_MENU_MOVE_UP:
    case ID_MENU_MOVE_DOWN: return MenuIconMove;
    case ID_MENU_COPY_LINK:
    case ID_MENU_COPY_PATH:
    case ID_MENU_EXPORT_CONFIG: return MenuIconCopy;
    case ID_MENU_CUT_LINK: return MenuIconCut;
    case ID_MENU_PASTE_LINK:
    case ID_MENU_IMPORT_CLIPBOARD:
    case ID_MENU_QUICK_IMPORT:
    case ID_MENU_IMPORT_CONFIG_MERGE: return MenuIconPaste;
    case ID_MENU_EDIT_LINK:
    case ID_MENU_EDIT_GROUP:
    case ID_MENU_EDIT_TAG: return MenuIconEdit;
    case ID_MENU_PROPERTIES: return MenuIconInfo;
    case ID_MENU_ABOUT: return MenuIconAbout;
    case ID_MENU_DELETE_LINK:
    case ID_MENU_DELETE_GROUP:
    case ID_MENU_DELETE_TAG: return MenuIconDelete;
    case ID_MENU_ADD_GROUP: return MenuIconGroup;
    case ID_MENU_ADD_TAG: return MenuIconTag;
    case ID_MENU_ADD_NOTE_TAG: return MenuIconNotebook;
    case ID_MENU_ADD_TODO_TAG:
    case ID_MENU_ADD_TODO_ITEM:
    case ID_MENU_TOGGLE_TODO_DONE: return MenuIconList;
    case ID_MENU_TOGGLE_TODO_ENABLED:
        return text.find(L"禁用") != std::wstring::npos ? MenuIconEyeOff : MenuIconEye;
    case ID_MENU_TODO_REMINDER_VIEWED: return MenuIconEye;
    case ID_MENU_TODO_REMINDER_IGNORE: return MenuIconClear;
    case ID_MENU_TODO_REMINDER_SNOOZE_5:
    case ID_MENU_TODO_REMINDER_SNOOZE_30:
    case ID_MENU_TODO_REMINDER_SNOOZE_60: return MenuIconHistory;
    case ID_MENU_COMPLETE_OVERDUE_TODOS: return MenuIconList;
    case ID_MENU_EDIT_TODO_ITEM: return MenuIconEdit;
    case ID_MENU_DELETE_TODO_ITEM: return MenuIconDelete;
    case ID_MENU_CLEAR_DONE_TODOS: return MenuIconClear;
    case ID_MENU_CLEAR_TAG_LINKS: return MenuIconClear;
    case ID_MENU_EXIT: return MenuIconExit;
    case ID_MENU_RUN_LINK: return MenuIconRun;
    case ID_MENU_TOGGLE_TOPMOST: return MenuIconPin;
    case ID_MENU_SETTINGS: return MenuIconSettings;
    case ID_MENU_HELP:
    case ID_MENU_FAQ: return MenuIconHelp;
    case ID_MENU_CHECK_UPDATE: return MenuIconDownload;
    case ID_MENU_REWARD: return MenuIconReward;
    case ID_MENU_RESET_LAYOUT: return MenuIconView;
    case ID_MENU_LAYOUT_LIST:
    case ID_MENU_ALL_LAYOUT_LIST: return MenuIconList;
    case ID_MENU_LAYOUT_TILE:
    case ID_MENU_ALL_LAYOUT_TILE: return MenuIconTile;
    case ID_MENU_ICON_SMALL:
    case ID_MENU_ICON_MEDIUM:
    case ID_MENU_ICON_LARGE:
    case ID_MENU_ALL_ICON_SMALL:
    case ID_MENU_ALL_ICON_MEDIUM:
    case ID_MENU_ALL_ICON_LARGE: return MenuIconSize;
    case ID_MENU_SORT_POS:
    case ID_MENU_SORT_RUNCOUNT:
    case ID_MENU_SORT_NAME:
    case ID_MENU_TODO_SORT_DUE:
    case ID_MENU_TODO_SORT_CREATED:
    case ID_MENU_TODO_SORT_TITLE:
    case ID_MENU_TODO_SORT_STATUS:
    case ID_MENU_ALL_SORT_POS:
    case ID_MENU_ALL_SORT_RUNCOUNT:
    case ID_MENU_ALL_SORT_NAME: return MenuIconSort;
    case ID_MENU_TOGGLE_TITLE:
    case ID_MENU_TOGGLE_GROUP:
    case ID_MENU_TOGGLE_TAG: return MenuIconEyeOff;
    case ID_MENU_SHOW: return MenuIconView;
    default:
        break;
    }

    if (text == L"移动到" || text == L"移动到标签") return MenuIconMove;
    if (text == L"复制到" || text == L"复制到标签") return MenuIconCopy;
    if (text == L"图标大小" || text == L"统一图标大小") return MenuIconSize;
    if (text == L"查看方式" || text == L"统一查看方式") return MenuIconView;
    if (text == L"排序方式" || text == L"统一排序方式" || text == L"手动排序") return MenuIconSort;
    if (text == L"列表") return MenuIconList;
    if (text == L"平铺") return MenuIconTile;
    if (text == L"系统功能") return MenuIconSystem;
    if (text == L"工具箱") return MenuIconTools;
    if (text == L"主题" || text == L"皮肤") return MenuIconTheme;
    if (text == L"无可选分组" || text == L"无可选标签" || text == L"无可用功能") return MenuIconInfo;
    return MenuIconNone;
}

const wchar_t* MenuIconName(MenuIcon icon) {
    switch (icon) {
    case MenuIconNone: return L"none";
    case MenuIconFile: return L"file";
    case MenuIconFolder: return L"folder";
    case MenuIconUrl: return L"url";
    case MenuIconSystem: return L"system";
    case MenuIconShield: return L"shield";
    case MenuIconOpenFolder: return L"open-folder";
    case MenuIconWindows: return L"windows";
    case MenuIconShortcut: return L"shortcut";
    case MenuIconRefresh: return L"refresh";
    case MenuIconMove: return L"move";
    case MenuIconCopy: return L"copy";
    case MenuIconCut: return L"cut";
    case MenuIconPaste: return L"paste";
    case MenuIconEdit: return L"edit";
    case MenuIconInfo: return L"info";
    case MenuIconDelete: return L"delete";
    case MenuIconGroup: return L"group";
    case MenuIconTag: return L"tag";
    case MenuIconTheme: return L"theme";
    case MenuIconSize: return L"size";
    case MenuIconView: return L"view";
    case MenuIconList: return L"list";
    case MenuIconTile: return L"tile";
    case MenuIconSort: return L"sort";
    case MenuIconSortAsc: return L"sort-asc";
    case MenuIconSortDesc: return L"sort-desc";
    case MenuIconClear: return L"clear";
    case MenuIconEye: return L"eye";
    case MenuIconEyeOff: return L"eye-off";
    case MenuIconAbout: return L"about";
    case MenuIconExit: return L"exit";
    case MenuIconRun: return L"run";
    case MenuIconPin: return L"pin";
    case MenuIconPinOff: return L"pin-off";
    case MenuIconSettings: return L"settings";
    case MenuIconHelp: return L"help";
    case MenuIconReward: return L"reward";
    case MenuIconPower: return L"power";
    case MenuIconRestart: return L"restart";
    case MenuIconLogout: return L"logout";
    case MenuIconLock: return L"lock";
    case MenuIconSleep: return L"sleep";
    case MenuIconMonitor: return L"monitor";
    case MenuIconVolumeUp: return L"volume-up";
    case MenuIconVolumeDown: return L"volume-down";
    case MenuIconVolumeMute: return L"volume-mute";
    case MenuIconTools: return L"tools";
    case MenuIconCalculator: return L"calculator";
    case MenuIconTerminal: return L"terminal";
    case MenuIconNotebook: return L"notebook";
    case MenuIconEnvironment: return L"environment";
    case MenuIconUser: return L"user";
    case MenuIconHistory: return L"history";
    case MenuIconCertificate: return L"certificate";
    case MenuIconComputer: return L"computer";
    case MenuIconDownload: return L"download";
    default: return L"unknown";
    }
}

bool MenuIconIsRenderable(MenuIcon icon) {
    return icon != MenuIconNone;
}

TablerIconManifest::Id MenuIconTablerId(MenuIcon icon) {
    return TablerIconManifest::FromName(MenuIconName(icon));
}

wchar_t MenuIconGlyph(MenuIcon icon) {
    return TablerIconManifest::Glyph(MenuIconTablerId(icon));
}

std::wstring MenuIconLinkIconValue(MenuIcon icon) {
    if (!MenuIconIsRenderable(icon)) {
        return {};
    }
    return std::wstring(kMenuIconLinkPrefix) + MenuIconStorageName(icon);
}

bool TryParseMenuIconLinkIcon(const std::wstring& value, MenuIcon& icon) {
    icon = MenuIconNone;
    if (value.rfind(kMenuIconLinkPrefix, 0) != 0) {
        return false;
    }
    icon = MenuIconFromStorageName(value.substr(std::wcslen(kMenuIconLinkPrefix)));
    return MenuIconIsRenderable(icon);
}

std::span<const MenuVisualRequirement> RequiredTopRightMenuVisuals() {
    return kTopRightMenuVisuals;
}
