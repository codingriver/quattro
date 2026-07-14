#include "../src/applaunchlocker/AppLaunchLockerCore.h"

#include <windows.h>

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
            StartupSourceType::ScheduledTask, StartupSourceType::Service, StartupSourceType::Driver,
            StartupSourceType::WmiSubscription, StartupSourceType::Winlogon, StartupSourceType::AppInitDll,
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
            item.source == StartupSourceType::Winlogon || item.source == StartupSourceType::AppInitDll ||
            item.source == StartupSourceType::Ifeo) {
            ok &= Check(item.readOnly && !item.canDisable, L"sensitive sources must always remain read-only");
        }
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
    if (ok) std::wcout << L"AppLaunchLocker tests passed\n";
    return ok ? 0 : 1;
}
