#include "Config.h"

#include "Utilities.h"

#include <algorithm>

namespace {
constexpr const wchar_t* kSection = L"main";

int Clamp(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}
}

ConfigService::ConfigService(std::filesystem::path configPath)
    : configPath_(std::move(configPath)) {
}

AppConfig ConfigService::Load() const {
    AppConfig config;
    config.autoRun = ReadBool(L"bAutoRun", config.autoRun);
    config.showTitle = ReadBool(L"bShowTitle", config.showTitle);
    config.showGroup = ReadBool(L"bShowGroup", config.showGroup);
    config.showTag = ReadBool(L"bShowTag", config.showTag);
    config.linkNameSingleLine = ReadBool(L"bLnkNameSingleline", config.linkNameSingleLine);
    config.showTooltip = ReadBool(L"bShowTooltip", config.showTooltip);

    config.autoDock = ReadBool(L"bAutoDock", ReadBool(L"bAutoDick", config.autoDock));
    config.dockCorner = ReadInt(L"bDockCorner", ReadInt(L"bDickCorner", config.dockCorner));
    config.dockDelay = ReadInt(L"nDockDelay", config.dockDelay);
    config.hideOnStart = ReadBool(L"bHideOnStart", config.hideOnStart);
    config.topMost = ReadBool(L"bTopMost", config.topMost);
    config.hideAfterLink = ReadBool(L"bHideAfterLink", config.hideAfterLink);
    config.hideWhenInactive = ReadBool(L"bHideUnhot", config.hideWhenInactive);

    config.doubleClickToRun = ReadBool(L"bDoubleClick", config.doubleClickToRun);
    config.deleteConfirm = ReadBool(L"bDelConfirm", config.deleteConfirm);
    config.saveRunCount = ReadBool(L"bSaveRunCount", config.saveRunCount);
    config.showRunCount = ReadBool(L"bRunCount", config.showRunCount);
    config.hideNotifyIcon = ReadBool(L"bHideNotify", config.hideNotifyIcon);
    config.showDate = ReadBool(L"bShowDate", config.showDate);
    config.showSearchButton = ReadBool(L"bShowBtnSearch", config.showSearchButton);
    config.showMenuButton = ReadBool(L"bShowBtnMenu", config.showMenuButton);
    config.showSkinButton = ReadBool(L"bShowBtnSkin", config.showSkinButton);

    config.mouseEnterActiveGroup = ReadBool(L"bMouseEnterActiveGroup", config.mouseEnterActiveGroup);
    config.mouseEnterActiveTag = ReadBool(L"bMouseEnterActiveTag", config.mouseEnterActiveTag);
    config.activeGroupDelay = ReadInt(L"nActiveGroupDelay", config.activeGroupDelay);
    config.activeTagDelay = ReadInt(L"nActiveTagDelay", config.activeTagDelay);

    config.currentGroupId = ReadInt(L"nCurGroup", config.currentGroupId);
    config.currentTagId = ReadInt(L"nCurTag", config.currentTagId);
    config.mainHotKey = ReadInt(L"nMainHotKey", config.mainHotKey);
    config.searchHotKey = ReadInt(L"nSearchHotKey", config.searchHotKey);

    config.width = Clamp(ReadInt(L"nWidth", config.width), 260, 1800);
    config.height = Clamp(ReadInt(L"nHeight", config.height), 320, 1600);
    config.posX = ReadInt(L"nPosX", config.posX);
    config.posY = ReadInt(L"nPosY", config.posY);
    config.groupWidth = Clamp(ReadInt(L"nGroupWidth", config.groupWidth), 40, 240);
    config.autoGroupWidth = ReadBool(L"bAutoGroupWidth", config.autoGroupWidth);
    config.tagWidth = Clamp(ReadInt(L"nTagWidth", config.tagWidth), 40, 240);
    config.autoTagHeight = ReadBool(L"bAutoTagHeight", config.autoTagHeight);
    config.attrWidth = ReadInt(L"nAttrWidth", config.attrWidth);
    config.attrHeight = ReadInt(L"nAttrHeight", config.attrHeight);

    config.version = ReadInt(L"nVersion", config.version);
    config.searchX = ReadInt(L"nSearchX", config.searchX);
    config.searchY = ReadInt(L"nSearchY", config.searchY);
    config.searchCount = ReadInt(L"nSearchCount", config.searchCount);
    config.focusSearch = ReadBool(L"bFocusSearch", config.focusSearch);
    config.alpha = Clamp(ReadInt(L"nAlpha", config.alpha), 64, 255);

    config.groupRight = ReadBool(L"bGroupRight", config.groupRight);
    config.tagRight = ReadBool(L"bTagRight", config.tagRight);
    config.tagAlign = ReadString(L"TagAlign", L"center");
    if (config.tagAlign != L"left" && config.tagAlign != L"center" && config.tagAlign != L"right") {
        config.tagAlign = L"center";
    }
    config.theme = ReadString(L"Theme", L"default");
    config.openDirCommand = ReadString(L"OpenDirCmd", L"");
    config.helpUrl = ReadString(L"HelpUrl", L"");
    config.updateUrl = ReadString(L"UpdateUrl", L"");
    config.faqUrl = ReadString(L"FaqUrl", L"");
    config.rewardUrl = ReadString(L"RewardUrl", L"");
    return config;
}

