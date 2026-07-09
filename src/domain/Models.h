#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class TodoScheduleKind {
    None = 0,
    Once = 1,
    Daily = 2,
    Weekly = 3,
    Monthly = 4,
    Yearly = 5,
    Hourly = 6,
    Minutely = 7,
    Secondly = 8,
    Cron = 9,
};

enum class TodoRepeatMode {
    FixedPoint = 0,
    Interval = 1,
};

struct AppConfig {
    bool autoRun = false;
    bool showTitle = true;
    bool showGroup = true;
    bool showTag = true;
    bool linkNameSingleLine = false;
    bool showTooltip = true;

    bool autoDock = true;
    int dockCorner = 0;
    int dockDelay = 1500;
    bool hideOnStart = false;
    bool topMost = true;
    bool hideAfterLink = true;
    bool hideWhenInactive = false;

    bool doubleClickToRun = false;
    bool deleteConfirm = true;
    bool saveRunCount = true;
    bool showRunCount = true;
    bool hideNotifyIcon = false;
    bool preferAdminRun = false;
    bool showToolboxButton = true;
    bool showSkinButton = false;
    bool loggingEnabled =
#if defined(QUATTRO_DEFAULT_LOGGING_ENABLED)
        QUATTRO_DEFAULT_LOGGING_ENABLED != 0;
#else
        true;
#endif

    bool mouseEnterActiveGroup = false;
    bool mouseEnterActiveTag = false;
    int activeGroupDelay = 0;
    int activeTagDelay = 0;

    int currentGroupId = 0;
    int currentTagId = 0;
    int mainHotKey = L'Q';

    int width = 400;
    int height = 560;
    int posX = 2;
    int posY = 0;
    int groupWidth = 72;
    bool autoGroupWidth = false;
    int tagWidth = 124;
    bool autoTagHeight = false;
    int attrWidth = 0;
    int attrHeight = 0;

    int version = 0;
    int alpha = 255;

    bool groupRight = false;
    bool tagRight = false;
    std::wstring tagAlign = L"center";
    std::wstring theme = L"default";
    std::wstring openDirCommand;
    std::wstring helpUrl;
    std::wstring updateUrl;
    std::wstring faqUrl;
    std::wstring rewardUrl;

    bool webDavEnabled = false;
    std::wstring webDavUrl;
    std::wstring webDavRemotePath = L"/Quattro/backups/";
    std::wstring webDavUserName;
    int webDavKeepCount = 10;

    bool httpServerEnabled = false;
    bool httpServerAutoStart = false;
    bool httpServerLanAccess = true;
    int httpServerPort = 18080;
    std::wstring httpServerRootPath;
};

struct Group {
    int id = 0;
    std::wstring name;
    int parentGroup = 0;
    std::wstring icon;
    int layout = 0;
    int iconSize = 0;
    int pos = 0;
    int type = 0;
    int sort = 0;
    std::wstring content;
    int flag = 0;
};

struct Link {
    int id = 0;
    std::wstring name;
    int parentGroup = 0;
    int type = 0;
    int pos = 0;
    std::wstring icon;
    std::wstring path;
    std::wstring parameter;
    std::wstring workDir;
    int hotKey = 0;
    int showCmd = 0;
    bool isAdmin = false;
    bool isCustomColor = false;
    std::wstring customColor;
    std::wstring remark;
    int runCount = 0;
    std::vector<std::uint8_t> pidl;
};

struct NotePage {
    int tagId = 0;
    std::wstring content;
    std::wstring updatedAt;
};

struct TodoItem {
    int id = 0;
    int tagId = 0;
    std::wstring title;
    std::wstring content;
    bool enabled = true;
    TodoScheduleKind scheduleKind = TodoScheduleKind::None;
    TodoRepeatMode repeatMode = TodoRepeatMode::FixedPoint;
    int repeatInterval = 1;
    int repeatLimit = 0;
    int repeatFinished = 0;
    std::wstring cronExpression;
    std::wstring anchorAt;
    std::wstring nextDueAt;
    std::wstring completedAt;
    int pos = 0;
    std::wstring createdAt;
    std::wstring updatedAt;
};

struct AppModel {
    std::vector<Group> groups;
    std::vector<Link> links;
    std::vector<NotePage> notes;
    std::vector<TodoItem> todos;
};
