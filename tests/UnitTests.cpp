#include "../src/domain/Config.h"
#include "../src/domain/LinkSorting.h"
#include "../src/services/ConfigPackageService.h"
#include "../src/services/Launcher.h"
#include "../src/domain/MenuCatalog.h"
#include "../src/domain/PluginRegistry.h"
#include "../src/services/ShellItemService.h"
#include "../src/services/Storage.h"
#include "../src/services/SystemFunctions.h"
#include "../src/services/UpdateCheckService.h"
#include "../src/theme/Theme.h"
#include "../src/domain/TodoSchedule.h"
#include "../src/common/Utilities.h"
#include "../src/services/WebDavClient.h"
#include "../src/services/WebDavCredentialService.h"
#include "../src/services/WebDavRecoveryService.h"
#include "../src/windows/MenuAnchorGeometry.h"
#include "Version.h"

#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_set>
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

std::string NarrowAscii(const std::wstring& value) {
    std::string result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::wstring ExpectedUpdateAssetName() {
#if defined(_M_X64) || defined(__x86_64__)
    return L"Quattro-x64.exe";
#else
    return L"Quattro-x86.exe";
#endif
}

bool ExecSql(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::cerr << "sqlite exec failed: " << (message ? message : "") << "\n";
        sqlite3_free(message);
        return false;
    }
    return true;
}
}

