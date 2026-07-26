#pragma once

#include "ScanExecutionService.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
    enum class RestoreValueType {
        String,
        Boolean,
        Number,
        Json,
    };
    // Core operations continue to consume normalized strings, while this map
    // preserves the JSON type so version 2 restore data can round-trip without loss.
    std::map<std::wstring, RestoreValueType> originalTypes;
};

struct ScanResult {
    std::vector<StartupItem> items;
    std::vector<std::wstring> warnings;
};

enum class StartupEntryState {
    Enabled,
    DisabledByTool,
    ReadOnly,
    Unknown,
};

struct StartupApplicationEntry {
    std::wstring entryId;
    StartupSourceType source = StartupSourceType::Registry;
    std::wstring name;
    std::wstring location;
    std::wstring command;
    StartupEntryState state = StartupEntryState::Unknown;
    bool requiresAdmin = false;
    bool canDisable = false;
    bool canRestore = false;
    bool readOnly = true;
    std::map<std::wstring, std::wstring> original;
};

struct StartupApplication {
    std::wstring appId;
    std::wstring displayName;
    std::wstring targetPath;
    std::vector<StartupApplicationEntry> entries;
};

struct StartupSnapshotEntry {
    std::wstring entryId;
    StartupSourceType source = StartupSourceType::Registry;
    std::wstring displayName;
    std::wstring targetPath;
    std::wstring state;
    std::wstring fingerprint;
};

struct StartupSnapshot {
    std::wstring capturedAt;
    std::wstring scanId;
    std::vector<StartupSnapshotEntry> entries;
};

struct StartupSnapshotDiff {
    std::vector<StartupSnapshotEntry> added;
    std::vector<StartupSnapshotEntry> removed;
    std::vector<StartupSnapshotEntry> changed;
    std::vector<StartupSnapshotEntry> stateChanged;
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

struct OperationItemResult {
    std::wstring targetId;
    std::wstring action;
    bool success = false;
    std::wstring message;
    bool rolledBack = false;
};

struct OperationResult {
    bool success = false;
    std::wstring message;
    bool partial = false;
    std::vector<OperationItemResult> items;
};

struct ElevatedOperationRequest {
    std::wstring requestId;
    std::wstring token;
    std::wstring action;
    std::wstring mode;
    std::vector<std::wstring> targets;
    std::uint64_t expiresAt = 0;
    std::filesystem::path requestPath;
    std::filesystem::path resultPath;
};

enum class AdBlockRecordState {
    Active,
    Inactive,
    PartiallyActive,
    Overwritten,
    BrokenRecord,
    Unknown,
};

struct AdBlockRecordStatus {
    AdBlockRecordState state = AdBlockRecordState::Unknown;
    std::wstring message;
    bool canRepair = false;
    bool canUnblock = true;
};

struct AdBlockPlanItem {
    std::wstring targetPath;
    std::wstring imageName;
    std::wstring mode;
    std::wstring impactText;
    std::wstring riskLevel; // ok | warn | blocked
    std::wstring reason;
    bool willModifyIfeo = false;
    bool willModifyStartupApproved = false;
    bool hasExistingIfeoDebugger = false;
};

struct AdBlockPlan {
    std::vector<AdBlockPlanItem> items;
    int blockableCount = 0;
    int warningCount = 0;
    int blockedCount = 0;
};

std::wstring StartupSourceKey(StartupSourceType source);
std::wstring StartupSourceText(StartupSourceType source);
bool StartupSourceFromKey(const std::wstring& key, StartupSourceType& source);
std::wstring StartupEntryStateText(StartupEntryState state);
std::wstring AdBlockRecordStateKey(AdBlockRecordState state);
std::wstring AdBlockRecordStateText(AdBlockRecordState state);
std::wstring StartupApplicationStateText(const StartupApplication& application);
std::vector<StartupApplication> BuildStartupApplications(
    const ScanResult& scan,
    const std::vector<DisabledRecord>& disabled);
std::filesystem::path AppLaunchLockerDataDirectory();
void AppendAppLaunchLockerLog(const std::wstring& message);
void SetOperationAuditContext(
    const std::wstring& requestId,
    const std::wstring& launchSource,
    bool elevated);
bool CreateElevatedOperationRequest(
    const std::wstring& action,
    const std::vector<std::wstring>& targets,
    const std::wstring& mode,
    ElevatedOperationRequest& request,
    std::wstring& error);
bool LoadAndConsumeElevatedOperationRequest(
    const std::filesystem::path& requestPath,
    const std::wstring& token,
    ElevatedOperationRequest& request,
    std::wstring& error);
bool WriteElevatedOperationResult(
    const ElevatedOperationRequest& request,
    const OperationResult& result,
    std::wstring& error);
bool ReadElevatedOperationResult(
    const ElevatedOperationRequest& request,
    OperationResult& result,
    std::wstring& error);
StartupSnapshot BuildStartupSnapshot(const ScanResult& scan, const std::wstring& scanId = L"");
StartupSnapshot BuildStartupSnapshot(
    const ScanResult& scan,
    const std::vector<DisabledRecord>& disabled,
    const std::wstring& scanId = L"");
StartupSnapshotDiff DiffStartupSnapshots(const StartupSnapshot& previous, const StartupSnapshot& current);

class StartupSnapshotStore {
public:
    StartupSnapshotStore();
    explicit StartupSnapshotStore(std::filesystem::path path);

