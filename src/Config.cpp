#include "Config.h"

#include "Utilities.h"

#include <algorithm>

namespace {
constexpr const wchar_t* kSection = L"main";
constexpr const wchar_t* kWebDavSection = L"webdav";
constexpr const wchar_t* kHttpSection = L"http";

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
    config.hideNotifyIcon = false;
    config.preferAdminRun = ReadBool(L"bPreferAdminRun", config.preferAdminRun);
    config.showToolboxButton = ReadBool(L"bShowBtnToolbox", ReadBool(L"bShowBtnMenu", config.showToolboxButton));
    config.showSkinButton = ReadBool(L"bShowBtnSkin", config.showSkinButton);

    config.mouseEnterActiveGroup = ReadBool(L"bMouseEnterActiveGroup", config.mouseEnterActiveGroup);
    config.mouseEnterActiveTag = ReadBool(L"bMouseEnterActiveTag", config.mouseEnterActiveTag);
    config.activeGroupDelay = ReadInt(L"nActiveGroupDelay", config.activeGroupDelay);
    config.activeTagDelay = ReadInt(L"nActiveTagDelay", config.activeTagDelay);

    config.currentGroupId = ReadInt(L"nCurGroup", config.currentGroupId);
    config.currentTagId = ReadInt(L"nCurTag", config.currentTagId);
    config.mainHotKey = ReadInt(L"nMainHotKey", config.mainHotKey);

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
    const std::filesystem::path webDavPath = WebDavConfigPath();
    const bool hasWebDavConfig = FileExists(webDavPath);
    config.webDavEnabled = hasWebDavConfig
        ? ReadExternalBool(webDavPath, kWebDavSection, L"Enabled", config.webDavEnabled)
        : ReadBool(L"WebDavEnabled", config.webDavEnabled);
    config.webDavUrl = hasWebDavConfig
        ? ReadExternalString(webDavPath, kWebDavSection, L"Url", L"")
        : ReadString(L"WebDavUrl", L"");
    config.webDavRemotePath = hasWebDavConfig
        ? ReadExternalString(webDavPath, kWebDavSection, L"RemotePath", L"/Quattro/backups/")
        : ReadString(L"WebDavRemotePath", L"/Quattro/backups/");
    config.webDavUserName = hasWebDavConfig
        ? ReadExternalString(webDavPath, kWebDavSection, L"UserName", L"")
        : ReadString(L"WebDavUserName", L"");
    config.webDavKeepCount = Clamp(hasWebDavConfig
        ? ReadExternalInt(webDavPath, kWebDavSection, L"KeepCount", config.webDavKeepCount)
        : ReadInt(L"WebDavKeepCount", config.webDavKeepCount), 1, 100);

    const std::filesystem::path httpPath = HttpConfigPath();
    const bool hasHttpConfig = FileExists(httpPath);
    config.httpServerEnabled = hasHttpConfig
        ? ReadExternalBool(httpPath, kHttpSection, L"Enabled", config.httpServerEnabled)
        : ReadBool(L"HttpServerEnabled", config.httpServerEnabled);
    config.httpServerAutoStart = hasHttpConfig
        ? ReadExternalBool(httpPath, kHttpSection, L"AutoStart", config.httpServerAutoStart)
        : ReadBool(L"HttpServerAutoStart", config.httpServerAutoStart);
    config.httpServerLanAccess = hasHttpConfig
        ? ReadExternalBool(httpPath, kHttpSection, L"LanAccess", config.httpServerLanAccess)
        : ReadBool(L"HttpServerLanAccess", config.httpServerLanAccess);
    config.httpServerPort = Clamp(hasHttpConfig
        ? ReadExternalInt(httpPath, kHttpSection, L"Port", config.httpServerPort)
        : ReadInt(L"HttpServerPort", config.httpServerPort), 1, 65535);
    config.httpServerRootPath = hasHttpConfig
        ? ReadExternalString(httpPath, kHttpSection, L"RootPath", L"")
        : ReadString(L"HttpServerRootPath", L"");
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
    WriteInt(L"bHideNotify", 0);
    WriteInt(L"bShowBtnToolbox", config.showToolboxButton ? 1 : 0);
    WriteString(L"bShowBtnMenu", L"");
    WriteInt(L"bShowBtnSkin", config.showSkinButton ? 1 : 0);
    WriteInt(L"bGroupRight", config.groupRight ? 1 : 0);
    WriteInt(L"bTagRight", config.tagRight ? 1 : 0);
    WriteInt(L"nGroupWidth", config.groupWidth);
    WriteInt(L"nTagWidth", config.tagWidth);
    WriteInt(L"bMouseEnterActiveGroup", config.mouseEnterActiveGroup ? 1 : 0);
    WriteInt(L"bMouseEnterActiveTag", config.mouseEnterActiveTag ? 1 : 0);
    WriteInt(L"nActiveGroupDelay", config.activeGroupDelay);
    WriteInt(L"nActiveTagDelay", config.activeTagDelay);
    WriteString(L"TagAlign", config.tagAlign);
    WriteInt(L"nAlpha", config.alpha);
    WriteString(L"OpenDirCmd", config.openDirCommand);
    WriteString(L"HelpUrl", config.helpUrl);
    WriteString(L"UpdateUrl", config.updateUrl);
    WriteString(L"FaqUrl", config.faqUrl);
    WriteString(L"RewardUrl", config.rewardUrl);
    SaveExternalNetworkSettings(config);
    DeleteLegacyNetworkSettings();
    WriteInt(L"nWidth", config.width);
    WriteInt(L"nHeight", config.height);
    WriteInt(L"nPosX", config.posX);
    WriteInt(L"nPosY", config.posY);
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
    WriteInt(L"bPreferAdminRun", config.preferAdminRun ? 1 : 0);
    WriteInt(L"bShowBtnToolbox", config.showToolboxButton ? 1 : 0);
    WriteString(L"bShowBtnMenu", L"");
    WriteInt(L"bShowBtnSkin", config.showSkinButton ? 1 : 0);
    WriteInt(L"bGroupRight", config.groupRight ? 1 : 0);
    WriteInt(L"bTagRight", config.tagRight ? 1 : 0);
    WriteInt(L"nGroupWidth", config.groupWidth);
    WriteInt(L"nTagWidth", config.tagWidth);
    WriteInt(L"bMouseEnterActiveGroup", config.mouseEnterActiveGroup ? 1 : 0);
    WriteInt(L"bMouseEnterActiveTag", config.mouseEnterActiveTag ? 1 : 0);
    WriteInt(L"nActiveGroupDelay", config.activeGroupDelay);
    WriteInt(L"nActiveTagDelay", config.activeTagDelay);
    WriteInt(L"nMainHotKey", config.mainHotKey);
    WriteString(L"TagAlign", config.tagAlign);
    WriteString(L"Theme", config.theme);
    WriteString(L"OpenDirCmd", config.openDirCommand);
    WriteString(L"HelpUrl", config.helpUrl);
    WriteString(L"UpdateUrl", config.updateUrl);
    WriteString(L"FaqUrl", config.faqUrl);
    WriteString(L"RewardUrl", config.rewardUrl);
    SaveExternalNetworkSettings(config);
    DeleteLegacyNetworkSettings();
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
    std::error_code ec;
    if (!configPath_.parent_path().empty()) {
        std::filesystem::create_directories(configPath_.parent_path(), ec);
    }
    WritePrivateProfileStringW(kSection, key, value.empty() ? nullptr : value.c_str(), configPath_.c_str());
}