void ConfigService::SaveWindowState(const AppConfig& config) const {
    WriteInt(L"bShowTitle", config.showTitle ? 1 : 0);
    WriteInt(L"bShowGroup", config.showGroup ? 1 : 0);
    WriteInt(L"bShowTag", config.showTag ? 1 : 0);
    WriteInt(L"bLnkNameSingleline", config.linkNameSingleLine ? 1 : 0);
    WriteInt(L"bShowTooltip", config.showTooltip ? 1 : 0);
    WriteInt(L"bHideAfterLink", config.hideAfterLink ? 1 : 0);
    WriteInt(L"bHideOnStart", config.hideOnStart ? 1 : 0);
    WriteInt(L"bTopMost", config.topMost ? 1 : 0);
    WriteInt(L"bRunCount", config.showRunCount ? 1 : 0);
    WriteInt(L"bDoubleClick", config.doubleClickToRun ? 1 : 0);
    WriteInt(L"bHideNotify", config.hideNotifyIcon ? 1 : 0);
    WriteInt(L"bShowDate", config.showDate ? 1 : 0);
    WriteInt(L"bShowBtnSearch", config.showSearchButton ? 1 : 0);
    WriteInt(L"bShowBtnMenu", config.showMenuButton ? 1 : 0);
    WriteInt(L"bShowBtnSkin", config.showSkinButton ? 1 : 0);
    WriteInt(L"bGroupRight", config.groupRight ? 1 : 0);
    WriteInt(L"bTagRight", config.tagRight ? 1 : 0);
    WriteInt(L"nGroupWidth", config.groupWidth);
    WriteInt(L"nTagWidth", config.tagWidth);
    WriteInt(L"bMouseEnterActiveGroup", config.mouseEnterActiveGroup ? 1 : 0);
    WriteInt(L"bMouseEnterActiveTag", config.mouseEnterActiveTag ? 1 : 0);
    WriteInt(L"nActiveGroupDelay", config.activeGroupDelay);
    WriteInt(L"nActiveTagDelay", config.activeTagDelay);
    WriteInt(L"bFocusSearch", config.focusSearch ? 1 : 0);
    WriteString(L"TagAlign", config.tagAlign);
    WriteInt(L"nAlpha", config.alpha);
    WriteString(L"OpenDirCmd", config.openDirCommand);
    WriteString(L"HelpUrl", config.helpUrl);
    WriteString(L"UpdateUrl", config.updateUrl);
    WriteString(L"FaqUrl", config.faqUrl);
    WriteString(L"RewardUrl", config.rewardUrl);
    WriteInt(L"nWidth", config.width);
    WriteInt(L"nHeight", config.height);
    WriteInt(L"nPosX", config.posX);
    WriteInt(L"nPosY", config.posY);
    WriteInt(L"nSearchX", config.searchX);
    WriteInt(L"nSearchY", config.searchY);
    WriteInt(L"nSearchCount", config.searchCount);
    WriteInt(L"nCurGroup", config.currentGroupId);
    WriteInt(L"nCurTag", config.currentTagId);
    WriteInt(L"bAutoDock", config.autoDock ? 1 : 0);
    WriteInt(L"bDockCorner", config.dockCorner);
    WriteString(L"bAutoDick", L"");
    WriteString(L"bDickCorner", L"");
}