    bool Load(StartupSnapshot& snapshot, std::wstring& error) const;
    bool Save(const StartupSnapshot& snapshot, std::wstring& error) const;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class DisabledItemStore {
public:
    DisabledItemStore();
    explicit DisabledItemStore(std::filesystem::path path);

    bool Load(std::vector<DisabledRecord>& records, std::wstring& error) const;
    bool Save(const std::vector<DisabledRecord>& records, std::wstring& error) const;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    mutable int observedRevision_ = -1;
};

class StartupManager {
public:
    StartupManager();
    explicit StartupManager(DisabledItemStore store);

    ScanResult ScanAll() const;
    std::shared_ptr<ScanTaskHandle> StartScanAll(
        std::function<void()> completionCallback = {}) const;
    bool LoadDisabled(std::vector<DisabledRecord>& records, std::wstring& error) const;
    OperationResult Disable(const std::wstring& itemId) const;
    OperationResult DisableMany(const std::vector<std::wstring>& itemIds) const;
    OperationResult Restore(const std::wstring& recordId) const;
    OperationResult RestoreMany(const std::vector<std::wstring>& recordIds) const;

private:
    ScanResult ScanAllCore(ScanTaskContext& context) const;
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
    std::shared_ptr<ScanTaskHandle> StartScanPathDetailed(
        std::wstring fileOrDir,
        AdBlockScanOptions options = {},
        std::function<void()> completionCallback = {}) const;
    // mode = L"exact"（精确路径）| L"name"（同名程序）| L"startup"（仅禁自启，系统开关）。
    AdBlockPlan BuildBlockPlan(const std::vector<std::wstring>& targetPaths, const std::wstring& mode) const;
    OperationResult Block(const std::wstring& targetPath, const std::wstring& mode) const;
    OperationResult Unblock(const std::wstring& recordId) const;
    OperationResult RepairRecord(const std::wstring& recordId) const;
    OperationResult UnblockAll() const;
    OperationResult CleanStaleRecords() const;
    AdBlockRecordStatus CheckRecordStatus(const DisabledRecord& record) const;
    bool ListBlocked(std::vector<DisabledRecord>& records, std::wstring& error) const;

private:
    AdBlockScanResult ScanPathDetailedCore(
        const std::wstring& fileOrDir,
        const AdBlockCancelCheck& shouldCancel,
        const AdBlockProgressCallback& reportProgress,
        AdBlockScanOptions options,
        ScanTaskContext& context) const;
    // 启动拦截：禁用目标 exe 的所有开机自启动注册项（StartupApproved 系统开关）。
    OperationResult BlockStartup(const std::wstring& targetExe) const;

    DisabledItemStore store_;
};
