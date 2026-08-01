#include "../src/applaunchlocker/AppLaunchLockerCore.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

bool Check(bool condition, const wchar_t* message) {
    if (!condition) std::wcerr << L"FAILED: " << message << L"\n";
    return condition;
}

std::wstring FieldOf(const StartupItem& item, const wchar_t* key) {
    const auto found = item.original.find(key);
    return found == item.original.end() ? std::wstring{} : found->second;
}

} // namespace

int wmain() {
    bool ok = true;
    const OperationResult complete{true, L"完成"};
    const OperationResult partial{true, L"部分完成", true};
    ok &= Check(!complete.partial, L"complete operation should not be partial by default");
    ok &= Check(partial.success && partial.partial, L"partial operation should retain partial outcome");

    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"AppLaunchLockerTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code cleanupError;
    std::filesystem::create_directories(directory, cleanupError);

    const std::filesystem::path file = directory / L"disabled-items.json";
    DisabledItemStore store(file);

    std::vector<DisabledRecord> records;
    std::wstring error;
    ok &= Check(store.Load(records, error), L"missing store should load as empty");
    ok &= Check(records.empty(), L"missing store should contain no records");

    DisabledRecord registry;
    registry.recordId = L"record-registry";
    registry.itemId = L"item-registry";
    registry.source = StartupSourceType::Registry;
    registry.name = L"Example \"启动项\"";
    registry.disabledAt = L"2026-07-13T10:00:00Z";
    registry.original = {
        {L"hive", L"HKCU"},
        {L"key", L"Software\\Example"},
        {L"valueName", L"Example"},
        {L"valueType", L"1"},
        {L"valueData", L"C:\\Example\\Example.exe --test"},
    };
    DisabledRecord service;
    service.recordId = L"record-service";
    service.itemId = L"item-service";
    service.source = StartupSourceType::Service;
    service.name = L"Example Service";
    service.disabledAt = L"2026-07-13T11:00:00Z";
    service.requiresAdmin = true;
    service.original = {{L"serviceName", L"ExampleSvc"}, {L"startType", L"2"}, {L"delayed", L"1"}};
    records = {registry, service};

    ok &= Check(store.Save(records, error), L"store save should succeed");
    {
        std::ifstream saved(file, std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
        ok &= Check(json.find("\"schemaVersion\": 2") != std::string::npos,
            L"store save should use schema version 2");
        ok &= Check(json.find("\"records\"") != std::string::npos && json.find("\"restore\"") != std::string::npos,
            L"schema version 2 should use records and restore fields");
    }
    std::vector<DisabledRecord> loaded;
    ok &= Check(store.Load(loaded, error), L"saved store should load");
    ok &= Check(loaded.size() == 2, L"saved store should preserve record count");
    if (loaded.size() == 2) {
        ok &= Check(loaded[0].name == registry.name, L"unicode and quotes should round-trip");
        ok &= Check(loaded[0].original == registry.original, L"registry restore fields should round-trip");
        ok &= Check(loaded[1].requiresAdmin, L"requiresAdmin should round-trip");
        ok &= Check(loaded[1].original == service.original, L"service restore fields should round-trip");
    }

    {
        const std::filesystem::path legacyFile = directory / L"legacy-disabled-items.json";
        {
            std::ofstream legacy(legacyFile, std::ios::binary | std::ios::trunc);
            legacy << R"({
  "version": 1,
  "items": [{
    "recordId": "legacy-record",
    "itemId": "legacy-item",
    "source": "registry",
    "name": "Legacy Item",
    "disabledAt": "2026-07-13T12:00:00Z",
    "requiresAdmin": false,
    "original": {"hive": "HKCU", "key": "Software\\Example"}
  }]
})";
        }
        DisabledItemStore legacyStore(legacyFile);
        std::vector<DisabledRecord> legacyRecords;
        ok &= Check(legacyStore.Load(legacyRecords, error), L"version 1 store should load for migration");
        ok &= Check(legacyRecords.size() == 1 && legacyRecords[0].itemId == L"legacy-item",
            L"version 1 record should preserve its entry id");
        ok &= Check(legacyStore.Save(legacyRecords, error), L"version 1 store should save as version 2");
        std::ifstream migrated(legacyFile, std::ios::binary);
        const std::string migratedJson((std::istreambuf_iterator<char>(migrated)), std::istreambuf_iterator<char>());
        ok &= Check(migratedJson.find("\"schemaVersion\": 2") != std::string::npos,
            L"version 1 store should migrate to version 2");
        ok &= Check(std::filesystem::exists(legacyFile.wstring() + L".bak"),
            L"version 1 migration should preserve the previous file as backup");
    }

    for (StartupSourceType source : {StartupSourceType::Registry, StartupSourceType::StartupFolder,
            StartupSourceType::ScheduledTask, StartupSourceType::Service, StartupSourceType::ActiveSetup,
            StartupSourceType::Driver, StartupSourceType::WmiSubscription, StartupSourceType::Winlogon,
            StartupSourceType::WinlogonNotify, StartupSourceType::AppInitDll, StartupSourceType::AppCertDll,
            StartupSourceType::BootExecute, StartupSourceType::KnownDll, StartupSourceType::ShellExtension,
            StartupSourceType::Ifeo}) {
        StartupSourceType parsed{};
        ok &= Check(StartupSourceFromKey(StartupSourceKey(source), parsed) && parsed == source,
            L"source key should round-trip");
    }

    {
        std::ofstream malformed(file, std::ios::binary | std::ios::trunc);
        malformed << "{not-json";
    }
    loaded.clear();
    ok &= Check(!store.Load(loaded, error), L"malformed store should block modifications");
    ok &= Check(!error.empty(), L"malformed store should report an error");

    {
        const std::filesystem::path adDir = directory / L"adblock";
        std::filesystem::create_directories(adDir, cleanupError);
        AdBlockManager adBlock(DisabledItemStore(adDir / L"blocked-items.json"));

        std::vector<DisabledRecord> blocked;
        std::wstring blockedError;
        ok &= Check(adBlock.ListBlocked(blocked, blockedError), L"empty blocked store should load");
        ok &= Check(blocked.empty(), L"empty blocked store should contain no records");

        const ScanResult missing = adBlock.ScanPath((adDir / L"does-not-exist").wstring());
        ok &= Check(missing.items.empty(), L"scan of missing path yields no items");
        ok &= Check(!missing.warnings.empty(), L"scan of missing path reports a warning");

        const std::filesystem::path scriptPath = adDir / L"sample.bat";
        const std::filesystem::path exePath = adDir / L"sample.exe";
        const std::filesystem::path nestedDir = adDir / L"nested" / L"deeper";
        const std::filesystem::path nestedExePath = nestedDir / L"nested.exe";
        std::filesystem::create_directories(nestedDir, cleanupError);
        { std::ofstream(scriptPath, std::ios::binary) << "@echo off\n"; }
        { std::ofstream(exePath, std::ios::binary) << "not-a-real-pe"; }
        { std::ofstream(nestedExePath, std::ios::binary) << "not-a-real-pe"; }

        const ScanResult scanDir = adBlock.ScanPath(adDir.wstring());
        bool sawScript = false;
        bool sawExe = false;
        bool sawNestedExe = false;
        for (const auto& item : scanDir.items) {
            if (FieldOf(item, L"adBlockStatus") == L"script") {
                sawScript = true;
                ok &= Check(!item.canDisable, L"script entry must not be blockable");
            }
            if (item.name == L"sample.exe") {
                sawExe = true;
                ok &= Check(FieldOf(item, L"adBlockStatus") == L"blockable-warn",
                    L"unsigned exe should be blockable with warning");
                ok &= Check(item.canDisable, L"unsigned exe should be blockable");
            }
            if (item.name == L"nested.exe") sawNestedExe = true;
        }
        ok &= Check(sawScript, L"scan should list the script entry");
        ok &= Check(sawExe, L"scan should list the exe entry");
        ok &= Check(sawNestedExe, L"directory scan should recurse into nested directories");

        std::vector<AdBlockScanProgress> progressEvents;
        AdBlockScanOptions parallelOptions;
        parallelOptions.batchSize = 1;
        parallelOptions.maxWorkers = 4;
        const AdBlockScanResult detailed = adBlock.ScanPathDetailed(
            adDir.wstring(),
            {},
            [&](const AdBlockScanProgress& progress) { progressEvents.push_back(progress); },
            parallelOptions);
        ok &= Check(!detailed.cancelled && detailed.error.empty(), L"detailed recursive scan should complete");
        ok &= Check(detailed.totalCandidates >= 3 && detailed.checkedCandidates == detailed.totalCandidates,
            L"detailed scan should check every discovered candidate");
        ok &= Check(detailed.workerCount >= 2 && detailed.workerCount <= 4,
            L"directory scan should use bounded parallel workers");
        ok &= Check(!progressEvents.empty() && progressEvents.front().phase == AdBlockScanPhase::Validating &&
                progressEvents.back().phase == AdBlockScanPhase::Completed,
            L"detailed scan should report validating through completed phases");

        const AdBlockPlan exactPlan = adBlock.BuildBlockPlan({exePath.wstring(), scriptPath.wstring()}, L"exact");
        ok &= Check(exactPlan.items.size() == 2, L"adblock plan should include every requested target");
        ok &= Check(exactPlan.blockableCount == 1 && exactPlan.warningCount == 1 && exactPlan.blockedCount == 1,
            L"adblock plan should classify blockable warning and skipped targets");
        ok &= Check(exactPlan.items[0].willModifyIfeo && exactPlan.items[0].impactText == L"仅阻止此完整路径",
            L"exact plan should describe IFEO precise-path impact");
        ok &= Check(exactPlan.items[1].riskLevel == L"blocked",
            L"script plan item should be skipped");

        ok &= Check(AdBlockRecordStateKey(AdBlockRecordState::Overwritten) == L"overwritten" &&
                AdBlockRecordStateText(AdBlockRecordState::Inactive) == L"已失效",
            L"adblock record state text should be stable");

        const std::filesystem::path cancelDir = adDir / L"cancel";
        std::filesystem::create_directories(cancelDir, cleanupError);
        for (int index = 0; index < 48; ++index) {
            std::ofstream(cancelDir / (L"candidate-" + std::to_wstring(index) + L".exe"), std::ios::binary)
                << "not-a-real-pe";
        }
        std::atomic_bool cancelRequested{false};
        AdBlockScanOptions cancelOptions;
        cancelOptions.batchSize = 1;
        cancelOptions.maxWorkers = 2;
        cancelOptions.batchDelay = std::chrono::milliseconds(5);
        const AdBlockScanResult cancelled = adBlock.ScanPathDetailed(
            cancelDir.wstring(),
            [&]() { return cancelRequested.load(); },
            [&](const AdBlockScanProgress& progress) {
                if (progress.phase == AdBlockScanPhase::Analyzing && progress.checkedCandidates >= 2) {
                    cancelRequested.store(true);
                }
            },
            cancelOptions);
        ok &= Check(cancelled.cancelled, L"detailed scan should honor cancellation");
        ok &= Check(cancelled.checkedCandidates > 0 && cancelled.checkedCandidates < cancelled.totalCandidates,
            L"cancelled scan should retain partial checked results");

        ok &= Check(!adBlock.Block(exePath.wstring(), L"bogus").success, L"unknown block mode rejected");
        ok &= Check(!adBlock.Block((adDir / L"missing.exe").wstring(), L"exact").success,
            L"missing block target rejected");
        ok &= Check(!adBlock.Block(scriptPath.wstring(), L"exact").success, L"script block target rejected");
        ok &= Check(!adBlock.Unblock(L"no-such-record").success, L"unblock of unknown record rejected");

        DisabledRecord staleIfeo;
        staleIfeo.recordId = L"stale-ifeo";
        staleIfeo.itemId = L"stale-item";
        staleIfeo.source = StartupSourceType::Ifeo;
        staleIfeo.name = L"stale.exe";
        staleIfeo.disabledAt = L"2026-07-25T00:00:00Z";
        staleIfeo.original = {
            {L"mechanism", L"ifeo"},
            {L"blockMode", L"name"},
            {L"ifeoImageName", L"quattro-test-stale-never-exists.exe"},
            {L"ifeoView", L"64"},
            {L"targetPath", (adDir / L"stale.exe").wstring()},
        };
        std::vector<DisabledRecord> staleRecords{staleIfeo};
        ok &= Check(DisabledItemStore(adDir / L"blocked-items.json").Save(staleRecords, blockedError),
            L"test stale adblock record should save");
        const AdBlockRecordStatus staleStatus = adBlock.CheckRecordStatus(staleIfeo);
        ok &= Check(staleStatus.state == AdBlockRecordState::Inactive,
            L"missing IFEO key should be reported as inactive");
        ok &= Check(staleStatus.canRepair, L"inactive IFEO record should advertise repair when restore fields exist");
        ok &= Check(!adBlock.RepairRecord(L"no-such-record").success,
            L"repair of unknown record rejected");
        ok &= Check(!adBlock.RepairRecord(staleIfeo.recordId).success,
            L"repair should reject stale IFEO record when target file is missing");
        ok &= Check(adBlock.CleanStaleRecords().success, L"clean stale records should remove inactive entries");
        std::vector<DisabledRecord> afterClean;
        ok &= Check(adBlock.ListBlocked(afterClean, blockedError) && afterClean.empty(),
            L"clean stale records should persist removal");

        ok &= Check(DisabledItemStore(adDir / L"blocked-items.json").Save(staleRecords, blockedError),
            L"test stale adblock record should save before unblock all");
        ok &= Check(adBlock.UnblockAll().success, L"unblock all should treat already-missing IFEO as restored");
        std::vector<DisabledRecord> afterUnblockAll;
        ok &= Check(adBlock.ListBlocked(afterUnblockAll, blockedError) && afterUnblockAll.empty(),
            L"unblock all should remove restored records");
    }

    std::filesystem::remove_all(directory, cleanupError);
    if (ok) std::wcout << L"AppLaunchLocker tests passed\n";
    return ok ? 0 : 1;
}
