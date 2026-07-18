#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
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

enum class AdBlockScanPhase {
    Validating,
    Enumerating,
    IndexingStartup,
    Analyzing,
    Completed,
    Cancelled,
};

struct AdBlockScanProgress {
    AdBlockScanPhase phase = AdBlockScanPhase::Validating;
    std::size_t enumeratedFiles = 0;
    std::size_t discoveredCandidates = 0;
    std::size_t checkedCandidates = 0;
    std::size_t totalCandidates = 0;
    std::size_t autoStartMatches = 0;
    std::size_t inaccessibleDirectories = 0;
    std::size_t workerCount = 0;
};

struct AdBlockScanResult {
    ScanResult scan;
    std::size_t enumeratedFiles = 0;
    std::size_t checkedCandidates = 0;
    std::size_t totalCandidates = 0;
    std::size_t autoStartMatches = 0;
    std::size_t inaccessibleDirectories = 0;
    std::size_t workerCount = 0;
    bool directory = false;
    bool cancelled = false;
    std::wstring error;
};

struct AdBlockScanOptions {
    std::size_t batchSize = 16;
    std::size_t maxWorkers = 4;
    // Acceptance tests may use this to keep the progress window observable.
    std::chrono::milliseconds batchDelay{0};
};

using AdBlockCancelCheck = std::function<bool()>;
using AdBlockProgressCallback = std::function<void(const AdBlockScanProgress&)>;

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

    // 兼容入口；目录扫描会递归所有子目录。
    ScanResult ScanPath(const std::wstring& fileOrDir) const;
    // 带进度、取消和并行处理的详细扫描入口；GUI、CLI 和测试应优先复用此接口。
    AdBlockScanResult ScanPathDetailed(
        const std::wstring& fileOrDir,
        const AdBlockCancelCheck& shouldCancel = {},
        const AdBlockProgressCallback& reportProgress = {},
        AdBlockScanOptions options = {}) const;
    // mode = L"exact"（精确路径）| L"name"（同名程序）| L"startup"（仅禁自启，系统开关）。
    OperationResult Block(const std::wstring& targetPath, const std::wstring& mode) const;
    OperationResult Unblock(const std::wstring& recordId) const;
    bool ListBlocked(std::vector<DisabledRecord>& records, std::wstring& error) const;

private:
    // 启动拦截：禁用目标 exe 的所有开机自启动注册项（StartupApproved 系统开关）。
    OperationResult BlockStartup(const std::wstring& targetExe) const;

    DisabledItemStore store_;
};
