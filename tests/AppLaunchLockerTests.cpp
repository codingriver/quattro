#include "../src/applaunchlocker/AppLaunchLockerCore.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
bool Check(bool condition, const wchar_t* message) {
    if (!condition) std::wcerr << L"FAILED: " << message << L"\n";
    return condition;
}
}

int wmain() {
    bool ok = true;
    const OperationResult complete{true, L"完成"};
    const OperationResult partial{true, L"部分完成", true};
    ok &= Check(!complete.partial, L"complete operation should not be partial by default");
    ok &= Check(partial.success && partial.partial, L"partial operation should retain partial outcome");
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"AppLaunchLockerTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
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
        ScanResult previousScan;
        StartupItem oldItem;
        oldItem.id = L"registry-old";
        oldItem.source = StartupSourceType::Registry;
        oldItem.name = L"Old App";
        oldItem.location = L"HKCU\\Run";
        oldItem.command = L"C:\\Old\\old.exe";
        oldItem.canDisable = true;
        oldItem.readOnly = false;
        previousScan.items.push_back(oldItem);

        StartupItem changedItem = oldItem;
        changedItem.id = L"registry-changed";
        changedItem.name = L"Changed App";
        changedItem.command = L"C:\\Changed\\old.exe";
        previousScan.items.push_back(changedItem);

        StartupItem stateItem = oldItem;
        stateItem.id = L"registry-state";
        stateItem.name = L"State App";
        previousScan.items.push_back(stateItem);

        ScanResult currentScan;
        StartupItem changedNow = changedItem;
        changedNow.command = L"C:\\Changed\\new.exe";
        currentScan.items.push_back(changedNow);

        StartupItem stateNow = stateItem;
        stateNow.canDisable = false;
        stateNow.readOnly = true;
        currentScan.items.push_back(stateNow);

        StartupItem addedItem;
        addedItem.id = L"registry-added";
        addedItem.source = StartupSourceType::Registry;
        addedItem.name = L"Added App";
        addedItem.location = L"HKCU\\Run";
        addedItem.command = L"C:\\Added\\added.exe";
        addedItem.canDisable = true;
        addedItem.readOnly = false;
        currentScan.items.push_back(addedItem);

        StartupSnapshot previous = BuildStartupSnapshot(previousScan, L"previous-scan");
        StartupSnapshot current = BuildStartupSnapshot(currentScan, L"current-scan");
        ok &= Check(previous.entries.size() == 3 && previous.scanId == L"previous-scan",
            L"snapshot builder should preserve scan id and entries");

        const std::filesystem::path snapshotFile = directory / L"startup-snapshot.json";
        StartupSnapshotStore snapshotStore(snapshotFile);
        ok &= Check(snapshotStore.Save(current, error), L"snapshot save should succeed");
        StartupSnapshot loadedSnapshot;
        ok &= Check(snapshotStore.Load(loadedSnapshot, error), L"snapshot load should succeed");
        ok &= Check(loadedSnapshot.entries.size() == current.entries.size() && loadedSnapshot.scanId == L"current-scan",
            L"snapshot should round-trip");

        const StartupSnapshotDiff diff = DiffStartupSnapshots(previous, current);
        ok &= Check(diff.added.size() == 1 && diff.added[0].entryId == L"registry-added",
            L"snapshot diff should detect added entries");
        ok &= Check(diff.removed.size() == 1 && diff.removed[0].entryId == L"registry-old",
            L"snapshot diff should detect removed entries");
        ok &= Check(diff.changed.size() == 1 && diff.changed[0].entryId == L"registry-changed",
            L"snapshot diff should detect content changes");
        ok &= Check(diff.stateChanged.size() == 1 && diff.stateChanged[0].entryId == L"registry-state",
            L"snapshot diff should detect state changes");

        {
            std::ofstream malformedSnapshot(snapshotFile, std::ios::binary | std::ios::trunc);
            malformedSnapshot << "{not-json";
        }
        ok &= Check(!snapshotStore.Load(loadedSnapshot, error), L"malformed snapshot should report an error");
        ok &= Check(!error.empty(), L"malformed snapshot should not be silently accepted");
    }

    const ScanResult scan = StartupManager(DisabledItemStore(directory / L"unused.json")).ScanAll();
    for (const auto& item : scan.items) {
        if (item.source == StartupSourceType::Driver || item.source == StartupSourceType::WmiSubscription ||
            item.source == StartupSourceType::Winlogon || item.source == StartupSourceType::WinlogonNotify ||
            item.source == StartupSourceType::AppInitDll || item.source == StartupSourceType::AppCertDll ||
            item.source == StartupSourceType::BootExecute || item.source == StartupSourceType::KnownDll ||
            item.source == StartupSourceType::ShellExtension || item.source == StartupSourceType::Ifeo) {
            ok &= Check(item.readOnly && !item.canDisable, L"sensitive sources must always remain read-only");
        }
    }

    std::error_code cleanupError;
    // 广告拦截（AdBlockManager）纯函数用例：仅走扫描与拒绝分支，绝不写入 HKLM。
    {
        const std::filesystem::path adDir = directory / L"adblock";
        std::filesystem::create_directories(adDir, cleanupError);
        AdBlockManager adBlock(DisabledItemStore(adDir / L"blocked-items.json"));

        // 空存储：列表为空。
        std::vector<DisabledRecord> blocked;
        std::wstring blockedError;
        ok &= Check(adBlock.ListBlocked(blocked, blockedError), L"empty blocked store should load");
        ok &= Check(blocked.empty(), L"empty blocked store should contain no records");

        // 不存在的路径：只报警告，无条目。
        const ScanResult missing = adBlock.ScanPath((adDir / L"does-not-exist").wstring());
        ok &= Check(missing.items.empty(), L"scan of missing path yields no items");
        ok &= Check(!missing.warnings.empty(), L"scan of missing path reports a warning");

        // 造一个脚本与一个未签名 exe，验证分类与守卫（脚本仅提示，未签名 exe 可拦截并告警）。
        const std::filesystem::path scriptPath = adDir / L"sample.bat";
        const std::filesystem::path exePath = adDir / L"sample.exe";
        const std::filesystem::path nestedDir = adDir / L"nested" / L"deeper";
        const std::filesystem::path nestedExePath = nestedDir / L"nested.exe";
        std::filesystem::create_directories(nestedDir, cleanupError);
        { std::ofstream(scriptPath, std::ios::binary) << "@echo off\n"; }
        { std::ofstream(exePath, std::ios::binary) << "not-a-real-pe"; }
        { std::ofstream(nestedExePath, std::ios::binary) << "not-a-real-pe"; }

        auto fieldOf = [](const StartupItem& item, const wchar_t* key) -> std::wstring {
            const auto found = item.original.find(key);
            return found == item.original.end() ? std::wstring{} : found->second;
        };

        const ScanResult scanDir = adBlock.ScanPath(adDir.wstring());
        bool sawScript = false;
        bool sawExe = false;
        bool sawNestedExe = false;
        for (const auto& item : scanDir.items) {
            if (fieldOf(item, L"adBlockStatus") == L"script") {
                sawScript = true;
                ok &= Check(!item.canDisable, L"script entry must not be blockable");
            }
            if (item.name == L"sample.exe") {
                sawExe = true;
                ok &= Check(fieldOf(item, L"adBlockStatus") == L"blockable-warn", L"unsigned exe should be blockable with warning");
                ok &= Check(item.canDisable, L"unsigned exe should be blockable");
            }
            if (item.name == L"nested.exe") sawNestedExe = true;
        }
        ok &= Check(sawScript, L"scan should list the script entry");
        ok &= Check(sawExe, L"scan should list the exe entry");
        ok &= Check(sawNestedExe, L"directory scan should recurse into nested directories");

        // 详细扫描：枚举后并行分析，并持续报告确定进度。
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

        // 取消：保留已完成的部分结果并以 Cancelled 结束。
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

        // 拒绝分支：未知模式、路径不存在、脚本目标——均不得写入注册表。
        ok &= Check(!adBlock.Block(exePath.wstring(), L"bogus").success, L"unknown block mode rejected");
        ok &= Check(!adBlock.Block((adDir / L"missing.exe").wstring(), L"exact").success, L"missing block target rejected");
        ok &= Check(!adBlock.Block(scriptPath.wstring(), L"exact").success, L"script block target rejected");

        // 启动拦截模式：临时目录内的 exe 不可能注册为开机自启动，须以“无自启动项”拒绝，
        // 且不静默回退 IFEO、不写注册表，「已拦截」列表保持为空。
        const OperationResult startupReject = adBlock.Block(exePath.wstring(), L"startup");
        ok &= Check(!startupReject.success, L"startup block on non-autostart target rejected");
        std::vector<DisabledRecord> afterStartup;
        std::wstring afterStartupError;
        ok &= Check(adBlock.ListBlocked(afterStartup, afterStartupError) && afterStartup.empty(),
            L"rejected startup block must not persist any record");

        // 解除不存在的记录：拒绝。
        ok &= Check(!adBlock.Unblock(L"no-such-record").success, L"unblock of unknown record rejected");
    }

    std::filesystem::remove_all(directory, cleanupError);
    if (ok) std::wcout << L"AppLaunchLocker tests passed\n";
    return ok ? 0 : 1;
}
