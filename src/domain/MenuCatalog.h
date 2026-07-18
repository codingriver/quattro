#pragma once

#include <span>
#include <string>
#include <windows.h>

constexpr UINT ID_MENU_ADD_LINK = 40001;
constexpr UINT ID_MENU_EDIT_LINK = 40002;
constexpr UINT ID_MENU_DELETE_LINK = 40003;
constexpr UINT ID_MENU_SHOW = 40004;
constexpr UINT ID_MENU_EXIT = 40005;
constexpr UINT ID_MENU_RUN_LINK = 40006;
constexpr UINT ID_MENU_OPEN_LOCATION = 40007;
constexpr UINT ID_MENU_COPY_PATH = 40008;
constexpr UINT ID_MENU_ADD_GROUP = 40009;
constexpr UINT ID_MENU_EDIT_GROUP = 40010;
constexpr UINT ID_MENU_DELETE_GROUP = 40011;
constexpr UINT ID_MENU_ADD_TAG = 40012;
constexpr UINT ID_MENU_EDIT_TAG = 40013;
constexpr UINT ID_MENU_DELETE_TAG = 40014;
constexpr UINT ID_MENU_QUICK_IMPORT = 40015;
constexpr UINT ID_MENU_SETTINGS = 40016;
constexpr UINT ID_MENU_IMPORT_CLIPBOARD = 40017;
constexpr UINT ID_MENU_SORT_POS = 40018;
constexpr UINT ID_MENU_SORT_RUNCOUNT = 40019;
constexpr UINT ID_MENU_SORT_NAME = 40020;
constexpr UINT ID_MENU_MOVE_UP = 40021;
constexpr UINT ID_MENU_MOVE_DOWN = 40022;
constexpr UINT ID_MENU_COPY_LINK = 40023;
constexpr UINT ID_MENU_CUT_LINK = 40024;
constexpr UINT ID_MENU_PASTE_LINK = 40025;
constexpr UINT ID_MENU_CLEAR_ICON_CACHE = 40026;
constexpr UINT ID_MENU_ABOUT = 40027;
constexpr UINT ID_MENU_HELP = 40028;
constexpr UINT ID_MENU_REFRESH_LINK_ICON = 40030;
constexpr UINT ID_MENU_REPAIR_LINK = 40031;
constexpr UINT ID_MENU_FAQ = 40032;
constexpr UINT ID_MENU_REWARD = 40033;
constexpr UINT ID_MENU_ADD_FILE = 40034;
constexpr UINT ID_MENU_ADD_FOLDER = 40035;
constexpr UINT ID_MENU_ADD_URL = 40036;
constexpr UINT ID_MENU_CLEAR_TAG_LINKS = 40038;
constexpr UINT ID_MENU_RUN_ADMIN = 40039;
constexpr UINT ID_MENU_RUN_PRIVATE = 40040;
constexpr UINT ID_MENU_COPY_URL = 40041;
constexpr UINT ID_MENU_WINDOWS_CONTEXT = 40042;
constexpr UINT ID_MENU_CREATE_DESKTOP_SHORTCUT = 40043;
constexpr UINT ID_MENU_PROPERTIES = 40044;
constexpr UINT ID_MENU_REFRESH_PAGE_ICONS = 40045;
constexpr UINT ID_MENU_TOGGLE_TITLE = 40046;
constexpr UINT ID_MENU_TOGGLE_GROUP = 40047;
constexpr UINT ID_MENU_TOGGLE_TAG = 40048;
constexpr UINT ID_MENU_TOGGLE_TOPMOST = 40049;
constexpr UINT ID_MENU_REFRESH_ALL_ICONS = 40050;
constexpr UINT ID_MENU_RESET_LAYOUT = 40051;
constexpr UINT ID_MENU_ADD_NOTE_TAG = 40052;
constexpr UINT ID_MENU_ADD_TODO_TAG = 40053;
constexpr UINT ID_MENU_ADD_TODO_ITEM = 40054;
constexpr UINT ID_MENU_EDIT_TODO_ITEM = 40055;
constexpr UINT ID_MENU_DELETE_TODO_ITEM = 40056;
constexpr UINT ID_MENU_TOGGLE_TODO_DONE = 40057;
constexpr UINT ID_MENU_CLEAR_DONE_TODOS = 40058;
constexpr UINT ID_MENU_TODO_SORT_DUE = 40060;
constexpr UINT ID_MENU_TODO_SORT_CREATED = 40061;
constexpr UINT ID_MENU_TODO_SORT_TITLE = 40062;
constexpr UINT ID_MENU_TODO_SORT_STATUS = 40063;
constexpr UINT ID_MENU_TOGGLE_TODO_ENABLED = 40064;
constexpr UINT ID_MENU_EXPORT_CONFIG = 40065;
constexpr UINT ID_MENU_IMPORT_CONFIG_MERGE = 40066;
constexpr UINT ID_MENU_CHECK_UPDATE = 40067;
constexpr UINT ID_MENU_RESTART_PRIVILEGE = 40069;
constexpr UINT ID_MENU_REFRESH_GROUP_LINKS = 40070;
constexpr UINT ID_MENU_TODO_REMINDER_VIEWED = 40071;
constexpr UINT ID_MENU_TODO_REMINDER_IGNORE = 40072;
constexpr UINT ID_MENU_TODO_REMINDER_SNOOZE_5 = 40073;
constexpr UINT ID_MENU_TODO_REMINDER_SNOOZE_30 = 40074;
constexpr UINT ID_MENU_TODO_REMINDER_SNOOZE_60 = 40075;
constexpr UINT ID_MENU_COMPLETE_OVERDUE_TODOS = 40076;
constexpr UINT ID_MENU_THEME_BASE = 43000;
constexpr UINT ID_MENU_LAYOUT_LIST = 44000;
constexpr UINT ID_MENU_LAYOUT_TILE = 44001;
constexpr UINT ID_MENU_ICON_SMALL = 44002;
constexpr UINT ID_MENU_ICON_MEDIUM = 44003;
constexpr UINT ID_MENU_ICON_LARGE = 44004;
constexpr UINT ID_MENU_ALL_LAYOUT_LIST = 44010;
constexpr UINT ID_MENU_ALL_LAYOUT_TILE = 44011;
constexpr UINT ID_MENU_ALL_ICON_SMALL = 44012;
constexpr UINT ID_MENU_ALL_ICON_MEDIUM = 44013;
constexpr UINT ID_MENU_ALL_ICON_LARGE = 44014;
constexpr UINT ID_MENU_ALL_SORT_POS = 44015;
constexpr UINT ID_MENU_ALL_SORT_RUNCOUNT = 44016;
constexpr UINT ID_MENU_ALL_SORT_NAME = 44017;
constexpr UINT ID_MENU_MOVE_TO_BASE = 41000;
constexpr UINT ID_MENU_COPY_TO_BASE = 42000;
constexpr UINT ID_MENU_MOVE_TAG_TO_BASE = 45000;
constexpr UINT ID_MENU_SYSTEM_FUNCTION_BASE = 46000;
constexpr UINT ID_MENU_SYSTEM_FUNCTION_LIMIT = 100;
constexpr UINT ID_MENU_TOOL_BASE = 47000;
constexpr UINT ID_MENU_TOOL_LIMIT = 100;
constexpr UINT ID_MENU_ADD_SYSTEM_FUNCTION_BASE = 48000;
constexpr UINT ID_MENU_ADD_SYSTEM_FUNCTION_LIMIT = 100;
constexpr UINT ID_MENU_BUILTIN_SYSTEM_ACTION_BASE = 49000;
constexpr UINT ID_MENU_BUILTIN_SYSTEM_ACTION_LIMIT = 100;
constexpr UINT ID_MENU_TRACKED_SHELL_ACTION_BASE = 50000;
constexpr UINT ID_MENU_TRACKED_SHELL_ACTION_LIMIT = 500;
constexpr UINT ID_MENU_DYNAMIC_TARGET_LIMIT = 500;

