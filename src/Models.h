#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AppConfig {
    bool autoRun = false;
    bool showTitle = true;
    bool showGroup = true;
    bool showTag = true;
    bool linkNameSingleLine = false;
    bool showTooltip = true;

    bool autoDock = false;
    int dockCorner = 0;
    int dockDelay = 0;
    bool hideOnStart = false;
    bool topMost = true;
    bool hideAfterLink = false;
    bool hideWhenInactive = false;

    bool doubleClickToRun = false;
    bool deleteConfirm = true;
    bool saveRunCount = true;
    bool showRunCount = true;
    bool hideNotifyIcon = false;
    bool showDate = true;
    bool showSearchButton = true;
    bool showMenuButton = true;
    bool showSkinButton = false;

    bool mouseEnterActiveGroup = false;
    bool mouseEnterActiveTag = false;
    int activeGroupDelay = 0;
    int activeTagDelay = 0;

    int currentGroupId = 0;
    int currentTagId = 0;
    int mainHotKey = 0;
    int searchHotKey = 0;

    int width = 388;
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
    int searchX = -1;
    int searchY = -1;
    int searchCount = 0;
    bool focusSearch = false;
    int alpha = 255;

    bool groupRight = false;
    bool tagRight = false;
    std::wstring tagAlign = L"center";
    std::wstring theme = L"gray";
    std::wstring openDirCommand;
    std::wstring helpUrl;
    std::wstring updateUrl;
    std::wstring faqUrl;
    std::wstring rewardUrl;
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

struct AppModel {
    std::vector<Group> groups;
    std::vector<Link> links;
};
