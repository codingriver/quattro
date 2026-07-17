#include "../src/domain/Config.h"
#include "../src/domain/ConfigVersion.h"
#include "../src/common/EmbeddedAssetInstaller.h"
#include "../src/domain/LinkSorting.h"
#include "../src/services/ConfigPackageService.h"
#include "../src/services/ContextMenuProviderIconService.h"
#include "../src/services/FileLockQueryService.h"
#include "../src/services/Launcher.h"
#include "../src/domain/MenuCatalog.h"
#include "../src/domain/PluginRegistry.h"
#include "../src/services/ShellContextMenuCacheService.h"
#include "../src/services/ShellContextMenuRefreshService.h"
#include "../src/services/ShellItemService.h"
#include "../src/services/SelectedPathCopyService.h"
#include "../src/services/TerminalContextMenuService.h"
#include "../src/services/Storage.h"
#include "../src/services/SystemFunctions.h"
#include "../src/services/UpdateCheckService.h"
#include "../src/theme/Theme.h"
#include "../src/theme/CatalogDialogLayout.h"
#include "../src/theme/ThemedFormLayout.h"
#include "../src/theme/ThemedUi.h"
#include "../src/theme/ThemedControls.h"
#include "../src/theme/ThemedWindowUi.h"
#include "../src/theme/ThemedD2D.h"
#include "../src/theme/ThemedGdiFallback.h"
#include "../src/domain/TodoSchedule.h"
#include "../src/common/Utilities.h"
#include "../src/common/EmbeddedExecutableManager.h"
#include "../src/services/WebDavClient.h"
#include "../src/services/WebDavCredentialService.h"
#include "../src/services/WebDavRecoveryService.h"
#include "../src/windows/MenuAnchorGeometry.h"
#include "../src/windows/MainTitleBuildMarkerLayout.h"
#include "../src/windows/ToastFeedback.h"
#include "Version.h"

#include <sqlite3.h>

#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <string>
#include <vector>

#ifndef HDS_NOSIZING
#define HDS_NOSIZING 0x0800
#endif

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

class TestTooltipRegistry final : public ThemedTooltipRegistry {
public:
    void ShowTooltip(
        const std::wstring& text,
        POINT screenPoint,
        const ThemedTooltipOptions& options) override {
        shownText = text;
        shownPoint = screenPoint;
        shownOptions = options;
        visible = true;
        ++showCount;
    }

    void HideTooltip() override {
        visible = false;
        ++hideCount;
    }