enum MenuIcon {
    MenuIconNone = 0,
    MenuIconFile,
    MenuIconFolder,
    MenuIconUrl,
    MenuIconSystem,
    MenuIconShield,
    MenuIconOpenFolder,
    MenuIconWindows,
    MenuIconShortcut,
    MenuIconRefresh,
    MenuIconMove,
    MenuIconCopy,
    MenuIconCut,
    MenuIconPaste,
    MenuIconEdit,
    MenuIconInfo,
    MenuIconDelete,
    MenuIconGroup,
    MenuIconTag,
    MenuIconTheme,
    MenuIconSize,
    MenuIconView,
    MenuIconList,
    MenuIconTile,
    MenuIconSort,
    MenuIconSortAsc,
    MenuIconSortDesc,
    MenuIconClear,
    MenuIconEye,
    MenuIconEyeOff,
    MenuIconAbout,
    MenuIconExit,
    MenuIconRun,
    MenuIconPin,
    MenuIconPinOff,
    MenuIconSettings,
    MenuIconHelp,
    MenuIconReward,
    MenuIconPower,
    MenuIconRestart,
    MenuIconLogout,
    MenuIconLock,
    MenuIconSleep,
    MenuIconMonitor,
    MenuIconVolumeUp,
    MenuIconVolumeDown,
    MenuIconVolumeMute,
    MenuIconTools,
    MenuIconCalculator,
    MenuIconTerminal,
    MenuIconNotebook,
    MenuIconEnvironment,
    MenuIconUser,
    MenuIconHistory,
    MenuIconCertificate,
    MenuIconComputer,
    MenuIconDownload,
};

struct MenuVisualRequirement {
    UINT_PTR command = 0;
    const wchar_t* text = L"";
    MenuIcon expectedIcon = MenuIconNone;
};

int MenuIconFor(UINT_PTR id, const std::wstring& text);
const wchar_t* MenuIconName(MenuIcon icon);
bool MenuIconIsRenderable(MenuIcon icon);
wchar_t MenuIconGlyph(MenuIcon icon);
std::wstring MenuIconLinkIconValue(MenuIcon icon);
bool TryParseMenuIconLinkIcon(const std::wstring& value, MenuIcon& icon);
std::span<const MenuVisualRequirement> RequiredTopRightMenuVisuals();