void ConfigService::Save(const AppConfig& config) const {
    SaveWindowState(config);
    WriteInt(L"bAutoRun", config.autoRun ? 1 : 0);
    WriteInt(L"bLnkNameSingleline", config.linkNameSingleLine ? 1 : 0);
    WriteInt(L"bShowTooltip", config.showTooltip ? 1 : 0);
    WriteInt(L"nDockDelay", config.dockDelay);
    WriteInt(L"bHideOnStart", config.hideOnStart ? 1 : 0);
    WriteInt(L"bHideUnhot", config.hideWhenInactive ? 1 : 0);
    WriteInt(L"bDelConfirm", config.deleteConfirm ? 1 : 0);
    WriteInt(L"bSaveRunCount", config.saveRunCount ? 1 : 0);
    WriteInt(L"bShowDate", config.showDate ? 1 : 0);
    WriteInt(L"bShowBtnSearch", config.showSearchButton ? 1 : 0);
    WriteInt(L"bShowBtnMenu", config.showMenuButton ? 1 : 0);
    WriteInt(L"bShowBtnSkin", config.showSkinButton ? 1 : 0);
    WriteInt(L"bFocusSearch", config.focusSearch ? 1 : 0);
    WriteInt(L"bGroupRight", config.groupRight ? 1 : 0);
    WriteInt(L"bTagRight", config.tagRight ? 1 : 0);
    WriteInt(L"nGroupWidth", config.groupWidth);
    WriteInt(L"nTagWidth", config.tagWidth);
    WriteInt(L"bMouseEnterActiveGroup", config.mouseEnterActiveGroup ? 1 : 0);
    WriteInt(L"bMouseEnterActiveTag", config.mouseEnterActiveTag ? 1 : 0);
    WriteInt(L"nActiveGroupDelay", config.activeGroupDelay);
    WriteInt(L"nActiveTagDelay", config.activeTagDelay);
    WriteInt(L"nMainHotKey", config.mainHotKey);
    WriteInt(L"nSearchHotKey", config.searchHotKey);
    WriteString(L"TagAlign", config.tagAlign);
    WriteString(L"Theme", config.theme);
    WriteString(L"OpenDirCmd", config.openDirCommand);
    WriteString(L"HelpUrl", config.helpUrl);
    WriteString(L"UpdateUrl", config.updateUrl);
    WriteString(L"FaqUrl", config.faqUrl);
    WriteString(L"RewardUrl", config.rewardUrl);
}

int ConfigService::ReadInt(const wchar_t* key, int fallback) const {
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, configPath_.c_str()));
}

bool ConfigService::ReadBool(const wchar_t* key, bool fallback) const {
    return ReadInt(key, fallback ? 1 : 0) != 0;
}

std::wstring ConfigService::ReadString(const wchar_t* key, const wchar_t* fallback) const {
    std::wstring buffer(512, L'\0');
    DWORD copied = GetPrivateProfileStringW(kSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), configPath_.c_str());
    while (copied == buffer.size() - 1) {
        buffer.resize(buffer.size() * 2);
        copied = GetPrivateProfileStringW(kSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), configPath_.c_str());
    }
    buffer.resize(copied);
    return buffer;
}

void ConfigService::WriteInt(const wchar_t* key, int value) const {
    WriteString(key, std::to_wstring(value));
}

void ConfigService::WriteString(const wchar_t* key, const std::wstring& value) const {
    WritePrivateProfileStringW(kSection, key, value.empty() ? nullptr : value.c_str(), configPath_.c_str());
}