int wmain() {
    const std::filesystem::path unitUserConfigRoot = std::filesystem::temp_directory_path() / (L"quattro_unit_user_config_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(unitUserConfigRoot, ec);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", unitUserConfigRoot.c_str());

    Check(Trim(L"  abc \t") == L"abc", "Trim");
    Check(ToLower(L"AbC") == L"abc", "ToLower");
    Check(FormatVersionForDisplay(L"0.1.0") == L"v0.1.0", "Version display adds prefix");
    Check(FormatVersionForDisplay(L"v0.1.0") == L"v0.1.0", "Version display preserves prefix");
    Check(FormatVersionForDisplay(L"V0.1.0") == L"v0.1.0", "Version display normalizes prefix");
    Check(FormatVersionForDisplay(L" 0.1.0 ") == L"v0.1.0", "Version display trims whitespace");
    Check(FormatVersionForDisplay(L"").empty(), "Version display preserves empty value");
    Check(QuattroUserConfigDirectory() == unitUserConfigRoot, "User config env override");
    Check(HasUrlScheme(L"https://example.com"), "HasUrlScheme");
    Check(NormalizeUrl(L"example.com") == L"https://example.com", "NormalizeUrl");
    Check(ParseInt(L"42").value_or(0) == 42, "ParseInt valid");
    Check(!ParseInt(L"42x").has_value(), "ParseInt invalid");
    Check(!Hex8(StablePathHash(L"abc")).empty(), "StablePathHash");
    Check(NormalizeTodoTimestamp(L"2026/06/26 09:30") == L"2026-06-26 09:30:00", "Todo timestamp normalize");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Daily, L"2026-06-26 09:30", L"2026-06-26 10:00") == L"2026-06-27 09:30:00", "Todo daily next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Minutely, 5, L"2026-06-26 09:30:10", L"2026-06-26 09:33:00") == L"2026-06-26 09:35:10", "Todo minute interval next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Hourly, TodoRepeatMode::FixedPoint, 1, L"2026-06-26 09:15:10", L"2026-06-26 09:30:00") == L"2026-06-26 10:15:10", "Todo hourly fixed point next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Daily, TodoRepeatMode::Interval, 2, L"2026-06-26 09:30:00", L"2026-06-29 10:00:00") == L"2026-06-30 09:30:00", "Todo daily interval next due");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Monthly, L"2024-01-31 09:00", L"2024-02-01 00:00") == L"2024-02-29 09:00:00", "Todo monthly month end");
    Check(ComputeNextTodoDueAt(TodoScheduleKind::Yearly, L"2024-02-29 09:00", L"2025-01-01 00:00") == L"2025-02-28 09:00:00", "Todo yearly leap day");
    Check(NormalizeTodoCronExpression(L" 0  30  9  *  *  * ") == L"0 30 9 * * *", "Todo cron normalize");
    Check(IsValidTodoCronExpression(L"0 30 9 * * *"), "Todo cron valid");
    Check(!IsValidTodoCronExpression(L"0 99 9 * * *"), "Todo cron invalid");
    Check(ComputeNextTodoCronDueAt(L"0 30 9 * * *", L"2026-06-26 09:00:00") == L"2026-06-26 09:30:00", "Todo cron daily next due");
    {
        struct AnchorArea {
            int id = 0;
            D2D1_RECT_F rect{};
        };
        const std::vector<AnchorArea> areas{
            AnchorArea{1, D2D1::RectF(10.0f, 20.0f, 30.0f, 60.0f)},
            AnchorArea{2, D2D1::RectF(100.0f, 200.0f, 140.0f, 260.0f)},
        };
        POINT anchor{};
        Check(
            quattro::windows::TryMatchedAreaCenterPoint(
                areas,
                [](const AnchorArea& area) { return area.id == 2; },
                anchor),
            "Windows native menu anchor finds current link");
        Check(anchor.x == 120 && anchor.y == 230, "Windows native menu anchor uses link center");
        Check(
            !quattro::windows::TryMatchedAreaCenterPoint(
                areas,
                [](const AnchorArea& area) { return area.id == 3; },
                anchor),
            "Windows native menu anchor reports missing link");
    }
    Check(
        UpdateCheckService::UpdateInfoUrlForConfig(L"").find(L"/releases/latest/download/latest.json") != std::wstring::npos,
        "Update default static manifest URL");
    Check(
        UpdateCheckService::UpdateInfoUrlForConfig(L"https://github.com/codingriver/quattro/releases").find(L"/releases/latest/download/latest.json") != std::wstring::npos,
        "Update GitHub releases URL maps to static manifest");
    Check(
        UpdateCheckService::UpdateInfoUrlForConfig(L"https://api.github.com/repos/codingriver/quattro/releases/latest") ==
            L"https://api.github.com/repos/codingriver/quattro/releases/latest",
        "Update GitHub API URL remains API");
    {
        std::wstring mirrorError;
        const std::vector<std::wstring> mirrors = UpdateCheckService::ParseGithubMirrorBasesJson(
            L"{\"githubMirrors\":[[\"https://mirror-a.local/\",\"https://mirror-b.local\"],[\"https://mirror-a.local\",\"ftp://ignored.local\",\"  \"]]}",
            mirrorError);
        Check(mirrors.size() == 2, "Update mirror json nested array parses and dedupes");
        Check(mirrors.size() > 0 && mirrors[0] == L"https://mirror-a.local", "Update mirror trims trailing slash");
        Check(mirrors.size() > 1 && mirrors[1] == L"https://mirror-b.local", "Update mirror preserves order");
        Check(
            UpdateCheckService::MirrorGithubUrl(
                L"https://github.com/codingriver/quattro/releases/latest/download/latest.json",
                L"https://mirror-a.local/") ==
                L"https://mirror-a.local/https://github.com/codingriver/quattro/releases/latest/download/latest.json",
            "Update mirror rewrites github URL");
        Check(
            UpdateCheckService::MirrorGithubUrl(L"https://example.com/latest.json", L"https://mirror-a.local") ==
                L"https://example.com/latest.json",
            "Update mirror leaves non-github URL unchanged");
        const std::vector<std::wstring> urls = UpdateCheckService::UpdateInfoUrlsForConfig(
            L"",
            std::vector<std::wstring>{L"https://mirror-a.local", L"https://mirror-b.local/"});
        Check(urls.size() == 3, "Update candidate URL count includes primary and mirrors");
        Check(urls.size() > 0 && urls[0].find(L"https://github.com/") == 0, "Update candidate primary first");
        Check(urls.size() > 1 && urls[1].find(L"https://mirror-a.local/https://github.com/") == 0, "Update candidate first mirror second");
        Check(urls.size() > 2 && urls[2].find(L"https://mirror-b.local/https://github.com/") == 0, "Update candidate second mirror third");
        const std::vector<std::wstring> customUrls = UpdateCheckService::UpdateInfoUrlsForConfig(
            L"https://updates.local/latest.json",
            std::vector<std::wstring>{L"https://mirror-a.local"});
        Check(customUrls.size() == 1 && customUrls[0] == L"https://updates.local/latest.json", "Update custom non-github URL skips mirrors");

        const std::filesystem::path mirrorConfigRoot = unitUserConfigRoot / L"mirror_config_unit";
        const std::filesystem::path mirrorConfigPath = mirrorConfigRoot / L"update-mirrors.json";
        std::filesystem::remove_all(mirrorConfigRoot, ec);
        std::wstring ensureError;
        Check(UpdateCheckService::EnsureGithubMirrorConfigFile(mirrorConfigRoot, ensureError), "Update mirror config generated");
        const std::wstring generatedConfig = LoadUtf8File(mirrorConfigPath);
        Check(generatedConfig.find(L"\"version\": \"" + std::wstring(QuattroVersionText()) + L"\"") != std::wstring::npos, "Update mirror config writes current version");
        Check(generatedConfig.find(L"\"githubMirrors\"") != std::wstring::npos, "Update mirror config writes mirrors");

        {
            std::ofstream file(mirrorConfigPath, std::ios::binary | std::ios::trunc);
            file << "{\n"
                 << "  \"version\": \"" << "0.0.0" << "\",\n"
                 << "  \"githubMirrors\": [[\"https://old-mirror.local\"]]\n"
                 << "}\n";
        }
        Check(UpdateCheckService::EnsureGithubMirrorConfigFile(mirrorConfigRoot, ensureError), "Update mirror config old version overwritten");
        const std::wstring overwrittenConfig = LoadUtf8File(mirrorConfigPath);
        Check(overwrittenConfig.find(L"https://old-mirror.local") == std::wstring::npos, "Update mirror config removes stale mirrors");
        Check(overwrittenConfig.find(L"\"version\": \"" + std::wstring(QuattroVersionText()) + L"\"") != std::wstring::npos, "Update mirror config overwrite writes current version");

        {
            std::ofstream file(mirrorConfigPath, std::ios::binary | std::ios::trunc);
            file << "{\n"
                 << "  \"version\": \"" << NarrowAscii(QuattroVersionText()) << "\",\n"
                 << "  \"githubMirrors\": [[\"https://custom-mirror.local\"]]\n"
                 << "}\n";
        }
        Check(UpdateCheckService::EnsureGithubMirrorConfigFile(mirrorConfigRoot, ensureError), "Update mirror config same version preserved");
        const std::wstring sameVersionConfig = LoadUtf8File(mirrorConfigPath);
        Check(sameVersionConfig.find(L"https://custom-mirror.local") != std::wstring::npos, "Update mirror config same version keeps custom mirrors");
    }
    {
        const std::wstring expectedAsset = ExpectedUpdateAssetName();
        const std::wstring staticManifest =
            L"{"
            L"\"version\":\"99.0.0\","
            L"\"releaseUrl\":\"https://github.com/codingriver/quattro/releases/tag/v99.0.0\","
            L"\"notes\":\"unit manifest\","
            L"\"checksumUrl\":\"https://github.com/codingriver/quattro/releases/download/v99.0.0/SHA256SUMS.txt\","
            L"\"assets\":["
            L"{\"name\":\"Quattro-x64.exe\",\"url\":\"https://github.com/codingriver/quattro/releases/download/v99.0.0/Quattro-x64.exe\",\"size\":123,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"},"
            L"{\"name\":\"Quattro-x86.exe\",\"url\":\"https://github.com/codingriver/quattro/releases/download/v99.0.0/Quattro-x86.exe\",\"size\":456,\"sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"}"
            L"]}";
        UpdateReleaseInfo manifestInfo;
        std::wstring manifestError;
        Check(UpdateCheckService::ParseReleaseInfoJson(staticManifest, manifestInfo, manifestError), "Update static manifest parse");
        Check(manifestInfo.latestVersion == L"99.0.0", "Update static manifest version");
        Check(manifestInfo.updateAvailable, "Update static manifest update available");
        Check(manifestInfo.assetName == expectedAsset, "Update static manifest picks architecture asset");
        Check(!manifestInfo.expectedSha256.empty(), "Update static manifest sha256");
        Check(manifestInfo.checksumDownloadUrl.find(L"SHA256SUMS.txt") != std::wstring::npos, "Update static manifest checksum url");

        const std::wstring githubApiJson =
            L"{"
            L"\"tag_name\":\"v99.1.0\","
            L"\"html_url\":\"https://github.com/codingriver/quattro/releases/tag/v99.1.0\","
            L"\"body\":\"unit api\","
            L"\"assets\":["
            L"{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://github.com/codingriver/quattro/releases/download/v99.1.0/SHA256SUMS.txt\",\"size\":99},"
            L"{\"name\":\"Quattro-x64.exe\",\"browser_download_url\":\"https://github.com/codingriver/quattro/releases/download/v99.1.0/Quattro-x64.exe\",\"size\":111},"
            L"{\"name\":\"Quattro-x86.exe\",\"browser_download_url\":\"https://github.com/codingriver/quattro/releases/download/v99.1.0/Quattro-x86.exe\",\"size\":222}"
            L"]}";
        UpdateReleaseInfo apiInfo;
        std::wstring apiError;
        Check(UpdateCheckService::ParseReleaseInfoJson(githubApiJson, apiInfo, apiError), "Update GitHub API parse");
        Check(apiInfo.latestVersion == L"v99.1.0", "Update GitHub API version");
        Check(apiInfo.assetName == expectedAsset, "Update GitHub API picks architecture asset");
        Check(apiInfo.expectedSha256.empty(), "Update GitHub API no inline sha256");
        Check(apiInfo.checksumDownloadUrl.find(L"SHA256SUMS.txt") != std::wstring::npos, "Update GitHub API checksum url");
    }

    const std::filesystem::path temp = std::filesystem::temp_directory_path() / L"quattro_unit_conf.ini";
    std::filesystem::remove(temp, ec);
    ConfigService service(temp);
    AppConfig config;
    Check(config.autoDock, "Config default auto dock");
    Check(config.dockDelay == 1500, "Config default dock delay");
    Check(config.topMost, "Config default top most");
    Check(config.hideAfterLink, "Config default hide after link");
    Check(!config.hideWhenInactive, "Config default hide inactive");
    Check(!config.hideOnStart, "Config default hide on start");
    Check(!config.autoRun, "Config default auto run");
    Check(config.loggingEnabled, "Config default logging enabled");
    Check(!config.hideNotifyIcon, "Config default tray visible");
    Check(config.width == 400, "Config default width fits three link columns");
    config.width = 500;
    config.height = 700;
    config.theme = L"default";
    config.helpUrl = L"https://help.local/";
    config.updateUrl = L"https://update.local/";
    config.webDavEnabled = true;
    config.webDavUrl = L"https://dav.local/remote.php/dav/files/unit/";
    config.webDavRemotePath = L"/Quattro/backups/";
    config.webDavUserName = L"unit";
    config.webDavKeepCount = 7;
    config.loggingEnabled = false;
    config.httpServerEnabled = true;
    config.httpServerAutoStart = true;
    config.httpServerLanAccess = false;
    config.httpServerPort = 45211;
    config.httpServerRootPath = L"C:\\QuattroWeb";
    service.Save(config);

    AppConfig loaded = service.Load();
    Check(loaded.width == 500, "Config width");
    Check(loaded.height == 700, "Config height");
    Check(loaded.theme == L"default", "Config theme");
    Check(loaded.helpUrl == L"https://help.local/", "Config help url");
    Check(loaded.updateUrl == L"https://update.local/", "Config update url");
    Check(loaded.webDavEnabled, "Config webdav enabled");
    Check(loaded.webDavUrl == L"https://dav.local/remote.php/dav/files/unit/", "Config webdav url");
    Check(loaded.webDavRemotePath == L"/Quattro/backups/", "Config webdav remote path");
    Check(loaded.webDavUserName == L"unit", "Config webdav user");
    Check(loaded.webDavKeepCount == 7, "Config webdav keep count");
    Check(!loaded.loggingEnabled, "Config logging enabled");
    Check(loaded.httpServerEnabled, "Config http enabled");
    Check(loaded.httpServerAutoStart, "Config http autostart");
    Check(!loaded.httpServerLanAccess, "Config http LAN access");
    Check(loaded.httpServerPort == 45211, "Config http port");
    Check(loaded.httpServerRootPath == L"C:\\QuattroWeb", "Config http root");
    Check(FileExists(unitUserConfigRoot / L"webdav.ini"), "Config webdav stored in user config directory");
    Check(FileExists(unitUserConfigRoot / L"http.ini"), "Config http stored in user config directory");
    const std::wstring savedConfigText = LoadUtf8File(temp);
    Check(savedConfigText.find(L"WebDavUrl") == std::wstring::npos, "Config removes legacy webdav fields");
    Check(savedConfigText.find(L"HttpServerPort") == std::wstring::npos, "Config removes legacy http fields");
    Check(savedConfigText.find(L"password") == std::wstring::npos && savedConfigText.find(L"Password") == std::wstring::npos, "Config does not persist webdav password");
    std::filesystem::remove(temp, ec);

    const std::filesystem::path legacyTrayPath = std::filesystem::temp_directory_path() / L"quattro_unit_legacy_tray.ini";
    std::filesystem::remove(legacyTrayPath, ec);
    {
        std::ofstream legacyFile(legacyTrayPath, std::ios::binary | std::ios::trunc);
        legacyFile << "[main]\n";
        legacyFile << "bHideNotify=1\n";
    }
    ConfigService legacyTrayService(legacyTrayPath);
    AppConfig legacyTrayConfig = legacyTrayService.Load();
    Check(!legacyTrayConfig.hideNotifyIcon, "Config ignores legacy hidden tray icon");
    legacyTrayService.Save(legacyTrayConfig);
    Check(LoadUtf8File(legacyTrayPath).find(L"bHideNotify=0") != std::wstring::npos, "Config clears legacy hidden tray icon");
    std::filesystem::remove(legacyTrayPath, ec);

    const std::filesystem::path recoveryPath = std::filesystem::temp_directory_path() / L"quattro_unit_webdav_recovery.ini";
    std::filesystem::remove(recoveryPath, ec);
    WebDavRecoveryService recovery(recoveryPath);
    std::wstring recoveryError;
    Check(recovery.Save(config, recoveryError), "WebDAV recovery save");
    Check(recoveryError.empty(), "WebDAV recovery save no error");
    Check(recovery.HasRecoverableSettings(), "WebDAV recovery has settings");
    AppConfig recovered;
    Check(recovery.Load(recovered), "WebDAV recovery load");
    Check(recovered.webDavEnabled, "WebDAV recovery enabled");
    Check(recovered.webDavUrl == config.webDavUrl, "WebDAV recovery url");
    Check(recovered.webDavRemotePath == config.webDavRemotePath, "WebDAV recovery remote path");
    Check(recovered.webDavUserName == config.webDavUserName, "WebDAV recovery user");
    Check(recovered.webDavKeepCount == config.webDavKeepCount, "WebDAV recovery keep count");
    const std::wstring recoveryText = LoadUtf8File(recoveryPath);
    Check(recoveryText.find(L"password") == std::wstring::npos && recoveryText.find(L"Password") == std::wstring::npos, "WebDAV recovery does not persist password");
    Check(recoveryText.find(L"credential_target=Quattro.WebDavBackup.Default") != std::wstring::npos, "WebDAV recovery stores stable credential target");
    Check(WebDavCredentialService::TargetName(config) == L"Quattro.WebDavBackup.Default", "WebDAV credential stable target");
    std::filesystem::remove(recoveryPath, ec);

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
    <Component name="miniButton">
        <Metric name="height" value="21"/>
        <State name="normal" icon="rgb(10,11,12)"/>
    </Component>
    <Component name="progressBar">
        <Metric name="height" value="15"/>
        <State name="normal" fill="rgb(13,14,15)"/>
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
    Check(Near(customTheme.metric(L"miniButton", L"height", 0.0f), 21.0f), "Theme mini button metric parse");
    Check(Near(customTheme.color(L"miniButton", L"normal", L"icon").r, 10.0f / 255.0f), "Theme mini button component parse");
    Check(Near(customTheme.metric(L"progressBar", L"height", 0.0f), 15.0f), "Theme progress bar metric parse");
    Check(Near(customTheme.color(L"progressBar", L"normal", L"fill").r, 13.0f / 255.0f), "Theme progress bar component parse");
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
    Check(Near(fallbackTheme.metric(L"progressBar", L"height", 0.0f), 16.0f), "Theme default progress bar metric");
    Check(fallbackTheme.color(L"progressBar", L"normal", L"fill").a > 0.9f, "Theme default progress bar color");
    Check(fallbackTheme.color(L"global", L"warning", L"text").r > 0.5f, "Theme default semantic warning");
    Check(Near(fallbackTheme.metric(L"global", L"fieldHeight", 0.0f), 32.0f), "Theme default global metric");
    Check(Near(fallbackTheme.metric(L"dialog", L"contentInsetX", 0.0f), 28.0f), "Theme default dialog standard inset");
    Check(Near(fallbackTheme.metric(L"dialog", L"labelMinWidth", 0.0f), 20.0f), "Theme default dialog label min width");
    Check(Near(fallbackTheme.metric(L"dialog", L"compactRowGap", 0.0f), 6.0f), "Theme default dialog compact row gap");
    Check(Near(fallbackTheme.metric(L"dialog", L"miniFooterInsetY", 0.0f), 18.0f), "Theme default dialog mini footer inset");
    Check(Near(fallbackTheme.metric(L"dialog", L"miniFooterButtonWidth", 0.0f), 72.0f), "Theme default dialog mini footer button");
    Check(Near(fallbackTheme.metric(L"dialog", L"overlaySectionGap", 0.0f), 14.0f), "Theme default dialog overlay section gap");
    Check(Near(fallbackTheme.metric(L"miniButton", L"height", 0.0f), 24.0f), "Theme default mini button metric");
    Check(fallbackTheme.color(L"miniButton", L"hover", L"icon").a > 0.9f, "Theme default mini button hover");
    Check(Near(fallbackTheme.metric(L"tabButton", L"minTextWidth", 0.0f), 18.0f), "Theme default tab button min text width");
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
    bool hasRemovedAudioActions = false;
    bool genericCommandActionsHaveIconOverrides = true;
    std::unordered_set<std::wstring> systemFunctionKeys;
    for (std::size_t i = 0; i < systemFunctions.size(); ++i) {
        Check(systemFunctions[i].key && *systemFunctions[i].key, "System function stable key exists");
        Check(systemFunctionKeys.insert(systemFunctions[i].key).second, "System function stable key unique");
        if (std::wstring(systemFunctions[i].name) == L"我的电脑") {
            hasComputer = true;
            Link systemLink;
            Check(ConfigureSystemFunctionLink(i, systemLink), "System function configure computer");
            Check(systemLink.name == L"我的电脑", "System function computer name");
            Check(systemLink.type == 3, "System function computer type");
            Check(systemLink.path == L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", "System function computer path");
            Check(systemLink.parameter.empty(), "System function computer parameter");
            Check(systemLink.systemFunctionKey == L"this-pc", "System function computer stable key");
            Check(SystemFunctionForLink(systemLink) != nullptr, "System function computer lookup");
            Check(BuiltinSystemFunctionForLink(systemLink) != nullptr, "Builtin system function computer lookup");
            Check(BuiltinSystemContextMenuItems(systemLink).size() == 3, "This PC fixed context menu count");
        }
        if (std::wstring(systemFunctions[i].name) == L"网络") {
            Link networkLink;
            Check(ConfigureSystemFunctionLink(i, networkLink), "System function configure network");
            Check(BuiltinSystemContextMenuItems(networkLink).size() == 2, "Network fixed context menu count");
        }
        if (std::wstring(systemFunctions[i].name) == L"回收站") {
            Link recycleBinLink;
            Check(ConfigureSystemFunctionLink(i, recycleBinLink), "System function configure recycle bin");
            const auto contextItems = BuiltinSystemContextMenuItems(recycleBinLink);
            Check(contextItems.size() == 1 && contextItems.front().action == BuiltinSystemContextAction::EmptyRecycleBin,
                  "Recycle Bin fixed context menu");
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
            hasRemovedAudioActions = true;
        }
        if (std::wstring(systemFunctions[i].name) == L"关机") {
            Link shutdownLink;
            Check(ConfigureSystemFunctionLink(i, shutdownLink), "System function configure shutdown");
            Check(shutdownLink.path == L"shutdown.exe", "System function shutdown path");
            Check(shutdownLink.parameter == L"/s /t 0", "System function shutdown parameter");
            Check(shutdownLink.showCmd == SW_HIDE, "System function shutdown show cmd");
            MenuIcon parsedIcon = MenuIconNone;
            Check(TryParseMenuIconLinkIcon(shutdownLink.icon, parsedIcon), "System function shutdown stored menu icon");
            Check(parsedIcon == MenuIconPower, "System function shutdown stored icon matches menu");
        }
    }
    Check(hasComputer, "System function contains computer");
    Check(hasStartupFolders, "System function contains startup folders");
    Check(hasPowerActions, "System function contains power actions");
    Check(!hasRemovedAudioActions, "System function excludes audio actions");
    Check(genericCommandActionsHaveIconOverrides, "System function generic command icon overrides");
    Link customComputerLink;
    customComputerLink.name = L"自定义电脑入口";
    customComputerLink.type = 3;
    customComputerLink.path = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    customComputerLink.showCmd = SW_SHOWNORMAL;
    Check(BuiltinSystemContextMenuItems(customComputerLink).empty(), "Custom matching target has no fixed context menu");
    Check(!RestoreLegacyBuiltinSystemFunctionKey(customComputerLink), "Custom matching target is not restored as builtin");

    Link legacyComputerLink;
    legacyComputerLink.name = L"我的电脑";
    legacyComputerLink.type = 3;
    legacyComputerLink.path = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    legacyComputerLink.showCmd = SW_SHOWNORMAL;
    Check(RestoreLegacyBuiltinSystemFunctionKey(legacyComputerLink), "Legacy builtin system function key restored");
    Check(legacyComputerLink.systemFunctionKey == L"this-pc", "Legacy builtin stable key restored");
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
    Check(MenuIconFor(ID_MENU_SYSTEM_FUNCTION_BASE + 32, L"环境变量") == MenuIconSystem, "System function dynamic icon");
    Check(MenuIconFor(ID_MENU_MOVE_TO_BASE + 12, L"目标标签") == MenuIconMove, "Move target dynamic icon");
    Check(MenuIconFor(ID_MENU_COPY_TO_BASE + 12, L"目标标签") == MenuIconCopy, "Copy target dynamic icon");
    Check(MenuIconFor(ID_MENU_MOVE_TAG_TO_BASE + 12, L"目标分组") == MenuIconMove, "Move tag target dynamic icon");
    Check(MenuIconFor(ID_MENU_CUT_LINK, L"剪切") == MenuIconCut, "Cut command icon");
    Check(MenuIconFor(ID_MENU_PASTE_LINK, L"粘贴") == MenuIconPaste, "Paste command icon");
    Check(MenuIconFor(ID_MENU_COPY_PATH, L"复制路径") == MenuIconCopy, "Copy path command icon");
    Check(MenuIconFor(ID_MENU_ADD_NOTE_TAG, L"新建便签") == MenuIconNotebook, "Note tag command icon");
    Check(MenuIconFor(ID_MENU_ADD_TODO_ITEM, L"新增待办事项") == MenuIconList, "Todo item command icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"禁用待办事项") == MenuIconEyeOff, "Todo disable command icon");
    Check(MenuIconFor(ID_MENU_TOGGLE_TODO_ENABLED, L"启用待办事项") == MenuIconEye, "Todo enable command icon");
    Check(MenuIconFor(ID_MENU_TODO_SORT_DUE, L"按提醒时间") == MenuIconSort, "Todo sort command icon");
    Check(MenuIconFor(ID_MENU_ALL_SORT_POS, L"手动排序") == MenuIconSort, "Manual sort command icon");
    Check(MenuIconIsRenderable(MenuIconSortAsc), "Sort ascending icon is renderable");
    Check(MenuIconIsRenderable(MenuIconSortDesc), "Sort descending icon is renderable");
    Check(std::wstring(MenuIconName(MenuIconSortAsc)) == L"sort-asc", "Sort ascending icon name");
    Check(std::wstring(MenuIconName(MenuIconSortDesc)) == L"sort-desc", "Sort descending icon name");
    Check(MenuIconGlyph(MenuIconSortAsc) == static_cast<wchar_t>(0xEB26), "Sort ascending tabler glyph");
    Check(MenuIconGlyph(MenuIconSortDesc) == static_cast<wchar_t>(0xEB27), "Sort descending tabler glyph");
    Check(MenuIconFor(ID_MENU_TOOL_BASE + 2, L"秒表") == MenuIconTools, "Builtin tool dynamic icon");
    Check(MenuIconFor(ID_MENU_ALL_LAYOUT_LIST, L"列表") == MenuIconList, "List layout icon");
    Check(MenuIconFor(ID_MENU_ALL_LAYOUT_TILE, L"平铺") == MenuIconTile, "Tile layout icon");
    Check(std::filesystem::exists(L"icons/menu/tabler/tabler-icons.ttf"), "Local menu icon font exists");
    Check(std::filesystem::exists(L"icons/menu/tabler/tabler-icons.css"), "Local menu icon css exists");
    Check(std::filesystem::exists(L"icons/menu/tabler/LICENSE"), "Local menu icon license exists");

    const std::filesystem::path pluginRoot = std::filesystem::temp_directory_path() / L"quattro_plugin_unit";
    std::filesystem::remove_all(pluginRoot, ec);
    PluginRegistry pluginRegistry(pluginRoot);
    auto plugins = pluginRegistry.LoadPlugins();
    Check(plugins.size() >= 3, "Plugin registry builtin count");
    bool hasAutoClickerName = false;
    bool hasProcessLocator = false;
    for (const auto& plugin : plugins) {
        if (plugin.id == L"quattro.builtin.clicker" && plugin.name == L"连点器") {
            hasAutoClickerName = true;
        }
        if (plugin.id == L"quattro.builtin.process-locator" &&
            plugin.name == L"进程定位器" &&
            plugin.engine == L"process-locator") {
            hasProcessLocator = true;
        }
    }
    Check(hasAutoClickerName, "Plugin clicker display name");
    Check(hasProcessLocator, "Plugin process locator registration");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.clicker"), "Plugin clicker enabled by default");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.timer"), "Plugin timer enabled by default");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.process-locator"), "Plugin process locator enabled by default");
    Check(pluginRegistry.SetEnabled(L"quattro.builtin.clicker", false), "Plugin builtin disable");
    Check(!pluginRegistry.IsEnabled(L"quattro.builtin.clicker"), "Plugin builtin can be disabled");
    Check(pluginRegistry.SetSetting(L"quattro.builtin.clicker", L"interval", L"250"), "Plugin setting save");
    Check(pluginRegistry.GetSetting(L"quattro.builtin.clicker", L"interval") == L"250", "Plugin setting load");
    PluginRegistry reloadedPluginRegistry(pluginRoot);
    Check(!reloadedPluginRegistry.IsEnabled(L"quattro.builtin.clicker"), "Plugin registry reload builtin disabled");
    std::filesystem::remove_all(pluginRoot, ec);

    Check(DefaultLinkSortDirection(1) == 1, "Link run count default descending");
    Check(DefaultLinkSortDirection(2) == 0, "Link name default ascending");
    Check(InitialSortKey(L" Beta") == L"B|beta", "Initial sort key trims and lowercases ascii");
    Check(InitialSortKey(L"9-app") == L"0|9-app", "Initial sort key groups numeric names");
    Link alphaLink;
    alphaLink.id = 1;
    alphaLink.name = L"Alpha";
    alphaLink.pos = 2;
    alphaLink.runCount = 3;
    Link betaLink;
    betaLink.id = 2;
    betaLink.name = L"Beta";
    betaLink.pos = 1;
    betaLink.runCount = 7;
    Check(LinkSortLess(alphaLink, betaLink, 0, 0) == false, "Manual link sort uses position ascending");
    Check(LinkSortLess(alphaLink, betaLink, 1, 0), "Run count ascending sort");
    Check(LinkSortLess(betaLink, alphaLink, 1, 1), "Run count descending sort");
    Check(LinkSortLess(alphaLink, betaLink, 2, 0), "Name ascending sort");
    Check(LinkSortLess(betaLink, alphaLink, 2, 1), "Name descending sort");

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
    tag.sort = 1;
    tag.sortDirection = 1;
    tag.content = L"all";
    Check(storage.InsertGroup(tag) && tag.id > 0, "Storage insert tag");
    tag.sort = 2;
    tag.sortDirection = 0;
    Check(storage.UpdateGroup(tag), "Storage update tag sort direction");

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
    Check(storage.InsertTodoItem(todo) && todo.id > 0 && todo.nextDueAt == L"2026-06-26 09:30:00" && todo.enabled, "Storage insert todo");
    todo.content = L"updated details";
    Check(storage.UpdateTodoItem(todo), "Storage update todo");
    Check(storage.SetTodoEnabled(todo.id, false), "Storage disable todo");
    Check(storage.SetTodoCompleted(todo.id, true), "Storage complete todo");

    TodoItem intervalTodo;
    intervalTodo.tagId = todoTag.id;
    intervalTodo.title = L"IntervalTodoItem";
    intervalTodo.scheduleKind = TodoScheduleKind::Minutely;
    intervalTodo.repeatMode = TodoRepeatMode::Interval;
    intervalTodo.repeatInterval = 5;
    intervalTodo.repeatLimit = 3;
    intervalTodo.anchorAt = L"2026-06-26 09:30:10";
    intervalTodo.pos = -1;
    Check(storage.InsertTodoItem(intervalTodo) && intervalTodo.id > 0 && intervalTodo.repeatMode == TodoRepeatMode::Interval, "Storage insert interval todo");

    TodoItem cronTodo;
    cronTodo.tagId = todoTag.id;
    cronTodo.title = L"CronTodoItem";
    cronTodo.scheduleKind = TodoScheduleKind::Cron;
    cronTodo.cronExpression = L" 0 30 9 * * * ";
    cronTodo.pos = -1;
    Check(storage.InsertTodoItem(cronTodo) && cronTodo.id > 0 && cronTodo.cronExpression == L"0 30 9 * * *" && !cronTodo.nextDueAt.empty(), "Storage insert cron todo");

    Link link;
    link.name = L"UnitLink";
    link.parentGroup = tag.id;
    link.path = L"https://example.com";
    link.type = 2;
    link.pos = -1;
    link.pidl = {6, 0, 1, 2, 3, 4, 0, 0};
    link.systemFunctionKey = L"this-pc";
    Check(storage.InsertLink(link) && link.id > 0, "Storage insert link");

    link.remark = L"updated";
    Check(storage.UpdateLink(link), "Storage update link");
    Check(storage.IncrementRunCount(link.id, 5), "Storage run count");

    AppModel loadedModel = storage.Load();
    bool foundTagSortDirection = false;
    for (const auto& item : loadedModel.groups) {
        if (item.id == tag.id && item.sort == 2 && item.sortDirection == 0) {
            foundTagSortDirection = true;
            break;
        }
    }
    Check(foundTagSortDirection, "Storage reload tag sort direction");
    bool foundLink = false;
    for (const auto& item : loadedModel.links) {
        if (item.id == link.id && item.remark == L"updated" && item.runCount == 5 && item.pidl == link.pidl &&
            item.systemFunctionKey == link.systemFunctionKey) {
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
    bool foundIntervalTodo = false;
    bool foundCronTodo = false;
    for (const auto& item : loadedModel.todos) {
        if (item.id == todo.id && item.content == L"updated details" && !item.enabled && !item.completedAt.empty()) {
            foundTodo = true;
        }
        if (item.id == intervalTodo.id && item.repeatMode == TodoRepeatMode::Interval && item.repeatInterval == 5 && item.repeatLimit == 3) {
            foundIntervalTodo = true;
        }
        if (item.id == cronTodo.id && item.scheduleKind == TodoScheduleKind::Cron && item.cronExpression == L"0 30 9 * * *" && !item.nextDueAt.empty()) {
            foundCronTodo = true;
        }
    }
    Check(foundTodo, "Storage reload todo");
    Check(foundIntervalTodo, "Storage reload interval todo");
    Check(foundCronTodo, "Storage reload cron todo");
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

    const std::filesystem::path legacySortRoot = std::filesystem::temp_directory_path() / L"quattro_sort_direction_legacy_unit";
    std::filesystem::remove_all(legacySortRoot, ec);
    std::filesystem::create_directories(legacySortRoot / L"db", ec);
    {
        sqlite3* legacyDb = nullptr;
        Check(sqlite3_open16((legacySortRoot / L"db" / L"link.db").c_str(), &legacyDb) == SQLITE_OK && legacyDb, "Legacy sort db open");
        if (legacyDb) {
            Check(ExecSql(
                      legacyDb,
                      "CREATE TABLE Version(ID INTEGER PRIMARY KEY, Ver INTEGER);"
                      "INSERT INTO Version(ID,Ver) VALUES(1,20000);"
                      "CREATE TABLE Groups("
                      "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "NAME CHARACTER(16) NOT NULL,"
                      "TYPE INTEGER DEFAULT 0,"
                      "SORT INTEGER DEFAULT 0,"
                      "POS INTEGER DEFAULT 0,"
                      "ParentGroup INTEGER DEFAULT 0,"
                      "ICON TEXT,"
                      "LAYOUT INTEGER DEFAULT 0,"
                      "ICONSIZE INTEGER DEFAULT 0,"
                      "FLAG INTEGER DEFAULT 0,"
                      "Content TEXT);"
                      "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT,SORT,TYPE) VALUES(1,'LegacyGroup',0,0,0,0,0,0);"
                      "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT,SORT,TYPE) VALUES(2,'LegacyTag',0,1,32,1,1,0);"),
                  "Legacy sort db seed");
            sqlite3_close(legacyDb);
        }
    }
    StorageService legacySortStorage(legacySortRoot);
    AppModel legacySortModel = legacySortStorage.Load();
    Check(legacySortStorage.sqliteAvailable(), "Legacy sort storage sqlite available");
    bool legacyRunCountDirectionMigrated = false;
    for (const auto& item : legacySortModel.groups) {
        if (item.name == L"LegacyTag" && item.sort == 1 && item.sortDirection == 1) {
            legacyRunCountDirectionMigrated = true;
            break;
        }
    }
    Check(legacyRunCountDirectionMigrated, "Legacy run count sort migrates to descending");
    std::filesystem::remove_all(legacySortRoot, ec);

    const std::filesystem::path legacySystemKeyRoot = std::filesystem::temp_directory_path() / L"quattro_system_function_key_legacy_unit";
    std::filesystem::remove_all(legacySystemKeyRoot, ec);
    std::filesystem::create_directories(legacySystemKeyRoot / L"db", ec);
    {
        sqlite3* legacyDb = nullptr;
        Check(sqlite3_open16((legacySystemKeyRoot / L"db" / L"link.db").c_str(), &legacyDb) == SQLITE_OK && legacyDb,
              "Legacy system function key db open");
        if (legacyDb) {
            Check(ExecSql(
                      legacyDb,
                      "CREATE TABLE Version(ID INTEGER PRIMARY KEY, Ver INTEGER);"
                      "INSERT INTO Version(ID,Ver) VALUES(1,20001);"
                      "CREATE TABLE Groups("
                      "ID INTEGER PRIMARY KEY AUTOINCREMENT,NAME CHARACTER(16) NOT NULL,TYPE INTEGER DEFAULT 0,"
                      "SORT INTEGER DEFAULT 0,SORTDIRECTION INTEGER DEFAULT 0,POS INTEGER DEFAULT 0,"
                      "ParentGroup INTEGER DEFAULT 0,ICON TEXT,LAYOUT INTEGER DEFAULT 0,ICONSIZE INTEGER DEFAULT 0,"
                      "FLAG INTEGER DEFAULT 0,Content TEXT);"
                      "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT) VALUES(1,'LegacyGroup',0,0,0,0);"
                      "INSERT INTO Groups(ID,NAME,POS,ParentGroup,ICONSIZE,LAYOUT) VALUES(2,'LegacyTag',0,1,32,0);"
                      "CREATE TABLE Links("
                      "ID INTEGER PRIMARY KEY AUTOINCREMENT,NAME TEXT NOT NULL,POS INTEGER DEFAULT 0,RunCount INTEGER DEFAULT 0,"
                      "ParentGroup INTEGER DEFAULT 0,TYPE INTEGER DEFAULT 0,ICON TEXT NOT NULL DEFAULT '',PATH TEXT NOT NULL DEFAULT '',"
                      "Parameter TEXT NOT NULL DEFAULT '',WorkDir TEXT NOT NULL DEFAULT '',HotKey INTEGER DEFAULT 0,ShowCmd INTEGER DEFAULT 0,"
                      "IsAdmin INTEGER DEFAULT 0,IsCustomColor INTEGER DEFAULT 0,CustomColor CHARACTER(10),Remark TEXT,Pidl BLOB);"
                      "INSERT INTO Links(ID,NAME,POS,ParentGroup,TYPE,PATH,ShowCmd) "
                      "VALUES(1,'我的电脑',0,2,3,'::{20D04FE0-3AEA-1069-A2D8-08002B30309D}',1);"),
                  "Legacy system function key db seed");
            sqlite3_close(legacyDb);
        }
    }
    StorageService legacySystemKeyStorage(legacySystemKeyRoot);
    AppModel legacySystemKeyModel = legacySystemKeyStorage.Load();
    Check(legacySystemKeyStorage.sqliteAvailable(), "Legacy system function key storage sqlite available");
    Check(legacySystemKeyModel.links.size() == 1 && legacySystemKeyModel.links.front().systemFunctionKey.empty(),
          "Legacy link survives system function key migration");
    if (!legacySystemKeyModel.links.empty()) {
        legacySystemKeyModel.links.front().systemFunctionKey = L"this-pc";
        Check(legacySystemKeyStorage.UpdateLink(legacySystemKeyModel.links.front()), "Migrated link writes system function key");
        AppModel reloadedLegacySystemKeyModel = legacySystemKeyStorage.Load();
        Check(reloadedLegacySystemKeyModel.links.size() == 1 &&
                  reloadedLegacySystemKeyModel.links.front().systemFunctionKey == L"this-pc",
              "Migrated link reloads system function key");
    }
    std::filesystem::remove_all(legacySystemKeyRoot, ec);

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
    sourceNoteTag.sort = 2;
    sourceNoteTag.sortDirection = 1;
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
    sourceLink.systemFunctionKey = L"this-pc";
    Check(sourceStorage.InsertLink(sourceLink), "Package source link insert");
    Link duplicateSourceLink;
    duplicateSourceLink.name = L"PackageDuplicateLink";
    duplicateSourceLink.parentGroup = sourceTag.id;
    duplicateSourceLink.path = L"https://target.example";
    duplicateSourceLink.type = 2;
    duplicateSourceLink.pos = -1;
    Check(sourceStorage.InsertLink(duplicateSourceLink), "Package source duplicate link insert");

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

    ConfigPackageService packageExporter(packageSourceRoot);
    ConfigPackageOptions packageOptions;
    ConfigPackageReport exportReport = packageExporter.ExportPackage(packagePath, packageOptions);
    Check(exportReport.ok && FileExists(packagePath), "Package export succeeds");

    ConfigPackageService packageImporter(packageTargetRoot);
    ConfigPackageReport importReport = packageImporter.ImportPackageMerge(packagePath, packageOptions);
    Check(importReport.ok, "Package merge import succeeds");
    Check(importReport.groupsAdded == 0, "Package merge reuses matching groups");
    Check(importReport.groupsMerged >= 2, "Package merge reports reused groups");
    Check(importReport.tagsAdded == 2, "Package merge import new tags");
    Check(importReport.tagsMerged >= 2, "Package merge reports reused tags");
    Check(importReport.linksAdded == 1, "Package merge import link count");
    Check(importReport.linksSkippedDuplicate == 1, "Package merge skips duplicate link");
    Check(importReport.notesAdded == 1, "Package merge import note count");
    Check(importReport.todosAdded == 1, "Package merge import todo count");
    Check(importReport.urlIconsAdded == 1, "Package merge import url icon count");

    AppModel mergedModel = targetStorage.Load();
    bool hasExistingLink = false;
    bool hasImportedLink = false;
    bool importedLinkParentExists = false;
    bool hasRenamedGroup = false;
    bool importedTagPreservesSortDirection = false;
    int duplicateLinkCount = 0;
    for (const auto& item : mergedModel.groups) {
        if (item.parentGroup == 0 && item.name == L"MergeGroup (导入)") {
            hasRenamedGroup = true;
        }
        if (item.name == L"PackageNote" && item.sort == 2 && item.sortDirection == 1) {
            importedTagPreservesSortDirection = true;
        }
    }
    for (const auto& item : mergedModel.links) {
        if (item.name == L"ExistingLink" && item.path == L"https://target.example") {
            hasExistingLink = true;
        }
        if (item.path == L"https://target.example") {
            ++duplicateLinkCount;
        }
        if (item.name == L"PackageLink" && item.path == L"https://package.example" && item.runCount == 9 &&
            item.systemFunctionKey == L"this-pc") {
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
    Check(!hasRenamedGroup, "Package merge reuses conflicting group");
    Check(importedTagPreservesSortDirection, "Package merge preserves imported tag sort direction");
    Check(duplicateLinkCount == 1, "Package merge keeps one duplicate link");
    Check(hasImportedNote, "Package merge imports note");
    Check(hasImportedTodo, "Package merge imports todo");
    Check(FileExists(packageTargetRoot / L"icons" / L"url" / L"example.png"), "Package merge imports url icon");
    Check(!FileExists(packageTargetRoot / L"theme" / L"source.xml"), "Package merge does not import theme");
    Check(DirectoryExists(packageTargetRoot / L"backups"), "Package merge creates backup");
    std::filesystem::remove_all(packageSourceRoot, ec);
    std::filesystem::remove_all(packageTargetRoot, ec);
    std::filesystem::remove(packagePath, ec);
    std::filesystem::remove_all(unitUserConfigRoot, ec);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);

    if (failures == 0) {
        std::cout << "unit tests complete\n";
    }
    return failures == 0 ? 0 : 1;
}
