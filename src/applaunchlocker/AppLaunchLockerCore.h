#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

enum class StartupSourceType {
    Registry,
    StartupFolder,
    ScheduledTask,
    Service,
    Driver,
    WmiSubscription,
    Winlogon,
    AppInitDll,
    Ifeo,
};

struct StartupItem {
    std::wstring id;
    std::wstring name;
    StartupSourceType source = StartupSourceType::Registry;
    std::wstring location;
    std::wstring command;
    bool requiresAdmin = false;
    bool canDisable = false;
    bool readOnly = true;
    std::map<std::wstring, std::wstring> original;
};

struct DisabledRecord {
    std::wstring recordId;
    std::wstring itemId;
    StartupSourceType source = StartupSourceType::Registry;
    std::wstring name;
    std::wstring disabledAt;
    bool requiresAdmin = false;
    std::map<std::wstring, std::wstring> original;
};

struct ScanResult {
    std::vector<StartupItem> items;
    std::vector<std::wstring> warnings;
};

struct OperationResult {
    bool success = false;
    std::wstring message;
};

std::wstring StartupSourceKey(StartupSourceType source);
std::wstring StartupSourceText(StartupSourceType source);
bool StartupSourceFromKey(const std::wstring& key, StartupSourceType& source);
std::filesystem::path AppLaunchLockerDataDirectory();
void AppendAppLaunchLockerLog(const std::wstring& message);

class DisabledItemStore {
public:
    DisabledItemStore();
    explicit DisabledItemStore(std::filesystem::path path);

    bool Load(std::vector<DisabledRecord>& records, std::wstring& error) const;
    bool Save(const std::vector<DisabledRecord>& records, std::wstring& error) const;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class StartupManager {
public:
    StartupManager();
    explicit StartupManager(DisabledItemStore store);

    ScanResult ScanAll() const;
    bool LoadDisabled(std::vector<DisabledRecord>& records, std::wstring& error) const;
    OperationResult Disable(const std::wstring& itemId) const;
    OperationResult Restore(const std::wstring& recordId) const;

private:
    DisabledItemStore store_;
};
