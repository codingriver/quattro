#pragma once

#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

struct TodoJsonExportOptions {
    bool includeCompleted = true;
    bool includeDisabled = true;
    bool onlyFuture = false;
};

enum class TodoJsonImportMode {
    Merge,
    ReplaceAll,
};

enum class TodoJsonRestoreDeletedPolicy {
    KeepDeleted,
    RestoreDeleted,
};

struct TodoJsonImportOptions {
    TodoJsonImportMode mode = TodoJsonImportMode::Merge;
    TodoJsonRestoreDeletedPolicy restoreDeletedPolicy = TodoJsonRestoreDeletedPolicy::KeepDeleted;
};

struct TodoJsonImportReport {
    bool ok = false;
    std::wstring message;
    int todosParsed = 0;
    int todosAdded = 0;
    int todosUpdatedFromRemote = 0;
    int todosKeptLocal = 0;
    int todosRestored = 0;
    int todosKeptDeleted = 0;
    int todosSkippedIdentical = 0;
    int todosConflicted = 0;
    int todosDeletedForReplace = 0;
    int todosFailed = 0;
    int groupsCreated = 0;
    int tagsCreated = 0;
    std::vector<std::wstring> warnings;
};

class TodoJsonBackupService {
public:
    explicit TodoJsonBackupService(std::filesystem::path appDirectory);

    static std::wstring DefaultFileName();

    bool ExportJson(const std::filesystem::path& targetPath, const TodoJsonExportOptions& options, std::wstring& error) const;
    TodoJsonImportReport PreviewImport(const std::filesystem::path& jsonPath, const TodoJsonImportOptions& options) const;
    TodoJsonImportReport ImportJson(const std::filesystem::path& jsonPath, const TodoJsonImportOptions& options) const;

private:
    std::filesystem::path appDirectory_;
};