std::filesystem::path ConfigService::WebDavConfigPath() const {
    return QuattroUserConfigDirectory() / L"webdav.ini";
}

std::filesystem::path ConfigService::HttpConfigPath() const {
    return QuattroUserConfigDirectory() / L"http.ini";
}

int ConfigService::ReadExternalInt(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, int fallback) const {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
}

bool ConfigService::ReadExternalBool(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, bool fallback) const {
    return ReadExternalInt(path, section, key, fallback ? 1 : 0) != 0;
}

std::wstring ConfigService::ReadExternalString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const {
    std::wstring buffer(512, L'\0');
    DWORD copied = GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    while (copied == buffer.size() - 1) {
        buffer.resize(buffer.size() * 2);
        copied = GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    }
    buffer.resize(copied);
    return buffer;
}

void ConfigService::WriteExternalInt(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, int value) const {
    WriteExternalString(path, section, key, std::to_wstring(value));
}

void ConfigService::WriteExternalString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const std::wstring& value) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    WritePrivateProfileStringW(section, key, value.empty() ? nullptr : value.c_str(), path.c_str());
}

void ConfigService::SaveExternalNetworkSettings(const AppConfig& config) const {
    const std::filesystem::path webDavPath = WebDavConfigPath();
    WriteExternalInt(webDavPath, kWebDavSection, L"Enabled", config.webDavEnabled ? 1 : 0);
    WriteExternalString(webDavPath, kWebDavSection, L"Url", config.webDavUrl);
    WriteExternalString(webDavPath, kWebDavSection, L"RemotePath", config.webDavRemotePath);
    WriteExternalString(webDavPath, kWebDavSection, L"UserName", config.webDavUserName);
    WriteExternalInt(webDavPath, kWebDavSection, L"KeepCount", config.webDavKeepCount);

    const std::filesystem::path httpPath = HttpConfigPath();
    WriteExternalInt(httpPath, kHttpSection, L"Enabled", config.httpServerEnabled ? 1 : 0);
    WriteExternalInt(httpPath, kHttpSection, L"AutoStart", config.httpServerAutoStart ? 1 : 0);
    WriteExternalInt(httpPath, kHttpSection, L"LanAccess", config.httpServerLanAccess ? 1 : 0);
    WriteExternalInt(httpPath, kHttpSection, L"Port", config.httpServerPort);
    WriteExternalString(httpPath, kHttpSection, L"RootPath", config.httpServerRootPath);
}

void ConfigService::DeleteLegacyNetworkSettings() const {
    WriteString(L"WebDavEnabled", L"");
    WriteString(L"WebDavUrl", L"");
    WriteString(L"WebDavRemotePath", L"");
    WriteString(L"WebDavUserName", L"");
    WriteString(L"WebDavKeepCount", L"");
    WriteString(L"HttpServerEnabled", L"");
    WriteString(L"HttpServerAutoStart", L"");
    WriteString(L"HttpServerLanAccess", L"");
    WriteString(L"HttpServerPort", L"");
    WriteString(L"HttpServerRootPath", L"");
}
