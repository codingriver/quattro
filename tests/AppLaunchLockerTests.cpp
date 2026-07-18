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
    std::vector<DisabledRecord> loaded;
    ok &= Check(store.Load(loaded, error), L"saved store should load");
    ok &= Check(loaded.size() == 2, L"saved store should preserve record count");
    if (loaded.size() == 2) {
        ok &= Check(loaded[0].name == registry.name, L"unicode and quotes should round-trip");
        ok &= Check(loaded[0].original == registry.original, L"registry restore fields should round-trip");
        ok &= Check(loaded[1].requiresAdmin, L"requiresAdmin should round-trip");
        ok &= Check(loaded[1].original == service.original, L"service restore fields should round-trip");
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
