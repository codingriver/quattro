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
    ActiveSetup,
    Driver,
    WmiSubscription,
    Winlogon,
    WinlogonNotify,
    AppInitDll,
    AppCertDll,
    BootExecute,
    KnownDll,
    ShellExtension,
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
    bool partial = false;
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

// 广告拦截（简化版）：对文件/文件夹内的可启动程序写 IFEO 禁止运行拦截。
// 与「自启动管理」独立，使用单独的 blocked-items.json 存储。
class AdBlockManager {
public:
    AdBlockManager();
    explicit AdBlockManager(DisabledItemStore store);

    // 扫描给定文件或文件夹下的可启动文件（不递归子目录）；每项 canDisable 已由守卫判定。
    ScanResult ScanPath(const std::wstring& fileOrDir) const;
    // mode = L"exact"（精确路径）| L"name"（同名程序）| L"startup"（仅禁自启，系统开关）。
    OperationResult Block(const std::wstring& targetPath, const std::wstring& mode) const;
    OperationResult Unblock(const std::wstring& recordId) const;
    bool ListBlocked(std::vector<DisabledRecord>& records, std::wstring& error) const;

private:
    // 启动拦截：禁用目标 exe 的所有开机自启动注册项（StartupApproved 系统开关）。
    OperationResult BlockStartup(const std::wstring& targetExe) const;

    DisabledItemStore store_;
};