    std::wstring shownText;
    POINT shownPoint{};
    ThemedTooltipOptions shownOptions{};
    bool visible = false;
    int showCount = 0;
    int hideCount = 0;
};
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
    Check(FormatByteSizeForDisplay(0) == L"0 B", "Byte size display zero");
    Check(FormatByteSizeForDisplay(1024) == L"1.00 KB", "Byte size display kilobytes");
    Check(FormatByteSizeForDisplay(12ull * 1024ull * 1024ull) == L"12.0 MB", "Byte size display megabytes");
    {
        const std::filesystem::path fileLockRoot = std::filesystem::temp_directory_path() /
            (L"quattro_file_lock_unit_" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(fileLockRoot, ec);
        std::filesystem::create_directories(fileLockRoot / L"nested");
        for (int index = 0; index < 48; ++index) {
            const std::filesystem::path parent = index % 2 == 0 ? fileLockRoot : fileLockRoot / L"nested";
            std::ofstream(parent / (L"resource-" + std::to_wstring(index) + L".txt"), std::ios::binary) << "unit";
        }

        std::vector<FileLockQueryProgress> progressEvents;
        FileLockQueryOptions parallelOptions;
        parallelOptions.batchSize = 4;
        parallelOptions.maxWorkers = 4;
        const FileLockQueryResult parallelResult = QueryFileLocks(
            fileLockRoot.wstring(),
            {},
            [&](const FileLockQueryProgress& progress) { progressEvents.push_back(progress); },
            parallelOptions);
        Check(parallelResult.error.empty(), "File lock directory query succeeds");
        Check(parallelResult.directory, "File lock query identifies directory input");
        Check(parallelResult.totalPaths == 49 && parallelResult.checkedPaths == 49,
            "File lock query checks root and every regular file");
        Check(parallelResult.workerCount >= 2 && parallelResult.workerCount <= 4,
            "File lock directory query uses bounded parallel workers");
        Check(
            std::any_of(progressEvents.begin(), progressEvents.end(), [](const FileLockQueryProgress& progress) {
                return progress.phase == FileLockQueryPhase::Enumerating;
            }),
            "File lock query reports enumeration progress");
        Check(
            !progressEvents.empty() && progressEvents.back().phase == FileLockQueryPhase::Completed &&
                progressEvents.back().checkedPaths == progressEvents.back().totalPaths,
            "File lock query reports completed progress");

        std::atomic_bool cancelRequested{false};
        FileLockQueryOptions cancelOptions;
        cancelOptions.batchSize = 1;
        cancelOptions.maxWorkers = 2;
        cancelOptions.batchDelay = std::chrono::milliseconds(3);
        const FileLockQueryResult cancelledResult = QueryFileLocks(
            fileLockRoot.wstring(),
            [&]() { return cancelRequested.load(); },
            [&](const FileLockQueryProgress& progress) {
                if (progress.phase == FileLockQueryPhase::Querying && progress.checkedPaths >= 2) {
                    cancelRequested.store(true);
                }
            },
            cancelOptions);
        Check(cancelledResult.cancelled, "File lock directory query supports cancellation");
        Check(cancelledResult.checkedPaths < cancelledResult.totalPaths,
            "File lock cancellation stops before all paths are checked");

        const FileLockQueryResult fileResult = QueryFileLocks((fileLockRoot / L"resource-0.txt").wstring());
        Check(fileResult.error.empty() && !fileResult.directory && fileResult.totalPaths == 1 &&
                fileResult.checkedPaths == 1 && fileResult.workerCount == 1,
            "File lock single-file query keeps the lightweight one-worker path");
        const FileLockQueryResult missingResult = QueryFileLocks((fileLockRoot / L"missing.txt").wstring());
        Check(!missingResult.error.empty(), "File lock query reports a missing path");
        std::filesystem::remove_all(fileLockRoot, ec);
    }
    for (const UINT dpi : {96u, 120u, 144u}) {
        const float scale = static_cast<float>(dpi) / 96.0f;
        const float textLeft = 36.0f * scale;
        const float textEnd = 276.0f * scale;
        const float markerWidth = 78.0f * scale;
        const float gap = 7.0f * scale;
        const MainTitleBuildMarkerLayout layout = CalculateMainTitleBuildMarkerLayout(
            textLeft, textEnd, markerWidth, gap);
        Check(
            layout.nameEnd <= layout.markerLeft - gap + 0.01f &&
                layout.markerLeft >= textLeft && layout.markerLeft + markerWidth <= textEnd + 0.01f,
            "Main title build marker layout keeps text separated at supported DPI");
    }
    Check(QuattroUserConfigDirectory() == unitUserConfigRoot, "User config env override");
    Check(QuattroEmbeddedExecutableRootDirectory() == unitUserConfigRoot / L"tools",
        "Embedded executable root follows user config override");
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    Check(SuppressForegroundActivation(), "Test no-focus flag accepts enabled value");
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"off");
    Check(!SuppressForegroundActivation(), "Test no-focus flag rejects disabled value");
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"yes");
    Check(QuattroTestMode(), "Quattro test mode accepts enabled value");
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    Check(BackgroundAcceptanceMode(), "Background acceptance mode is detected");
    ApplyWindowBackgroundPolicy(nullptr);
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"interactive");
    Check(!BackgroundAcceptanceMode(), "Interactive acceptance mode is not background");
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", nullptr);
    Check(
        SelectedPathCopyService::FormatPaths({L"C:\\资料\\报告.docx", L"D:\\项目", L"\\\\server\\share\\文件.txt"}) ==
            L"C:\\资料\\报告.docx\r\nD:\\项目\r\n\\\\server\\share\\文件.txt",
        "Selected paths use CRLF without trailing newline");
    Check(SelectedPathCopyService::FormatPaths({L"C:\\single.txt"}) == L"C:\\single.txt",
        "Single selected path has no separator");
    Check(SelectedPathCopyService::FormatPaths({}).empty(), "Empty selected path list formats empty");

    const std::wstring clipboardPaths = L"C:\\资料\\报告.docx\r\nD:\\项目";
    std::wstring clipboardError;
    Check(SelectedPathCopyService::WriteClipboardText(nullptr, clipboardPaths, clipboardError),
        "Selected paths clipboard write succeeds");
    std::wstring clipboardRoundTrip;
    if (OpenClipboard(nullptr)) {
        HANDLE clipboardHandle = GetClipboardData(CF_UNICODETEXT);
        if (clipboardHandle) {
            const wchar_t* clipboardValue = static_cast<const wchar_t*>(GlobalLock(clipboardHandle));
            if (clipboardValue) {
                clipboardRoundTrip = clipboardValue;
                GlobalUnlock(clipboardHandle);
            }
        }
        CloseClipboard();
    }
    Check(clipboardRoundTrip == clipboardPaths, "Selected paths clipboard round trip");
    clipboardError.clear();
    Check(!SelectedPathCopyService::WriteClipboardText(nullptr, L"", clipboardError) && !clipboardError.empty(),
        "Empty selected paths do not overwrite clipboard");
    {
        static const unsigned char bytes[] = {'a', 'b', 'c'};
        const EmbeddedExecutableDescriptor descriptor{
            L"unit-tool",
            L"UnitTool.exe",
            L"1.2.3",
            L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            bytes,
            sizeof(bytes),
        };
        const std::filesystem::path embeddedRoot = unitUserConfigRoot / L"embedded-test";
        EmbeddedExecutablePrepareResult first = PrepareEmbeddedExecutable(descriptor, {embeddedRoot});
        Check(first.success && first.written && !first.updated, "Embedded executable first prepare writes file");
        Check(first.path == embeddedRoot / L"unit-tool" / L"1.2.3" / L"UnitTool.exe",
            "Embedded executable uses id and version directories");
        Check(LoadUtf8File(first.path) == L"abc", "Embedded executable writes exact bytes");
        EmbeddedExecutablePrepareResult second = PrepareEmbeddedExecutable(descriptor, {embeddedRoot});
        Check(second.success && !second.written && !second.updated, "Embedded executable reuses matching file");
        {
            std::ofstream corrupt(first.path, std::ios::binary | std::ios::trunc);
            corrupt << "damaged";
        }
        EmbeddedExecutablePrepareResult repaired = PrepareEmbeddedExecutable(descriptor, {embeddedRoot});
        Check(repaired.success && repaired.updated, "Embedded executable repairs mismatched file");
        Check(LoadUtf8File(repaired.path) == L"abc", "Embedded executable repair restores exact bytes");
        EmbeddedExecutableDescriptor invalid = descriptor;
        invalid.id = L"..";
        Check(!PrepareEmbeddedExecutable(invalid, {embeddedRoot}).success,
            "Embedded executable rejects unsafe path components");
    }
    {
        ThemedMenuFontCache menuFont;
        HFONT font96 = menuFont.FontForDpi(96);
        LOGFONTW actual{};
        NONCLIENTMETRICSW expected{};
        expected.cbSize = sizeof(expected);
        const bool hasExpected = SystemParametersInfoForDpi(
            SPI_GETNONCLIENTMETRICS, sizeof(expected), &expected, 0, 96) != FALSE;
        Check(font96 != nullptr, "System menu font creates");
        Check(menuFont.FontForDpi(96) == font96, "System menu font caches by DPI");
        Check(GetObjectW(font96, sizeof(actual), &actual) == sizeof(actual), "System menu font exposes LOGFONT");
        if (hasExpected) {
            Check(std::wstring(actual.lfFaceName) == expected.lfMenuFont.lfFaceName, "System menu font matches face");
            Check(actual.lfHeight == expected.lfMenuFont.lfHeight, "System menu font matches height");
            Check(actual.lfWeight == expected.lfMenuFont.lfWeight, "System menu font matches weight");
        }
        const SIZE measured = menuFont.MeasureText(nullptr, L"打开文件位置");
        Check(measured.cx > 0 && measured.cy > 0, "System menu font measures text");
        Check(menuFont.Scale(28) == 28, "System menu metrics preserve 96 DPI");
        HFONT font144 = menuFont.FontForDpi(144);
        NONCLIENTMETRICSW expected144{};
        expected144.cbSize = sizeof(expected144);
        LOGFONTW actual144{};
        const bool hasExpected144 = SystemParametersInfoForDpi(
            SPI_GETNONCLIENTMETRICS, sizeof(expected144), &expected144, 0, 144) != FALSE;
        Check(font144 != nullptr, "System menu font creates at 150 percent DPI");
        Check(GetObjectW(font144, sizeof(actual144), &actual144) == sizeof(actual144),
            "System menu font exposes 150 percent DPI LOGFONT");
        if (hasExpected144) {
            Check(actual144.lfHeight == expected144.lfMenuFont.lfHeight,
                "System menu font matches 150 percent DPI height");
        }
        Check(menuFont.Scale(28) == 42, "System menu metrics scale at 150 percent DPI");
    }
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
        Check(manifestInfo.assetSizeBytes == (expectedAsset == L"Quattro-x64.exe" ? 123ull : 456ull), "Update static manifest asset size");
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
        Check(apiInfo.assetSizeBytes == (expectedAsset == L"Quattro-x64.exe" ? 111ull : 222ull), "Update GitHub API asset size");
        Check(apiInfo.expectedSha256.empty(), "Update GitHub API no inline sha256");
        Check(apiInfo.checksumDownloadUrl.find(L"SHA256SUMS.txt") != std::wstring::npos, "Update GitHub API checksum url");
    }

    const std::filesystem::path temp = std::filesystem::temp_directory_path() / L"quattro_unit_conf.ini";
    std::filesystem::remove(temp, ec);
    ConfigService service(temp);
    AppConfig config;
    Check(config.autoDock, "Config default auto dock");
    Check(config.dockDelay == 1000, "Config default dock delay");
    Check(!config.topMost, "Local config default is not top most");
    Check(config.hideAfterLink, "Config default hide after link");
    Check(!config.hideWhenInactive, "Config default hide inactive");
    Check(!config.hideOnStart, "Config default hide on start");
    Check(!config.autoRun, "Config default auto run");
    Check(config.loggingEnabled, "Config default logging enabled");
    for (const auto& provider : TrackedContextMenuProviders()) {
        Check(!(config.*(provider.configMember)), "Config default context menu tracking disabled");
    }
    Check(!config.hideNotifyIcon, "Config default tray visible");
    Check(config.width == 400, "Config default width fits three link columns");
    Check(config.copySelectedPathsHotKey == L'C', "Config default copy selected paths hotkey");
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
    for (const auto& provider : TrackedContextMenuProviders()) {
        config.*(provider.configMember) = true;
    }
    config.httpServerEnabled = true;
    config.httpServerAutoStart = true;
    config.httpServerLanAccess = false;
    config.httpServerPort = 45211;
    config.httpServerRootPath = L"C:\\QuattroWeb";
    config.copySelectedPathsHotKey = L'X';
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
    for (const auto& provider : TrackedContextMenuProviders()) {
        Check(loaded.*(provider.configMember), "Config context menu tracking round trip");
    }
    Check(loaded.httpServerEnabled, "Config http enabled");
    Check(loaded.httpServerAutoStart, "Config http autostart");
    Check(!loaded.httpServerLanAccess, "Config http LAN access");
    Check(loaded.httpServerPort == 45211, "Config http port");
    Check(loaded.httpServerRootPath == L"C:\\QuattroWeb", "Config http root");
    Check(loaded.copySelectedPathsHotKey == L'X', "Config copy selected paths hotkey round trip");
    Check(FileExists(unitUserConfigRoot / L"webdav.ini"), "Config webdav stored in user config directory");
    Check(FileExists(unitUserConfigRoot / L"http.ini"), "Config http stored in user config directory");
    Check(FileExists(unitUserConfigRoot / L"context-menu.ini"), "Context menu settings stored in user config directory");
    const std::wstring savedConfigText = LoadUtf8File(temp);
    Check(savedConfigText.find(L"WebDavUrl") == std::wstring::npos, "Config removes legacy webdav fields");
    Check(savedConfigText.find(L"HttpServerPort") == std::wstring::npos, "Config removes legacy http fields");
    Check(savedConfigText.find(L"bTrackGitContextMenu") == std::wstring::npos, "Config removes legacy context menu fields");
    Check(savedConfigText.find(L"password") == std::wstring::npos && savedConfigText.find(L"Password") == std::wstring::npos, "Config does not persist webdav password");
    std::filesystem::remove(temp, ec);

    const std::filesystem::path migrateConfigPath = std::filesystem::temp_directory_path() / L"quattro_unit_conf_migrate.ini";
    std::filesystem::remove(migrateConfigPath, ec);
    WritePrivateProfileStringW(L"main", L"nVersion", L"0", migrateConfigPath.c_str());
    WritePrivateProfileStringW(L"main", L"bTopMost", L"0", migrateConfigPath.c_str());
    WritePrivateProfileStringW(L"main", L"nWidth", L"777", migrateConfigPath.c_str());
    WritePrivateProfileStringW(L"main", L"nHeight", L"99999", migrateConfigPath.c_str());
    WritePrivateProfileStringW(L"main", L"bShowBtnSkin", L"2", migrateConfigPath.c_str());
    WritePrivateProfileStringW(L"main", L"TagAlign", L"sideways", migrateConfigPath.c_str());
    WritePrivateProfileStringW(L"main", L"Theme", L"customTheme", migrateConfigPath.c_str());
    ConfigService migrateService(migrateConfigPath);
    Check(!migrateService.UpgradeToSchemaVersion(kCurrentConfigSchemaVersion), "Config migration reports incompatible fields");
    AppConfig migratedConfig = migrateService.Load();
    Check(migratedConfig.version == kCurrentConfigSchemaVersion, "Config migration writes current schema version");
    Check(!migratedConfig.topMost, "Config migration preserves valid bool field");
    Check(migratedConfig.width == 777, "Config migration preserves valid integer field");
    Check(migratedConfig.height == AppConfig{}.height, "Config migration resets invalid clamped integer");
    Check(migratedConfig.showSkinButton == AppConfig{}.showSkinButton, "Config migration resets invalid bool");
    Check(migratedConfig.tagAlign == AppConfig{}.tagAlign, "Config migration resets invalid enum text");
    Check(migratedConfig.theme == L"customTheme", "Config migration preserves valid theme name");
    std::filesystem::remove(migrateConfigPath, ec);

    {
        const std::filesystem::path assetModuleRoot = std::filesystem::temp_directory_path() /
            (L"quattro_unit_asset_module_" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(assetModuleRoot, ec);
        std::filesystem::remove_all(unitUserConfigRoot, ec);
        std::filesystem::create_directories(assetModuleRoot, ec);
        EmbeddedAssetInstallResult firstInstall = PrepareEmbeddedAssets(assetModuleRoot);
        const std::filesystem::path defaultThemePath = firstInstall.appDirectory / L"theme" / L"default.xml";
        const std::filesystem::path embeddedCssPath = firstInstall.appDirectory / L"icons" / L"menu" / L"tabler" / L"tabler-icons.css";
        const std::filesystem::path embeddedFontPath = firstInstall.appDirectory / L"icons" / L"menu" / L"tabler" / L"tabler-icons.ttf";
        const std::filesystem::path customThemePath = firstInstall.appDirectory / L"theme" / L"custom.xml";
        const std::wstring embeddedDefaultTheme = LoadUtf8File(defaultThemePath);
        const bool expectCompressedAssets = QuattroUsesCompressedEmbeddedAssets();
        Check(firstInstall.failures == 0, "Embedded assets first install succeeds");
        Check(firstInstall.rawAssets + firstInstall.compressedAssets == firstInstall.filesWritten && firstInstall.filesWritten >= 3,
            "Embedded asset catalog covers every installed file");
        Check(
            expectCompressedAssets
                ? firstInstall.compressedAssets == firstInstall.filesWritten && firstInstall.filesDecompressed == firstInstall.filesWritten
                : firstInstall.rawAssets == firstInstall.filesWritten && firstInstall.compressedAssets == 0 && firstInstall.filesDecompressed == 0,
            "Embedded asset storage follows the build configuration");
        Check(FileExists(defaultThemePath) && embeddedDefaultTheme.find(L"version=\"2\"") != std::wstring::npos,
            "Embedded assets install default theme");
        Check(FileExists(embeddedCssPath) && std::filesystem::file_size(embeddedCssPath, ec) > 0,
            "Embedded assets install icon stylesheet");
        Check(FileExists(embeddedFontPath) && std::filesystem::file_size(embeddedFontPath, ec) > 0,
            "Embedded assets install icon font");

        EmbeddedAssetInstallResult unchangedInstall = PrepareEmbeddedAssets(assetModuleRoot);
        Check(unchangedInstall.failures == 0 && unchangedInstall.filesDecompressed == 0,
            "Embedded assets skip decompression when files match manifest hashes");
        Check(unchangedInstall.filesSkipped == firstInstall.filesWritten,
            "Embedded assets skip every unchanged file");
        {
            std::ofstream theme(defaultThemePath, std::ios::binary | std::ios::trunc);
            theme << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  << "<Theme id=\"default\" displayName=\"default\" version=\"2\"><Palette>"
                  << "<Color name=\"unit-mutated\" value=\"rgb(1,2,3)\"/>"
                  << "</Palette></Theme>\n";
        }
        {
            std::ofstream custom(customThemePath, std::ios::binary | std::ios::trunc);
            custom << "<Theme id=\"custom\" displayName=\"custom\" version=\"1\"/>\n";
        }
        EmbeddedAssetInstallResult secondInstall = PrepareEmbeddedAssets(assetModuleRoot);
        Check(secondInstall.failures == 0 && secondInstall.filesUpdated == 1,
            "Embedded assets overwrite only the modified default theme");
        Check(secondInstall.filesDecompressed == (expectCompressedAssets ? 1 : 0),
            "Embedded assets only decompress modified files in compressed builds");
        Check(secondInstall.filesBackedUp == 1 && FileExists(secondInstall.backupDirectory / L"theme" / L"default.xml"),
            "Embedded assets back up modified managed files");
        const std::wstring restoredDefaultTheme = LoadUtf8File(defaultThemePath);
        Check(restoredDefaultTheme == embeddedDefaultTheme, "Embedded assets restore default theme by exact content");
        Check(FileExists(customThemePath), "Embedded assets keep custom theme files");
        std::cout << "embedded_asset_acceptance=passed"
                  << " first_total_ms=" << firstInstall.totalMilliseconds
                  << " unchanged_total_ms=" << unchangedInstall.totalMilliseconds
                  << " changed_total_ms=" << secondInstall.totalMilliseconds
                  << " storage=" << (expectCompressedAssets ? "xpress" : "raw")
                  << " unchanged_decompressed=" << unchangedInstall.filesDecompressed
                  << " changed_decompressed=" << secondInstall.filesDecompressed
                  << "\n";
        std::filesystem::remove_all(assetModuleRoot, ec);
    }

    const std::filesystem::path shellMenuCacheRoot = std::filesystem::temp_directory_path() /
        (L"quattro_unit_shell_menu_cache_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(shellMenuCacheRoot, ec);
    Link cachedLink;
    cachedLink.id = 701;
    cachedLink.path = L"C:\\Work\\Project";
    ShellContextMenuSnapshot shellSnapshot;
    shellSnapshot.complete = true;
    ShellContextMenuItem gitItem;
    gitItem.providerId = ShellContextMenuProviderId::Git;
    gitItem.text = L"Git Bash Here";
    gitItem.verb = L"git_shell";
    ShellContextMenuItem svnItem;
    svnItem.providerId = ShellContextMenuProviderId::Svn;
    svnItem.text = L"SVN Checkout...";
    svnItem.verb = L"svn_checkout";
    ShellContextMenuItem codeItem;
    codeItem.providerId = ShellContextMenuProviderId::VsCode;
    codeItem.text = L"Open with Code";
    codeItem.verb = L"openwithcode";
    codeItem.iconWidth = 2;
    codeItem.iconHeight = 2;
    codeItem.iconPixels = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    ShellContextMenuItem archiveItem;
    archiveItem.providerId = ShellContextMenuProviderId::Archive;
    archiveItem.text = L"添加到 \"Project.rar\"";
    archiveItem.iconWidth = 1;
    archiveItem.iconHeight = 1;
    archiveItem.iconPixels = {0xFF112233u};
    ShellContextMenuItem everythingItem;
    everythingItem.providerId = ShellContextMenuProviderId::Everything;
    everythingItem.text = L"Search Everything...";
    everythingItem.verb = L"everything_search";
    ShellContextMenuItem notepadPlusPlusItem;
    notepadPlusPlusItem.providerId = ShellContextMenuProviderId::NotepadPlusPlus;
    notepadPlusPlusItem.text = L"Edit with Notepad++";
    notepadPlusPlusItem.verb = L"notepad++";
    shellSnapshot.items = {gitItem, svnItem, codeItem, archiveItem, everythingItem, notepadPlusPlusItem};
    ShellContextMenuTrackingOptions allTracking;
    for (const auto& provider : TrackedContextMenuProviders()) {
        allTracking.*(provider.trackingMember) = true;
    }
    {
        ShellContextMenuCacheService sharedShellMenuCache;
        sharedShellMenuCache.Reset();
        sharedShellMenuCache.Update(cachedLink, shellSnapshot, allTracking);
        Check(
            FileExists(unitUserConfigRoot / L"cache" / L"shell-context-menu.bin"),
            "Default shell menu cache stored in user config directory");
        sharedShellMenuCache.Reset();
    }
    Link secondCachedLink = cachedLink;
    secondCachedLink.id = 702;
    secondCachedLink.path = L"C:\\Work\\SecondProject";
    ShellContextMenuSnapshot secondSnapshot;
    secondSnapshot.complete = true;
    ShellContextMenuItem secondCodeItem = codeItem;
    secondCodeItem.iconWidth = 0;
    secondCodeItem.iconHeight = 0;
    secondCodeItem.iconPixels.clear();
    ShellContextMenuItem secondArchiveItem = archiveItem;
    secondArchiveItem.text = L"添加到 \"SecondProject.rar\"";
    secondArchiveItem.iconWidth = 0;
    secondArchiveItem.iconHeight = 0;
    secondArchiveItem.iconPixels.clear();
    ShellContextMenuItem terminalItem;
    terminalItem.providerId = ShellContextMenuProviderId::Terminal;
    terminalItem.text = L"此处打开 CMD 窗口";
    terminalItem.verb = L"cmd";
    terminalItem.actionKind = ShellContextMenuActionKind::Terminal;
    terminalItem.actionId = L"cmd";
    terminalItem.executable = L"C:\\Windows\\System32\\cmd.exe";
    terminalItem.arguments = L"/K";
    terminalItem.workingDirectory = secondCachedLink.path;
    secondSnapshot.items = {secondCodeItem, secondArchiveItem, terminalItem};
    {
        ShellContextMenuCacheService shellMenuCache(shellMenuCacheRoot);
        shellMenuCache.Update(cachedLink, shellSnapshot, allTracking);
        shellMenuCache.Update(secondCachedLink, secondSnapshot, allTracking);
        Check(shellMenuCache.ItemsFor(cachedLink, allTracking).size() == 6, "Shell menu cache update");
        const auto sharedCodeItems = shellMenuCache.ItemsFor(secondCachedLink, allTracking);
        const auto sharedArchive = std::find_if(sharedCodeItems.begin(), sharedCodeItems.end(), [](const auto& item) {
            return item.providerId == ShellContextMenuProviderId::Archive;
        });
        Check(
            sharedCodeItems.size() == 3 && sharedCodeItems.front().iconPixels == codeItem.iconPixels,
            "Shell menu cache shares icons across links");
        Check(
            sharedArchive != sharedCodeItems.end() && sharedArchive->iconPixels == archiveItem.iconPixels,
            "Shell menu cache normalizes dynamic archive labels");
        ShellContextMenuTrackingOptions gitOnly;
        gitOnly.git = true;
        Check(shellMenuCache.ItemsFor(cachedLink, gitOnly).size() == 1, "Shell menu cache provider filter");
    }
    {
        ShellContextMenuCacheService shellMenuCache(shellMenuCacheRoot);
        const auto persistedItems = shellMenuCache.ItemsFor(cachedLink, allTracking);
        Check(persistedItems.size() == 6, "Shell menu cache persistence");
        const auto persistedCode = std::find_if(persistedItems.begin(), persistedItems.end(), [](const auto& item) {
            return item.providerId == ShellContextMenuProviderId::VsCode;
        });
        Check(
            persistedCode != persistedItems.end() && persistedCode->iconWidth == 2 &&
            persistedCode->iconHeight == 2 && persistedCode->iconPixels == codeItem.iconPixels,
            "Shell menu cache native icon persistence");
        const auto bestCodeIcon = shellMenuCache.BestIconForProvider(ShellContextMenuProviderId::VsCode);
        Check(
            bestCodeIcon && bestCodeIcon->width == 2 && bestCodeIcon->height == 2 &&
            bestCodeIcon->pixels == codeItem.iconPixels,
            "Shell menu cache exposes the best provider icon");
        Check(
            !shellMenuCache.BestIconForProvider(L"missing-provider"),
            "Shell menu cache reports a missing provider icon");
        const auto persistedSharedCode = shellMenuCache.ItemsFor(secondCachedLink, allTracking);
        Check(
            persistedSharedCode.size() == 3 && persistedSharedCode.front().iconPixels == codeItem.iconPixels,
            "Shell menu shared icon pool persistence");
        const auto persistedTerminal = std::find_if(persistedSharedCode.begin(), persistedSharedCode.end(), [](const auto& item) {
            return item.actionKind == ShellContextMenuActionKind::Terminal;
        });
        Check(
            persistedTerminal != persistedSharedCode.end() && persistedTerminal->actionId == L"cmd" &&
            persistedTerminal->executable == terminalItem.executable &&
            persistedTerminal->workingDirectory == terminalItem.workingDirectory,
            "Shell menu terminal action persistence");
        Link changedTarget = cachedLink;
        changedTarget.path = L"C:\\Work\\Other";
        Check(shellMenuCache.ItemsFor(changedTarget, allTracking).empty(), "Shell menu cache target invalidation");
        shellMenuCache.RemoveProvider(ShellContextMenuProviderId::Git);
        const auto withoutGit = shellMenuCache.ItemsFor(cachedLink, allTracking);
        Check(withoutGit.size() == 5 && withoutGit.front().providerId == ShellContextMenuProviderId::Svn, "Shell menu cache provider removal");
        shellMenuCache.RemoveProvider(ShellContextMenuProviderId::Everything);
        const auto withoutEverything = shellMenuCache.ItemsFor(cachedLink, allTracking);
        Check(
            std::none_of(withoutEverything.begin(), withoutEverything.end(), [](const auto& item) {
                return item.providerId == ShellContextMenuProviderId::Everything;
            }),
            "Shell menu cache Everything provider removal");
        Check(shellMenuCache.Reset(), "Shell menu cache reset");
        Check(
            shellMenuCache.ItemsFor(cachedLink, allTracking).empty() &&
            shellMenuCache.ItemsFor(secondCachedLink, allTracking).empty(),
            "Shell menu cache reset clears menu structure and icons");
        {
            ShellContextMenuCacheService resetCache(shellMenuCacheRoot);
            Check(
                resetCache.ItemsFor(cachedLink, allTracking).empty() &&
                resetCache.ItemsFor(secondCachedLink, allTracking).empty(),
                "Shell menu cache reset persistence");
        }
        shellMenuCache.Remove(cachedLink.id);
        Check(shellMenuCache.ItemsFor(cachedLink, allTracking).empty(), "Shell menu cache link removal");
    }
    std::filesystem::remove_all(shellMenuCacheRoot, ec);

    const std::filesystem::path shellMenuBatchRoot = std::filesystem::temp_directory_path() /
        (L"quattro_unit_shell_menu_batch_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(shellMenuBatchRoot, ec);
    {
        ShellContextMenuCacheService batchCache(shellMenuBatchRoot);
        batchCache.UpdateBatch({
            ShellContextMenuCacheUpdate{cachedLink, shellSnapshot, allTracking},
            ShellContextMenuCacheUpdate{secondCachedLink, secondSnapshot, allTracking},
        });
        Check(
            batchCache.ItemsFor(cachedLink, allTracking).size() == 6 &&
            batchCache.ItemsFor(secondCachedLink, allTracking).size() == 3,
            "Shell menu cache batch update applies every snapshot");
    }
    {
        ShellContextMenuCacheService batchCache(shellMenuBatchRoot);
        Check(
            batchCache.ItemsFor(cachedLink, allTracking).size() == 6 &&
            batchCache.ItemsFor(secondCachedLink, allTracking).size() == 3,
            "Shell menu cache batch update persists once for all snapshots");
    }
    std::filesystem::remove_all(shellMenuBatchRoot, ec);

    const std::filesystem::path terminalTargetRoot = std::filesystem::temp_directory_path() /
        (L"quattro_unit_terminal_menu_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(terminalTargetRoot, ec);
    std::filesystem::create_directories(terminalTargetRoot, ec);
    const std::filesystem::path terminalTargetFile = terminalTargetRoot / L"sample.txt";
    {
        std::ofstream file(terminalTargetFile, std::ios::binary | std::ios::trunc);
        file << "terminal";
    }
    Link terminalDirectoryLink;
    terminalDirectoryLink.id = 710;
    terminalDirectoryLink.path = terminalTargetRoot.wstring();
    Link terminalFileLink;
    terminalFileLink.id = 711;
    terminalFileLink.path = terminalTargetFile.wstring();
    const auto directoryWorkingPath = TerminalContextMenuService::WorkingDirectoryFor(terminalDirectoryLink);
    const auto fileWorkingPath = TerminalContextMenuService::WorkingDirectoryFor(terminalFileLink);
    Check(
        directoryWorkingPath && fileWorkingPath &&
        std::filesystem::equivalent(*directoryWorkingPath, terminalTargetRoot, ec) &&
        std::filesystem::equivalent(*fileWorkingPath, terminalTargetRoot, ec),
        "Terminal menu resolves file and directory working paths");
    Link terminalUrlLink;
    terminalUrlLink.type = 2;
    terminalUrlLink.path = L"https://example.com";
    Check(!TerminalContextMenuService::WorkingDirectoryFor(terminalUrlLink), "Terminal menu ignores URL targets");
    const auto terminalPrograms = TerminalContextMenuService::DetectAvailablePrograms();
    Check(
        !terminalPrograms.programs.empty() && terminalPrograms.programs.front().id == L"cmd",
        "Terminal menu detects CMD first");
    Check(
        std::none_of(terminalPrograms.programs.begin(), terminalPrograms.programs.end(), [](const auto& program) {
            return program.id == L"windowsterminal" || program.id == L"wt";
        }),
        "Terminal menu excludes Windows Terminal");
    const auto terminalDirectoryItems = TerminalContextMenuService::ItemsFor(terminalDirectoryLink, terminalPrograms);
    Check(
        !terminalDirectoryItems.empty() &&
        terminalDirectoryItems.front().actionKind == ShellContextMenuActionKind::Terminal &&
        terminalDirectoryItems.front().actionId == L"cmd" &&
        terminalDirectoryItems.front().workingDirectory == directoryWorkingPath->wstring(),
        "Terminal menu builds cached commands without execution-time probing");
    ShellContextMenuLocator invalidTerminalCommand;
    invalidTerminalCommand.actionKind = ShellContextMenuActionKind::Terminal;
    invalidTerminalCommand.actionId = L"cmd";
    invalidTerminalCommand.executable = L"C:\\Windows\\System32\\notepad.exe";
    invalidTerminalCommand.workingDirectory = terminalTargetRoot.wstring();
    std::wstring invalidTerminalError;
    Check(
        !TerminalContextMenuService::Invoke(nullptr, invalidTerminalCommand, invalidTerminalError) &&
        !invalidTerminalError.empty(),
        "Terminal menu rejects a cached executable that does not match its adapter");

    int refreshQueryCount = 0;
    bool refreshTrackingWasCombined = true;
    bool refreshLinksHadPidls = true;
    ShellContextMenuRefreshService refreshService(
        [&](HWND, const Link& link, const ShellContextMenuTrackingOptions& tracking, ShellContextMenuSnapshot& snapshot) {
            ++refreshQueryCount;
            refreshTrackingWasCombined = refreshTrackingWasCombined && tracking.git && !tracking.terminal;
            refreshLinksHadPidls = refreshLinksHadPidls && !link.pidl.empty();
            snapshot.complete = true;
            ShellContextMenuItem item;
            item.providerId = ShellContextMenuProviderId::Git;
            item.text = L"Git test action";
            snapshot.items = {item};
            return true;
        });
    ShellContextMenuRefreshRequest refreshRequest;
    refreshRequest.tracking.git = true;
    Link refreshDirectoryLink = terminalDirectoryLink;
    refreshDirectoryLink.id = 712;
    Link refreshFileLink = terminalFileLink;
    refreshFileLink.id = 713;
    Link refreshUrlLink = terminalUrlLink;
    refreshUrlLink.id = 714;
    Link refreshMissingLink;
    refreshMissingLink.id = 715;
    refreshMissingLink.name = L"missing";
    refreshMissingLink.path = (terminalTargetRoot / L"missing-target").wstring();
    refreshRequest.links = {
        refreshDirectoryLink,
        refreshFileLink,
        refreshUrlLink,
        refreshMissingLink,
    };
    const ShellContextMenuRefreshResult refreshResult = refreshService.Refresh(refreshRequest);
    Check(
        refreshResult.totalLinks == 4 && refreshResult.succeededLinks == 2 &&
        refreshResult.skippedLinks == 1 && refreshResult.failures.size() == 1,
        "Shell menu refresh reports real link outcomes");
    Check(
        refreshQueryCount == 2 && refreshTrackingWasCombined && refreshLinksHadPidls,
        "Shell menu refresh queries each eligible real link once with combined tracking");
    Check(
        refreshResult.updates.size() == 2 && refreshResult.menuItemCount == 2 &&
        refreshResult.updates.front().nativeSnapshot.complete,
        "Shell menu refresh returns cache-ready snapshots");

    int providerResolveCount = 0;
    ContextMenuProviderIconService providerIconService(
        [&](const TrackedContextMenuProviderBinding& provider,
            const std::optional<ShellContextMenuCachedIcon>&,
            std::stop_token) {
            ++providerResolveCount;
            ContextMenuProviderIconInfo info;
            info.providerId = provider.providerId;
            info.installed = true;
            info.installedViaProbe = true;
            info.attempted = true;
            return info;
        });
    const auto providerIcons = providerIconService.Load();
    Check(
        providerIcons.size() == TrackedContextMenuProviderCount &&
        providerResolveCount == static_cast<int>(TrackedContextMenuProviderCount),
        "Provider icon load resolves each provider exactly once");
    Check(
        !providerIcons.empty() && providerIcons.front().providerId == ShellContextMenuProviderId::VsCode &&
        providerIcons.back().providerId == ShellContextMenuProviderId::Vim,
        "Provider icon load preserves the shared provider order");
    std::filesystem::remove_all(terminalTargetRoot, ec);

    // Executable menu icons must resolve even when the target exposes no icon at
    // resource index 0 (launcher stubs such as Cmder.exe). cmd.exe always has an
    // embedded icon, and the shell-association fallback covers the stub case.
    ShellContextMenuItem cmdIconItem;
    Check(
        ShellItemService::LoadExecutableMenuIcon(L"C:\\Windows\\System32\\cmd.exe", cmdIconItem) &&
        !cmdIconItem.iconPixels.empty() && cmdIconItem.iconWidth > 0 && cmdIconItem.iconHeight > 0,
        "Executable menu icon loads for a normal executable");
    {
        // A single miss is invisible to a user but a real regression: the GDI
        // handle produced by CreateDIBSection can legitimately have its high bit
        // set, so any signed-handle validity check drops icons at random. Loop to
        // catch that class of flake deterministically.
        bool iconStable = true;
        for (int repeat = 0; repeat < 200 && iconStable; ++repeat) {
            ShellContextMenuItem probe;
            iconStable = ShellItemService::LoadExecutableMenuIcon(
                             L"C:\\Windows\\System32\\cmd.exe", probe) &&
                         !probe.iconPixels.empty();
        }
        Check(iconStable, "Executable menu icon loads consistently across repeats");
    }
    ShellContextMenuItem missingIconItem;
    Check(
        !ShellItemService::LoadExecutableMenuIcon(L"C:\\does-not-exist\\nope.exe", missingIconItem) &&
        missingIconItem.iconPixels.empty(),
        "Executable menu icon reports failure for a missing target");

    // Save() must be atomic: a successful update leaves no stray temp file, and a
    // leftover temp file from a previous crash must not corrupt the real cache.
    const std::filesystem::path atomicCacheRoot = std::filesystem::temp_directory_path() /
        (L"quattro_unit_shell_menu_atomic_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(atomicCacheRoot, ec);
    const std::filesystem::path atomicCacheFile = atomicCacheRoot / L"cache" / L"shell-context-menu.bin";
    const std::filesystem::path atomicCacheTemp = std::filesystem::path(atomicCacheFile).concat(L".tmp");
    Link atomicLink;
    atomicLink.id = 720;
    atomicLink.path = L"C:\\Work\\Atomic";
    ShellContextMenuSnapshot atomicSnapshot;
    atomicSnapshot.complete = true;
    ShellContextMenuItem atomicItem;
    atomicItem.providerId = ShellContextMenuProviderId::Git;
    atomicItem.text = L"Git Bash Here";
    atomicItem.verb = L"git_shell";
    atomicSnapshot.items = {atomicItem};
    ShellContextMenuTrackingOptions atomicTracking;
    atomicTracking.git = true;
    {
        ShellContextMenuCacheService atomicCache(atomicCacheRoot);
        atomicCache.Update(atomicLink, atomicSnapshot, atomicTracking);
        Check(
            std::filesystem::exists(atomicCacheFile) && !std::filesystem::exists(atomicCacheTemp),
            "Shell menu cache save is atomic and leaves no temp file");
    }
    {
        // Simulate a stale temp file from an interrupted write; the next save must
        // overwrite it and the reload must still see the persisted entry.
        std::ofstream staleTemp(atomicCacheTemp, std::ios::binary | std::ios::trunc);
        staleTemp << "garbage";
        staleTemp.close();
        ShellContextMenuCacheService atomicCache(atomicCacheRoot);
        Check(
            atomicCache.ItemsFor(atomicLink, atomicTracking).size() == 1,
            "Shell menu cache ignores stale temp file on load");
        Link atomicSecond = atomicLink;
        atomicSecond.id = 721;
        atomicSecond.path = L"C:\\Work\\AtomicSecond";
        atomicCache.Update(atomicSecond, atomicSnapshot, atomicTracking);
        Check(!std::filesystem::exists(atomicCacheTemp), "Shell menu cache save replaces stale temp file");
    }
    {
        ShellContextMenuCacheService atomicCache(atomicCacheRoot);
        Check(
            atomicCache.ItemsFor(atomicLink, atomicTracking).size() == 1,
            "Shell menu cache survives atomic save cycle");
    }
    std::filesystem::remove_all(atomicCacheRoot, ec);

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
    Check(Near(fallbackTheme.metric(L"toggle", L"height", 0.0f), 24.0f), "Theme default toggle metric");
    Check(Near(fallbackTheme.metric(L"slider", L"thumbSize", 0.0f), 14.0f), "Theme default slider metric");
    Check(Near(fallbackTheme.metric(L"progressBar", L"height", 0.0f), 16.0f), "Theme default progress bar metric");
    Check(fallbackTheme.color(L"progressBar", L"normal", L"fill").a > 0.9f, "Theme default progress bar color");
    Check(fallbackTheme.color(L"global", L"warning", L"text").r > 0.5f, "Theme default semantic warning");
    Check(fallbackTheme.color(L"text", L"success", L"text").a > 0.9f, "Theme default text success");
    Check(fallbackTheme.color(L"text", L"danger", L"text").a > 0.9f, "Theme default text danger");
    Check(fallbackTheme.color(L"link", L"hover", L"text").a > 0.9f, "Theme default link hover");
    Check(fallbackTheme.color(L"table", L"normal", L"border").a > 0.9f, "Theme default table border");
    Check(fallbackTheme.color(L"table", L"normal", L"grid").a > 0.9f, "Theme default table grid");
    Check(Near(fallbackTheme.metric(L"table", L"gridWidth", 0.0f), 1.0f), "Theme default table grid width");
    Check(Near(fallbackTheme.metric(L"tableHeader", L"height", 0.0f), 28.0f), "Theme default table header height");
    Check(Near(fallbackTheme.metric(L"tooltip", L"maxWidth", 0.0f), 420.0f), "Theme default tooltip max width");
    Check(fallbackTheme.color(L"groupBox", L"normal", L"border").a > 0.9f, "Theme default group box border");
    Check(fallbackTheme.color(L"tabControl", L"normal", L"bg").a > 0.9f, "Theme default tab control background");
    Check(Near(fallbackTheme.metric(L"toolbar", L"itemGap", 0.0f), 4.0f), "Theme default toolbar gap");
    Check(Near(fallbackTheme.metric(L"global", L"fieldHeight", 0.0f), 28.0f), "Theme default global metric");
    Check(Near(fallbackTheme.metric(L"global", L"captionLineHeight", 0.0f), 16.0f), "Theme caption line height scale");
    Check(Near(fallbackTheme.metric(L"global", L"bodyLineHeight", 0.0f), 20.0f), "Theme body line height scale");
    Check(Near(fallbackTheme.metric(L"global", L"titleLineHeight", 0.0f), 24.0f), "Theme title line height scale");
    Check(Near(fallbackTheme.metric(L"global", L"smallControlHeight", 0.0f), 24.0f), "Theme small control height scale");
    Check(Near(fallbackTheme.metric(L"global", L"mediumControlHeight", 0.0f), 28.0f), "Theme medium control height scale");
    Check(Near(fallbackTheme.metric(L"global", L"largeControlHeight", 0.0f), 32.0f), "Theme large control height scale");
    Check(Near(fallbackTheme.metric(L"dialog", L"contentInsetX", 0.0f), 28.0f), "Theme default dialog standard inset");
    Check(Near(fallbackTheme.metric(L"dialog", L"labelMinWidth", 0.0f), 20.0f), "Theme default dialog label min width");
    Check(Near(fallbackTheme.metric(L"dialog", L"compactRowGap", 0.0f), 6.0f), "Theme default dialog compact row gap");
    Check(Near(fallbackTheme.metric(L"dialog", L"miniFooterInsetY", 0.0f), 16.0f), "Theme default dialog mini footer inset");
    Check(Near(fallbackTheme.metric(L"dialog", L"miniFooterButtonWidth", 0.0f), 72.0f), "Theme default dialog mini footer button");
    Check(Near(fallbackTheme.metric(L"dialog", L"overlaySectionGap", 0.0f), 12.0f), "Theme default dialog overlay section gap");
    Check(Near(fallbackTheme.metric(L"button", L"height", 0.0f), 28.0f), "Theme medium button height");
    Check(Near(fallbackTheme.metric(L"button", L"largeHeight", 0.0f), 32.0f), "Theme footer button height");
    Check(Near(fallbackTheme.metric(L"edit", L"height", 0.0f), 28.0f), "Theme edit height");
    Check(Near(fallbackTheme.metric(L"edit", L"textHeight", 0.0f), 20.0f), "Theme edit body line height");
    Check(Near(fallbackTheme.metric(L"comboBox", L"itemHeight", 0.0f), 28.0f), "Theme combo item height");
    Check(Near(fallbackTheme.metric(L"tabButton", L"height", 0.0f), 28.0f), "Theme tab height");
    Check(fallbackTheme.color(L"tabButton", L"emphasizedSelected", L"text").r > 0.9f,
        "Theme emphasized tab selected text");
    Check(fallbackTheme.color(L"tabButton", L"minimalSelected", L"underline").b > 0.8f,
        "Theme minimal tab underline");
    Check(fallbackTheme.color(L"tabButton", L"softPillSelected", L"bg").a > 0.9f,
        "Theme soft pill tab selected background");
    Check(fallbackTheme.color(L"tabButton", L"connectedSelected", L"border").a > 0.9f,
        "Theme connected tab selected border");
    Check(Near(fallbackTheme.metric(L"tabButton", L"softPillRadius", 0.0f), 14.0f),
        "Theme soft pill tab radius");
    Check(Near(fallbackTheme.metric(L"listItem", L"twoLineHeight", 0.0f), 48.0f), "Theme two-line result row height");
    Check(Near(fallbackTheme.metric(L"miniButton", L"height", 0.0f), 24.0f), "Theme default mini button metric");
    Check(fallbackTheme.color(L"miniButton", L"hover", L"icon").a > 0.9f, "Theme default mini button hover");
    Check(Near(fallbackTheme.metric(L"tabButton", L"minTextWidth", 0.0f), 18.0f), "Theme default tab button min text width");
    ThemedEditOptions editOptions{};
    editOptions.mode = ThemedEditMode::MultiLine;
    editOptions.content = ThemedEditContent::Password;
    editOptions.readOnly = true;
    editOptions.enabled = false;
    editOptions.error = true;
    editOptions.selectAllOnFocus = true;
    editOptions.maxLength = 128;
    editOptions.placeholder = L"unit placeholder";
    Check(editOptions.mode == ThemedEditMode::MultiLine, "Themed edit mode composes with other options");
    Check(editOptions.acceptsReturn, "Multiline themed edits accept return by default");
    Check(editOptions.content == ThemedEditContent::Password, "Themed edit content composes with mode");
    Check(editOptions.readOnly && !editOptions.enabled && editOptions.error, "Themed edit state options compose");
    Check(editOptions.selectAllOnFocus && editOptions.maxLength == 128 && !editOptions.placeholder.empty(), "Themed edit behavior options compose");
    ThemedLabelOptions labelOptions{};
    labelOptions.align = ThemedTextAlign::End;
    Check(labelOptions.align == ThemedTextAlign::End, "Themed label alignment is semantic");
    labelOptions.lines = ThemedLabelLines::Two;
    Check(labelOptions.lines == ThemedLabelLines::Two, "Themed label line count is semantic");
    ThemedFramedTextOptions framedTextOptions{};
    framedTextOptions.align = ThemedTextAlign::Center;
    framedTextOptions.wrap = true;
    framedTextOptions.multiline = true;
    Check(framedTextOptions.align == ThemedTextAlign::Center && framedTextOptions.wrap && framedTextOptions.multiline, "Themed framed text options compose");
    ThemedStatusTextOptions statusOptions{};
    statusOptions.role = ThemedStatusRole::Warning;
    statusOptions.align = ThemedTextAlign::Start;
    Check(statusOptions.role == ThemedStatusRole::Warning && statusOptions.align == ThemedTextAlign::Start, "Themed status options compose");
    ThemedComboBoxOptions comboOptions{};
    comboOptions.enabled = false;
    Check(!comboOptions.enabled, "Themed combo state is semantic");
    ThemedListBoxOptions listOptions{};
    listOptions.selection = ThemedListSelection::Multiple;
    listOptions.scroll = ThemedListScroll::Both;
    listOptions.notify = false;
    Check(listOptions.selection == ThemedListSelection::Multiple && listOptions.scroll == ThemedListScroll::Both && !listOptions.notify, "Themed list options compose");
    ThemedCheckBoxOptions checkBoxOptions{};
    checkBoxOptions.checked = true;
    checkBoxOptions.enabled = false;
    Check(checkBoxOptions.checked && !checkBoxOptions.enabled, "Themed checkbox options compose");
    ThemedProgressBarOptions progressOptions{};
    progressOptions.value = 0.5;
    progressOptions.indeterminate = true;
    progressOptions.enabled = false;
    Check(Near(static_cast<float>(progressOptions.value), 0.5f) && progressOptions.indeterminate && !progressOptions.enabled, "Themed progress options compose");
    ThemedToggleOptions toggleOptions{};
    toggleOptions.checked = true;
    toggleOptions.enabled = false;
    Check(toggleOptions.checked && !toggleOptions.enabled, "Themed toggle options compose");
    ThemedHotKeyCaptureOptions hotKeyCaptureOptions{};
    hotKeyCaptureOptions.enabled = false;
    Check(!hotKeyCaptureOptions.enabled, "Themed hot key capture options compose");
    ThemedRadioButtonOptions radioOptions{};
    radioOptions.group = 7;
    radioOptions.checked = true;
    Check(radioOptions.group == 7 && radioOptions.checked, "Themed radio options compose");
    ThemedSliderOptions sliderOptions{};
    sliderOptions.minimum = -10.0;
    sliderOptions.maximum = 10.0;
    sliderOptions.step = 0.5;
    sliderOptions.value = 2.5;
    Check(Near(static_cast<float>(sliderOptions.value), 2.5f) && Near(static_cast<float>(sliderOptions.step), 0.5f), "Themed slider options compose");
    ThemedLinkOptions linkOptions{};
    linkOptions.role = ThemedLinkRole::External;
    linkOptions.visited = true;
    Check(linkOptions.role == ThemedLinkRole::External && linkOptions.visited, "Themed link options compose");
    ThemedTableOptions tableOptions{};
    tableOptions.checkable = true;
    tableOptions.view = ThemedTableView::Icons;
    tableOptions.showColumnGridLines = true;
    Check(tableOptions.checkable && tableOptions.view == ThemedTableView::Icons && tableOptions.showColumnGridLines,
        "Themed table options compose");
    ThemedTooltipOptions tooltipOptions{};
    tooltipOptions.role = ThemedTooltipRole::Warning;
    tooltipOptions.placement = ThemedTooltipPlacement::Cursor;
    Check(tooltipOptions.role == ThemedTooltipRole::Warning && tooltipOptions.placement == ThemedTooltipPlacement::Cursor, "Themed tooltip options compose");
    ThemedToastOptions toastOptions{};
    toastOptions.role = ThemedToastRole::Success;
    toastOptions.anchor = ThemedToastAnchor::ScreenBottomRight;
    toastOptions.durationMs = 1500;
    Check(toastOptions.role == ThemedToastRole::Success && toastOptions.anchor == ThemedToastAnchor::ScreenBottomRight && toastOptions.durationMs == 1500,
        "Themed toast options compose");
    const ImportToastSummary importSuccess{3, 0};
    Check(
        ImportToastRole(importSuccess) == ThemedToastRole::Success &&
            ImportToastText(importSuccess, L"工作") == L"已添加 3 项到“工作”。",
        "Import toast success text and role");
    const ImportToastSummary importPartial{2, 1};
    Check(
        ImportToastRole(importPartial) == ThemedToastRole::Warning &&
            ImportToastText(importPartial, L"工作") == L"已添加 2 项，1 项失败。",
        "Import toast partial text and role");
    Check(
        OperationToastRole(true, false) == ThemedToastRole::Success &&
            OperationToastRole(true, true) == ThemedToastRole::Warning &&
            OperationToastRole(false, false) == ThemedToastRole::Danger,
        "Operation toast outcome roles");
    Check(fallbackTheme.color(L"toggle", L"disabled", L"text").a > 0.9f, "Theme default toggle text state");
    Check(fallbackTheme.color(L"radio", L"hover", L"border").a > 0.9f, "Theme default radio hover state");
    Check(fallbackTheme.color(L"slider", L"disabled", L"thumb").a > 0.9f, "Theme default slider disabled state");
    Check(fallbackTheme.color(L"toast", L"success", L"border").a > 0.9f, "Theme default toast success state");
    Check(Near(fallbackTheme.metric(L"toast", L"maxWidth", 0.0f), 360.0f), "Theme default toast max width");
    Check(ThemedWindowUi::ScaleForDpi(544, 120) == 680, "Themed window scales logical width at 125 percent DPI");
    Check(ThemedWindowUi::ScaleForDpi(441, 144) == 662, "Themed window scales logical height at 150 percent DPI");
    Check(ThemedWindowUi::ScaleForDpi(kThemedManagementClientWidth, 120) == 950,
        "Management window scales logical width at 125 percent DPI");
    Check(ThemedWindowUi::ScaleForDpi(kThemedManagementClientHeight, 144) == 780,
        "Management window scales logical height at 150 percent DPI");
    Check(ThemedD2D::ScaleDip(28, 120) == 35, "D2D logical metric scales to 125 percent DPI");
    Check(ThemedD2D::ScaleDip(28, 144) == 42, "D2D logical metric scales to 150 percent DPI");
    Check(ThemedControls::ComboBoxDropdownHeight(fallbackTheme, 120) == 35 + 35 * 6,
        "Themed combo dropdown scales item rows at 125 percent DPI");
    Check(ThemedControls::ComboBoxDropdownHeight(fallbackTheme, 144) == 42 + 42 * 6,
        "Themed combo dropdown scales item rows at 150 percent DPI");
    Check(ThemedD2D::IsAvailable(), "Direct2D and DirectWrite factories are available");
    HWND controlParent = CreateWindowExW(
        0, L"STATIC", L"", WS_POPUP,
        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(controlParent != nullptr, "Themed control test parent created");
    if (controlParent) {
        ThemedUi controlUi(
            GetModuleHandleW(nullptr), controlParent, fallbackTheme,
            reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)),
            DialogLayoutKind::Compact, 320, 200);
        TestTooltipRegistry tooltipRegistry;
        ThemedUi tooltipUi(
            GetModuleHandleW(nullptr), controlParent, fallbackTheme,
            reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)),
            DialogLayoutKind::Compact, 320, 200,
            nullptr, nullptr, &tooltipRegistry);
        ThemedUi dpi125Ui(
            GetModuleHandleW(nullptr), controlParent, fallbackTheme,
            reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)),
            DialogLayoutKind::Compact, 680, 551, nullptr, nullptr, nullptr, nullptr, 120);
        ThemedUi dpi150Ui(
            GetModuleHandleW(nullptr), controlParent, fallbackTheme,
            reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)),
            DialogLayoutKind::Compact, 816, 662, nullptr, nullptr, nullptr, nullptr, 144);
        Check(dpi125Ui.layout().contentInsetX == ThemedWindowUi::ScaleForDpi(controlUi.layout().contentInsetX, 120),
            "Themed UI scales dialog metrics at 125 percent DPI");
        Check(dpi150Ui.checkBoxHeight() == ThemedWindowUi::ScaleForDpi(controlUi.checkBoxHeight(), 144),
            "Themed UI scales component templates at 150 percent DPI");
        Check(dpi125Ui.tableHeightForRows(8, false) - dpi125Ui.tableHeightForRows(7, false)
                == ThemedWindowUi::ScaleForDpi(28, 120),
            "Themed table visible-row height scales at 125 percent DPI");
        Check(dpi150Ui.tableHeightForRows(8, false) - dpi150Ui.tableHeightForRows(7, false)
                == ThemedWindowUi::ScaleForDpi(28, 144),
            "Themed table visible-row height scales at 150 percent DPI");
        Check(controlUi.comboBoxHeight() == controlUi.editHeight(),
            "Themed UI aligns combo and edit template heights");
        Check(controlUi.listItemHeight(true) == controlUi.scale(48),
            "Themed UI exposes the shared two-line result row height");
        Check(controlUi.footerButtonHeight() == controlUi.scale(32),
            "Themed UI exposes the large footer button template");
        Check(dpi125Ui.footerButtonHeight() == ThemedWindowUi::ScaleForDpi(controlUi.footerButtonHeight(), 120),
            "Themed UI scales footer buttons at 125 percent DPI");
        const ThemedFormLayout sectionLayout(controlUi);
        const ThemedContentInsets sectionInsets = controlUi.groupBoxInsets();
        Check(Near(fallbackTheme.metric(L"groupBox", L"titleInsetY", 0.0f), 4.0f),
            "Theme default group box title top inset");
        Check(sectionInsets.top == controlUi.scale(28),
            "Themed group box combines title top inset, title height, and content gap");
        Check(dpi125Ui.groupBoxInsets().top == ThemedWindowUi::ScaleForDpi(sectionInsets.top, 120),
            "Themed group box top inset scales at 125 percent DPI");
        const ThemedSectionGeometry section = sectionLayout.section(
            10, 20, 280,
            {sectionLayout.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
             sectionLayout.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit, ThemedSectionItemKind::CompactButton})});
        const int sectionRowGap = controlUi.scale(static_cast<int>(fallbackTheme.metric(L"groupBox", L"contentRowGap", 4.0f)));
        Check(section.rowTops.size() == 2 &&
                section.rowTops[1] - section.rowTops[0] == controlUi.editHeight() + sectionRowGap,
            "Themed section inserts one public row gap between rows");
        Check(section.frame.bottom == section.rowTops[1] + controlUi.editHeight() + sectionInsets.bottom,
            "Themed section does not append row gap after final row");
        Check(sectionLayout.sectionItemY(section, 0, controlUi.labelHeight()) ==
                section.rowTops[0] + (controlUi.editHeight() - controlUi.labelHeight()) / 2,
            "Themed section vertically centers label in field row");
        const RECT framedTextFrame{8, 116, 260, 188};
        HWND runtimeSingleLineFrame = controlUi.FramedStatic(L"one line", framedTextFrame);
        ThemedFramedTextOptions runtimeFramedTextOptions{};
        runtimeFramedTextOptions.wrap = true;
        runtimeFramedTextOptions.multiline = true;
        HWND runtimeMultilineFrame = controlUi.FramedStatic(L"名称：Item\n来源：注册表\n状态：可禁用", framedTextFrame, runtimeFramedTextOptions);
        RECT singleLineRect{};
        RECT multilineRect{};
        GetWindowRect(runtimeSingleLineFrame, &singleLineRect);
        GetWindowRect(runtimeMultilineFrame, &multilineRect);
        MapWindowPoints(HWND_DESKTOP, controlParent, reinterpret_cast<POINT*>(&singleLineRect), 2);
        MapWindowPoints(HWND_DESKTOP, controlParent, reinterpret_cast<POINT*>(&multilineRect), 2);
        Check(singleLineRect.bottom - singleLineRect.top == controlUi.scale(20),
            "Themed framed static keeps existing single-line text height");
        Check(multilineRect.top < singleLineRect.top && multilineRect.top <= framedTextFrame.top + controlUi.scale(6),
            "Themed framed static multiline text is top aligned");
        Check(multilineRect.bottom - multilineRect.top > controlUi.scale(40),
            "Themed framed static multiline text keeps enough height for detail rows");
        ThemedToggleOptions runtimeToggleOptions{};
        runtimeToggleOptions.checked = true;
        HWND runtimeToggle = controlUi.Toggle(7101, L"toggle", 8, 8, 120, runtimeToggleOptions);
        Check(ThemedUi::IsChecked(runtimeToggle), "Themed toggle initial checked state");
        ThemedUi::SetChecked(runtimeToggle, false);
        Check(!ThemedUi::IsChecked(runtimeToggle), "Themed toggle public state update");

        ThemedRadioButtonOptions firstRadioOptions{};
        firstRadioOptions.group = 3;
        firstRadioOptions.checked = true;
        HWND firstRadio = controlUi.RadioButton(7102, L"first", 8, 40, 120, firstRadioOptions);
        ThemedRadioButtonOptions secondRadioOptions{};
        secondRadioOptions.group = 3;
        HWND secondRadio = controlUi.RadioButton(7103, L"second", 136, 40, 120, secondRadioOptions);
        ThemedUi::SetChecked(secondRadio, true);
        Check(!ThemedUi::IsChecked(firstRadio) && ThemedUi::IsChecked(secondRadio), "Themed radio group is exclusive");

        ThemedSliderOptions runtimeSliderOptions{};
        runtimeSliderOptions.minimum = 0.0;
        runtimeSliderOptions.maximum = 10.0;
        runtimeSliderOptions.step = 2.0;
        HWND runtimeSlider = controlUi.Slider(7104, 8, 72, 220, runtimeSliderOptions);
        ThemedUi::SetSliderValue(runtimeSlider, 3.7);
        Check(Near(static_cast<float>(ThemedUi::SliderValue(runtimeSlider)), 4.0f), "Themed slider applies public step");
        HWND runtimeTooltipButton = tooltipUi.Button(
            7106, L"tooltip", 8, 104,
            ThemedButtonRole::Normal,
            ThemedButtonSize::Compact,
            ThemedButtonWidthMode::Text);
        {
            HDC screen = GetDC(nullptr);
            HDC bitmapDc = screen ? CreateCompatibleDC(screen) : nullptr;
            HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, 180, 40) : nullptr;
            HGDIOBJ oldBitmap = bitmapDc && bitmap ? SelectObject(bitmapDc, bitmap) : nullptr;
            if (bitmapDc && bitmap) {
                ThemedGdiFallback::FillSolidRect(bitmapDc, RECT{0, 0, 180, 40}, RGB(255, 255, 255));
                DRAWITEMSTRUCT draw{};
                draw.CtlType = ODT_BUTTON;
                draw.hwndItem = runtimeTooltipButton;
                draw.hDC = bitmapDc;
                draw.rcItem = RECT{0, 0, 180, 40};
                SetEnvironmentVariableW(L"QUATTRO_FORCE_GDI_FALLBACK", L"1");
                Check(!ThemedD2D::IsAvailable(), "D2D fallback switch disables the visual backend");
                Check(ThemedControls::Draw(fallbackTheme, &draw),
                    "Public button draws through isolated GDI fallback");
                SetEnvironmentVariableW(L"QUATTRO_FORCE_GDI_FALLBACK", nullptr);
                Check(ThemedD2D::IsAvailable(), "D2D backend recovers after fallback switch is cleared");
                Check(GetPixel(bitmapDc, 2, 2) != RGB(255, 255, 255),
                    "Fallback button paint changes the target surface");
            }
            if (bitmapDc && oldBitmap) SelectObject(bitmapDc, oldBitmap);
            if (bitmap) DeleteObject(bitmap);
            if (bitmapDc) DeleteDC(bitmapDc);
            if (screen) ReleaseDC(nullptr, screen);
        }
        auto catalogStatusBarPaints = [&](const wchar_t* fallbackFlag) {
            HDC screen = GetDC(nullptr);
            HDC bitmapDc = screen ? CreateCompatibleDC(screen) : nullptr;
            HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, 320, 120) : nullptr;
            HGDIOBJ oldBitmap = bitmapDc && bitmap ? SelectObject(bitmapDc, bitmap) : nullptr;
            bool changed = false;
            if (bitmapDc && bitmap) {
                ThemedGdiFallback::FillSolidRect(bitmapDc, RECT{0, 0, 320, 120}, RGB(255, 255, 255));
                SetEnvironmentVariableW(L"QUATTRO_FORCE_GDI_FALLBACK", fallbackFlag);
                CatalogDialogLayout().DrawStatusBar(fallbackTheme, bitmapDc, RECT{0, 0, 320, 120});
                changed = GetPixel(bitmapDc, 10, 96) != RGB(255, 255, 255);
                SetEnvironmentVariableW(L"QUATTRO_FORCE_GDI_FALLBACK", nullptr);
            }
            if (bitmapDc && oldBitmap) SelectObject(bitmapDc, oldBitmap);
            if (bitmap) DeleteObject(bitmap);
            if (bitmapDc) DeleteDC(bitmapDc);
            if (screen) ReleaseDC(nullptr, screen);
            return changed;
        };
        Check(catalogStatusBarPaints(nullptr), "Catalog dialog status bar paints through D2D facade");
        Check(catalogStatusBarPaints(L"1"), "Catalog dialog status bar paints through GDI fallback");
        ThemedTooltipOptions runtimeTooltipOptions{};
        runtimeTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
        tooltipUi.SetTooltip(runtimeTooltipButton, L"button description", runtimeTooltipOptions);
        SendMessageW(runtimeTooltipButton, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
        SendMessageW(runtimeTooltipButton, WM_MOUSEMOVE, 0, MAKELPARAM(3, 2));
        SendMessageW(runtimeTooltipButton, WM_MOUSEMOVE, 0, MAKELPARAM(4, 2));
        Check(
            tooltipRegistry.visible && tooltipRegistry.showCount == 1 &&
                tooltipRegistry.shownText == L"button description" &&
                tooltipRegistry.shownOptions.placement == ThemedTooltipPlacement::Cursor,
            "Themed control tooltip shows once for one hover session");
        SendMessageW(runtimeTooltipButton, WM_MOUSELEAVE, 0, 0);
        Check(!tooltipRegistry.visible && tooltipRegistry.hideCount == 1,
            "Themed control tooltip hides when pointer leaves");
        HWND runtimeCombo = controlUi.ComboBox(7105, 240, 72, controlUi.comboBoxWidth({L"Short", L"Long option"}));
        ThemedUi::SetComboBoxItems(runtimeCombo, {L"Short", L"Long option"}, 1);
        Check(ThemedUi::ComboBoxSelectedIndex(runtimeCombo) == 1, "Themed combo public item and selection state");
        ThemedUi::SetComboBoxSelectedIndex(runtimeCombo, 0);
        Check(ThemedUi::ComboBoxSelectedIndex(runtimeCombo) == 0, "Themed combo public selection update");
        Check(controlUi.tableColumnWidth(L"column") > controlUi.textWidth(L"column"),
            "Themed table column width includes public cell padding");
        Check(controlUi.tableColumnWidth({L"short", L"a much wider status"})
                >= controlUi.tableColumnWidth(L"a much wider status"),
            "Themed table column width measures every public candidate text");
        Check(controlUi.tableHeightForRows(8, false) - controlUi.tableHeightForRows(7, false)
                == controlUi.scale(static_cast<int>(fallbackTheme.metric(L"listItem", L"height", 28.0f))),
            "Themed table visible-row height advances by the public row template");
        const ThemedFormLayout publicForm(controlUi);
        const int tableContentHeight = controlUi.tableHeightForRows(7, false);
        const ThemedSectionGeometry tableSection = publicForm.contentSection(8, 12, 320, tableContentHeight);
        Check(tableSection.content.bottom - tableSection.content.top == tableContentHeight,
            "Themed form continuous-content section preserves the public table height");
        const RECT publicTabStrip = controlUi.tabStripRect(RECT{10, 10, 300, 300});
        Check(publicTabStrip.bottom < 300 && controlUi.tabPageTop(publicTabStrip) > publicTabStrip.bottom,
            "Themed tab layout separates strip and page content");
        HWND runtimeLink = controlUi.LinkText(510, L"link", 0, 100, 120, linkOptions);
        Check(runtimeLink != nullptr, "Themed link public factory");
        ThemedUi::SetLinkVisited(runtimeLink, false);
        HWND runtimeTable = controlUi.Table(
            511, RECT{0, 130, 360, 260},
            {ThemedTableColumn{L"name", L"Name", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 120},
             ThemedTableColumn{L"value", L"Value", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining}},
            tableOptions);
        Check(runtimeTable != nullptr, "Themed table public factory");
        ThemedUi::SetTableRows(runtimeTable, {
            ThemedTableRow{42, {{L"row"}, {L"value"}}, true, true},
            ThemedTableRow{43, {{L"disabled"}, {L"preserved"}}, true, false},
        });
        Check(ThemedUi::TableRowCount(runtimeTable) == 2 && ThemedUi::TableRowKey(runtimeTable, 0) == 42, "Themed table public row model");
        Check(ThemedUi::IsTableChecked(runtimeTable, 0), "Themed table public checked state");
        Check(ThemedUi::IsTableChecked(runtimeTable, 1), "Themed table preserves disabled checked state");
        ThemedUi::SetTableChecked(runtimeTable, 1, false);
        Check(ThemedUi::IsTableChecked(runtimeTable, 1), "Themed table public setter leaves disabled rows unchanged");
        RECT runtimeTableClient{};
        GetClientRect(runtimeTable, &runtimeTableClient);
        const int runtimeTableColumnWidth =
            ListView_GetColumnWidth(runtimeTable, 0) + ListView_GetColumnWidth(runtimeTable, 1);
        Check(runtimeTableColumnWidth <= runtimeTableClient.right - runtimeTableClient.left,
            "Themed table keeps public facade columns inside the client width by default");
        HWND noResizeTable = controlUi.Table(
            7110, RECT{0, 270, 360, 360},
            {ThemedTableColumn{L"name", L"Name", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 120},
             ThemedTableColumn{L"value", L"Value", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining}},
            ThemedTableOptions{});
        HWND noResizeHeader = ListView_GetHeader(noResizeTable);
        Check(noResizeHeader != nullptr, "Themed table creates a native header after columns are inserted");
        Check(noResizeHeader && ((GetWindowLongPtrW(noResizeHeader, GWL_STYLE) & HDS_NOSIZING) != 0),
            "Themed table disables header resize by default after column creation");
        ThemedTableOptions resizeTableOptions{};
        resizeTableOptions.allowColumnResize = true;
        HWND resizeTable = controlUi.Table(
            7111, RECT{370, 270, 730, 360},
            {ThemedTableColumn{L"name", L"Name", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 120},
             ThemedTableColumn{L"value", L"Value", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining}},
            resizeTableOptions);
        HWND resizeHeader = ListView_GetHeader(resizeTable);
        Check(resizeHeader && ((GetWindowLongPtrW(resizeHeader, GWL_STYLE) & HDS_NOSIZING) == 0),
            "Themed table preserves explicit header resize opt-in");
        HWND groupChild = CreateWindowExW(0, L"STATIC", L"child", WS_CHILD | WS_VISIBLE, 0, 0, 40, 20, controlParent, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND runtimeGroup = controlUi.GroupBox(512, L"Group", RECT{380, 0, 620, 120});
        const RECT runtimeGroupContent = ThemedUi::GroupContentRect(runtimeGroup);
        const UINT runtimeGroupDpi = GetDpiForWindow(runtimeGroup);
        const int runtimeGroupContentTop = ThemedWindowUi::ScaleForDpi(4, runtimeGroupDpi)
            + ThemedWindowUi::ScaleForDpi(20, runtimeGroupDpi)
            + ThemedWindowUi::ScaleForDpi(4, runtimeGroupDpi);
        Check(runtimeGroupContent.top == runtimeGroupContentTop,
            "Themed group box public content rect includes the scaled title top inset");
        ThemedUi::BindGroupChildren(runtimeGroup, {groupChild});
        ThemedUi::SetGroupEnabled(runtimeGroup, false);
        Check(!IsWindowEnabled(groupChild), "Themed group box propagates enabled state");
        HWND page0 = CreateWindowExW(0, L"STATIC", L"page0", WS_CHILD | WS_VISIBLE, 0, 0, 40, 20, controlParent, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND page1 = CreateWindowExW(0, L"STATIC", L"page1", WS_CHILD | WS_VISIBLE, 0, 0, 40, 20, controlParent, nullptr, GetModuleHandleW(nullptr), nullptr);
        ThemedTabControlOptions runtimeTabOptions{};
        runtimeTabOptions.appearance = ThemedTabControlAppearance::EmphasizedSegmented;
        HWND runtimeTabs = controlUi.TabControl(
            513, RECT{380, 130, 700, 190},
            {{601, L"One", true}, {602, L"Two", true}}, runtimeTabOptions);
        ThemedUi::BindTabPage(runtimeTabs, 0, {page0});
        ThemedUi::BindTabPage(runtimeTabs, 1, {page1});
        ThemedUi::SetActiveTab(runtimeTabs, 1);
        const bool page0Visible = (GetWindowLongW(page0, GWL_STYLE) & WS_VISIBLE) != 0;
        const bool page1Visible = (GetWindowLongW(page1, GWL_STYLE) & WS_VISIBLE) != 0;
        Check(ThemedUi::ActiveTab(runtimeTabs) == 1 && !page0Visible && page1Visible, "Themed tab control binds page visibility");
        ThemedTabControlOptions minimalTabOptions{};
        minimalTabOptions.appearance = ThemedTabControlAppearance::MinimalUnderline;
        ThemedTabControlOptions softPillTabOptions{};
        softPillTabOptions.appearance = ThemedTabControlAppearance::SoftPill;
        ThemedTabControlOptions connectedTabOptions{};
        connectedTabOptions.appearance = ThemedTabControlAppearance::ConnectedTabs;
        Check(connectedTabOptions.containerStyle == ThemedTabControlContainerStyle::AppearanceDefault,
            "Themed tab container style defaults to appearance behavior");
        Check(controlUi.TabControl(517, RECT{0, 400, 300, 434}, {{711, L"One"}, {712, L"Two"}}, minimalTabOptions) != nullptr,
            "Themed minimal underline tab control factory");
        Check(controlUi.TabControl(518, RECT{0, 440, 300, 474}, {{721, L"One"}, {722, L"Two"}}, softPillTabOptions) != nullptr,
            "Themed soft pill tab control factory");
        HWND connectedTabs = controlUi.TabControl(519, RECT{0, 480, 300, 514}, {{731, L"One"}, {732, L"Two"}}, connectedTabOptions);
        Check(connectedTabs != nullptr,
            "Themed connected tab control factory");
        if (connectedTabs) {
            Check(ThemedControls::TabContainerStyle(connectedTabs) ==
                    static_cast<int>(ThemedTabControlContainerStyle::AppearanceDefault),
                "Themed tab container style runtime default");

            ThemedTabControlOptions borderlessOptions = connectedTabOptions;
            borderlessOptions.containerStyle = ThemedTabControlContainerStyle::Borderless;
            HWND borderlessTabs = controlUi.TabControl(
                520, RECT{0, 520, 300, 554}, {{741, L"One"}, {742, L"Two"}}, borderlessOptions);
            Check(borderlessTabs && ThemedControls::TabContainerStyle(borderlessTabs) ==
                    static_cast<int>(ThemedTabControlContainerStyle::Borderless),
                "Themed tab container style runtime borderless");

            HDC screen = GetDC(nullptr);
            HDC dc = screen ? CreateCompatibleDC(screen) : nullptr;
            HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, 320, 40) : nullptr;
            HGDIOBJ oldBitmap = dc && bitmap ? SelectObject(dc, bitmap) : nullptr;
            auto drawContainer = [&](HWND tab, int height) {
                DRAWITEMSTRUCT draw{};
                draw.CtlType = ODT_BUTTON;
                draw.hwndItem = tab;
                draw.hDC = dc;
                draw.rcItem = RECT{0, 0, 320, height};
                ThemedControls::Draw(fallbackTheme, &draw);
                return GetPixel(dc, 160, 0);
            };
            const COLORREF framedTop = dc && bitmap ? drawContainer(connectedTabs, 40) : CLR_INVALID;
            const COLORREF borderlessTop = dc && bitmap && borderlessTabs ? drawContainer(borderlessTabs, 40) : CLR_INVALID;
            const auto colorRef = [](const Color& color) {
                return RGB(
                    static_cast<BYTE>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                    static_cast<BYTE>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                    static_cast<BYTE>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f));
            };
            const COLORREF connectedBorder = colorRef(fallbackTheme.color(L"tabControl", L"connected", L"border"));
            const COLORREF connectedBg = colorRef(fallbackTheme.color(L"tabControl", L"connected", L"bg"));
            Check(framedTop == connectedBorder && borderlessTop == connectedBg,
                "Themed connected tab container borderless drawing removes only outer frame");
            if (dc && oldBitmap) SelectObject(dc, oldBitmap);
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
            if (screen) ReleaseDC(nullptr, screen);
        }
        const std::vector<ThemedTabControlAppearance> verticalAppearances{
            ThemedTabControlAppearance::Standard,
            ThemedTabControlAppearance::EmphasizedSegmented,
            ThemedTabControlAppearance::MinimalUnderline,
            ThemedTabControlAppearance::SoftPill,
            ThemedTabControlAppearance::ConnectedTabs,
        };
        const LONG controlParentStyle = GetWindowLongW(controlParent, GWL_STYLE);
        SetWindowLongW(controlParent, GWL_STYLE, controlParentStyle | WS_VISIBLE);
        for (std::size_t index = 0; index < verticalAppearances.size(); ++index) {
            const int tabId = 530 + static_cast<int>(index);
            const int firstId = 800 + static_cast<int>(index) * 2;
            const int secondId = firstId + 1;
            ThemedTabControlOptions verticalOptions{};
            verticalOptions.appearance = verticalAppearances[index];
            verticalOptions.orientation = ThemedTabControlOrientation::Vertical;
            HWND verticalTabs = controlUi.TabControl(
                tabId,
                RECT{0, 0, 140, 96},
                {{firstId, L"One"}, {secondId, L"Two"}},
                verticalOptions);
            HWND firstButton = verticalTabs ? GetDlgItem(verticalTabs, firstId) : nullptr;
            HWND secondButton = verticalTabs ? GetDlgItem(verticalTabs, secondId) : nullptr;
            RECT firstRect{};
            RECT secondRect{};
            if (firstButton) GetWindowRect(firstButton, &firstRect);
            if (secondButton) GetWindowRect(secondButton, &secondRect);
            if (verticalTabs) {
                MapWindowPoints(HWND_DESKTOP, verticalTabs, reinterpret_cast<POINT*>(&firstRect), 2);
                MapWindowPoints(HWND_DESKTOP, verticalTabs, reinterpret_cast<POINT*>(&secondRect), 2);
            }
            Check(verticalTabs && firstButton && secondButton &&
                    firstRect.left == secondRect.left && secondRect.top > firstRect.top &&
                    (firstRect.right - firstRect.left) == (secondRect.right - secondRect.left),
                "Themed tab appearance supports vertical layout");
            if (firstButton) {
                SendMessageW(firstButton, WM_KEYDOWN, VK_RIGHT, 0);
                Check(ThemedUi::ActiveTab(verticalTabs) == 0,
                    "Vertical themed tabs ignore horizontal navigation keys");
                SendMessageW(firstButton, WM_KEYDOWN, VK_DOWN, 0);
                Check(ThemedUi::ActiveTab(verticalTabs) == 1,
                    "Vertical themed tabs use vertical navigation keys");
            }
        }
        SetWindowLongW(controlParent, GWL_STYLE, controlParentStyle);
        ThemedUi::BindTabPageRoot(runtimeTabs, 1, page1);
        HWND panelChild = CreateWindowExW(0, L"STATIC", L"panel child", WS_CHILD | WS_VISIBLE, 400, 310, 80, 20, controlParent, nullptr, GetModuleHandleW(nullptr), nullptr);
        ThemedPanelOptions panelOptions{};
        panelOptions.role = ThemedPanelRole::Inset;
        panelOptions.scrollable = true;
        HWND runtimePanel = controlUi.Panel(516, RECT{380, 300, 620, 380}, panelOptions);
        const RECT runtimePanelContent = ThemedUi::PanelContentRect(runtimePanel);
        const UINT runtimePanelDpi = GetDpiForWindow(runtimePanel);
        Check(runtimePanelContent.left == ThemedWindowUi::ScaleForDpi(10, runtimePanelDpi) &&
                runtimePanelContent.top == ThemedWindowUi::ScaleForDpi(8, runtimePanelDpi) &&
                runtimePanelContent.right == 240 - ThemedWindowUi::ScaleForDpi(10, runtimePanelDpi) &&
                runtimePanelContent.bottom == 80 - ThemedWindowUi::ScaleForDpi(8, runtimePanelDpi),
            "Themed panel public content rect preserves themed DPI-scaled insets");
        ThemedUi::BindPanelChildren(runtimePanel, {panelChild});
        Check(GetWindow(runtimePanel, GW_HWNDNEXT) == nullptr,
            "Themed panel remains behind sibling content in the parent z-order");
        ThemedUi::SetPanelEnabled(runtimePanel, false);
        Check(runtimePanel != nullptr && !IsWindowEnabled(panelChild), "Themed panel propagates enabled state");
        ThemedUi::SetPanelEnabled(runtimePanel, true);
        ThemedUi::SetPanelRole(runtimePanel, ThemedPanelRole::Raised);
        Check(ThemedUi::IsPanelEnabled(runtimePanel) && ThemedUi::IsPanelVisible(runtimePanel), "Themed panel public state queries");
        HWND fixedPanelChild = CreateWindowExW(0, L"STATIC", L"fixed panel child", WS_CHILD | WS_VISIBLE,
            12, 14, 80, 20, controlParent, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND fixedPanel = controlUi.Panel(7120, RECT{0, 520, 240, 600});
        ThemedUi::BindPanelChildren(fixedPanel, {fixedPanelChild});
        SCROLLINFO fixedPanelScroll{sizeof(fixedPanelScroll), SIF_RANGE | SIF_PAGE | SIF_POS};
        GetScrollInfo(fixedPanel, SB_VERT, &fixedPanelScroll);
        Check((GetWindowLongPtrW(fixedPanel, GWL_STYLE) & WS_VSCROLL) == 0 &&
                fixedPanelScroll.nMin == 0 && fixedPanelScroll.nMax == 0 && fixedPanelScroll.nPage == 0,
            "Themed non-scrollable panel binding does not expose a vertical scrollbar range");
        RECT fixedChildBefore{};
        RECT fixedChildAfter{};
        GetWindowRect(fixedPanelChild, &fixedChildBefore);
        SetWindowPos(fixedPanel, nullptr, 0, 520, 300, 100, SWP_NOACTIVATE | SWP_NOZORDER);
        GetWindowRect(fixedPanelChild, &fixedChildAfter);
        Check(EqualRect(&fixedChildBefore, &fixedChildAfter),
            "Themed non-scrollable panel resize does not overwrite externally scaled child geometry");
        HWND runtimeToolbar = controlUi.ToolBar(
            514, RECT{380, 200, 700, 240},
            {{701, L"Run"}, {702, L"Pin", ThemedToolItemKind::Toggle, ThemedToolItemAlignment::Leading, true, false}});
        ThemedUi::SetToolChecked(runtimeToolbar, 702, true);
        Check(ThemedUi::IsToolChecked(runtimeToolbar, 702), "Themed toolbar public toggle state");
        ThemedToolItem iconTool{703, L"Open"};
        iconTool.icon = LoadIconW(nullptr, IDI_APPLICATION);
        iconTool.display = ThemedToolItemDisplay::IconAndText;
        HWND overflowToolbar = controlUi.ToolBar(
            515, RECT{380, 250, 500, 290},
            {iconTool, ThemedToolItem{0, L"", ThemedToolItemKind::Separator}, ThemedToolItem{704, L"Long action"}});
        Check(overflowToolbar != nullptr, "Themed toolbar public icon item factory");
        HWND overflowButton = GetDlgItem(overflowToolbar, 0x7ff0);
        Check(overflowButton != nullptr && (GetWindowLongW(overflowButton, GWL_STYLE) & WS_VISIBLE) != 0, "Themed toolbar enables public automatic overflow");
        ThemedUi::BeginToolBarUpdate(runtimeToolbar);
        ThemedUi::SetToolText(runtimeToolbar, 701, L"Run now");
        ThemedUi::SetToolTooltip(runtimeToolbar, 701, L"Execute");
        ThemedUi::SetToolAlignment(runtimeToolbar, 702, ThemedToolItemAlignment::Trailing);
        ThemedUi::EndToolBarUpdate(runtimeToolbar);
        Check(ThemedUi::HasTool(runtimeToolbar, 701) && ThemedUi::IsToolEnabled(runtimeToolbar, 701), "Themed toolbar dynamic query state");
        Check(ThemedUi::InsertTool(runtimeToolbar, 1, ThemedToolItem{705, L"More"}), "Themed toolbar dynamic insert");
        Check(ThemedUi::MoveTool(runtimeToolbar, 705, 0) && ThemedUi::ToolIndex(runtimeToolbar, 705) == 0, "Themed toolbar dynamic reorder");
        Check(ThemedUi::RemoveTool(runtimeToolbar, 705) && !ThemedUi::HasTool(runtimeToolbar, 705), "Themed toolbar dynamic remove");
        HWND dpiChild = CreateWindowExW(0, L"STATIC", L"dpi", WS_CHILD | WS_VISIBLE,
            10, 10, 40, 20, controlParent, nullptr, GetModuleHandleW(nullptr), nullptr);
        ThemedWindowUi dpiWindow(
            GetModuleHandleW(nullptr), nullptr, controlParent, fallbackTheme,
            DialogLayoutKind::Compact, 320, 200);
        HWND dpiTable = dpiWindow.ui().Table(
            517,
            RECT{80, 40, 300, 140},
            {
                ThemedTableColumn{L"name", L"Name", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 60},
                ThemedTableColumn{L"path", L"Path", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                ThemedTableColumn{L"action", L"Action", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, 50},
            });
        Check(dpiTable != nullptr && ListView_GetColumnWidth(dpiTable, 0) == 60 && ListView_GetColumnWidth(dpiTable, 2) == 50,
            "Themed table starts with configured fixed column widths");
        RECT suggested{0, 0, 400, 250};
        LRESULT dpiResult = 0;
        Check(dpiWindow.HandleMessage(WM_DPICHANGED, MAKEWPARAM(120, 120), reinterpret_cast<LPARAM>(&suggested), dpiResult),
            "Themed window handles 125 percent DPI change");
        RECT dpiChildRect{};
        GetWindowRect(dpiChild, &dpiChildRect);
        Check(dpiChildRect.right - dpiChildRect.left == 50 && dpiChildRect.bottom - dpiChildRect.top == 25,
            "Themed window scales child geometry at 125 percent DPI");
        Check(ListView_GetColumnWidth(dpiTable, 0) == 75 && ListView_GetColumnWidth(dpiTable, 2) == 63,
            "Themed table scales fixed columns at 125 percent DPI");
        suggested = RECT{0, 0, 480, 300};
        Check(dpiWindow.HandleMessage(WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested), dpiResult),
            "Themed window handles 150 percent DPI change");
        GetWindowRect(dpiChild, &dpiChildRect);
        Check(dpiChildRect.right - dpiChildRect.left == 60 && dpiChildRect.bottom - dpiChildRect.top == 30 && dpiWindow.ui().dpi() == 144,
            "Themed window keeps public UI metrics at 150 percent DPI");
        Check(ListView_GetColumnWidth(dpiTable, 0) == 90 && ListView_GetColumnWidth(dpiTable, 2) == 76,
            "Themed table keeps scaled fixed columns at 150 percent DPI");
        DestroyWindow(controlParent);
    }
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
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Open with Code") == ShellContextMenuProviderId::VsCode, "Shell menu detects VS Code");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Search Everything...") == ShellContextMenuProviderId::Everything, "Shell menu detects Everything");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Edit with Notepad++") == ShellContextMenuProviderId::NotepadPlusPlus, "Shell menu detects Notepad++");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"在终端中打开") == ShellContextMenuProviderId::Terminal, "Shell menu detects terminal");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"7-Zip") == ShellContextMenuProviderId::Archive, "Shell menu detects archive tool");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Git Bash Here") == ShellContextMenuProviderId::Git, "Shell menu keeps Git Bash in Git provider");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Open with Cursor") == ShellContextMenuProviderId::Cursor, "Shell menu detects Cursor");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"通过 Cursor 打开") == ShellContextMenuProviderId::Cursor, "Shell menu detects Cursor Chinese label");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"", L"cursor") == ShellContextMenuProviderId::Cursor, "Shell menu detects Cursor verb");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Open with Sublime Text") == ShellContextMenuProviderId::SublimeText, "Shell menu detects Sublime Text");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"", L"subl") == ShellContextMenuProviderId::SublimeText, "Shell menu detects Sublime verb");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Open with Windsurf") == ShellContextMenuProviderId::Windsurf, "Shell menu detects Windsurf");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Open with Trae") == ShellContextMenuProviderId::Trae, "Shell menu detects Trae");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Open with Zed") == ShellContextMenuProviderId::Zed, "Shell menu detects Zed");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"", L"zed") == ShellContextMenuProviderId::Zed, "Shell menu detects Zed verb");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Edit with Vim") == ShellContextMenuProviderId::Vim, "Shell menu detects Vim");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"", L"gvim") == ShellContextMenuProviderId::Vim, "Shell menu detects gVim verb");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Open with VSCodium", L"vscodium") == ShellContextMenuProviderId::VsCode, "Shell menu keeps VSCodium in VS Code provider");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Edit with Notepad3") == ShellContextMenuProviderId::NotepadPlusPlus, "Shell menu keeps Notepad3 in Notepad++ provider");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"Pick cursor color").empty(), "Shell menu ignores cursor without open-with anchor");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"", L"7zed").empty(), "Shell menu ignores zed substring verbs");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"", L"vimeo").empty(), "Shell menu ignores vim substring verbs");
    Check(ShellItemService::DetectTrackedContextMenuProvider(L"普通菜单").empty(), "Shell menu ignores unknown provider");

    {
        // 设置页表格行序：已安装保持绑定表相对顺序，未安装沉底。
        std::vector<bool> installed(TrackedContextMenuProviderCount, true);
        installed[0] = false;
        installed[3] = false;
        const auto order = TrackedContextMenuDisplayOrder(installed);
        Check(order.size() == TrackedContextMenuProviderCount, "Tracked provider display order covers all providers");
        std::vector<bool> seen(TrackedContextMenuProviderCount, false);
        bool uniqueIndices = true;
        for (std::size_t index : order) {
            if (index >= seen.size() || seen[index]) {
                uniqueIndices = false;
                break;
            }
            seen[index] = true;
        }
        Check(uniqueIndices, "Tracked provider display order has unique indices");
        Check(order[order.size() - 2] == 0 && order.back() == 3,
              "Tracked provider display order sinks uninstalled providers keeping table order");
        bool installedKeepOrder = true;
        for (std::size_t i = 0; i + 1 < order.size() - 2; ++i) {
            if (order[i] > order[i + 1]) {
                installedKeepOrder = false;
                break;
            }
        }
        Check(installedKeepOrder, "Tracked provider display order keeps installed relative order");
        const auto allInstalled = TrackedContextMenuDisplayOrder(std::vector<bool>(TrackedContextMenuProviderCount, true));
        bool identity = allInstalled.size() == TrackedContextMenuProviderCount;
        for (std::size_t i = 0; identity && i < allInstalled.size(); ++i) {
            identity = allInstalled[i] == i;
        }
        Check(identity, "Tracked provider display order is identity when all installed");
        // 探测输入短于表长时缺省视为已安装。
        const auto shortInput = TrackedContextMenuDisplayOrder({});
        Check(shortInput.size() == TrackedContextMenuProviderCount, "Tracked provider display order tolerates short input");
    }
    {
        // 绑定表自检：providerId/显示名非空且 providerId 唯一，
        // 表内行序即右键菜单 provider 顺序的唯一来源。
        std::unordered_set<std::wstring> providerIds;
        bool bindingsValid = true;
        for (const auto& provider : TrackedContextMenuProviders()) {
            if (!provider.providerId || !*provider.providerId ||
                !provider.displayName || !*provider.displayName ||
                !providerIds.insert(provider.providerId).second) {
                bindingsValid = false;
                break;
            }
        }
        Check(bindingsValid, "Tracked provider bindings have unique ids and display names");
    }
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
    Check(MenuIconFor(ID_MENU_REFRESH_GROUP_LINKS, L"刷新") == MenuIconRefresh, "Group refresh command icon");
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
    bool hasProcessTools = false;
    bool hasLegacyProcessTool = false;
    bool hasAppLaunchLocker = false;
    bool hasAdBlock = false;
    for (const auto& plugin : plugins) {
        if (plugin.id == L"quattro.builtin.clicker" && plugin.name == L"连点器") {
            hasAutoClickerName = true;
        }
        if (plugin.id == L"quattro.builtin.process-tools" &&
            plugin.name == L"进程工具" &&
            plugin.engine == L"process-tools") {
            hasProcessTools = true;
        }
        if (plugin.id == L"quattro.builtin.port-inspector" ||
            plugin.id == L"quattro.builtin.process-inspector" ||
            plugin.id == L"quattro.builtin.process-locator" ||
            plugin.id == L"quattro.builtin.file-lock-inspector") {
            hasLegacyProcessTool = true;
        }
        if (plugin.id == L"quattro.builtin.app-launch-locker" &&
            plugin.name == L"自启动管理" &&
            plugin.engine == L"app-launch-locker") {
            hasAppLaunchLocker = true;
        }
        if (plugin.id == L"quattro.builtin.ad-block" &&
            plugin.name == L"广告拦截" &&
            plugin.engine == L"ad-block") {
            hasAdBlock = true;
        }
    }
    Check(hasAutoClickerName, "Plugin clicker display name");
    Check(hasProcessTools, "Plugin process tools registration");
    Check(!hasLegacyProcessTool, "Legacy process tools are hidden from the toolbox registry");
    Check(hasAppLaunchLocker == !QuattroIsOfficialBuild(),
        "Plugin AppLaunchLocker registration follows build identity");
    Check(hasAdBlock, "Plugin AdBlock external tool registration");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.clicker"), "Plugin clicker enabled by default");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.timer"), "Plugin timer enabled by default");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.process-tools"), "Plugin process tools enabled by default");
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.app-launch-locker") == !QuattroIsOfficialBuild(),
        "Plugin AppLaunchLocker availability follows build identity");
    if (QuattroIsOfficialBuild()) {
        Check(!pluginRegistry.SetEnabled(L"quattro.builtin.app-launch-locker", true),
            "Plugin AppLaunchLocker cannot be enabled in official builds");
    }
    Check(pluginRegistry.IsEnabled(L"quattro.builtin.ad-block"), "Plugin AdBlock enabled by default");
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
